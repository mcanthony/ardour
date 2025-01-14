/*
    Copyright (C) 2004

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#ifndef COMPILER_MSVC
#include <stdbool.h>
#endif
#include <cstdio>

#include "ardour/audioengine.h"
#include "ardour/debug.h"
#include "ardour/session.h"
#include "ardour/tempo.h"
#include "ardour/windows_vst_plugin.h"
#include "ardour/vestige/aeffectx.h"
#include "ardour/vst_types.h"
#ifdef WINDOWS_VST_SUPPORT
#include <fst.h>
#endif

#include "i18n.h"

using namespace ARDOUR;

#define SHOW_CALLBACK(MSG) DEBUG_TRACE (PBD::DEBUG::VSTCallbacks, string_compose (MSG " val = %1 idx = %2", index, value))

int Session::vst_current_loading_id = 0;
const char* Session::vst_can_do_strings[] = {
	X_("supplyIdle"),
	X_("sendVstTimeInfo"),
	X_("sendVstEvents"),
	X_("sendVstMidiEvent"),
	X_("receiveVstEvents"),
	X_("receiveVstMidiEvent"),
	X_("supportShell"),
	X_("shellCategory"),
	X_("shellCategorycurID")
};
const int Session::vst_can_do_string_count = sizeof (vst_can_do_strings) / sizeof (char*);

intptr_t Session::vst_callback (
	AEffect* effect,
	int32_t opcode,
	int32_t index,
	intptr_t value,
	void* ptr,
	float opt
	)
{
	static VstTimeInfo _timeInfo;
	VSTPlugin* plug;
	Session* session;

	if (effect && effect->user) {
		plug = (VSTPlugin *) (effect->user);
		session = &plug->session();
		DEBUG_TRACE (PBD::DEBUG::VSTCallbacks, string_compose ("am callback 0x%1%2, opcode = %3%4, plugin = \"%5\" ",
					std::hex, (void*) DEBUG_THREAD_SELF,
					std::dec, opcode, plug->name()));
	} else {
		plug = 0;
		session = 0;
		DEBUG_TRACE (PBD::DEBUG::VSTCallbacks, string_compose ("am callback 0x%1%2, opcode = %3%4",
					std::hex, (void*) DEBUG_THREAD_SELF,
					std::dec, opcode));
	}

	switch(opcode){

	case audioMasterAutomate:
		SHOW_CALLBACK ("audioMasterAutomate");
		// index, value, returns 0
		if (plug) {
			plug->set_parameter (index, opt);
		}
		return 0;

	case audioMasterVersion:
		SHOW_CALLBACK ("audioMasterVersion");
		// vst version, currently 2 (0 for older)
		return 2400;

	case audioMasterCurrentId:
		SHOW_CALLBACK ("audioMasterCurrentId");
		// returns the unique id of a plug that's currently loading
		return vst_current_loading_id;

	case audioMasterIdle:
		SHOW_CALLBACK ("audioMasterIdle");
#ifdef WINDOWS_VST_SUPPORT
		fst_audio_master_idle();
#endif
		if (effect) {
			effect->dispatcher(effect, effEditIdle, 0, 0, NULL, 0.0f);
		}
		return 0;

	case audioMasterPinConnected:
		SHOW_CALLBACK ("audioMasterPinConnected");
		// inquire if an input or output is beeing connected;
		// index enumerates input or output counting from zero:
		// value is 0 for input and != 0 otherwise. note: the
		// return value is 0 for <true> such that older versions
		// will always return true.
		if (!plug) {
			// we don't know.
			// but ardour always connects all buffers, so we're good
			return 0;
		}
		switch (value) {
			case 0:
				if (index < plug->plugin()->numInputs) {
					return 0;
				}
				break;
			case 1:
				if (index < plug->plugin()->numOutputs) {
					return 0;
				}
				break;
			default:
				break;
		}
		return 1;

	case audioMasterWantMidi:
		SHOW_CALLBACK ("audioMasterWantMidi");
		// <value> is a filter which is currently ignored
		if (plug && plug->get_info() != NULL) {
			plug->get_info()->n_inputs.set_midi (1);
		}
		return 0;

	case audioMasterGetTime:
		SHOW_CALLBACK ("audioMasterGetTime");
		// returns const VstTimeInfo* (or 0 if not supported)
		// <value> should contain a mask indicating which fields are required
		// (see valid masks above), as some items may require extensive
		// conversions
		_timeInfo.flags = 0;

		if (session) {
			framepos_t now = session->transport_frame();

			_timeInfo.samplePos = now;
			_timeInfo.sampleRate = session->frame_rate();

			const TempoMetric& tm (session->tempo_map().metric_at (now));

			if (value & (kVstTempoValid)) {
				const Tempo& t (tm.tempo());
				_timeInfo.tempo = t.beats_per_minute ();
				_timeInfo.flags |= (kVstTempoValid);
			}
			if (value & (kVstTimeSigValid)) {
				const Meter& m (tm.meter());
				_timeInfo.timeSigNumerator = m.divisions_per_bar ();
				_timeInfo.timeSigDenominator = m.note_divisor ();
				_timeInfo.flags |= (kVstTimeSigValid);
			}
			if ((value & (kVstPpqPosValid)) || (value & (kVstBarsValid))) {
				Timecode::BBT_Time bbt;

				try {
					session->tempo_map().bbt_time_rt (now, bbt);

					/* PPQ = pulse per quarter
					 * VST's "pulse" is our "division".
					 *
					 * 8 divisions per bar, 1 division = quarter, so 8 quarters per bar, ppq = 1
					 * 8 divisions per bar, 1 division = eighth, so  4 quarters per bar, ppq = 2
					 * 4 divisions per bar, 1 division = quarter, so  4 quarters per bar, ppq = 1
					 * 4 divisions per bar, 1 division = half, so 8 quarters per bar, ppq = 0.5
					 * 4 divisions per bar, 1 division = fifth, so (4 * 5/4) quarters per bar, ppq = 5/4
					 *
					 * general: divs_per_bar / (note_type / 4.0)
					 */
					double ppq_scaling =  tm.meter().note_divisor() / 4.0;

					/* Note that this assumes constant meter/tempo throughout the session. Stupid VST */
					double ppqBar = double(bbt.bars - 1) * tm.meter().divisions_per_bar();
					double ppqBeat = double(bbt.beats - 1);
					double ppqTick = double(bbt.ticks) / Timecode::BBT_Time::ticks_per_beat;

					ppqBar *= ppq_scaling;
					ppqBeat *= ppq_scaling;
					ppqTick *= ppq_scaling;

					if (value & (kVstPpqPosValid)) {
						_timeInfo.ppqPos = ppqBar + ppqBeat + ppqTick;
						_timeInfo.flags |= (kVstPpqPosValid);
					}

					if (value & (kVstBarsValid)) {
						_timeInfo.barStartPos = ppqBar;
						_timeInfo.flags |= (kVstBarsValid);
					}

				} catch (...) {
					/* relax */
				}
			}

			if (value & (kVstSmpteValid)) {
				Timecode::Time t;

				session->timecode_time (now, t);

				_timeInfo.smpteOffset = (t.hours * t.rate * 60.0 * 60.0) +
					(t.minutes * t.rate * 60.0) +
					(t.seconds * t.rate) +
					(t.frames) +
					(t.subframes);

				_timeInfo.smpteOffset *= 80.0; /* VST spec is 1/80th frames */

				if (session->timecode_drop_frames()) {
					if (session->timecode_frames_per_second() == 30.0) {
						_timeInfo.smpteFrameRate = 5;
					} else {
						_timeInfo.smpteFrameRate = 4; /* 29.97 assumed, thanks VST */
					}
				} else {
					if (session->timecode_frames_per_second() == 24.0) {
						_timeInfo.smpteFrameRate = 0;
					} else if (session->timecode_frames_per_second() == 24.975) {
						_timeInfo.smpteFrameRate = 2;
					} else if (session->timecode_frames_per_second() == 25.0) {
						_timeInfo.smpteFrameRate = 1;
					} else {
						_timeInfo.smpteFrameRate = 3; /* 30 fps */
					}
				}
				_timeInfo.flags |= (kVstSmpteValid);
			}

			if (session->transport_speed() != 0.0f) {
				_timeInfo.flags |= (kVstTransportPlaying);
			}

			if (session->get_play_loop()) {
			}

		} else {
			_timeInfo.samplePos = 0;
			_timeInfo.sampleRate = AudioEngine::instance()->sample_rate();
		}

		return (intptr_t) &_timeInfo;

	case audioMasterProcessEvents:
		SHOW_CALLBACK ("audioMasterProcessEvents");
		// VstEvents* in <ptr>
		if (plug && plug->midi_buffer()) {
			VstEvents* v = (VstEvents*)ptr;
			for (int n = 0 ; n < v->numEvents; ++n) {
				VstMidiEvent *vme = (VstMidiEvent*) (v->events[n]->dump);
				if (vme->type == kVstMidiType) {
					plug->midi_buffer()->push_back(vme->deltaFrames, 3, (uint8_t*)vme->midiData);
				}
			}
		}
		return 0;

	case audioMasterSetTime:
		SHOW_CALLBACK ("audioMasterSetTime");
		// VstTimenfo* in <ptr>, filter in <value>, not supported

	case audioMasterTempoAt:
		SHOW_CALLBACK ("audioMasterTempoAt");
		// returns tempo (in bpm * 10000) at sample frame location passed in <value>
		if (session) {
			const Tempo& t (session->tempo_map().tempo_at (value));
			return t.beats_per_minute() * 1000;
		} else {
			return 0;
		}
		break;

	case audioMasterGetNumAutomatableParameters:
		SHOW_CALLBACK ("audioMasterGetNumAutomatableParameters");
		return 0;

	case audioMasterGetParameterQuantization:
		SHOW_CALLBACK ("audioMasterGetParameterQuantization");
		// returns the integer value for +1.0 representation,
		// or 1 if full single float precision is maintained
		// in automation. parameter index in <value> (-1: all, any)
		return 0;

	case audioMasterIOChanged:
		SHOW_CALLBACK ("audioMasterIOChanged");
		// numInputs and/or numOutputs has changed
		return 0;

	case audioMasterNeedIdle:
		SHOW_CALLBACK ("audioMasterNeedIdle");
		// plug needs idle calls (outside its editor window)
		if (plug) {
			plug->state()->wantIdle = 1;
		}
		return 0;

	case audioMasterSizeWindow:
		SHOW_CALLBACK ("audioMasterSizeWindow");
		// index: width, value: height
		return 0;

	case audioMasterGetSampleRate:
		SHOW_CALLBACK ("audioMasterGetSampleRate");
		if (session) {
			return session->frame_rate();
		}
		return 0;

	case audioMasterGetBlockSize:
		SHOW_CALLBACK ("audioMasterGetBlockSize");
		if (session) {
			return session->get_block_size();
		}
		return 0;

	case audioMasterGetInputLatency:
		SHOW_CALLBACK ("audioMasterGetInputLatency");
		return 0;

	case audioMasterGetOutputLatency:
		SHOW_CALLBACK ("audioMasterGetOutputLatency");
		return 0;

	case audioMasterGetPreviousPlug:
		SHOW_CALLBACK ("audioMasterGetPreviousPlug");
		// input pin in <value> (-1: first to come), returns cEffect*
		return 0;

	case audioMasterGetNextPlug:
		SHOW_CALLBACK ("audioMasterGetNextPlug");
		// output pin in <value> (-1: first to come), returns cEffect*

	case audioMasterWillReplaceOrAccumulate:
		SHOW_CALLBACK ("audioMasterWillReplaceOrAccumulate");
		// returns: 0: not supported, 1: replace, 2: accumulate
		return 0;

	case audioMasterGetCurrentProcessLevel:
		SHOW_CALLBACK ("audioMasterGetCurrentProcessLevel");
		// returns: 0: not supported,
		// 1: currently in user thread (gui)
		// 2: currently in audio thread (where process is called)
		// 3: currently in 'sequencer' thread (midi, timer etc)
		// 4: currently offline processing and thus in user thread
		// other: not defined, but probably pre-empting user thread.
		return 0;

	case audioMasterGetAutomationState:
		SHOW_CALLBACK ("audioMasterGetAutomationState");
		// returns 0: not supported, 1: off, 2:read, 3:write, 4:read/write
		// offline
		return 0;

	case audioMasterOfflineStart:
		SHOW_CALLBACK ("audioMasterOfflineStart");
		return 0;

	case audioMasterOfflineRead:
		SHOW_CALLBACK ("audioMasterOfflineRead");
		// ptr points to offline structure, see below. return 0: error, 1 ok
		return 0;

	case audioMasterOfflineWrite:
		SHOW_CALLBACK ("audioMasterOfflineWrite");
		// same as read
		return 0;

	case audioMasterOfflineGetCurrentPass:
		SHOW_CALLBACK ("audioMasterOfflineGetCurrentPass");
		return 0;

	case audioMasterOfflineGetCurrentMetaPass:
		SHOW_CALLBACK ("audioMasterOfflineGetCurrentMetaPass");
		return 0;

	case audioMasterSetOutputSampleRate:
		SHOW_CALLBACK ("audioMasterSetOutputSampleRate");
		// for variable i/o, sample rate in <opt>
		return 0;

	case audioMasterGetSpeakerArrangement:
		SHOW_CALLBACK ("audioMasterGetSpeakerArrangement");
		// (long)input in <value>, output in <ptr>
		return 0;

	case audioMasterGetVendorString:
		SHOW_CALLBACK ("audioMasterGetVendorString");
		// fills <ptr> with a string identifying the vendor (max 64 char)
		strcpy ((char*) ptr, "Linux Audio Systems");
		return 0;

	case audioMasterGetProductString:
		SHOW_CALLBACK ("audioMasterGetProductString");
		// fills <ptr> with a string with product name (max 64 char)
		strcpy ((char*) ptr, PROGRAM_NAME);
		return 0;

	case audioMasterGetVendorVersion:
		SHOW_CALLBACK ("audioMasterGetVendorVersion");
		// returns vendor-specific version
		return 900;

	case audioMasterVendorSpecific:
		SHOW_CALLBACK ("audioMasterVendorSpecific");
		// no definition, vendor specific handling
		return 0;

	case audioMasterSetIcon:
		SHOW_CALLBACK ("audioMasterSetIcon");
		// void* in <ptr>, format not defined yet
		return 0;

	case audioMasterCanDo:
		SHOW_CALLBACK ("audioMasterCanDo");
		// string in ptr,  (const char*)ptr
		for (int i = 0; i < vst_can_do_string_count; i++) {
			if (! strcmp(vst_can_do_strings[i], (const char*)ptr)) {
				return 1;
			}
		}
		return 0;

	case audioMasterGetLanguage:
		SHOW_CALLBACK ("audioMasterGetLanguage");
		// see enum
		return 0;

	case audioMasterOpenWindow:
		SHOW_CALLBACK ("audioMasterOpenWindow");
		// returns platform specific ptr
		return 0;

	case audioMasterCloseWindow:
		SHOW_CALLBACK ("audioMasterCloseWindow");
		// close window, platform specific handle in <ptr>
		return 0;

	case audioMasterGetDirectory:
		SHOW_CALLBACK ("audioMasterGetDirectory");
		// get plug directory, FSSpec on MAC, else char*
		return 0;

	case audioMasterUpdateDisplay:
		SHOW_CALLBACK ("audioMasterUpdateDisplay");
		// something has changed, update 'multi-fx' display
		if (effect) {
			effect->dispatcher(effect, effEditIdle, 0, 0, NULL, 0.0f);
		}
		return 0;

	case audioMasterBeginEdit:
		SHOW_CALLBACK ("audioMasterBeginEdit");
		// begin of automation session (when mouse down), parameter index in <index>
		return 0;

	case audioMasterEndEdit:
		SHOW_CALLBACK ("audioMasterEndEdit");
		// end of automation session (when mouse up),     parameter index in <index>
		return 0;

	case audioMasterOpenFileSelector:
		SHOW_CALLBACK ("audioMasterOpenFileSelector");
		// open a fileselector window with VstFileSelect* in <ptr>
		return 0;

	default:
		DEBUG_TRACE (PBD::DEBUG::VSTCallbacks, string_compose ("VST master dispatcher: undefed: %1\n", opcode));
		break;
	}

	return 0;
}

