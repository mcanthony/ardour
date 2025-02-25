/*
    Copyright (C) 1999-2010 Paul Davis

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

#include <stdint.h>

#include <algorithm>
#include <string>
#include <vector>
#include <sstream>
#include <cstdio> /* sprintf(3) ... grrr */
#include <cmath>
#include <cerrno>
#include <unistd.h>
#include <limits.h>

#include <glibmm/threads.h>
#include <glibmm/miscutils.h>
#include <glibmm/fileutils.h>

#include <boost/algorithm/string/erase.hpp>

#include "pbd/basename.h"
#include "pbd/boost_debug.h"
#include "pbd/convert.h"
#include "pbd/convert.h"
#include "pbd/error.h"
#include "pbd/file_utils.h"
#include "pbd/md5.h"
#include "pbd/search_path.h"
#include "pbd/stacktrace.h"
#include "pbd/stl_delete.h"
#include "pbd/replace_all.h"
#include "pbd/unwind.h"

#include "ardour/amp.h"
#include "ardour/analyser.h"
#include "ardour/async_midi_port.h"
#include "ardour/audio_buffer.h"
#include "ardour/audio_diskstream.h"
#include "ardour/audio_port.h"
#include "ardour/audio_track.h"
#include "ardour/audioengine.h"
#include "ardour/audiofilesource.h"
#include "ardour/auditioner.h"
#include "ardour/buffer_manager.h"
#include "ardour/buffer_set.h"
#include "ardour/bundle.h"
#include "ardour/butler.h"
#include "ardour/click.h"
#include "ardour/control_protocol_manager.h"
#include "ardour/data_type.h"
#include "ardour/debug.h"
#include "ardour/directory_names.h"
#ifdef USE_TRACKS_CODE_FEATURES
#include "ardour/engine_state_controller.h"
#endif
#include "ardour/filename_extensions.h"
#include "ardour/graph.h"
#include "ardour/midiport_manager.h"
#include "ardour/scene_changer.h"
#include "ardour/midi_track.h"
#include "ardour/midi_ui.h"
#include "ardour/operations.h"
#include "ardour/playlist.h"
#include "ardour/plugin.h"
#include "ardour/plugin_insert.h"
#include "ardour/process_thread.h"
#include "ardour/profile.h"
#include "ardour/rc_configuration.h"
#include "ardour/recent_sessions.h"
#include "ardour/region.h"
#include "ardour/region_factory.h"
#include "ardour/route_graph.h"
#include "ardour/route_group.h"
#include "ardour/route_sorters.h"
#include "ardour/send.h"
#include "ardour/session.h"
#include "ardour/session_directory.h"
#include "ardour/session_playlists.h"
#include "ardour/smf_source.h"
#include "ardour/source_factory.h"
#include "ardour/speakers.h"
#include "ardour/tempo.h"
#include "ardour/track.h"
#include "ardour/user_bundle.h"
#include "ardour/utils.h"

#include "midi++/port.h"
#include "midi++/mmc.h"

#include "i18n.h"

#include <glibmm/checksum.h>

namespace ARDOUR {
class MidiSource;
class Processor;
class Speakers;
}

using namespace std;
using namespace ARDOUR;
using namespace PBD;

bool Session::_disable_all_loaded_plugins = false;
bool Session::_bypass_all_loaded_plugins = false;

PBD::Signal1<int,uint32_t> Session::AudioEngineSetupRequired;
PBD::Signal1<void,std::string> Session::Dialog;
PBD::Signal0<int> Session::AskAboutPendingState;
PBD::Signal2<int, framecnt_t, framecnt_t> Session::AskAboutSampleRateMismatch;
PBD::Signal0<void> Session::SendFeedback;
PBD::Signal3<int,Session*,std::string,DataType> Session::MissingFile;

PBD::Signal1<void, framepos_t> Session::StartTimeChanged;
PBD::Signal1<void, framepos_t> Session::EndTimeChanged;
PBD::Signal2<void,std::string, std::string> Session::Exported;
PBD::Signal1<int,boost::shared_ptr<Playlist> > Session::AskAboutPlaylistDeletion;
PBD::Signal0<void> Session::Quit;
PBD::Signal0<void> Session::FeedbackDetected;
PBD::Signal0<void> Session::SuccessfulGraphSort;
PBD::Signal2<void,std::string,std::string> Session::VersionMismatch;

const framecnt_t Session::bounce_chunk_size = 8192;
static void clean_up_session_event (SessionEvent* ev) { delete ev; }
const SessionEvent::RTeventCallback Session::rt_cleanup (clean_up_session_event);

// seconds should be added after the region exceeds end marker
#ifdef USE_TRACKS_CODE_FEATURES
const uint32_t Session::session_end_shift = 5;
#else
const uint32_t Session::session_end_shift = 0;
#endif

/** @param snapshot_name Snapshot name, without .ardour suffix */
Session::Session (AudioEngine &eng,
                  const string& fullpath,
                  const string& snapshot_name,
                  BusProfile* bus_profile,
                  string mix_template)
	: playlists (new SessionPlaylists)
	, _engine (eng)
	, process_function (&Session::process_with_events)
	, _bounce_processing_active (false)
	, waiting_for_sync_offset (false)
	, _base_frame_rate (0)
	, _current_frame_rate (0)
	, _nominal_frame_rate (0)
	, transport_sub_state (0)
	, _record_status (Disabled)
	, _transport_frame (0)
	, _session_range_location (0)
	, _slave (0)
	, _silent (false)
	, _transport_speed (0)
	, _default_transport_speed (1.0)
	, _last_transport_speed (0)
	, _target_transport_speed (0.0)
	, auto_play_legal (false)
	, _last_slave_transport_frame (0)
	, maximum_output_latency (0)
	, _requested_return_frame (-1)
	, current_block_size (0)
	, _worst_output_latency (0)
	, _worst_input_latency (0)
 	, _worst_track_latency (0)
	, _have_captured (false)
	, _non_soloed_outs_muted (false)
	, _listen_cnt (0)
	, _solo_isolated_cnt (0)
	, _writable (false)
	, _was_seamless (Config->get_seamless_loop ())
	, _under_nsm_control (false)
	, _xrun_count (0)
	, delta_accumulator_cnt (0)
	, average_slave_delta (1800) // !!! why 1800 ???
	, average_dir (0)
	, have_first_delta_accumulator (false)
	, _slave_state (Stopped)
	, _mtc_active (false)
	, _ltc_active (false)
	, post_export_sync (false)
	, post_export_position (0)
	, _exporting (false)
	, _export_started (false)
	, _export_rolling (false)
	, _pre_export_mmc_enabled (false)
	, _name (snapshot_name)
	, _is_new (true)
	, _send_qf_mtc (false)
	, _pframes_since_last_mtc (0)
	, session_midi_feedback (0)
	, play_loop (false)
	, loop_changing (false)
	, last_loopend (0)
	, _session_dir (new SessionDirectory (fullpath))
	, _current_snapshot_name (snapshot_name)
	, state_tree (0)
	, state_was_pending (false)
	, _state_of_the_state (StateOfTheState(CannotSave|InitialConnecting|Loading))
	, _suspend_save (0)
	, _save_queued (false)
	, _last_roll_location (0)
	, _last_roll_or_reversal_location (0)
	, _last_record_location (0)
	, pending_locate_roll (false)
	, pending_locate_frame (0)
	, pending_locate_flush (false)
	, pending_abort (false)
	, pending_auto_loop (false)
	, _butler (new Butler (*this))
	, _post_transport_work (0)
	,  cumulative_rf_motion (0)
	, rf_scale (1.0)
	, _locations (new Locations (*this))
	, _ignore_skips_updates (false)
	, _rt_thread_active (false)
	, _rt_emit_pending (false)
	, step_speed (0)
	, outbound_mtc_timecode_frame (0)
	, next_quarter_frame_to_send (-1)
	, _frames_per_timecode_frame (0)
	, _frames_per_hour (0)
	, _timecode_frames_per_hour (0)
	, last_timecode_valid (false)
	, last_timecode_when (0)
	, _send_timecode_update (false)
	, ltc_encoder (0)
	, ltc_enc_buf(0)
	, ltc_buf_off (0)
	, ltc_buf_len (0)
	, ltc_speed (0)
	, ltc_enc_byte (0)
	, ltc_enc_pos (0)
	, ltc_enc_cnt (0)
	, ltc_enc_off (0)
	, restarting (false)
	, ltc_prev_cycle (0)
	, ltc_timecode_offset (0)
	, ltc_timecode_negative_offset (false)
	, midi_control_ui (0)
	, _tempo_map (0)
	, _all_route_group (new RouteGroup (*this, "all"))
	, routes (new RouteList)
	, _adding_routes_in_progress (false)
	, _reconnecting_routes_in_progress (false)
	, _route_deletion_in_progress (false)
	, destructive_index (0)
	, _track_number_decimals(1)
	, default_fade_steepness (0)
	, default_fade_msecs (0)
	, _total_free_4k_blocks (0)
	, _total_free_4k_blocks_uncertain (false)
	, no_questions_about_missing_files (false)
	, _playback_load (0)
	, _capture_load (0)
	, _bundles (new BundleList)
	, _bundle_xml_node (0)
	, _current_trans (0)
	, _clicking (false)
	, click_data (0)
	, click_emphasis_data (0)
	, click_length (0)
	, click_emphasis_length (0)
	, _clicks_cleared (0)
	, _play_range (false)
	, _range_selection (-1,-1)
	, _object_selection (-1,-1)
	, main_outs (0)
	, first_file_data_format_reset (true)
	, first_file_header_format_reset (true)
	, have_looped (false)
	, _have_rec_enabled_track (false)
    , _have_rec_disabled_track (true)
	, _step_editors (0)
	, _suspend_timecode_transmission (0)
	,  _speakers (new Speakers)
	, _order_hint (-1)
	, ignore_route_processor_changes (false)
	, _scene_changer (0)
	, _midi_ports (0)
	, _mmc (0)
{
	uint32_t sr = 0;

	pthread_mutex_init (&_rt_emit_mutex, 0);
	pthread_cond_init (&_rt_emit_cond, 0);

	pre_engine_init (fullpath);

	if (_is_new) {

		Stateful::loading_state_version = CURRENT_SESSION_FILE_VERSION;

#ifdef USE_TRACKS_CODE_FEATURES
		sr = EngineStateController::instance()->get_current_sample_rate();
#endif
		if (ensure_engine (sr)) {
			destroy ();
			throw SessionException (_("Cannot connect to audio/midi engine"));
		}

		if (create (mix_template, bus_profile)) {
			destroy ();
			throw SessionException (_("Session initialization failed"));
		}

		/* if a mix template was provided, then ::create() will
		 * have copied it into the session and we need to load it
		 * so that we have the state ready for ::set_state()
		 * after the engine is started.
		 *
		 * Note that we do NOT try to get the sample rate from
		 * the template at this time, though doing so would
		 * be easy if we decided this was an appropriate part
		 * of a template.
		 */

		if (!mix_template.empty()) {
			if (load_state (_current_snapshot_name)) {
				throw SessionException (_("Failed to load template/snapshot state"));
			}
			store_recent_templates (mix_template);
		}

		/* load default session properties - if any */
		config.load_state();

	} else {

		if (load_state (_current_snapshot_name)) {
			throw SessionException (_("Failed to load state"));
		}

		/* try to get sample rate from XML state so that we
		 * can influence the SR if we set up the audio
		 * engine.
		 */

		if (state_tree) {
			const XMLProperty* prop;
			if ((prop = state_tree->root()->property (X_("sample-rate"))) != 0) {
				sr = atoi (prop->value());
			}
		}

		if (ensure_engine (sr)) {
			destroy ();
			throw SessionException (_("Cannot connect to audio/midi engine"));
		}
	}

	if (post_engine_init ()) {
		destroy ();
		throw SessionException (_("Cannot configure audio/midi engine with session parameters"));
	}

	store_recent_sessions (_name, _path);

	bool was_dirty = dirty();

	_state_of_the_state = StateOfTheState (_state_of_the_state & ~Dirty);

	Config->ParameterChanged.connect_same_thread (*this, boost::bind (&Session::config_changed, this, _1, false));
	config.ParameterChanged.connect_same_thread (*this, boost::bind (&Session::config_changed, this, _1, true));

	if (was_dirty) {
		DirtyChanged (); /* EMIT SIGNAL */
	}

	StartTimeChanged.connect_same_thread (*this, boost::bind (&Session::start_time_changed, this, _1));
	EndTimeChanged.connect_same_thread (*this, boost::bind (&Session::end_time_changed, this, _1));

	emit_thread_start ();

	/* hook us up to the engine since we are now completely constructed */

	BootMessage (_("Connect to engine"));

	_engine.set_session (this);
	_engine.reset_timebase ();

#ifdef USE_TRACKS_CODE_FEATURES

	EngineStateController::instance()->set_session(this);

	if (_is_new ) {
		if ( ARDOUR::Profile->get_trx () ) {

			/* Waves Tracks: fill session with tracks basing on the amount of inputs.
			 * each available input must have corresponding track when session starts.
			 */

			uint32_t how_many (0);

			std::vector<std::string> inputs;
			EngineStateController::instance()->get_physical_audio_inputs(inputs);

			how_many = inputs.size();

			list<boost::shared_ptr<AudioTrack> > tracks;

			// Track names after driver
			if (Config->get_tracks_auto_naming() == NameAfterDriver) {
				string track_name = "";
				for (std::vector<string>::size_type i = 0; i < inputs.size(); ++i) {
					string track_name;
					track_name = inputs[i];
					replace_all (track_name, "system:capture", "");

					list<boost::shared_ptr<AudioTrack> > single_track = new_audio_track (1, 1, Normal, 0, 1, track_name);
					tracks.insert(tracks.begin(), single_track.front());
				}
			} else { // Default track names
				tracks = new_audio_track (1, 1, Normal, 0, how_many, string());
			}

			if (tracks.size() != how_many) {
				destroy ();
				throw failed_constructor ();
			}
		}
	}
#endif

	_is_new = false;
	session_loaded ();

	BootMessage (_("Session loading complete"));
}

Session::~Session ()
{
#ifdef PT_TIMING
	ST.dump ("ST.dump");
#endif
	destroy ();
}

int
Session::ensure_engine (uint32_t desired_sample_rate)
{
	if (_engine.current_backend() == 0) {
		/* backend is unknown ... */
		boost::optional<int> r = AudioEngineSetupRequired (desired_sample_rate);
		if (r.get_value_or (-1) != 0) {
			return -1;
		}
	} else if (_engine.setup_required()) {
		/* backend is known, but setup is needed */
		boost::optional<int> r = AudioEngineSetupRequired (desired_sample_rate);
		if (r.get_value_or (-1) != 0) {
			return -1;
		}
	} else if (!_engine.running()) {
		if (_engine.start()) {
			return -1;
		}
	}

	/* at this point the engine should be running
	*/

	if (!_engine.running()) {
		return -1;
	}

	return immediately_post_engine ();

}

int
Session::immediately_post_engine ()
{
	/* Do various initializations that should take place directly after we
	 * know that the engine is running, but before we either create a
	 * session or set state for an existing one.
	 */

	if (how_many_dsp_threads () > 1) {
		/* For now, only create the graph if we are using >1 DSP threads, as
		   it is a bit slower than the old code with 1 thread.
		*/
		_process_graph.reset (new Graph (*this));
	}

	/* every time we reconnect, recompute worst case output latencies */

	_engine.Running.connect_same_thread (*this, boost::bind (&Session::initialize_latencies, this));

	if (synced_to_engine()) {
		_engine.transport_stop ();
	}

	if (config.get_jack_time_master()) {
		_engine.transport_locate (_transport_frame);
	}

	try {
		BootMessage (_("Set up LTC"));
		setup_ltc ();
		BootMessage (_("Set up Click"));
		setup_click ();
		BootMessage (_("Set up standard connections"));
		setup_bundles ();
	}

	catch (failed_constructor& err) {
		return -1;
	}

	/* TODO, connect in different thread. (PortRegisteredOrUnregistered may be in RT context)
	 * can we do that? */
	 _engine.PortRegisteredOrUnregistered.connect_same_thread (*this, boost::bind (&Session::setup_bundles, this));

	return 0;
}

void
Session::destroy ()
{
	vector<void*> debug_pointers;

	/* if we got to here, leaving pending capture state around
	   is a mistake.
	*/

	remove_pending_capture_state ();

	_state_of_the_state = StateOfTheState (CannotSave|Deletion);

	/* disconnect from any and all signals that we are connected to */

	drop_connections ();

	/* shutdown control surface protocols while we still have ports
	   and the engine to move data to any devices.
	*/

	ControlProtocolManager::instance().drop_protocols ();

	_engine.remove_session ();

#ifdef USE_TRACKS_CODE_FEATURES
	EngineStateController::instance()->remove_session();
#endif

	/* deregister all ports - there will be no process or any other
	 * callbacks from the engine any more.
	 */

	Port::PortDrop (); /* EMIT SIGNAL */

	ltc_tx_cleanup();

	/* clear history so that no references to objects are held any more */

	_history.clear ();

	/* clear state tree so that no references to objects are held any more */

	delete state_tree;
	state_tree = 0;

	/* reset dynamic state version back to default */

	Stateful::loading_state_version = 0;

	_butler->drop_references ();
	delete _butler;
	_butler = 0;

	delete _all_route_group;

	DEBUG_TRACE (DEBUG::Destruction, "delete route groups\n");
	for (list<RouteGroup *>::iterator i = _route_groups.begin(); i != _route_groups.end(); ++i) {
		delete *i;
	}

	if (click_data != default_click) {
		delete [] click_data;
	}

	if (click_emphasis_data != default_click_emphasis) {
		delete [] click_emphasis_data;
	}

	clear_clicks ();

	/* need to remove auditioner before monitoring section
	 * otherwise it is re-connected */
	auditioner.reset ();

	/* drop references to routes held by the monitoring section
	 * specifically _monitor_out aux/listen references */
	remove_monitor_section();

	/* clear out any pending dead wood from RCU managed objects */

	routes.flush ();
	_bundles.flush ();

	AudioDiskstream::free_working_buffers();

	/* tell everyone who is still standing that we're about to die */
	drop_references ();

	/* tell everyone to drop references and delete objects as we go */

	DEBUG_TRACE (DEBUG::Destruction, "delete regions\n");
	RegionFactory::delete_all_regions ();

	DEBUG_TRACE (DEBUG::Destruction, "delete routes\n");

	/* reset these three references to special routes before we do the usual route delete thing */

	_master_out.reset ();
	_monitor_out.reset ();

	{
		RCUWriter<RouteList> writer (routes);
		boost::shared_ptr<RouteList> r = writer.get_copy ();

		for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
			DEBUG_TRACE(DEBUG::Destruction, string_compose ("Dropping for route %1 ; pre-ref = %2\n", (*i)->name(), (*i).use_count()));
			(*i)->drop_references ();
		}

		r->clear ();
		/* writer goes out of scope and updates master */
	}
	routes.flush ();

	{
		DEBUG_TRACE (DEBUG::Destruction, "delete sources\n");
		Glib::Threads::Mutex::Lock lm (source_lock);
		for (SourceMap::iterator i = sources.begin(); i != sources.end(); ++i) {
			DEBUG_TRACE(DEBUG::Destruction, string_compose ("Dropping for source %1 ; pre-ref = %2\n", i->second->name(), i->second.use_count()));
			i->second->drop_references ();
		}

		sources.clear ();
	}

	/* not strictly necessary, but doing it here allows the shared_ptr debugging to work */
	playlists.reset ();

	emit_thread_terminate ();

	pthread_cond_destroy (&_rt_emit_cond);
	pthread_mutex_destroy (&_rt_emit_mutex);

	delete _scene_changer; _scene_changer = 0;
	delete midi_control_ui; midi_control_ui = 0;

	delete _mmc; _mmc = 0;
	delete _midi_ports; _midi_ports = 0;
	delete _locations; _locations = 0;

	delete _tempo_map;

	DEBUG_TRACE (DEBUG::Destruction, "Session::destroy() done\n");

#ifdef BOOST_SP_ENABLE_DEBUG_HOOKS
	boost_debug_list_ptrs ();
#endif
}

void
Session::setup_ltc ()
{
	XMLNode* child = 0;

	_ltc_input.reset (new IO (*this, X_("LTC In"), IO::Input));
	_ltc_output.reset (new IO (*this, X_("LTC Out"), IO::Output));

	if (state_tree && (child = find_named_node (*state_tree->root(), X_("LTC In"))) != 0) {
		_ltc_input->set_state (*(child->children().front()), Stateful::loading_state_version);
	} else {
		{
			Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());
			_ltc_input->ensure_io (ChanCount (DataType::AUDIO, 1), true, this);
		}
		reconnect_ltc_input ();
	}

	if (state_tree && (child = find_named_node (*state_tree->root(), X_("LTC Out"))) != 0) {
		_ltc_output->set_state (*(child->children().front()), Stateful::loading_state_version);
	} else {
		{
			Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());
			_ltc_output->ensure_io (ChanCount (DataType::AUDIO, 1), true, this);
		}
		reconnect_ltc_output ();
	}

	/* fix up names of LTC ports because we don't want the normal
	 * IO style of NAME/TYPE-{in,out}N
	 */

	_ltc_input->nth (0)->set_name (X_("LTC-in"));
	_ltc_output->nth (0)->set_name (X_("LTC-out"));
}

void
Session::setup_click ()
{
	_clicking = false;
	_click_io.reset (new ClickIO (*this, X_("Click")));
	_click_gain.reset (new Amp (*this));
	_click_gain->activate ();
	if (state_tree) {
		setup_click_state (state_tree->root());
	} else {
		setup_click_state (0);
	}
}

void
Session::setup_click_state (const XMLNode* node)
{
	const XMLNode* child = 0;

	if (node && (child = find_named_node (*node, "Click")) != 0) {

		/* existing state for Click */
		int c = 0;

		if (Stateful::loading_state_version < 3000) {
			c = _click_io->set_state_2X (*child->children().front(), Stateful::loading_state_version, false);
		} else {
			const XMLNodeList& children (child->children());
			XMLNodeList::const_iterator i = children.begin();
			if ((c = _click_io->set_state (**i, Stateful::loading_state_version)) == 0) {
				++i;
				if (i != children.end()) {
					c = _click_gain->set_state (**i, Stateful::loading_state_version);
				}
			}
		}

		if (c == 0) {
			_clicking = Config->get_clicking ();

		} else {

			error << _("could not setup Click I/O") << endmsg;
			_clicking = false;
		}


	} else {

		/* default state for Click: dual-mono to first 2 physical outputs */

		vector<string> outs;
		_engine.get_physical_outputs (DataType::AUDIO, outs);

		for (uint32_t physport = 0; physport < 2; ++physport) {
			if (outs.size() > physport) {
				if (_click_io->add_port (outs[physport], this)) {
					// relax, even though its an error
				}
			}
		}

		if (_click_io->n_ports () > ChanCount::ZERO) {
			_clicking = Config->get_clicking ();
		}
	}
}

void
Session::setup_bundles ()
{

	{
		RCUWriter<BundleList> writer (_bundles);
		boost::shared_ptr<BundleList> b = writer.get_copy ();
		for (BundleList::iterator i = b->begin(); i != b->end();) {
			if (boost::dynamic_pointer_cast<UserBundle>(*i)) {
				++i;
				continue;
			}
			i = b->erase(i);
		}
	}

	vector<string> inputs[DataType::num_types];
	vector<string> outputs[DataType::num_types];
	for (uint32_t i = 0; i < DataType::num_types; ++i) {
		_engine.get_physical_inputs (DataType (DataType::Symbol (i)), inputs[i]);
		_engine.get_physical_outputs (DataType (DataType::Symbol (i)), outputs[i]);
	}

	/* Create a set of Bundle objects that map
	   to the physical I/O currently available.  We create both
	   mono and stereo bundles, so that the common cases of mono
	   and stereo tracks get bundles to put in their mixer strip
	   in / out menus.  There may be a nicer way of achieving that;
	   it doesn't really scale that well to higher channel counts
	*/

	/* mono output bundles */

	for (uint32_t np = 0; np < outputs[DataType::AUDIO].size(); ++np) {
		char buf[64];
		std::string pn = _engine.get_pretty_name_by_name (outputs[DataType::AUDIO][np]);
		if (!pn.empty()) {
			snprintf (buf, sizeof (buf), _("out %s"), pn.c_str());
		} else {
			snprintf (buf, sizeof (buf), _("out %" PRIu32), np+1);
		}

		boost::shared_ptr<Bundle> c (new Bundle (buf, true));
		c->add_channel (_("mono"), DataType::AUDIO);
		c->set_port (0, outputs[DataType::AUDIO][np]);

		add_bundle (c, false);
	}

	/* stereo output bundles */

	for (uint32_t np = 0; np < outputs[DataType::AUDIO].size(); np += 2) {
		if (np + 1 < outputs[DataType::AUDIO].size()) {
			char buf[32];
			snprintf (buf, sizeof(buf), _("out %" PRIu32 "+%" PRIu32), np + 1, np + 2);
                        boost::shared_ptr<Bundle> c (new Bundle (buf, true));
			c->add_channel (_("L"), DataType::AUDIO);
			c->set_port (0, outputs[DataType::AUDIO][np]);
			c->add_channel (_("R"), DataType::AUDIO);
			c->set_port (1, outputs[DataType::AUDIO][np + 1]);

			add_bundle (c, false);
		}
	}

	/* mono input bundles */

	for (uint32_t np = 0; np < inputs[DataType::AUDIO].size(); ++np) {
		char buf[64];
		std::string pn = _engine.get_pretty_name_by_name (inputs[DataType::AUDIO][np]);
		if (!pn.empty()) {
			snprintf (buf, sizeof (buf), _("in %s"), pn.c_str());
		} else {
			snprintf (buf, sizeof (buf), _("in %" PRIu32), np+1);
		}

		boost::shared_ptr<Bundle> c (new Bundle (buf, false));
		c->add_channel (_("mono"), DataType::AUDIO);
		c->set_port (0, inputs[DataType::AUDIO][np]);

		add_bundle (c, false);
	}

	/* stereo input bundles */

	for (uint32_t np = 0; np < inputs[DataType::AUDIO].size(); np += 2) {
		if (np + 1 < inputs[DataType::AUDIO].size()) {
			char buf[32];
			snprintf (buf, sizeof(buf), _("in %" PRIu32 "+%" PRIu32), np + 1, np + 2);

			boost::shared_ptr<Bundle> c (new Bundle (buf, false));
			c->add_channel (_("L"), DataType::AUDIO);
			c->set_port (0, inputs[DataType::AUDIO][np]);
			c->add_channel (_("R"), DataType::AUDIO);
			c->set_port (1, inputs[DataType::AUDIO][np + 1]);

			add_bundle (c, false);
		}
	}

	/* MIDI input bundles */

	for (uint32_t np = 0; np < inputs[DataType::MIDI].size(); ++np) {
		string n = inputs[DataType::MIDI][np];
		std::string pn = _engine.get_pretty_name_by_name (n);
		if (!pn.empty()) {
			n = pn;
		} else {
			boost::erase_first (n, X_("alsa_pcm:"));
		}
		boost::shared_ptr<Bundle> c (new Bundle (n, false));
		c->add_channel ("", DataType::MIDI);
		c->set_port (0, inputs[DataType::MIDI][np]);
		add_bundle (c, false);
	}

	/* MIDI output bundles */

	for (uint32_t np = 0; np < outputs[DataType::MIDI].size(); ++np) {
		string n = outputs[DataType::MIDI][np];
		std::string pn = _engine.get_pretty_name_by_name (n);
		if (!pn.empty()) {
			n = pn;
		} else {
			boost::erase_first (n, X_("alsa_pcm:"));
		}
		boost::shared_ptr<Bundle> c (new Bundle (n, true));
		c->add_channel ("", DataType::MIDI);
		c->set_port (0, outputs[DataType::MIDI][np]);
		add_bundle (c, false);
	}

	// we trust the backend to only calls us if there's a change
	BundleAddedOrRemoved (); /* EMIT SIGNAL */
}

void
Session::auto_connect_master_bus ()
{
	if (!_master_out || !Config->get_auto_connect_standard_busses() || _monitor_out) {
		return;
	}

	// Waves Tracks: Do not connect master bas for Tracks if AutoConnectMaster option is not set
	// In this case it means "Multi Out" output mode
	if (ARDOUR::Profile->get_trx() && !(Config->get_output_auto_connect() & AutoConnectMaster) ) {
		return;
	}

	/* if requested auto-connect the outputs to the first N physical ports.
	 */

	uint32_t limit = _master_out->n_outputs().n_total();
	vector<string> outputs[DataType::num_types];

	for (uint32_t i = 0; i < DataType::num_types; ++i) {
		_engine.get_physical_outputs (DataType (DataType::Symbol (i)), outputs[i]);
	}

	for (uint32_t n = 0; n < limit; ++n) {
		boost::shared_ptr<Port> p = _master_out->output()->nth (n);
		string connect_to;
		if (outputs[p->type()].size() > n) {
			connect_to = outputs[p->type()][n];
		}

		if (!connect_to.empty() && p->connected_to (connect_to) == false) {
			if (_master_out->output()->connect (p, connect_to, this)) {
				error << string_compose (_("cannot connect master output %1 to %2"), n, connect_to)
				      << endmsg;
				break;
			}
		}
	}
}

void
Session::remove_monitor_section ()
{
	if (!_monitor_out || Profile->get_trx()) {
		return;
	}

	/* force reversion to Solo-In-Place */
	Config->set_solo_control_is_listen_control (false);

	/* if we are auditioning, cancel it ... this is a workaround
	   to a problem (auditioning does not execute the process graph,
	   which is needed to remove routes when using >1 core for processing)
	*/
	cancel_audition ();

	{
		/* Hold process lock while doing this so that we don't hear bits and
		 * pieces of audio as we work on each route.
		 */

		Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());

		/* Connect tracks to monitor section. Note that in an
		   existing session, the internal sends will already exist, but we want the
		   routes to notice that they connect to the control out specifically.
		*/


		boost::shared_ptr<RouteList> r = routes.reader ();
		PBD::Unwinder<bool> uw (ignore_route_processor_changes, true);

		for (RouteList::iterator x = r->begin(); x != r->end(); ++x) {

			if ((*x)->is_monitor()) {
				/* relax */
			} else if ((*x)->is_master()) {
				/* relax */
			} else {
				(*x)->remove_aux_or_listen (_monitor_out);
			}
		}
	}

	remove_route (_monitor_out);
	auto_connect_master_bus ();

	if (auditioner) {
		auditioner->connect ();
	}
}

void
Session::add_monitor_section ()
{
	RouteList rl;

	if (_monitor_out || !_master_out || Profile->get_trx()) {
		return;
	}

	boost::shared_ptr<Route> r (new Route (*this, _("Monitor"), Route::MonitorOut, DataType::AUDIO));

	if (r->init ()) {
		return;
	}

#ifdef BOOST_SP_ENABLE_DEBUG_HOOKS
	// boost_debug_shared_ptr_mark_interesting (r.get(), "Route");
#endif
	try {
		Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());
		r->input()->ensure_io (_master_out->output()->n_ports(), false, this);
		r->output()->ensure_io (_master_out->output()->n_ports(), false, this);
	} catch (...) {
		error << _("Cannot create monitor section. 'Monitor' Port name is not unique.") << endmsg;
		return;
	}

	rl.push_back (r);
	add_routes (rl, false, false, false);

	assert (_monitor_out);

	/* AUDIO ONLY as of june 29th 2009, because listen semantics for anything else
	   are undefined, at best.
	*/

	uint32_t limit = _monitor_out->n_inputs().n_audio();

	if (_master_out) {

		/* connect the inputs to the master bus outputs. this
		 * represents a separate data feed from the internal sends from
		 * each route. as of jan 2011, it allows the monitor section to
		 * conditionally ignore either the internal sends or the normal
		 * input feed, but we should really find a better way to do
		 * this, i think.
		 */

		_master_out->output()->disconnect (this);

		for (uint32_t n = 0; n < limit; ++n) {
			boost::shared_ptr<AudioPort> p = _monitor_out->input()->ports().nth_audio_port (n);
			boost::shared_ptr<AudioPort> o = _master_out->output()->ports().nth_audio_port (n);

			if (o) {
				string connect_to = o->name();
				if (_monitor_out->input()->connect (p, connect_to, this)) {
					error << string_compose (_("cannot connect control input %1 to %2"), n, connect_to)
					      << endmsg;
					break;
				}
			}
		}
	}

	/* if monitor section is not connected, connect it to physical outs
	 */

	if (Config->get_auto_connect_standard_busses() && !_monitor_out->output()->connected ()) {

		if (!Config->get_monitor_bus_preferred_bundle().empty()) {

			boost::shared_ptr<Bundle> b = bundle_by_name (Config->get_monitor_bus_preferred_bundle());

			if (b) {
				_monitor_out->output()->connect_ports_to_bundle (b, true, this);
			} else {
				warning << string_compose (_("The preferred I/O for the monitor bus (%1) cannot be found"),
							   Config->get_monitor_bus_preferred_bundle())
					<< endmsg;
			}

		} else {

			/* Monitor bus is audio only */

			vector<string> outputs[DataType::num_types];

			for (uint32_t i = 0; i < DataType::num_types; ++i) {
				_engine.get_physical_outputs (DataType (DataType::Symbol (i)), outputs[i]);
			}

			uint32_t mod = outputs[DataType::AUDIO].size();
			uint32_t limit = _monitor_out->n_outputs().get (DataType::AUDIO);

			if (mod != 0) {

				for (uint32_t n = 0; n < limit; ++n) {

					boost::shared_ptr<Port> p = _monitor_out->output()->ports().port(DataType::AUDIO, n);
					string connect_to;
					if (outputs[DataType::AUDIO].size() > (n % mod)) {
						connect_to = outputs[DataType::AUDIO][n % mod];
					}

					if (!connect_to.empty()) {
						if (_monitor_out->output()->connect (p, connect_to, this)) {
							error << string_compose (
								_("cannot connect control output %1 to %2"),
								n, connect_to)
							      << endmsg;
							break;
						}
					}
				}
			}
		}
	}

	/* Hold process lock while doing this so that we don't hear bits and
	 * pieces of audio as we work on each route.
	 */

	Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());

	/* Connect tracks to monitor section. Note that in an
	   existing session, the internal sends will already exist, but we want the
	   routes to notice that they connect to the control out specifically.
	*/


	boost::shared_ptr<RouteList> rls = routes.reader ();

	PBD::Unwinder<bool> uw (ignore_route_processor_changes, true);

	for (RouteList::iterator x = rls->begin(); x != rls->end(); ++x) {

		if ((*x)->is_monitor()) {
			/* relax */
		} else if ((*x)->is_master()) {
			/* relax */
		} else {
			(*x)->enable_monitor_send ();
		}
	}

	if (auditioner) {
		auditioner->connect ();
	}
}

void
Session::reset_monitor_section ()
{
	/* Process lock should be held by the caller.*/

	if (!_monitor_out || Profile->get_trx()) {
		return;
	}

	uint32_t limit = _master_out->n_outputs().n_audio();

	/* connect the inputs to the master bus outputs. this
	 * represents a separate data feed from the internal sends from
	 * each route. as of jan 2011, it allows the monitor section to
	 * conditionally ignore either the internal sends or the normal
	 * input feed, but we should really find a better way to do
	 * this, i think.
	 */

	_master_out->output()->disconnect (this);
	_monitor_out->output()->disconnect (this);

	_monitor_out->input()->ensure_io (_master_out->output()->n_ports(), false, this);
	_monitor_out->output()->ensure_io (_master_out->output()->n_ports(), false, this);

	for (uint32_t n = 0; n < limit; ++n) {
		boost::shared_ptr<AudioPort> p = _monitor_out->input()->ports().nth_audio_port (n);
		boost::shared_ptr<AudioPort> o = _master_out->output()->ports().nth_audio_port (n);

		if (o) {
			string connect_to = o->name();
			if (_monitor_out->input()->connect (p, connect_to, this)) {
				error << string_compose (_("cannot connect control input %1 to %2"), n, connect_to)
				      << endmsg;
				break;
			}
		}
	}

	/* connect monitor section to physical outs
	 */

	if (Config->get_auto_connect_standard_busses()) {

		if (!Config->get_monitor_bus_preferred_bundle().empty()) {

			boost::shared_ptr<Bundle> b = bundle_by_name (Config->get_monitor_bus_preferred_bundle());

			if (b) {
				_monitor_out->output()->connect_ports_to_bundle (b, true, this);
			} else {
				warning << string_compose (_("The preferred I/O for the monitor bus (%1) cannot be found"),
							   Config->get_monitor_bus_preferred_bundle())
					<< endmsg;
			}

		} else {

			/* Monitor bus is audio only */

			vector<string> outputs[DataType::num_types];

			for (uint32_t i = 0; i < DataType::num_types; ++i) {
				_engine.get_physical_outputs (DataType (DataType::Symbol (i)), outputs[i]);
			}

			uint32_t mod = outputs[DataType::AUDIO].size();
			uint32_t limit = _monitor_out->n_outputs().get (DataType::AUDIO);

			if (mod != 0) {

				for (uint32_t n = 0; n < limit; ++n) {

					boost::shared_ptr<Port> p = _monitor_out->output()->ports().port(DataType::AUDIO, n);
					string connect_to;
					if (outputs[DataType::AUDIO].size() > (n % mod)) {
						connect_to = outputs[DataType::AUDIO][n % mod];
					}

					if (!connect_to.empty()) {
						if (_monitor_out->output()->connect (p, connect_to, this)) {
							error << string_compose (
								_("cannot connect control output %1 to %2"),
								n, connect_to)
							      << endmsg;
							break;
						}
					}
				}
			}
		}
	}

	/* Connect tracks to monitor section. Note that in an
	   existing session, the internal sends will already exist, but we want the
	   routes to notice that they connect to the control out specifically.
	*/


	boost::shared_ptr<RouteList> rls = routes.reader ();

	PBD::Unwinder<bool> uw (ignore_route_processor_changes, true);

	for (RouteList::iterator x = rls->begin(); x != rls->end(); ++x) {

		if ((*x)->is_monitor()) {
			/* relax */
		} else if ((*x)->is_master()) {
			/* relax */
		} else {
			(*x)->enable_monitor_send ();
		}
	}
}

void
Session::hookup_io ()
{
	/* stop graph reordering notifications from
	   causing resorts, etc.
	*/

	_state_of_the_state = StateOfTheState (_state_of_the_state | InitialConnecting);

	if (!auditioner) {

		/* we delay creating the auditioner till now because
		   it makes its own connections to ports.
		*/

		try {
			boost::shared_ptr<Auditioner> a (new Auditioner (*this));
			if (a->init()) {
				throw failed_constructor ();
			}
			a->use_new_diskstream ();
			auditioner = a;
		}

		catch (failed_constructor& err) {
			warning << _("cannot create Auditioner: no auditioning of regions possible") << endmsg;
		}
	}

	/* load bundles, which we may have postponed earlier on */
	if (_bundle_xml_node) {
		load_bundles (*_bundle_xml_node);
		delete _bundle_xml_node;
	}

	/* Tell all IO objects to connect themselves together */

	IO::enable_connecting ();

	/* Now tell all "floating" ports to connect to whatever
	   they should be connected to.
	*/

	AudioEngine::instance()->reconnect_ports ();

	/* Anyone who cares about input state, wake up and do something */

	IOConnectionsComplete (); /* EMIT SIGNAL */

	_state_of_the_state = StateOfTheState (_state_of_the_state & ~InitialConnecting);

	/* now handle the whole enchilada as if it was one
	   graph reorder event.
	*/

	graph_reordered ();

	/* update the full solo state, which can't be
	   correctly determined on a per-route basis, but
	   needs the global overview that only the session
	   has.
	*/

	update_route_solo_state ();
}

void
Session::track_playlist_changed (boost::weak_ptr<Track> wp)
{
	boost::shared_ptr<Track> track = wp.lock ();
	if (!track) {
		return;
	}

	boost::shared_ptr<Playlist> playlist;

	if ((playlist = track->playlist()) != 0) {
		playlist->RegionAdded.connect_same_thread (*this, boost::bind (&Session::playlist_region_added, this, _1));
		playlist->RangesMoved.connect_same_thread (*this, boost::bind (&Session::playlist_ranges_moved, this, _1));
		playlist->RegionsExtended.connect_same_thread (*this, boost::bind (&Session::playlist_regions_extended, this, _1));
	}
}

bool
Session::record_enabling_legal () const
{
	/* this used to be in here, but survey says.... we don't need to restrict it */
	// if (record_status() == Recording) {
	//	return false;
	// }

	if (Config->get_all_safe()) {
		return false;
	}
	return true;
}

void
Session::set_track_monitor_input_status (bool yn)
{
	boost::shared_ptr<RouteList> rl = routes.reader ();
	for (RouteList::iterator i = rl->begin(); i != rl->end(); ++i) {
		boost::shared_ptr<AudioTrack> tr = boost::dynamic_pointer_cast<AudioTrack> (*i);
		if (tr && tr->record_enabled ()) {
			//cerr << "switching to input = " << !auto_input << __FILE__ << __LINE__ << endl << endl;
			tr->request_input_monitoring (yn);
		}
	}
}

void
Session::auto_punch_start_changed (Location* location)
{
	replace_event (SessionEvent::PunchIn, location->start());

	if (get_record_enabled() && config.get_punch_in()) {
		/* capture start has been changed, so save new pending state */
		save_state ("", true);
	}
}

void
Session::auto_punch_end_changed (Location* location)
{
	framepos_t when_to_stop = location->end();
	// when_to_stop += _worst_output_latency + _worst_input_latency;
	replace_event (SessionEvent::PunchOut, when_to_stop);
}

void
Session::auto_punch_changed (Location* location)
{
	framepos_t when_to_stop = location->end();

	replace_event (SessionEvent::PunchIn, location->start());
	//when_to_stop += _worst_output_latency + _worst_input_latency;
	replace_event (SessionEvent::PunchOut, when_to_stop);
}

/** @param loc A loop location.
 *  @param pos Filled in with the start time of the required fade-out (in session frames).
 *  @param length Filled in with the length of the required fade-out.
 */
void
Session::auto_loop_declick_range (Location* loc, framepos_t & pos, framepos_t & length)
{
	pos = max (loc->start(), loc->end() - 64);
	length = loc->end() - pos;
}

void
Session::auto_loop_changed (Location* location)
{
	replace_event (SessionEvent::AutoLoop, location->end(), location->start());
	framepos_t dcp;
	framecnt_t dcl;
	auto_loop_declick_range (location, dcp, dcl);

	if (transport_rolling() && play_loop) {

		replace_event (SessionEvent::AutoLoopDeclick, dcp, dcl);

		// if (_transport_frame > location->end()) {

		if (_transport_frame < location->start() || _transport_frame > location->end()) {
			// relocate to beginning of loop
			clear_events (SessionEvent::LocateRoll);

			request_locate (location->start(), true);

		}
		else if (Config->get_seamless_loop() && !loop_changing) {

			// schedule a locate-roll to refill the diskstreams at the
			// previous loop end
			loop_changing = true;

			if (location->end() > last_loopend) {
				clear_events (SessionEvent::LocateRoll);
				SessionEvent *ev = new SessionEvent (SessionEvent::LocateRoll, SessionEvent::Add, last_loopend, last_loopend, 0, true);
				queue_event (ev);
			}

		}
	} else {
		clear_events (SessionEvent::AutoLoopDeclick);
		clear_events (SessionEvent::AutoLoop);
	}

	/* possibly move playhead if not rolling; if we are rolling we'll move
	   to the loop start on stop if that is appropriate.
	 */

	framepos_t pos;

	if (!transport_rolling() && select_playhead_priority_target (pos)) {
		if (pos == location->start()) {
			request_locate (pos);
		}
	}


	last_loopend = location->end();
	set_dirty ();
}

void
Session::set_auto_punch_location (Location* location)
{
	Location* existing;

	if ((existing = _locations->auto_punch_location()) != 0 && existing != location) {
		punch_connections.drop_connections();
		existing->set_auto_punch (false, this);
		remove_event (existing->start(), SessionEvent::PunchIn);
		clear_events (SessionEvent::PunchOut);
		auto_punch_location_changed (0);
	}

	set_dirty();

	if (location == 0) {
		return;
	}

	if (location->end() <= location->start()) {
		error << _("Session: you can't use that location for auto punch (start <= end)") << endmsg;
		return;
	}

	punch_connections.drop_connections ();

	location->StartChanged.connect_same_thread (punch_connections, boost::bind (&Session::auto_punch_start_changed, this, location));
	location->EndChanged.connect_same_thread (punch_connections, boost::bind (&Session::auto_punch_end_changed, this, location));
	location->Changed.connect_same_thread (punch_connections, boost::bind (&Session::auto_punch_changed, this, location));

	location->set_auto_punch (true, this);

	auto_punch_changed (location);

	auto_punch_location_changed (location);
}

void
Session::set_session_extents (framepos_t start, framepos_t end)
{
	Location* existing;
	if ((existing = _locations->session_range_location()) == 0) {
		//if there is no existing session, we need to make a new session location  (should never happen)
		existing = new Location (*this, 0, 0, _("session"), Location::IsSessionRange);
	}

	if (end <= start) {
		error << _("Session: you can't use that location for session start/end)") << endmsg;
		return;
	}

	existing->set( start, end );

	set_dirty();
}

void
Session::set_auto_loop_location (Location* location)
{
	Location* existing;

	if ((existing = _locations->auto_loop_location()) != 0 && existing != location) {
		loop_connections.drop_connections ();
		existing->set_auto_loop (false, this);
		remove_event (existing->end(), SessionEvent::AutoLoop);
		framepos_t dcp;
		framecnt_t dcl;
		auto_loop_declick_range (existing, dcp, dcl);
		remove_event (dcp, SessionEvent::AutoLoopDeclick);
		auto_loop_location_changed (0);
	}

	set_dirty();

	if (location == 0) {
		return;
	}

	if (location->end() <= location->start()) {
		error << _("You cannot use this location for auto-loop because it has zero or negative length") << endmsg;
		return;
	}

	last_loopend = location->end();

	loop_connections.drop_connections ();

	location->StartChanged.connect_same_thread (loop_connections, boost::bind (&Session::auto_loop_changed, this, location));
	location->EndChanged.connect_same_thread (loop_connections, boost::bind (&Session::auto_loop_changed, this, location));
	location->Changed.connect_same_thread (loop_connections, boost::bind (&Session::auto_loop_changed, this, location));
	location->FlagsChanged.connect_same_thread (loop_connections, boost::bind (&Session::auto_loop_changed, this, location));

	location->set_auto_loop (true, this);

	if (Config->get_loop_is_mode() && play_loop && Config->get_seamless_loop()) {
		// set all tracks to use internal looping
		boost::shared_ptr<RouteList> rl = routes.reader ();
		for (RouteList::iterator i = rl->begin(); i != rl->end(); ++i) {
			boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
			if (tr && !tr->hidden()) {
				tr->set_loop (location);
			}
		}
	}

	/* take care of our stuff first */

	auto_loop_changed (location);

	/* now tell everyone else */

	auto_loop_location_changed (location);
}

void
Session::update_marks (Location*)
{
	set_dirty ();
}

void
Session::update_skips (Location* loc, bool consolidate)
{
	if (_ignore_skips_updates) {
		return;
	}

	Locations::LocationList skips;

        if (consolidate) {
	        PBD::Unwinder<bool> uw (_ignore_skips_updates, true);
	        consolidate_skips (loc);
        }

	sync_locations_to_skips ();

	set_dirty ();
}

void
Session::consolidate_skips (Location* loc)
{
        Locations::LocationList all_locations = _locations->list ();

        for (Locations::LocationList::iterator l = all_locations.begin(); l != all_locations.end(); ) {

                if (!(*l)->is_skip ()) {
                        ++l;
                        continue;
                }

                /* don't test against self */

                if (*l == loc) {
                        ++l;
                        continue;
                }

                switch (Evoral::coverage ((*l)->start(), (*l)->end(), loc->start(), loc->end())) {
                case Evoral::OverlapInternal:
                case Evoral::OverlapExternal:
                case Evoral::OverlapStart:
                case Evoral::OverlapEnd:
                        /* adjust new location to cover existing one */
                        loc->set_start (min (loc->start(), (*l)->start()));
                        loc->set_end (max (loc->end(), (*l)->end()));
                        /* we don't need this one any more */
                        _locations->remove (*l);
                        /* the location has been deleted, so remove reference to it in our local list */
                        l = all_locations.erase (l);
                        break;

                case Evoral::OverlapNone:
                        ++l;
                        break;
                }
        }
}

void
Session::sync_locations_to_skips ()
{
	/* This happens asynchronously (in the audioengine thread). After the clear is done, we will call
	 * Session::_sync_locations_to_skips() from the audioengine thread.
	 */
	clear_events (SessionEvent::Skip, boost::bind (&Session::_sync_locations_to_skips, this));
}

void
Session::_sync_locations_to_skips ()
{
	/* called as a callback after existing Skip events have been cleared from a realtime audioengine thread */

	Locations::LocationList const & locs (_locations->list());

	for (Locations::LocationList::const_iterator i = locs.begin(); i != locs.end(); ++i) {

		Location* location = *i;

		if (location->is_skip() && location->is_skipping()) {
			SessionEvent* ev = new SessionEvent (SessionEvent::Skip, SessionEvent::Add, location->start(), location->end(), 1.0);
			queue_event (ev);
		}
	}
}


void
Session::location_added (Location *location)
{
        if (location->is_auto_punch()) {
                set_auto_punch_location (location);
        }

        if (location->is_auto_loop()) {
                set_auto_loop_location (location);
        }

        if (location->is_session_range()) {
                /* no need for any signal handling or event setting with the session range,
                   because we keep a direct reference to it and use its start/end directly.
                */
                _session_range_location = location;
        }

        if (location->is_mark()) {
                /* listen for per-location signals that require us to do any * global updates for marks */

                location->StartChanged.connect_same_thread (skip_update_connections, boost::bind (&Session::update_marks, this, location));
                location->EndChanged.connect_same_thread (skip_update_connections, boost::bind (&Session::update_marks, this, location));
                location->Changed.connect_same_thread (skip_update_connections, boost::bind (&Session::update_marks, this, location));
                location->FlagsChanged.connect_same_thread (skip_update_connections, boost::bind (&Session::update_marks, this, location));
        }

        if (location->is_skip()) {
                /* listen for per-location signals that require us to update skip-locate events */

                location->StartChanged.connect_same_thread (skip_update_connections, boost::bind (&Session::update_skips, this, location, true));
                location->EndChanged.connect_same_thread (skip_update_connections, boost::bind (&Session::update_skips, this, location, true));
                location->Changed.connect_same_thread (skip_update_connections, boost::bind (&Session::update_skips, this, location, true));
                location->FlagsChanged.connect_same_thread (skip_update_connections, boost::bind (&Session::update_skips, this, location, false));

                update_skips (location, true);
        }

	set_dirty ();
}

void
Session::location_removed (Location *location)
{
        if (location->is_auto_loop()) {
	        set_auto_loop_location (0);
	        set_track_loop (false);
        }

        if (location->is_auto_punch()) {
                set_auto_punch_location (0);
        }

        if (location->is_session_range()) {
                /* this is never supposed to happen */
                error << _("programming error: session range removed!") << endl;
        }

        if (location->is_skip()) {

                update_skips (location, false);
        }

	set_dirty ();
}

void
Session::locations_changed ()
{
        _locations->apply (*this, &Session::_locations_changed);
}

void
Session::_locations_changed (const Locations::LocationList& locations)
{
        /* There was some mass-change in the Locations object.

           We might be re-adding a location here but it doesn't actually matter
           for all the locations that the Session takes an interest in.
        */

	{
		PBD::Unwinder<bool> protect_ignore_skip_updates (_ignore_skips_updates, true);
		for (Locations::LocationList::const_iterator i = locations.begin(); i != locations.end(); ++i) {
			location_added (*i);
		}
	}

	update_skips (NULL, false);
}

void
Session::enable_record ()
{
	if (_transport_speed != 0.0 && _transport_speed != 1.0) {
		/* no recording at anything except normal speed */
		return;
	}

	while (1) {
		RecordState rs = (RecordState) g_atomic_int_get (&_record_status);

		if (rs == Recording) {
			break;
		}

		if (g_atomic_int_compare_and_exchange (&_record_status, rs, Recording)) {

			_last_record_location = _transport_frame;
			send_immediate_mmc (MIDI::MachineControlCommand (MIDI::MachineControl::cmdRecordStrobe));

			if (Config->get_monitoring_model() == HardwareMonitoring && config.get_auto_input()) {
				set_track_monitor_input_status (true);
			}

			RecordStateChanged ();
			break;
		}
	}
}

void
Session::disable_record (bool rt_context, bool force)
{
	RecordState rs;

	if ((rs = (RecordState) g_atomic_int_get (&_record_status)) != Disabled) {

		if (!Config->get_latched_record_enable () || force) {
			g_atomic_int_set (&_record_status, Disabled);
			send_immediate_mmc (MIDI::MachineControlCommand (MIDI::MachineControl::cmdRecordExit));
		} else {
			if (rs == Recording) {
				g_atomic_int_set (&_record_status, Enabled);
			}
		}

		if (Config->get_monitoring_model() == HardwareMonitoring && config.get_auto_input()) {
			set_track_monitor_input_status (false);
		}

		RecordStateChanged (); /* emit signal */

		if (!rt_context) {
			remove_pending_capture_state ();
		}
	}
}

void
Session::step_back_from_record ()
{
	if (g_atomic_int_compare_and_exchange (&_record_status, Recording, Enabled)) {

		if (Config->get_monitoring_model() == HardwareMonitoring && config.get_auto_input()) {
			set_track_monitor_input_status (false);
		}

		RecordStateChanged (); /* emit signal */
	}
}

void
Session::maybe_enable_record ()
{
	if (_step_editors > 0) {
		return;
	}

	g_atomic_int_set (&_record_status, Enabled);

	/* This function is currently called from somewhere other than an RT thread.
	   This save_state() call therefore doesn't impact anything.  Doing it here
	   means that we save pending state of which sources the next record will use,
	   which gives us some chance of recovering from a crash during the record.
	*/

	save_state ("", true);

	if (_transport_speed) {
		if (!config.get_punch_in()) {
			enable_record ();
		}
	} else {
		send_immediate_mmc (MIDI::MachineControlCommand (MIDI::MachineControl::cmdRecordPause));
		RecordStateChanged (); /* EMIT SIGNAL */
	}

	set_dirty();
}

framepos_t
Session::audible_frame () const
{
	framepos_t ret;
	framepos_t tf;
	framecnt_t offset;

	offset = worst_playback_latency ();

	if (synced_to_engine()) {
		/* Note: this is basically just sync-to-JACK */
		tf = _engine.transport_frame();
	} else {
		tf = _transport_frame;
	}

	ret = tf;

	if (!non_realtime_work_pending()) {

		/* MOVING */

		/* Check to see if we have passed the first guaranteed
		   audible frame past our last start position. if not,
		   return that last start point because in terms
		   of audible frames, we have not moved yet.

		   `Start position' in this context means the time we last
		   either started, located, or changed transport direction.
		*/

		if (_transport_speed > 0.0f) {

			if (!play_loop || !have_looped) {
				if (tf < _last_roll_or_reversal_location + offset) {
					return _last_roll_or_reversal_location;
				}
			}


			/* forwards */
			ret -= offset;

		} else if (_transport_speed < 0.0f) {

			/* XXX wot? no backward looping? */

			if (tf > _last_roll_or_reversal_location - offset) {
				return _last_roll_or_reversal_location;
			} else {
				/* backwards */
				ret += offset;
			}
		}
	}

	return ret;
}

void
Session::set_frame_rate (framecnt_t frames_per_second)
{
	/** \fn void Session::set_frame_size(framecnt_t)
		the AudioEngine object that calls this guarantees
		that it will not be called while we are also in
		::process(). Its fine to do things that block
		here.
	*/

	_base_frame_rate = frames_per_second;
	_nominal_frame_rate = frames_per_second;

	sync_time_vars();

	clear_clicks ();
	reset_write_sources (false);

	// XXX we need some equivalent to this, somehow
	// SndFileSource::setup_standard_crossfades (frames_per_second);

	set_dirty();

	/* XXX need to reset/reinstantiate all LADSPA plugins */
}

void
Session::set_block_size (pframes_t nframes)
{
	/* the AudioEngine guarantees
	   that it will not be called while we are also in
	   ::process(). It is therefore fine to do things that block
	   here.
	*/

	{
		current_block_size = nframes;

		ensure_buffers ();

		boost::shared_ptr<RouteList> r = routes.reader ();

		for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
			(*i)->set_block_size (nframes);
		}

		boost::shared_ptr<RouteList> rl = routes.reader ();
		for (RouteList::iterator i = rl->begin(); i != rl->end(); ++i) {
			boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
			if (tr) {
				tr->set_block_size (nframes);
			}
		}

		set_worst_io_latencies ();
	}
}


static void
trace_terminal (boost::shared_ptr<Route> r1, boost::shared_ptr<Route> rbase)
{
	boost::shared_ptr<Route> r2;

	if (r1->feeds (rbase) && rbase->feeds (r1)) {
		info << string_compose(_("feedback loop setup between %1 and %2"), r1->name(), rbase->name()) << endmsg;
		return;
	}

	/* make a copy of the existing list of routes that feed r1 */

	Route::FedBy existing (r1->fed_by());

	/* for each route that feeds r1, recurse, marking it as feeding
	   rbase as well.
	*/

	for (Route::FedBy::iterator i = existing.begin(); i != existing.end(); ++i) {
		if (!(r2 = i->r.lock ())) {
			/* (*i) went away, ignore it */
			continue;
		}

		/* r2 is a route that feeds r1 which somehow feeds base. mark
		   base as being fed by r2
		*/

		rbase->add_fed_by (r2, i->sends_only);

		if (r2 != rbase) {

			/* 2nd level feedback loop detection. if r1 feeds or is fed by r2,
			   stop here.
			*/

			if (r1->feeds (r2) && r2->feeds (r1)) {
				continue;
			}

			/* now recurse, so that we can mark base as being fed by
			   all routes that feed r2
			*/

			trace_terminal (r2, rbase);
		}

	}
}

void
Session::resort_routes ()
{
	/* don't do anything here with signals emitted
	   by Routes during initial setup or while we
	   are being destroyed.
	*/

	if (_state_of_the_state & (InitialConnecting | Deletion)) {
		return;
	}

	{
		RCUWriter<RouteList> writer (routes);
		boost::shared_ptr<RouteList> r = writer.get_copy ();
		resort_routes_using (r);
		/* writer goes out of scope and forces update */
	}

#ifndef NDEBUG
	boost::shared_ptr<RouteList> rl = routes.reader ();
	for (RouteList::iterator i = rl->begin(); i != rl->end(); ++i) {
		DEBUG_TRACE (DEBUG::Graph, string_compose ("%1 fed by ...\n", (*i)->name()));

		const Route::FedBy& fb ((*i)->fed_by());

		for (Route::FedBy::const_iterator f = fb.begin(); f != fb.end(); ++f) {
			boost::shared_ptr<Route> sf = f->r.lock();
			if (sf) {
				DEBUG_TRACE (DEBUG::Graph, string_compose ("\t%1 (sends only ? %2)\n", sf->name(), f->sends_only));
			}
		}
	}
#endif

}

/** This is called whenever we need to rebuild the graph of how we will process
 *  routes.
 *  @param r List of routes, in any order.
 */

void
Session::resort_routes_using (boost::shared_ptr<RouteList> r)
{
	/* We are going to build a directed graph of our routes;
	   this is where the edges of that graph are put.
	*/

	GraphEdges edges;

	/* Go through all routes doing two things:
	 *
	 * 1. Collect the edges of the route graph.  Each of these edges
	 *    is a pair of routes, one of which directly feeds the other
	 *    either by a JACK connection or by an internal send.
	 *
	 * 2. Begin the process of making routes aware of which other
	 *    routes directly or indirectly feed them.  This information
	 *    is used by the solo code.
	 */

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {

		/* Clear out the route's list of direct or indirect feeds */
		(*i)->clear_fed_by ();

		for (RouteList::iterator j = r->begin(); j != r->end(); ++j) {

			bool via_sends_only;

			/* See if this *j feeds *i according to the current state of the JACK
			   connections and internal sends.
			*/
			if ((*j)->direct_feeds_according_to_reality (*i, &via_sends_only)) {
				/* add the edge to the graph (part #1) */
				edges.add (*j, *i, via_sends_only);
				/* tell the route (for part #2) */
				(*i)->add_fed_by (*j, via_sends_only);
			}
		}
	}

	/* Attempt a topological sort of the route graph */
	boost::shared_ptr<RouteList> sorted_routes = topological_sort (r, edges);

	if (sorted_routes) {
		/* We got a satisfactory topological sort, so there is no feedback;
		   use this new graph.

		   Note: the process graph rechain does not require a
		   topologically-sorted list, but hey ho.
		*/
		if (_process_graph) {
			_process_graph->rechain (sorted_routes, edges);
		}

		_current_route_graph = edges;

		/* Complete the building of the routes' lists of what directly
		   or indirectly feeds them.
		*/
		for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
			trace_terminal (*i, *i);
		}

		*r = *sorted_routes;

#ifndef NDEBUG
		DEBUG_TRACE (DEBUG::Graph, "Routes resorted, order follows:\n");
		for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
			DEBUG_TRACE (DEBUG::Graph, string_compose ("\t%1 signal order %2\n",
								   (*i)->name(), (*i)->order_key ()));
		}
#endif

		SuccessfulGraphSort (); /* EMIT SIGNAL */

	} else {
		/* The topological sort failed, so we have a problem.  Tell everyone
		   and stick to the old graph; this will continue to be processed, so
		   until the feedback is fixed, what is played back will not quite
		   reflect what is actually connected.  Note also that we do not
		   do trace_terminal here, as it would fail due to an endless recursion,
		   so the solo code will think that everything is still connected
		   as it was before.
		*/

		FeedbackDetected (); /* EMIT SIGNAL */
	}

}

/** Find a route name starting with \a base, maybe followed by the
 *  lowest \a id.  \a id will always be added if \a definitely_add_number
 *  is true on entry; otherwise it will only be added if required
 *  to make the name unique.
 *
 *  Names are constructed like e.g. "Audio 3" for base="Audio" and id=3.
 *  The available route name with the lowest ID will be used, and \a id
 *  will be set to the ID.
 *
 *  \return false if a route name could not be found, and \a track_name
 *  and \a id do not reflect a free route name.
 */
bool
Session::find_route_name (string const & base, uint32_t& id, string& name, bool definitely_add_number)
{
	/* the base may conflict with ports that do not belong to existing
	   routes, but hidden objects like the click track. So check port names
	   before anything else.
	*/

	for (vector<string>::const_iterator reserved = reserved_io_names.begin(); reserved != reserved_io_names.end(); ++reserved) {
		if (base == *reserved) {
			definitely_add_number = true;
			if (id < 1) {
				id = 1;
			}
			break;
		}
	}

	if (!definitely_add_number && route_by_name (base) == 0) {
		/* juse use the base */
		name = base;
		return true;
	}

	do {
		name = string_compose ("%1 %2", base, id);

		if (route_by_name (name) == 0) {
			return true;
		}

		++id;

	} while (id < (UINT_MAX-1));

	return false;
}

/** Count the total ins and outs of all non-hidden tracks in the session and return them in in and out */
void
Session::count_existing_track_channels (ChanCount& in, ChanCount& out)
{
	in  = ChanCount::ZERO;
	out = ChanCount::ZERO;

	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
                boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
		if (tr && !tr->is_auditioner()) {
			in  += tr->n_inputs();
			out += tr->n_outputs();
		}
	}
}

string
Session::default_track_name_pattern (DataType t)
{
	switch (t) {
	case DataType::AUDIO:
		if (Profile->get_trx()) {
			return _("Track ");
		} else {
			return _("Audio ");
		}
		break;

	case DataType::MIDI:
		return _("MIDI ");
	}

	return "";
}

/** Caller must not hold process lock
 *  @param name_template string to use for the start of the name, or "" to use "MIDI".
 *  @param instrument plugin info for the instrument to insert pre-fader, if any
 */
list<boost::shared_ptr<MidiTrack> >
Session::new_midi_track (const ChanCount& input, const ChanCount& output, boost::shared_ptr<PluginInfo> instrument,
			 TrackMode mode, RouteGroup* route_group, uint32_t how_many, string name_template)
{
	string track_name;
	uint32_t track_id = 0;
	string port;
	RouteList new_routes;
	list<boost::shared_ptr<MidiTrack> > ret;

	const string name_pattern = default_track_name_pattern (DataType::MIDI);
	bool const use_number = (how_many != 1) || name_template.empty () || (name_template == name_pattern);

	while (how_many) {
		if (!find_route_name (name_template.empty() ? _("MIDI") : name_template, ++track_id, track_name, use_number)) {
			error << "cannot find name for new midi track" << endmsg;
			goto failed;
		}

		boost::shared_ptr<MidiTrack> track;

		try {
			track.reset (new MidiTrack (*this, track_name, Route::Flag (0), mode));

			if (track->init ()) {
				goto failed;
			}

			track->use_new_diskstream();

#ifdef BOOST_SP_ENABLE_DEBUG_HOOKS
			// boost_debug_shared_ptr_mark_interesting (track.get(), "Track");
#endif
			{
				Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());
				if (track->input()->ensure_io (input, false, this)) {
					error << "cannot configure " << input << " out configuration for new midi track" << endmsg;
					goto failed;
				}

				if (track->output()->ensure_io (output, false, this)) {
					error << "cannot configure " << output << " out configuration for new midi track" << endmsg;
					goto failed;
				}
			}

			track->non_realtime_input_change();

			if (route_group) {
				route_group->add (track);
			}

			track->DiskstreamChanged.connect_same_thread (*this, boost::bind (&Session::resort_routes, this));

			if (Config->get_remote_model() == UserOrdered) {
				track->set_remote_control_id (next_control_id());
			}

			new_routes.push_back (track);
			ret.push_back (track);

			RouteAddedOrRemoved (true); /* EMIT SIGNAL */
		}

		catch (failed_constructor &err) {
			error << _("Session: could not create new midi track.") << endmsg;
			goto failed;
		}

		catch (AudioEngine::PortRegistrationFailure& pfe) {

			error << string_compose (_("No more JACK ports are available. You will need to stop %1 and restart JACK with more ports if you need this many tracks."), PROGRAM_NAME) << endmsg;
			goto failed;
		}

		--how_many;
	}

  failed:
	if (!new_routes.empty()) {
		StateProtector sp (this);
		if (Profile->get_trx()) {
			add_routes (new_routes, false, false, false);
		} else {
			add_routes (new_routes, true, true, false);
		}

		if (instrument) {
			for (RouteList::iterator r = new_routes.begin(); r != new_routes.end(); ++r) {
				PluginPtr plugin = instrument->load (*this);
				boost::shared_ptr<Processor> p (new PluginInsert (*this, plugin));
				(*r)->add_processor (p, PreFader);

			}
		}
	}

	return ret;
}

void
Session::midi_output_change_handler (IOChange change, void * /*src*/, boost::weak_ptr<Route> wmt)
{
        boost::shared_ptr<Route> midi_track (wmt.lock());

        if (!midi_track) {
                return;
        }

	if ((change.type & IOChange::ConfigurationChanged) && Config->get_output_auto_connect() != ManualConnect) {

                if (change.after.n_audio() <= change.before.n_audio()) {
                        return;
                }

                /* new audio ports: make sure the audio goes somewhere useful,
                   unless the user has no-auto-connect selected.

                   The existing ChanCounts don't matter for this call as they are only
                   to do with matching input and output indices, and we are only changing
                   outputs here.
                */

                ChanCount dummy;

                auto_connect_route (midi_track, dummy, dummy, false, false, ChanCount(), change.before);
        }
}

/** @param connect_inputs true to connect inputs as well as outputs, false to connect just outputs.
 *  @param input_start Where to start from when auto-connecting inputs; e.g. if this is 0, auto-connect starting from input 0.
 *  @param output_start As \a input_start, but for outputs.
 */
void
Session::auto_connect_route (boost::shared_ptr<Route> route, ChanCount& existing_inputs, ChanCount& existing_outputs,
                             bool with_lock, bool connect_inputs, ChanCount input_start, ChanCount output_start)
{
	if (!IO::connecting_legal) {
		return;
	}

	Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock (), Glib::Threads::NOT_LOCK);

	if (with_lock) {
		lm.acquire ();
	}

	/* If both inputs and outputs are auto-connected to physical ports,
	   use the max of input and output offsets to ensure auto-connected
	   port numbers always match up (e.g. the first audio input and the
	   first audio output of the route will have the same physical
	   port number).  Otherwise just use the lowest input or output
	   offset possible.
	*/

	DEBUG_TRACE (DEBUG::Graph,
	             string_compose("Auto-connect: existing in = %1 out = %2\n",
	                            existing_inputs, existing_outputs));

	const bool in_out_physical =
		(Config->get_input_auto_connect() & AutoConnectPhysical)
		&& (Config->get_output_auto_connect() & AutoConnectPhysical)
		&& connect_inputs;

	const ChanCount in_offset = in_out_physical
		? ChanCount::max(existing_inputs, existing_outputs)
                : existing_inputs;

	const ChanCount out_offset = in_out_physical
		? ChanCount::max(existing_inputs, existing_outputs)
		: existing_outputs;

	for (DataType::iterator t = DataType::begin(); t != DataType::end(); ++t) {
		vector<string> physinputs;
		vector<string> physoutputs;

		_engine.get_physical_outputs (*t, physoutputs);
		_engine.get_physical_inputs (*t, physinputs);

		if (!physinputs.empty() && connect_inputs) {
			uint32_t nphysical_in = physinputs.size();

			DEBUG_TRACE (DEBUG::Graph,
			             string_compose("There are %1 physical inputs of type %2\n",
			                            nphysical_in, *t));

			for (uint32_t i = input_start.get(*t); i < route->n_inputs().get(*t) && i < nphysical_in; ++i) {
				string port;

				if (Config->get_input_auto_connect() & AutoConnectPhysical) {
					DEBUG_TRACE (DEBUG::Graph,
					             string_compose("Get index %1 + %2 % %3 = %4\n",
					                            in_offset.get(*t), i, nphysical_in,
					                            (in_offset.get(*t) + i) % nphysical_in));
					port = physinputs[(in_offset.get(*t) + i) % nphysical_in];
				}

				DEBUG_TRACE (DEBUG::Graph,
				             string_compose("Connect route %1 IN to %2\n",
				                            route->name(), port));

				if (!port.empty() && route->input()->connect (route->input()->ports().port(*t, i), port, this)) {
					break;
				}

                                ChanCount one_added (*t, 1);
                                existing_inputs += one_added;
			}
		}

		if (!physoutputs.empty()) {
			uint32_t nphysical_out = physoutputs.size();
			for (uint32_t i = output_start.get(*t); i < route->n_outputs().get(*t); ++i) {
				string port;

				/* Waves Tracks:
				 * do not create new connections if we reached the limit of physical outputs
				 * in Multi Out mode
				 */

				if (!(Config->get_output_auto_connect() & AutoConnectMaster) &&
				    ARDOUR::Profile->get_trx () &&
				    existing_outputs.get(*t) == nphysical_out ) {
					break;
				}

				if ((*t) == DataType::MIDI && (Config->get_output_auto_connect() & AutoConnectPhysical)) {
					port = physoutputs[(out_offset.get(*t) + i) % nphysical_out];
				} else if ((*t) == DataType::AUDIO && (Config->get_output_auto_connect() & AutoConnectMaster)) {
                                        /* master bus is audio only */
					if (_master_out && _master_out->n_inputs().get(*t) > 0) {
						port = _master_out->input()->ports().port(*t,
								i % _master_out->input()->n_ports().get(*t))->name();
					}
				}

				DEBUG_TRACE (DEBUG::Graph,
				             string_compose("Connect route %1 OUT to %2\n",
				                            route->name(), port));

				if (!port.empty() && route->output()->connect (route->output()->ports().port(*t, i), port, this)) {
					break;
				}

                                ChanCount one_added (*t, 1);
                                existing_outputs += one_added;
			}
		}
	}
}

#ifdef USE_TRACKS_CODE_FEATURES

static bool
compare_routes_by_remote_id (const boost::shared_ptr<Route>& route1, const boost::shared_ptr<Route>& route2)
{
	return route1->remote_control_id() < route2->remote_control_id();
}

void
Session::reconnect_existing_routes (bool withLock, bool reconnect_master, bool reconnect_inputs, bool reconnect_outputs)
{
	// it is not allowed to perform connection
	if (!IO::connecting_legal) {
		return;
	}

	// if we are deleting routes we will call this once at the end
	if (_route_deletion_in_progress) {
		return;
	}

	Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock (), Glib::Threads::NOT_LOCK);

	if (withLock) {
		lm.acquire ();
	}

	// We need to disconnect the route's inputs and outputs first
	// basing on autoconnect configuration
	bool reconnectIputs = !(Config->get_input_auto_connect() & ManualConnect) && reconnect_inputs;
	bool reconnectOutputs = !(Config->get_output_auto_connect() & ManualConnect) && reconnect_outputs;

	ChanCount existing_inputs;
	ChanCount existing_outputs;
	count_existing_track_channels (existing_inputs, existing_outputs);

	//ChanCount inputs = ChanCount::ZERO;
	//ChanCount outputs = ChanCount::ZERO;

	RouteList existing_routes = *routes.reader ();
	existing_routes.sort (compare_routes_by_remote_id);

	{
		PBD::Unwinder<bool> protect_ignore_changes (_reconnecting_routes_in_progress, true);

		vector<string> physinputs;
		vector<string> physoutputs;

		EngineStateController::instance()->get_physical_audio_outputs(physoutputs);
		EngineStateController::instance()->get_physical_audio_inputs(physinputs);

		uint32_t input_n = 0;
		uint32_t output_n = 0;
		RouteList::iterator rIter = existing_routes.begin();
		const AutoConnectOption current_input_auto_connection (Config->get_input_auto_connect());
		const AutoConnectOption current_output_auto_connection (Config->get_output_auto_connect());
		for (; rIter != existing_routes.end(); ++rIter) {
			if (*rIter == _master_out || *rIter == _monitor_out ) {
				continue;
			}

			if (current_output_auto_connection == AutoConnectPhysical) {
				(*rIter)->amp()->deactivate();
			} else if (current_output_auto_connection == AutoConnectMaster) {
				(*rIter)->amp()->activate();
			}

			if (reconnectIputs) {
				(*rIter)->input()->disconnect (this); //GZ: check this; could be heavy

				for (uint32_t route_input_n = 0; route_input_n < (*rIter)->n_inputs().get(DataType::AUDIO); ++route_input_n) {

					if (current_input_auto_connection & AutoConnectPhysical) {

						if ( input_n == physinputs.size() ) {
							break;
						}

						string port = physinputs[input_n];

						if (port.empty() ) {
							error << "Physical Input number "<< input_n << " is unavailable and cannot be connected" << endmsg;
						}

						//GZ: check this; could be heavy
						(*rIter)->input()->connect ((*rIter)->input()->ports().port(DataType::AUDIO, route_input_n), port, this);
						++input_n;
					}
				}
			}

			if (reconnectOutputs) {

				//normalize route ouptuts: reduce the amount outputs to be equal to the amount of inputs
				if (current_output_auto_connection & AutoConnectPhysical) {

					//GZ: check this; could be heavy
					(*rIter)->output()->disconnect (this);
					size_t route_inputs_count = (*rIter)->n_inputs().get(DataType::AUDIO);

					//GZ: check this; could be heavy
					(*rIter)->output()->ensure_io(ChanCount(DataType::AUDIO, route_inputs_count), false, this );

				} else if (current_output_auto_connection & AutoConnectMaster){

					if (!reconnect_master) {
						continue;
					}

					//GZ: check this; could be heavy
					(*rIter)->output()->disconnect (this);

					if (_master_out) {
						uint32_t master_inputs_count = _master_out->n_inputs().get(DataType::AUDIO);
						(*rIter)->output()->ensure_io(ChanCount(DataType::AUDIO, master_inputs_count), false, this );
					} else {
						error << error << "Master bus is not available" << endmsg;
						break;
					}
				}

				for (uint32_t route_output_n = 0; route_output_n < (*rIter)->n_outputs().get(DataType::AUDIO); ++route_output_n) {
					if (current_output_auto_connection & AutoConnectPhysical) {

						if ( output_n == physoutputs.size() ) {
							break;
						}

						string port = physoutputs[output_n];

						if (port.empty() ) {
							error << "Physical Output number "<< output_n << " is unavailable and cannot be connected" << endmsg;
						}

						//GZ: check this; could be heavy
						(*rIter)->output()->connect ((*rIter)->output()->ports().port(DataType::AUDIO, route_output_n), port, this);
						++output_n;

					} else if (current_output_auto_connection & AutoConnectMaster) {

						if ( route_output_n == _master_out->n_inputs().get(DataType::AUDIO) ) {
							break;
						}

						// connect to master bus
						string port = _master_out->input()->ports().port(DataType::AUDIO, route_output_n)->name();

						if (port.empty() ) {
							error << "MasterBus Input number "<< route_output_n << " is unavailable and cannot be connected" << endmsg;
						}


						//GZ: check this; could be heavy
						(*rIter)->output()->connect ((*rIter)->output()->ports().port(DataType::AUDIO, route_output_n), port, this);

					}
				}
			}

			//auto_connect_route (*rIter, inputs, outputs, false, reconnectIputs);
		}

		_master_out->output()->disconnect (this);
		auto_connect_master_bus ();
	}

	graph_reordered ();

	session_routes_reconnected (); /* EMIT SIGNAL */
}

void
Session::reconnect_midi_scene_ports(bool inputs)
{
    if (inputs ) {

        boost::shared_ptr<MidiPort> scene_in_ptr = scene_in();
        if (scene_in_ptr) {
            scene_in_ptr->disconnect_all ();

            std::vector<EngineStateController::MidiPortState> midi_port_states;
            EngineStateController::instance()->get_physical_midi_input_states (midi_port_states);

            std::vector<EngineStateController::MidiPortState>::iterator state_iter = midi_port_states.begin();

            for (; state_iter != midi_port_states.end(); ++state_iter) {
                if (state_iter->active && state_iter->available && state_iter->scene_connected) {
                    scene_in_ptr->connect (state_iter->name);
                }
            }
        }

    } else {

        boost::shared_ptr<MidiPort> scene_out_ptr = scene_out();

        if (scene_out_ptr ) {
            scene_out_ptr->disconnect_all ();

            std::vector<EngineStateController::MidiPortState> midi_port_states;
            EngineStateController::instance()->get_physical_midi_output_states (midi_port_states);

            std::vector<EngineStateController::MidiPortState>::iterator state_iter = midi_port_states.begin();

            for (; state_iter != midi_port_states.end(); ++state_iter) {
                if (state_iter->active && state_iter->available && state_iter->scene_connected) {
                    scene_out_ptr->connect (state_iter->name);
                }
            }
        }
    }
}

void
Session::reconnect_mtc_ports ()
{
	boost::shared_ptr<MidiPort> mtc_in_ptr = _midi_ports->mtc_input_port();

	if (!mtc_in_ptr) {
		return;
	}

	mtc_in_ptr->disconnect_all ();

	std::vector<EngineStateController::MidiPortState> midi_port_states;
	EngineStateController::instance()->get_physical_midi_input_states (midi_port_states);

	std::vector<EngineStateController::MidiPortState>::iterator state_iter = midi_port_states.begin();

	for (; state_iter != midi_port_states.end(); ++state_iter) {
		if (state_iter->available && state_iter->mtc_in) {
			mtc_in_ptr->connect (state_iter->name);
		}
	}

	if (!_midi_ports->mtc_input_port ()->connected () &&
	    config.get_external_sync () &&
	    (Config->get_sync_source () == MTC) ) {
		config.set_external_sync (false);
	}

	if ( ARDOUR::Profile->get_trx () ) {
		// Tracks need this signal to update timecode_source_dropdown
		MtcOrLtcInputPortChanged (); //emit signal
	}
}

void
Session::reconnect_mmc_ports(bool inputs)
{
	if (inputs ) { // get all enabled midi input ports

		boost::shared_ptr<MidiPort> mmc_in_ptr = _midi_ports->mmc_in();
		if (mmc_in_ptr) {
			mmc_in_ptr->disconnect_all ();
			std::vector<std::string> enabled_midi_inputs;
			EngineStateController::instance()->get_physical_midi_inputs (enabled_midi_inputs);

			std::vector<std::string>::iterator port_iter = enabled_midi_inputs.begin();

			for (; port_iter != enabled_midi_inputs.end(); ++port_iter) {
				mmc_in_ptr->connect (*port_iter);
			}

		}
	} else { // get all enabled midi output ports

		boost::shared_ptr<MidiPort> mmc_out_ptr = _midi_ports->mmc_out();
		if (mmc_out_ptr ) {
			mmc_out_ptr->disconnect_all ();
			std::vector<std::string> enabled_midi_outputs;
			EngineStateController::instance()->get_physical_midi_outputs (enabled_midi_outputs);

			std::vector<std::string>::iterator port_iter = enabled_midi_outputs.begin();

			for (; port_iter != enabled_midi_outputs.end(); ++port_iter) {
				mmc_out_ptr->connect (*port_iter);
			}
		}
	}
}

#endif

/** Caller must not hold process lock
 *  @param name_template string to use for the start of the name, or "" to use "Audio".
 */
list< boost::shared_ptr<AudioTrack> >
Session::new_audio_track (int input_channels, int output_channels, TrackMode mode, RouteGroup* route_group,
			  uint32_t how_many, string name_template)
{
	string track_name;
	uint32_t track_id = 0;
	string port;
	RouteList new_routes;
	list<boost::shared_ptr<AudioTrack> > ret;

	const string name_pattern = default_track_name_pattern (DataType::AUDIO);
	bool const use_number = (how_many != 1) || name_template.empty () || (name_template == name_pattern);

	while (how_many) {

		if (!find_route_name (name_template.empty() ? _(name_pattern.c_str()) : name_template, ++track_id, track_name, use_number)) {
			error << "cannot find name for new audio track" << endmsg;
			goto failed;
		}

		boost::shared_ptr<AudioTrack> track;

		try {
			track.reset (new AudioTrack (*this, track_name, Route::Flag (0), mode));

			if (track->init ()) {
				goto failed;
			}

			if (ARDOUR::Profile->get_trx ()) {
				// TRACKS considers it's not a USE CASE, it's
				// a piece of behavior of the session model:
				//
				// Gain for a newly created route depends on
				// the current output_auto_connect mode:
				//
				//  0 for Stereo Out mode
				//  0 Multi Out mode
				if (Config->get_output_auto_connect() & AutoConnectMaster) {
					track->set_gain (dB_to_coefficient (0), 0);
				}
			}

			track->use_new_diskstream();

#ifdef BOOST_SP_ENABLE_DEBUG_HOOKS
			// boost_debug_shared_ptr_mark_interesting (track.get(), "Track");
#endif
			{
				Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());

				if (track->input()->ensure_io (ChanCount(DataType::AUDIO, input_channels), false, this)) {
					error << string_compose (
						_("cannot configure %1 in/%2 out configuration for new audio track"),
						input_channels, output_channels)
					      << endmsg;
					goto failed;
				}

				if (track->output()->ensure_io (ChanCount(DataType::AUDIO, output_channels), false, this)) {
					error << string_compose (
						_("cannot configure %1 in/%2 out configuration for new audio track"),
						input_channels, output_channels)
					      << endmsg;
					goto failed;
				}
			}

			if (route_group) {
				route_group->add (track);
			}

			track->non_realtime_input_change();

			track->DiskstreamChanged.connect_same_thread (*this, boost::bind (&Session::resort_routes, this));
			if (Config->get_remote_model() == UserOrdered) {
				track->set_remote_control_id (next_control_id());
			}

			new_routes.push_back (track);
			ret.push_back (track);

			RouteAddedOrRemoved (true); /* EMIT SIGNAL */
		}

		catch (failed_constructor &err) {
			error << _("Session: could not create new audio track.") << endmsg;
			goto failed;
		}

		catch (AudioEngine::PortRegistrationFailure& pfe) {

			error << pfe.what() << endmsg;
			goto failed;
		}

		--how_many;
	}

  failed:
	if (!new_routes.empty()) {
		StateProtector sp (this);
		if (Profile->get_trx()) {
			add_routes (new_routes, false, false, false);
		} else {
			add_routes (new_routes, true, true, false);
		}
	}

	return ret;
}

/** Caller must not hold process lock.
 *  @param name_template string to use for the start of the name, or "" to use "Bus".
 */
RouteList
Session::new_audio_route (int input_channels, int output_channels, RouteGroup* route_group, uint32_t how_many, string name_template)
{
	string bus_name;
	uint32_t bus_id = 0;
	string port;
	RouteList ret;

	bool const use_number = (how_many != 1) || name_template.empty () || name_template == _("Bus");

	while (how_many) {
		if (!find_route_name (name_template.empty () ? _("Bus") : name_template, ++bus_id, bus_name, use_number)) {
			error << "cannot find name for new audio bus" << endmsg;
			goto failure;
		}

		try {
			boost::shared_ptr<Route> bus (new Route (*this, bus_name, Route::Flag(0), DataType::AUDIO));

			if (bus->init ()) {
				goto failure;
			}

#ifdef BOOST_SP_ENABLE_DEBUG_HOOKS
			// boost_debug_shared_ptr_mark_interesting (bus.get(), "Route");
#endif
			{
				Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());

				if (bus->input()->ensure_io (ChanCount(DataType::AUDIO, input_channels), false, this)) {
					error << string_compose (_("cannot configure %1 in/%2 out configuration for new audio track"),
								 input_channels, output_channels)
					      << endmsg;
					goto failure;
				}


				if (bus->output()->ensure_io (ChanCount(DataType::AUDIO, output_channels), false, this)) {
					error << string_compose (_("cannot configure %1 in/%2 out configuration for new audio track"),
								 input_channels, output_channels)
					      << endmsg;
					goto failure;
				}
			}

			if (route_group) {
				route_group->add (bus);
			}
			if (Config->get_remote_model() == UserOrdered) {
				bus->set_remote_control_id (next_control_id());
			}

			bus->add_internal_return ();

			ret.push_back (bus);

			RouteAddedOrRemoved (true); /* EMIT SIGNAL */

			ARDOUR::GUIIdle ();
		}


		catch (failed_constructor &err) {
			error << _("Session: could not create new audio route.") << endmsg;
			goto failure;
		}

		catch (AudioEngine::PortRegistrationFailure& pfe) {
			error << pfe.what() << endmsg;
			goto failure;
		}


		--how_many;
	}

  failure:
	if (!ret.empty()) {
		StateProtector sp (this);
		if (Profile->get_trx()) {
			add_routes (ret, false, false, false);
		} else {
			add_routes (ret, false, true, true); // autoconnect // outputs only
		}
	}

	return ret;

}

RouteList
Session::new_route_from_template (uint32_t how_many, const std::string& template_path, const std::string& name_base)
{
	RouteList ret;
	uint32_t control_id;
	XMLTree tree;
	uint32_t number = 0;
	const uint32_t being_added = how_many;

	if (!tree.read (template_path.c_str())) {
		return ret;
	}

	XMLNode* node = tree.root();

	IO::disable_connecting ();

	control_id = next_control_id ();

	while (how_many) {

		XMLNode node_copy (*node);

		/* Remove IDs of everything so that new ones are used */
		node_copy.remove_property_recursively (X_("id"));

		try {
			string name;

			if (!name_base.empty()) {

				/* if we're adding more than one routes, force
				 * all the names of the new routes to be
				 * numbered, via the final parameter.
				 */

				if (!find_route_name (name_base.c_str(), ++number, name, (being_added > 1))) {
					fatal << _("Session: UINT_MAX routes? impossible!") << endmsg;
					/*NOTREACHDE*/
				}

			} else {

				string const route_name  = node_copy.property(X_("name"))->value ();

				/* generate a new name by adding a number to the end of the template name */
				if (!find_route_name (route_name.c_str(), ++number, name, true)) {
					fatal << _("Session: UINT_MAX routes? impossible!") << endmsg;
					abort(); /*NOTREACHED*/
				}
			}

			/* set this name in the XML description that we are about to use */
			Route::set_name_in_state (node_copy, name);

			/* trim bitslots from listen sends so that new ones are used */
			XMLNodeList children = node_copy.children ();
			for (XMLNodeList::iterator i = children.begin(); i != children.end(); ++i) {
				if ((*i)->name() == X_("Processor")) {
					XMLProperty* role = (*i)->property (X_("role"));
					if (role && role->value() == X_("Listen")) {
						(*i)->remove_property (X_("bitslot"));
					}
				}
			}

			boost::shared_ptr<Route> route (XMLRouteFactory (node_copy, 3000));

			if (route == 0) {
				error << _("Session: cannot create track/bus from template description") << endmsg;
				goto out;
			}

			if (boost::dynamic_pointer_cast<Track>(route)) {
				/* force input/output change signals so that the new diskstream
				   picks up the configuration of the route. During session
				   loading this normally happens in a different way.
				*/

				Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());

				IOChange change (IOChange::Type (IOChange::ConfigurationChanged | IOChange::ConnectionsChanged));
				change.after = route->input()->n_ports();
				route->input()->changed (change, this);
				change.after = route->output()->n_ports();
				route->output()->changed (change, this);
			}

			route->set_remote_control_id (control_id);
			++control_id;

			ret.push_back (route);

			RouteAddedOrRemoved (true); /* EMIT SIGNAL */
		}

		catch (failed_constructor &err) {
			error << _("Session: could not create new route from template") << endmsg;
			goto out;
		}

		catch (AudioEngine::PortRegistrationFailure& pfe) {
			error << pfe.what() << endmsg;
			goto out;
		}

		--how_many;
	}

  out:
	if (!ret.empty()) {
		StateProtector sp (this);
		if (Profile->get_trx()) {
			add_routes (ret, false, false, false);
		} else {
			add_routes (ret, true, true, false);
		}
		IO::enable_connecting ();
	}

	return ret;
}

void
Session::add_routes (RouteList& new_routes, bool input_auto_connect, bool output_auto_connect, bool save)
{
	try {
		PBD::Unwinder<bool> aip (_adding_routes_in_progress, true);
		add_routes_inner (new_routes, input_auto_connect, output_auto_connect);

	} catch (...) {
		error << _("Adding new tracks/busses failed") << endmsg;
	}

	graph_reordered ();

	update_latency (true);
	update_latency (false);

	set_dirty();

	if (save) {
		save_state (_current_snapshot_name);
	}

	reassign_track_numbers();

	update_route_record_state ();

	RouteAdded (new_routes); /* EMIT SIGNAL */
}

void
Session::add_routes_inner (RouteList& new_routes, bool input_auto_connect, bool output_auto_connect)
{
        ChanCount existing_inputs;
        ChanCount existing_outputs;
	uint32_t order = next_control_id();

	if (_order_hint > -1) {
		order = _order_hint;
		_order_hint = -1;
	}

        count_existing_track_channels (existing_inputs, existing_outputs);

	{
		RCUWriter<RouteList> writer (routes);
		boost::shared_ptr<RouteList> r = writer.get_copy ();
		r->insert (r->end(), new_routes.begin(), new_routes.end());

		/* if there is no control out and we're not in the middle of loading,
		   resort the graph here. if there is a control out, we will resort
		   toward the end of this method. if we are in the middle of loading,
		   we will resort when done.
		*/

		if (!_monitor_out && IO::connecting_legal) {
			resort_routes_using (r);
		}
	}

	for (RouteList::iterator x = new_routes.begin(); x != new_routes.end(); ++x) {

		boost::weak_ptr<Route> wpr (*x);
		boost::shared_ptr<Route> r (*x);

		r->listen_changed.connect_same_thread (*this, boost::bind (&Session::route_listen_changed, this, _2, wpr));
		r->solo_changed.connect_same_thread (*this, boost::bind (&Session::route_solo_changed, this, _1, _3, wpr));
		r->solo_isolated_changed.connect_same_thread (*this, boost::bind (&Session::route_solo_isolated_changed, this, _1, wpr));
		r->mute_changed.connect_same_thread (*this, boost::bind (&Session::route_mute_changed, this, _1));
		r->output()->changed.connect_same_thread (*this, boost::bind (&Session::set_worst_io_latencies_x, this, _1, _2));
		r->processors_changed.connect_same_thread (*this, boost::bind (&Session::route_processors_changed, this, _1));

		if (r->is_master()) {
			_master_out = r;
		}

		if (r->is_monitor()) {
			_monitor_out = r;
		}

		boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (r);
		if (tr) {
			tr->PlaylistChanged.connect_same_thread (*this, boost::bind (&Session::track_playlist_changed, this, boost::weak_ptr<Track> (tr)));
			track_playlist_changed (boost::weak_ptr<Track> (tr));
			tr->RecordEnableChanged.connect_same_thread (*this, boost::bind (&Session::update_route_record_state, this));

			boost::shared_ptr<MidiTrack> mt = boost::dynamic_pointer_cast<MidiTrack> (tr);
			if (mt) {
				mt->StepEditStatusChange.connect_same_thread (*this, boost::bind (&Session::step_edit_status_change, this, _1));
                                mt->output()->changed.connect_same_thread (*this, boost::bind (&Session::midi_output_change_handler, this, _1, _2, boost::weak_ptr<Route>(mt)));
			}
		}


		if (input_auto_connect || output_auto_connect) {
			auto_connect_route (r, existing_inputs, existing_outputs, true, input_auto_connect);
		}

		/* order keys are a GUI responsibility but we need to set up
		   reasonable defaults because they also affect the remote control
		   ID in most situations.
		*/

		if (!r->has_order_key ()) {
			if (r->is_auditioner()) {
				/* use an arbitrarily high value */
				r->set_order_key (UINT_MAX);
			} else {
				DEBUG_TRACE (DEBUG::OrderKeys, string_compose ("while adding, set %1 to order key %2\n", r->name(), order));
				r->set_order_key (order);
				order++;
			}
		}

		ARDOUR::GUIIdle ();
	}

	if (_monitor_out && IO::connecting_legal) {
		Glib::Threads::Mutex::Lock lm (_engine.process_lock());

		for (RouteList::iterator x = new_routes.begin(); x != new_routes.end(); ++x) {
			if ((*x)->is_monitor()) {
				/* relax */
			} else if ((*x)->is_master()) {
					/* relax */
			} else {
				(*x)->enable_monitor_send ();
			}
		}
	}
}

void
Session::globally_set_send_gains_to_zero (boost::shared_ptr<Route> dest)
{
	boost::shared_ptr<RouteList> r = routes.reader ();
	boost::shared_ptr<Send> s;

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		if ((s = (*i)->internal_send_for (dest)) != 0) {
			s->amp()->gain_control()->set_value (GAIN_COEFF_ZERO);
		}
	}
}

void
Session::globally_set_send_gains_to_unity (boost::shared_ptr<Route> dest)
{
	boost::shared_ptr<RouteList> r = routes.reader ();
	boost::shared_ptr<Send> s;

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		if ((s = (*i)->internal_send_for (dest)) != 0) {
			s->amp()->gain_control()->set_value (GAIN_COEFF_UNITY);
		}
	}
}

void
Session::globally_set_send_gains_from_track(boost::shared_ptr<Route> dest)
{
	boost::shared_ptr<RouteList> r = routes.reader ();
	boost::shared_ptr<Send> s;

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		if ((s = (*i)->internal_send_for (dest)) != 0) {
			s->amp()->gain_control()->set_value ((*i)->gain_control()->get_value());
		}
	}
}

/** @param include_buses true to add sends to buses and tracks, false for just tracks */
void
Session::globally_add_internal_sends (boost::shared_ptr<Route> dest, Placement p, bool include_buses)
{
	boost::shared_ptr<RouteList> r = routes.reader ();
	boost::shared_ptr<RouteList> t (new RouteList);

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		/* no MIDI sends because there are no MIDI busses yet */
		if (include_buses || boost::dynamic_pointer_cast<AudioTrack>(*i)) {
			t->push_back (*i);
		}
	}

	add_internal_sends (dest, p, t);
}

void
Session::add_internal_sends (boost::shared_ptr<Route> dest, Placement p, boost::shared_ptr<RouteList> senders)
{
	for (RouteList::iterator i = senders->begin(); i != senders->end(); ++i) {
		add_internal_send (dest, (*i)->before_processor_for_placement (p), *i);
	}
}

void
Session::add_internal_send (boost::shared_ptr<Route> dest, int index, boost::shared_ptr<Route> sender)
{
	add_internal_send (dest, sender->before_processor_for_index (index), sender);
}

void
Session::add_internal_send (boost::shared_ptr<Route> dest, boost::shared_ptr<Processor> before, boost::shared_ptr<Route> sender)
{
	if (sender->is_monitor() || sender->is_master() || sender == dest || dest->is_monitor() || dest->is_master()) {
		return;
	}

	if (!dest->internal_return()) {
		dest->add_internal_return ();
	}

	sender->add_aux_send (dest, before);

	graph_reordered ();
}


void
Session::remove_routes (boost::shared_ptr<RouteList> routes_to_remove)
{
	PBD::Unwinder<bool> uw_flag (_route_deletion_in_progress, true);

	{ // RCU Writer scope
		RCUWriter<RouteList> writer (routes);
		boost::shared_ptr<RouteList> rs = writer.get_copy ();


		for (RouteList::iterator iter = routes_to_remove->begin(); iter != routes_to_remove->end(); ++iter) {

			if (*iter == _master_out) {
				continue;
			}

			(*iter)->set_solo (false, this);

			rs->remove (*iter);

			/* deleting the master out seems like a dumb
			   idea, but its more of a UI policy issue
			   than our concern.
			*/

			if (*iter == _master_out) {
				_master_out = boost::shared_ptr<Route> ();
			}

			if (*iter == _monitor_out) {
				_monitor_out.reset ();
			}

			// We need to disconnect the route's inputs and outputs

			(*iter)->input()->disconnect (0);
			(*iter)->output()->disconnect (0);

			/* if the route had internal sends sending to it, remove them */
			if ((*iter)->internal_return()) {

				boost::shared_ptr<RouteList> r = routes.reader ();
				for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
					boost::shared_ptr<Send> s = (*i)->internal_send_for (*iter);
					if (s) {
						(*i)->remove_processor (s);
					}
				}
			}

			/* if the monitoring section had a pointer to this route, remove it */
			if (_monitor_out && !(*iter)->is_master() && !(*iter)->is_monitor()) {
				Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());
				PBD::Unwinder<bool> uw (ignore_route_processor_changes, true);
				(*iter)->remove_aux_or_listen (_monitor_out);
			}

			boost::shared_ptr<MidiTrack> mt = boost::dynamic_pointer_cast<MidiTrack> (*iter);
			if (mt && mt->step_editing()) {
				if (_step_editors > 0) {
					_step_editors--;
				}
			}
		}

		/* writer goes out of scope, forces route list update */

	} // end of RCU Writer scope

	update_route_solo_state ();
	RouteAddedOrRemoved (false); /* EMIT SIGNAL */
	update_latency_compensation ();
	set_dirty();

	/* Re-sort routes to remove the graph's current references to the one that is
	 * going away, then flush old references out of the graph.
	 * Wave Tracks: reconnect routes
	 */

#ifdef USE_TRACKS_CODE_FEATURES
		reconnect_existing_routes(true, false);
#else
		routes.flush (); // maybe unsafe, see below.
		resort_routes ();
#endif

	if (_process_graph) {
		_process_graph->clear_other_chain ();
	}

	/* get rid of it from the dead wood collection in the route list manager */
	/* XXX i think this is unsafe as it currently stands, but i am not sure. (pd, october 2nd, 2006) */

	routes.flush ();

	/* try to cause everyone to drop their references
	 * and unregister ports from the backend
	 */

	for (RouteList::iterator iter = routes_to_remove->begin(); iter != routes_to_remove->end(); ++iter) {
		(*iter)->drop_references ();
	}

	Route::RemoteControlIDChange(); /* EMIT SIGNAL */

	/* save the new state of the world */

	if (save_state (_current_snapshot_name)) {
		save_history (_current_snapshot_name);
	}

	reassign_track_numbers();
	update_route_record_state ();
}

void
Session::remove_route (boost::shared_ptr<Route> route)
{
	boost::shared_ptr<RouteList> rl (new RouteList);
	rl->push_back (route);
	remove_routes (rl);
}

void
Session::route_mute_changed (void* /*src*/)
{
	set_dirty ();
}

void
Session::route_listen_changed (bool group_override, boost::weak_ptr<Route> wpr)
{
	boost::shared_ptr<Route> route = wpr.lock();
	if (!route) {
		error << string_compose (_("programming error: %1"), X_("invalid route weak ptr passed to route_listen_changed")) << endmsg;
		return;
	}

	if (route->listening_via_monitor ()) {

		if (Config->get_exclusive_solo()) {
			/* new listen: disable all other listen, except solo-grouped channels */
			RouteGroup* rg = route->route_group ();
			bool leave_group_alone = (rg && rg->is_active() && rg->is_solo());
			if (group_override && rg) {
				leave_group_alone = !leave_group_alone;
			}
			boost::shared_ptr<RouteList> r = routes.reader ();
			for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
				if ((*i) == route || (*i)->solo_isolated() || (*i)->is_master() || (*i)->is_monitor() || (*i)->is_auditioner() || (leave_group_alone && ((*i)->route_group() == rg))) {
					continue;
				}
				(*i)->set_listen (false, this, group_override);
			}
		}

		_listen_cnt++;

	} else if (_listen_cnt > 0) {

		_listen_cnt--;
	}

	update_route_solo_state ();
}
void
Session::route_solo_isolated_changed (void* /*src*/, boost::weak_ptr<Route> wpr)
{
	boost::shared_ptr<Route> route = wpr.lock ();

	if (!route) {
		/* should not happen */
		error << string_compose (_("programming error: %1"), X_("invalid route weak ptr passed to route_solo_isolated_changed")) << endmsg;
		return;
	}

	bool send_changed = false;

	if (route->solo_isolated()) {
		if (_solo_isolated_cnt == 0) {
			send_changed = true;
		}
		_solo_isolated_cnt++;
	} else if (_solo_isolated_cnt > 0) {
		_solo_isolated_cnt--;
		if (_solo_isolated_cnt == 0) {
			send_changed = true;
		}
	}

	if (send_changed) {
		IsolatedChanged (); /* EMIT SIGNAL */
	}
}

void
Session::route_solo_changed (bool self_solo_change, bool group_override,  boost::weak_ptr<Route> wpr)
{
	DEBUG_TRACE (DEBUG::Solo, string_compose ("route solo change, self = %1\n", self_solo_change));

	if (!self_solo_change) {
		// session doesn't care about changes to soloed-by-others
		return;
	}

	boost::shared_ptr<Route> route = wpr.lock ();
	assert (route);

	boost::shared_ptr<RouteList> r = routes.reader ();
	int32_t delta;

	if (route->self_soloed()) {
		delta = 1;
	} else {
		delta = -1;
	}

	RouteGroup* rg = route->route_group ();
	bool leave_group_alone = (rg && rg->is_active() && rg->is_solo());
	if (group_override && rg) {
		leave_group_alone = !leave_group_alone;
	}
	if (delta == 1 && Config->get_exclusive_solo()) {

		/* new solo: disable all other solos, but not the group if its solo-enabled */

		for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
			if ((*i) == route || (*i)->is_master() || (*i)->is_monitor() || (*i)->is_auditioner() ||
			    (leave_group_alone && ((*i)->route_group() == rg))) {
				continue;
			}
			(*i)->set_solo (false, this, group_override);
		}
	}

	DEBUG_TRACE (DEBUG::Solo, string_compose ("propagate solo change, delta = %1\n", delta));

	RouteList uninvolved;

	DEBUG_TRACE (DEBUG::Solo, string_compose ("%1\n", route->name()));

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		bool via_sends_only;
		bool in_signal_flow;

		if ((*i) == route || (*i)->is_master() || (*i)->is_monitor() || (*i)->is_auditioner() ||
		    (leave_group_alone && ((*i)->route_group() == rg))) {
			continue;
		}

		in_signal_flow = false;

		DEBUG_TRACE (DEBUG::Solo, string_compose ("check feed from %1\n", (*i)->name()));

		if ((*i)->feeds (route, &via_sends_only)) {
			DEBUG_TRACE (DEBUG::Solo, string_compose ("\tthere is a feed from %1\n", (*i)->name()));
			if (!via_sends_only) {
				if (!route->soloed_by_others_upstream()) {
					(*i)->mod_solo_by_others_downstream (delta);
				} else {
					DEBUG_TRACE (DEBUG::Solo, "\talready soloed by others upstream\n");
				}
			} else {
				DEBUG_TRACE (DEBUG::Solo, string_compose ("\tthere is a send-only feed from %1\n", (*i)->name()));
			}
			in_signal_flow = true;
		} else {
			DEBUG_TRACE (DEBUG::Solo, string_compose ("\tno feed from %1\n", (*i)->name()));
		}

		DEBUG_TRACE (DEBUG::Solo, string_compose ("check feed to %1\n", (*i)->name()));

		if (route->feeds (*i, &via_sends_only)) {
			/* propagate solo upstream only if routing other than
			   sends is involved, but do consider the other route
			   (*i) to be part of the signal flow even if only
			   sends are involved.
			*/
			DEBUG_TRACE (DEBUG::Solo, string_compose ("%1 feeds %2 via sends only %3 sboD %4 sboU %5\n",
								  route->name(),
								  (*i)->name(),
								  via_sends_only,
								  route->soloed_by_others_downstream(),
								  route->soloed_by_others_upstream()));
			if (!via_sends_only) {
				//NB. Triggers Invert Push, which handles soloed by downstream
				DEBUG_TRACE (DEBUG::Solo, string_compose ("\tmod %1 by %2\n", (*i)->name(), delta));
				(*i)->mod_solo_by_others_upstream (delta);
			} else {
				DEBUG_TRACE (DEBUG::Solo, string_compose ("\tfeed to %1 ignored, sends-only\n", (*i)->name()));
			}
			in_signal_flow = true;
		} else {
			DEBUG_TRACE (DEBUG::Solo, "\tno feed to\n");
		}

		if (!in_signal_flow) {
			uninvolved.push_back (*i);
		}
	}

	DEBUG_TRACE (DEBUG::Solo, "propagation complete\n");

	update_route_solo_state (r);

	/* now notify that the mute state of the routes not involved in the signal
	   pathway of the just-solo-changed route may have altered.
	*/

	for (RouteList::iterator i = uninvolved.begin(); i != uninvolved.end(); ++i) {
		DEBUG_TRACE (DEBUG::Solo, string_compose ("mute change for %1, which neither feeds or is fed by %2\n", (*i)->name(), route->name()));
		(*i)->act_on_mute ();
		(*i)->mute_changed (this);
	}

	SoloChanged (); /* EMIT SIGNAL */
	set_dirty();
}

void
Session::update_route_solo_state (boost::shared_ptr<RouteList> r)
{
	/* now figure out if anything that matters is soloed (or is "listening")*/

	bool something_soloed = false;
	uint32_t listeners = 0;
	uint32_t isolated = 0;

	if (!r) {
		r = routes.reader();
	}

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		if (!(*i)->is_master() && !(*i)->is_monitor() && !(*i)->is_auditioner() && (*i)->self_soloed()) {
			something_soloed = true;
		}

		if (!(*i)->is_auditioner() && (*i)->listening_via_monitor()) {
			if (Config->get_solo_control_is_listen_control()) {
				listeners++;
			} else {
				(*i)->set_listen (false, this);
			}
		}

		if ((*i)->solo_isolated()) {
			isolated++;
		}
	}

	if (something_soloed != _non_soloed_outs_muted) {
		_non_soloed_outs_muted = something_soloed;
		SoloActive (_non_soloed_outs_muted); /* EMIT SIGNAL */
	}

	_listen_cnt = listeners;

	if (isolated != _solo_isolated_cnt) {
		_solo_isolated_cnt = isolated;
		IsolatedChanged (); /* EMIT SIGNAL */
	}

	DEBUG_TRACE (DEBUG::Solo, string_compose ("solo state updated by session, soloed? %1 listeners %2 isolated %3\n",
						  something_soloed, listeners, isolated));
}

boost::shared_ptr<RouteList>
Session::get_routes_with_internal_returns() const
{
	boost::shared_ptr<RouteList> r = routes.reader ();
	boost::shared_ptr<RouteList> rl (new RouteList);

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		if ((*i)->internal_return ()) {
			rl->push_back (*i);
		}
	}
	return rl;
}

bool
Session::io_name_is_legal (const std::string& name)
{
	boost::shared_ptr<RouteList> r = routes.reader ();

	for (vector<string>::const_iterator reserved = reserved_io_names.begin(); reserved != reserved_io_names.end(); ++reserved) {
		if (name == *reserved) {
			return false;
		}
	}

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		if ((*i)->name() == name) {
			return false;
		}

		if ((*i)->has_io_processor_named (name)) {
			return false;
		}
	}

	return true;
}

void
Session::set_exclusive_input_active (boost::shared_ptr<RouteList> rl, bool onoff, bool flip_others)
{
	RouteList rl2;
	vector<string> connections;

	/* if we are passed only a single route and we're not told to turn
	 * others off, then just do the simple thing.
	 */

	if (flip_others == false && rl->size() == 1) {
		boost::shared_ptr<MidiTrack> mt = boost::dynamic_pointer_cast<MidiTrack> (rl->front());
		if (mt) {
			mt->set_input_active (onoff);
			return;
		}
	}

	for (RouteList::iterator rt = rl->begin(); rt != rl->end(); ++rt) {

		PortSet& ps ((*rt)->input()->ports());

		for (PortSet::iterator p = ps.begin(); p != ps.end(); ++p) {
			p->get_connections (connections);
		}

		for (vector<string>::iterator s = connections.begin(); s != connections.end(); ++s) {
			routes_using_input_from (*s, rl2);
		}

		/* scan all relevant routes to see if others are on or off */

		bool others_are_already_on = false;

		for (RouteList::iterator r = rl2.begin(); r != rl2.end(); ++r) {

			boost::shared_ptr<MidiTrack> mt = boost::dynamic_pointer_cast<MidiTrack> (*r);

			if (!mt) {
				continue;
			}

			if ((*r) != (*rt)) {
				if (mt->input_active()) {
					others_are_already_on = true;
				}
			} else {
				/* this one needs changing */
				mt->set_input_active (onoff);
			}
		}

		if (flip_others) {

			/* globally reverse other routes */

			for (RouteList::iterator r = rl2.begin(); r != rl2.end(); ++r) {
				if ((*r) != (*rt)) {
					boost::shared_ptr<MidiTrack> mt = boost::dynamic_pointer_cast<MidiTrack> (*r);
					if (mt) {
						mt->set_input_active (!others_are_already_on);
					}
				}
			}
		}
	}
}

void
Session::routes_using_input_from (const string& str, RouteList& rl)
{
	boost::shared_ptr<RouteList> r = routes.reader();

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		if ((*i)->input()->connected_to (str)) {
			rl.push_back (*i);
		}
	}
}

boost::shared_ptr<Route>
Session::route_by_name (string name)
{
	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		if ((*i)->name() == name) {
			return *i;
		}
	}

	return boost::shared_ptr<Route> ((Route*) 0);
}

boost::shared_ptr<Route>
Session::route_by_id (PBD::ID id)
{
	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		if ((*i)->id() == id) {
			return *i;
		}
	}

	return boost::shared_ptr<Route> ((Route*) 0);
}

boost::shared_ptr<Track>
Session::track_by_diskstream_id (PBD::ID id)
{
	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		boost::shared_ptr<Track> t = boost::dynamic_pointer_cast<Track> (*i);
		if (t && t->using_diskstream_id (id)) {
			return t;
		}
	}

	return boost::shared_ptr<Track> ();
}

boost::shared_ptr<Route>
Session::route_by_remote_id (uint32_t id)
{
	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		if ((*i)->remote_control_id() == id) {
			return *i;
		}
	}

	return boost::shared_ptr<Route> ((Route*) 0);
}


void
Session::reassign_track_numbers ()
{
	int64_t tn = 0;
	int64_t bn = 0;
	RouteList r (*(routes.reader ()));
	SignalOrderRouteSorter sorter;
	r.sort (sorter);

	StateProtector sp (this);

	for (RouteList::iterator i = r.begin(); i != r.end(); ++i) {
		if (boost::dynamic_pointer_cast<Track> (*i)) {
			(*i)->set_track_number(++tn);
		}
		else if (!(*i)->is_master() && !(*i)->is_monitor() && !(*i)->is_auditioner()) {
			(*i)->set_track_number(--bn);
		}
	}
	const uint32_t decimals = ceilf (log10f (tn + 1));
	const bool decimals_changed = _track_number_decimals != decimals;
	_track_number_decimals = decimals;

	if (decimals_changed && config.get_track_name_number ()) {
		for (RouteList::iterator i = r.begin(); i != r.end(); ++i) {
			boost::shared_ptr<Track> t = boost::dynamic_pointer_cast<Track> (*i);
			if (t) {
				t->resync_track_name();
			}
		}
		// trigger GUI re-layout
		config.ParameterChanged("track-name-number");
	}
}

void
Session::playlist_region_added (boost::weak_ptr<Region> w)
{
	boost::shared_ptr<Region> r = w.lock ();
	if (!r) {
		return;
	}

	/* These are the operations that are currently in progress... */
	list<GQuark> curr = _current_trans_quarks;
	curr.sort ();

	/* ...and these are the operations during which we want to update
	   the session range location markers.
	*/
	list<GQuark> ops;
	ops.push_back (Operations::capture);
	ops.push_back (Operations::paste);
	ops.push_back (Operations::duplicate_region);
	ops.push_back (Operations::insert_file);
	ops.push_back (Operations::insert_region);
	ops.push_back (Operations::drag_region_brush);
	ops.push_back (Operations::region_drag);
	ops.push_back (Operations::selection_grab);
	ops.push_back (Operations::region_fill);
	ops.push_back (Operations::fill_selection);
	ops.push_back (Operations::create_region);
	ops.push_back (Operations::region_copy);
	ops.push_back (Operations::fixed_time_region_copy);
	ops.sort ();

	/* See if any of the current operations match the ones that we want */
	list<GQuark> in;
	set_intersection (_current_trans_quarks.begin(), _current_trans_quarks.end(), ops.begin(), ops.end(), back_inserter (in));

	/* If so, update the session range markers */
	if (!in.empty ()) {
		maybe_update_session_range (r->position (), r->last_frame ());
	}
}

/** Update the session range markers if a is before the current start or
 *  b is after the current end.
 */
void
Session::maybe_update_session_range (framepos_t a, framepos_t b)
{
	if (_state_of_the_state & Loading) {
		return;
	}

	framepos_t session_end_marker_shift_samples = session_end_shift * _nominal_frame_rate;

	if (_session_range_location == 0) {

		set_session_range_location (a, b + session_end_marker_shift_samples);

	} else {

		if (a < _session_range_location->start()) {
			_session_range_location->set_start (a);
		}

		if (b > _session_range_location->end()) {
			_session_range_location->set_end (b);
		}
	}
}

void
Session::playlist_ranges_moved (list<Evoral::RangeMove<framepos_t> > const & ranges)
{
	for (list<Evoral::RangeMove<framepos_t> >::const_iterator i = ranges.begin(); i != ranges.end(); ++i) {
		maybe_update_session_range (i->to, i->to + i->length);
	}
}

void
Session::playlist_regions_extended (list<Evoral::Range<framepos_t> > const & ranges)
{
	for (list<Evoral::Range<framepos_t> >::const_iterator i = ranges.begin(); i != ranges.end(); ++i) {
		maybe_update_session_range (i->from, i->to);
	}
}

/* Region management */

boost::shared_ptr<Region>
Session::find_whole_file_parent (boost::shared_ptr<Region const> child) const
{
	const RegionFactory::RegionMap& regions (RegionFactory::regions());
	RegionFactory::RegionMap::const_iterator i;
	boost::shared_ptr<Region> region;

	Glib::Threads::Mutex::Lock lm (region_lock);

	for (i = regions.begin(); i != regions.end(); ++i) {

		region = i->second;

		if (region->whole_file()) {

			if (child->source_equivalent (region)) {
				return region;
			}
		}
	}

	return boost::shared_ptr<Region> ();
}

int
Session::destroy_sources (list<boost::shared_ptr<Source> > srcs)
{
	set<boost::shared_ptr<Region> > relevant_regions;

	for (list<boost::shared_ptr<Source> >::iterator s = srcs.begin(); s != srcs.end(); ++s) {
		RegionFactory::get_regions_using_source (*s, relevant_regions);
	}

	for (set<boost::shared_ptr<Region> >::iterator r = relevant_regions.begin(); r != relevant_regions.end(); ) {
		set<boost::shared_ptr<Region> >::iterator tmp;

		tmp = r;
		++tmp;

		playlists->destroy_region (*r);
		RegionFactory::map_remove (*r);

		(*r)->drop_sources ();
		(*r)->drop_references ();

		relevant_regions.erase (r);

		r = tmp;
	}

	for (list<boost::shared_ptr<Source> >::iterator s = srcs.begin(); s != srcs.end(); ) {

		{
			Glib::Threads::Mutex::Lock ls (source_lock);
			/* remove from the main source list */
			sources.erase ((*s)->id());
		}

		(*s)->mark_for_remove ();
		(*s)->drop_references ();

		s = srcs.erase (s);
	}

	return 0;
}

int
Session::remove_last_capture ()
{
	list<boost::shared_ptr<Source> > srcs;

	boost::shared_ptr<RouteList> rl = routes.reader ();
	for (RouteList::iterator i = rl->begin(); i != rl->end(); ++i) {
		boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
		if (!tr) {
			continue;
		}

		list<boost::shared_ptr<Source> >& l = tr->last_capture_sources();

		if (!l.empty()) {
			srcs.insert (srcs.end(), l.begin(), l.end());
			l.clear ();
		}
	}

	destroy_sources (srcs);

	save_state (_current_snapshot_name);

	return 0;
}

/* Source Management */

void
Session::add_source (boost::shared_ptr<Source> source)
{
	pair<SourceMap::key_type, SourceMap::mapped_type> entry;
	pair<SourceMap::iterator,bool> result;

	entry.first = source->id();
	entry.second = source;

	{
		Glib::Threads::Mutex::Lock lm (source_lock);
		result = sources.insert (entry);
	}

	if (result.second) {

		/* yay, new source */

		boost::shared_ptr<FileSource> fs = boost::dynamic_pointer_cast<FileSource> (source);

		if (fs) {
			if (!fs->within_session()) {
				ensure_search_path_includes (Glib::path_get_dirname (fs->path()), fs->type());
			}
		}

		set_dirty();

		boost::shared_ptr<AudioFileSource> afs;

		if ((afs = boost::dynamic_pointer_cast<AudioFileSource>(source)) != 0) {
			if (Config->get_auto_analyse_audio()) {
				Analyser::queue_source_for_analysis (source, false);
			}
		}

		source->DropReferences.connect_same_thread (*this, boost::bind (&Session::remove_source, this, boost::weak_ptr<Source> (source)));
	}
}

void
Session::remove_source (boost::weak_ptr<Source> src)
{
	if (_state_of_the_state & Deletion) {
		return;
	}

	SourceMap::iterator i;
	boost::shared_ptr<Source> source = src.lock();

	if (!source) {
		return;
	}

	{
		Glib::Threads::Mutex::Lock lm (source_lock);

		if ((i = sources.find (source->id())) != sources.end()) {
			sources.erase (i);
		}
	}

	if (!(_state_of_the_state & StateOfTheState (InCleanup|Loading))) {

		/* save state so we don't end up with a session file
		   referring to non-existent sources.
		*/

		save_state (_current_snapshot_name);
	}
}

boost::shared_ptr<Source>
Session::source_by_id (const PBD::ID& id)
{
	Glib::Threads::Mutex::Lock lm (source_lock);
	SourceMap::iterator i;
	boost::shared_ptr<Source> source;

	if ((i = sources.find (id)) != sources.end()) {
		source = i->second;
	}

	return source;
}

boost::shared_ptr<AudioFileSource>
Session::audio_source_by_path_and_channel (const string& path, uint16_t chn) const
{
	/* Restricted to audio files because only audio sources have channel
	   as a property.
	*/

	Glib::Threads::Mutex::Lock lm (source_lock);

	for (SourceMap::const_iterator i = sources.begin(); i != sources.end(); ++i) {
		boost::shared_ptr<AudioFileSource> afs
			= boost::dynamic_pointer_cast<AudioFileSource>(i->second);

		if (afs && afs->path() == path && chn == afs->channel()) {
			return afs;
		}
	}

	return boost::shared_ptr<AudioFileSource>();
}

boost::shared_ptr<MidiSource>
Session::midi_source_by_path (const std::string& path) const
{
	/* Restricted to MIDI files because audio sources require a channel
	   for unique identification, in addition to a path.
	*/

	Glib::Threads::Mutex::Lock lm (source_lock);

	for (SourceMap::const_iterator s = sources.begin(); s != sources.end(); ++s) {
		boost::shared_ptr<MidiSource> ms
			= boost::dynamic_pointer_cast<MidiSource>(s->second);
		boost::shared_ptr<FileSource> fs
			= boost::dynamic_pointer_cast<FileSource>(s->second);

		if (ms && fs && fs->path() == path) {
			return ms;
		}
	}

	return boost::shared_ptr<MidiSource>();
}

uint32_t
Session::count_sources_by_origin (const string& path)
{
	uint32_t cnt = 0;
	Glib::Threads::Mutex::Lock lm (source_lock);

	for (SourceMap::iterator i = sources.begin(); i != sources.end(); ++i) {
		boost::shared_ptr<FileSource> fs
			= boost::dynamic_pointer_cast<FileSource>(i->second);

		if (fs && fs->origin() == path) {
			++cnt;
		}
	}

	return cnt;
}

static string
peak_file_helper (const string& peak_path, const string& file_path, const string& file_base, bool hash) {
	if (hash) {
		std::string checksum = Glib::Checksum::compute_checksum(Glib::Checksum::CHECKSUM_SHA1, file_path + G_DIR_SEPARATOR + file_base);
		return Glib::build_filename (peak_path, checksum + peakfile_suffix);
	} else {
		return Glib::build_filename (peak_path, file_base + peakfile_suffix);
	}
}

string
Session::construct_peak_filepath (const string& filepath, const bool in_session, const bool old_peak_name) const
{
	string interchange_dir_string = string (interchange_dir_name) + G_DIR_SEPARATOR;

	if (Glib::path_is_absolute (filepath)) {

		/* rip the session dir from the audiofile source */

		string session_path;
		bool in_another_session = true;

		if (filepath.find (interchange_dir_string) != string::npos) {

			session_path = Glib::path_get_dirname (filepath); /* now ends in audiofiles */
			session_path = Glib::path_get_dirname (session_path); /* now ends in session name */
			session_path = Glib::path_get_dirname (session_path); /* now ends in interchange */
			session_path = Glib::path_get_dirname (session_path); /* now has session path */

			/* see if it is within our session */

			for (vector<space_and_path>::const_iterator i = session_dirs.begin(); i != session_dirs.end(); ++i) {
				if (i->path == session_path) {
					in_another_session = false;
					break;
				}
			}
		} else {
			in_another_session = false;
		}


		if (in_another_session) {
			SessionDirectory sd (session_path);
			return peak_file_helper (sd.peak_path(), "", Glib::path_get_basename (filepath), !old_peak_name);
		}
	}

	/* 1) if file belongs to this session
	 * it may be a relative path (interchange/...)
	 * or just basename (session_state, remove source)
	 * -> just use the basename
	 */
	std::string filename = Glib::path_get_basename (filepath);
	std::string path;

	/* 2) if the file is outside our session dir:
	 * (imported but not copied) add the path for check-summming */
	if (!in_session) {
		path = Glib::path_get_dirname (filepath);
	}

	return peak_file_helper (_session_dir->peak_path(), path, Glib::path_get_basename (filepath), !old_peak_name);
}

string
Session::new_audio_source_path_for_embedded (const std::string& path)
{
	/* embedded source:
	 *
	 * we know that the filename is already unique because it exists
	 * out in the filesystem.
	 *
	 * However, when we bring it into the session, we could get a
	 * collision.
	 *
	 * Eg. two embedded files:
	 *
	 *          /foo/bar/baz.wav
	 *          /frob/nic/baz.wav
	 *
	 * When merged into session, these collide.
	 *
	 * There will not be a conflict with in-memory sources
	 * because when the source was created we already picked
	 * a unique name for it.
	 *
	 * This collision is not likely to be common, but we have to guard
	 * against it.  So, if there is a collision, take the md5 hash of the
	 * the path, and use that as the filename instead.
	 */

	SessionDirectory sdir (get_best_session_directory_for_new_audio());
	string base = Glib::path_get_basename (path);
	string newpath = Glib::build_filename (sdir.sound_path(), base);

	if (Glib::file_test (newpath, Glib::FILE_TEST_EXISTS)) {

		MD5 md5;

		md5.digestString (path.c_str());
		md5.writeToString ();
		base = md5.digestChars;

		string ext = get_suffix (path);

		if (!ext.empty()) {
			base += '.';
			base += ext;
		}

		newpath = Glib::build_filename (sdir.sound_path(), base);

		/* if this collides, we're screwed */

		if (Glib::file_test (newpath, Glib::FILE_TEST_EXISTS)) {
			error << string_compose (_("Merging embedded file %1: name collision AND md5 hash collision!"), path) << endmsg;
			return string();
		}

	}

	return newpath;
}

/** Return true if there are no audio file sources that use @param name as
 * the filename component of their path.
 *
 * Return false otherwise.
 *
 * This method MUST ONLY be used to check in-session, mono files since it
 * hard-codes the channel of the audio file source we are looking for as zero.
 *
 * If/when Ardour supports native files in non-mono formats, the logic here
 * will need to be revisited.
 */
bool
Session::audio_source_name_is_unique (const string& name)
{
	std::vector<string> sdirs = source_search_path (DataType::AUDIO);
	vector<space_and_path>::iterator i;
	uint32_t existing = 0;

	for (vector<string>::const_iterator i = sdirs.begin(); i != sdirs.end(); ++i) {

		/* note that we search *without* the extension so that
		   we don't end up both "Audio 1-1.wav" and "Audio 1-1.caf"
		   in the event that this new name is required for
		   a file format change.
		*/

		const string spath = *i;

		if (matching_unsuffixed_filename_exists_in (spath, name)) {
			existing++;
			break;
		}

		/* it is possible that we have the path already
		 * assigned to a source that has not yet been written
		 * (ie. the write source for a diskstream). we have to
		 * check this in order to make sure that our candidate
		 * path isn't used again, because that can lead to
		 * two Sources point to the same file with different
		 * notions of their removability.
		 */


		string possible_path = Glib::build_filename (spath, name);

		if (audio_source_by_path_and_channel (possible_path, 0)) {
			existing++;
			break;
		}
	}

	return (existing == 0);
}

string
Session::format_audio_source_name (const string& legalized_base, uint32_t nchan, uint32_t chan, bool destructive, bool take_required, uint32_t cnt, bool related_exists)
{
	ostringstream sstr;
	const string ext = native_header_format_extension (config.get_native_file_header_format(), DataType::AUDIO);

	if (Profile->get_trx() && destructive) {
		sstr << 'T';
		sstr << setfill ('0') << setw (4) << cnt;
		sstr << legalized_base;
	} else {
		sstr << legalized_base;

		if (take_required || related_exists) {
			sstr << '-';
			sstr << cnt;
		}
	}

	if (nchan == 2) {
		if (chan == 0) {
			sstr << "%L";
		} else {
			sstr << "%R";
		}
	} else if (nchan > 2) {
		if (nchan < 26) {
			sstr << '%';
			sstr << 'a' + chan;
		} else {
			/* XXX what? more than 26 channels! */
			sstr << '%';
			sstr << chan+1;
		}
	}

	sstr << ext;

	return sstr.str();
}

/** Return a unique name based on \a base for a new internal audio source */
string
Session::new_audio_source_path (const string& base, uint32_t nchan, uint32_t chan, bool destructive, bool take_required)
{
	uint32_t cnt;
	string possible_name;
	const uint32_t limit = 9999; // arbitrary limit on number of files with the same basic name
	string legalized;
	bool some_related_source_name_exists = false;

	legalized = legalize_for_path (base);

	// Find a "version" of the base name that doesn't exist in any of the possible directories.

	for (cnt = (destructive ? ++destructive_index : 1); cnt <= limit; ++cnt) {

		possible_name = format_audio_source_name (legalized, nchan, chan, destructive, take_required, cnt, some_related_source_name_exists);

		if (audio_source_name_is_unique (possible_name)) {
			break;
		}

		some_related_source_name_exists = true;

		if (cnt > limit) {
			error << string_compose(
					_("There are already %1 recordings for %2, which I consider too many."),
					limit, base) << endmsg;
			destroy ();
			throw failed_constructor();
		}
	}

	/* We've established that the new name does not exist in any session
	 * directory, so now find out which one we should use for this new
	 * audio source.
	 */

	SessionDirectory sdir (get_best_session_directory_for_new_audio());

	std::string s = Glib::build_filename (sdir.sound_path(), possible_name);

	return s;
}

/** Return a unique name based on `base` for a new internal MIDI source */
string
Session::new_midi_source_path (const string& base)
{
	uint32_t cnt;
	char buf[PATH_MAX+1];
	const uint32_t limit = 10000;
	string legalized;
	string possible_path;
	string possible_name;

	buf[0] = '\0';
	legalized = legalize_for_path (base);

	// Find a "version" of the file name that doesn't exist in any of the possible directories.
	std::vector<string> sdirs = source_search_path(DataType::MIDI);

	/* - the main session folder is the first in the vector.
	 * - after checking all locations for file-name uniqueness,
	 *   we keep the one from the last iteration as new file name
	 * - midi files are small and should just be kept in the main session-folder
	 *
	 * -> reverse the array, check main session folder last and use that as location
	 *    for MIDI files.
	 */
	std::reverse(sdirs.begin(), sdirs.end());

	for (cnt = 1; cnt <= limit; ++cnt) {

		vector<space_and_path>::iterator i;
		uint32_t existing = 0;

		for (vector<string>::const_iterator i = sdirs.begin(); i != sdirs.end(); ++i) {

			snprintf (buf, sizeof(buf), "%s-%u.mid", legalized.c_str(), cnt);
			possible_name = buf;

			possible_path = Glib::build_filename (*i, possible_name);

			if (Glib::file_test (possible_path, Glib::FILE_TEST_EXISTS)) {
				existing++;
			}

			if (midi_source_by_path (possible_path)) {
				existing++;
			}
		}

		if (existing == 0) {
			break;
		}

		if (cnt > limit) {
			error << string_compose(
					_("There are already %1 recordings for %2, which I consider too many."),
					limit, base) << endmsg;
			destroy ();
			return 0;
		}
	}

	/* No need to "find best location" for software/app-based RAID, because
	   MIDI is so small that we always put it in the same place.
	*/

	return possible_path;
}


/** Create a new within-session audio source */
boost::shared_ptr<AudioFileSource>
Session::create_audio_source_for_session (size_t n_chans, string const & base, uint32_t chan, bool destructive)
{
	const string path = new_audio_source_path (base, n_chans, chan, destructive, true);

	if (!path.empty()) {
		return boost::dynamic_pointer_cast<AudioFileSource> (
			SourceFactory::createWritable (DataType::AUDIO, *this, path, destructive, frame_rate(), true, true));
	} else {
		throw failed_constructor ();
	}
}

/** Create a new within-session MIDI source */
boost::shared_ptr<MidiSource>
Session::create_midi_source_for_session (string const & basic_name)
{
	const string path = new_midi_source_path (basic_name);

	if (!path.empty()) {
		return boost::dynamic_pointer_cast<SMFSource> (
			SourceFactory::createWritable (
				DataType::MIDI, *this, path, false, frame_rate()));
	} else {
		throw failed_constructor ();
	}
}

/** Create a new within-session MIDI source */
boost::shared_ptr<MidiSource>
Session::create_midi_source_by_stealing_name (boost::shared_ptr<Track> track)
{
	/* the caller passes in the track the source will be used in,
	   so that we can keep the numbering sane.

	   Rationale: a track with the name "Foo" that has had N
	   captures carried out so far will ALREADY have a write source
	   named "Foo-N+1.mid" waiting to be used for the next capture.

	   If we call new_midi_source_name() we will get "Foo-N+2". But
	   there is no region corresponding to "Foo-N+1", so when
	   "Foo-N+2" appears in the track, the gap presents the user
	   with odd behaviour - why did it skip past Foo-N+1?

	   We could explain this to the user in some odd way, but
	   instead we rename "Foo-N+1.mid" as "Foo-N+2.mid", and then
	   use "Foo-N+1" here.

	   If that attempted rename fails, we get "Foo-N+2.mid" anyway.
	*/

	boost::shared_ptr<MidiTrack> mt = boost::dynamic_pointer_cast<MidiTrack> (track);
	assert (mt);
	std::string name = track->steal_write_source_name ();

	if (name.empty()) {
		return boost::shared_ptr<MidiSource>();
	}

	/* MIDI files are small, just put them in the first location of the
	   session source search path.
	*/

	const string path = Glib::build_filename (source_search_path (DataType::MIDI).front(), name);

	return boost::dynamic_pointer_cast<SMFSource> (
		SourceFactory::createWritable (
			DataType::MIDI, *this, path, false, frame_rate()));
}


void
Session::add_playlist (boost::shared_ptr<Playlist> playlist, bool unused)
{
	if (playlist->hidden()) {
		return;
	}

	playlists->add (playlist);

	if (unused) {
		playlist->release();
	}

	set_dirty();
}

void
Session::remove_playlist (boost::weak_ptr<Playlist> weak_playlist)
{
	if (_state_of_the_state & Deletion) {
		return;
	}

	boost::shared_ptr<Playlist> playlist (weak_playlist.lock());

	if (!playlist) {
		return;
	}

	playlists->remove (playlist);

	set_dirty();
}

void
Session::set_audition (boost::shared_ptr<Region> r)
{
	pending_audition_region = r;
	add_post_transport_work (PostTransportAudition);
	_butler->schedule_transport_work ();
}

void
Session::audition_playlist ()
{
	SessionEvent* ev = new SessionEvent (SessionEvent::Audition, SessionEvent::Add, SessionEvent::Immediate, 0, 0.0);
	ev->region.reset ();
	queue_event (ev);
}

void
Session::non_realtime_set_audition ()
{
	assert (pending_audition_region);
	auditioner->audition_region (pending_audition_region);
	pending_audition_region.reset ();
	AuditionActive (true); /* EMIT SIGNAL */
}

void
Session::audition_region (boost::shared_ptr<Region> r)
{
	SessionEvent* ev = new SessionEvent (SessionEvent::Audition, SessionEvent::Add, SessionEvent::Immediate, 0, 0.0);
	ev->region = r;
	queue_event (ev);
}

void
Session::cancel_audition ()
{
	if (!auditioner) {
		return;
	}
	if (auditioner->auditioning()) {
		auditioner->cancel_audition ();
		AuditionActive (false); /* EMIT SIGNAL */
	}
}

bool
Session::RoutePublicOrderSorter::operator() (boost::shared_ptr<Route> a, boost::shared_ptr<Route> b)
{
	if (a->is_monitor()) {
		return true;
	}
	if (b->is_monitor()) {
		return false;
	}
	return a->order_key () < b->order_key ();
}

bool
Session::is_auditioning () const
{
	/* can be called before we have an auditioner object */
	if (auditioner) {
		return auditioner->auditioning();
	} else {
		return false;
	}
}

void
Session::graph_reordered ()
{
	/* don't do this stuff if we are setting up connections
	   from a set_state() call or creating new tracks. Ditto for deletion.
	*/

	if ((_state_of_the_state & (InitialConnecting|Deletion)) || _adding_routes_in_progress || _reconnecting_routes_in_progress || _route_deletion_in_progress) {
		return;
	}

	/* every track/bus asked for this to be handled but it was deferred because
	   we were connecting. do it now.
	*/

	request_input_change_handling ();

	resort_routes ();

	/* force all diskstreams to update their capture offset values to
	   reflect any changes in latencies within the graph.
	*/

	boost::shared_ptr<RouteList> rl = routes.reader ();
	for (RouteList::iterator i = rl->begin(); i != rl->end(); ++i) {
		boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
		if (tr) {
			tr->set_capture_offset ();
		}
	}
}

/** @return Number of frames that there is disk space available to write,
 *  if known.
 */
boost::optional<framecnt_t>
Session::available_capture_duration ()
{
	Glib::Threads::Mutex::Lock lm (space_lock);

	if (_total_free_4k_blocks_uncertain) {
		return boost::optional<framecnt_t> ();
	}

	float sample_bytes_on_disk = 4.0; // keep gcc happy

	switch (config.get_native_file_data_format()) {
	case FormatFloat:
		sample_bytes_on_disk = 4.0;
		break;

	case FormatInt24:
		sample_bytes_on_disk = 3.0;
		break;

	case FormatInt16:
		sample_bytes_on_disk = 2.0;
		break;

	default:
		/* impossible, but keep some gcc versions happy */
		fatal << string_compose (_("programming error: %1"),
					 X_("illegal native file data format"))
		      << endmsg;
		abort(); /*NOTREACHED*/
	}

	double scale = 4096.0 / sample_bytes_on_disk;

	if (_total_free_4k_blocks * scale > (double) max_framecnt) {
		return max_framecnt;
	}

	return (framecnt_t) floor (_total_free_4k_blocks * scale);
}

void
Session::add_bundle (boost::shared_ptr<Bundle> bundle, bool emit_signal)
{
	{
		RCUWriter<BundleList> writer (_bundles);
		boost::shared_ptr<BundleList> b = writer.get_copy ();
		b->push_back (bundle);
	}

	if (emit_signal) {
		BundleAddedOrRemoved (); /* EMIT SIGNAL */
	}

	set_dirty();
}

void
Session::remove_bundle (boost::shared_ptr<Bundle> bundle)
{
	bool removed = false;

	{
		RCUWriter<BundleList> writer (_bundles);
		boost::shared_ptr<BundleList> b = writer.get_copy ();
		BundleList::iterator i = find (b->begin(), b->end(), bundle);

		if (i != b->end()) {
			b->erase (i);
			removed = true;
		}
	}

	if (removed) {
		 BundleAddedOrRemoved (); /* EMIT SIGNAL */
	}

	set_dirty();
}

boost::shared_ptr<Bundle>
Session::bundle_by_name (string name) const
{
	boost::shared_ptr<BundleList> b = _bundles.reader ();

	for (BundleList::const_iterator i = b->begin(); i != b->end(); ++i) {
		if ((*i)->name() == name) {
			return* i;
		}
	}

	return boost::shared_ptr<Bundle> ();
}

void
Session::tempo_map_changed (const PropertyChange&)
{
	clear_clicks ();

	playlists->update_after_tempo_map_change ();

	_locations->apply (*this, &Session::update_locations_after_tempo_map_change);

	set_dirty ();
}

void
Session::update_locations_after_tempo_map_change (const Locations::LocationList& loc)
{
	for (Locations::LocationList::const_iterator i = loc.begin(); i != loc.end(); ++i) {
		(*i)->recompute_frames_from_bbt ();
	}
}

/** Ensures that all buffers (scratch, send, silent, etc) are allocated for
 * the given count with the current block size.
 */
void
Session::ensure_buffers (ChanCount howmany)
{
	BufferManager::ensure_buffers (howmany, bounce_processing() ? bounce_chunk_size : 0);
}

void
Session::ensure_buffer_set(BufferSet& buffers, const ChanCount& count)
{
	for (DataType::iterator t = DataType::begin(); t != DataType::end(); ++t) {
		buffers.ensure_buffers(*t, count.get(*t), _engine.raw_buffer_size(*t));
	}
}

uint32_t
Session::next_insert_id ()
{
	/* this doesn't really loop forever. just think about it */

	while (true) {
		for (boost::dynamic_bitset<uint32_t>::size_type n = 0; n < insert_bitset.size(); ++n) {
			if (!insert_bitset[n]) {
				insert_bitset[n] = true;
				return n;

			}
		}

		/* none available, so resize and try again */

		insert_bitset.resize (insert_bitset.size() + 16, false);
	}
}

uint32_t
Session::next_send_id ()
{
	/* this doesn't really loop forever. just think about it */

	while (true) {
		for (boost::dynamic_bitset<uint32_t>::size_type n = 0; n < send_bitset.size(); ++n) {
			if (!send_bitset[n]) {
				send_bitset[n] = true;
				return n;

			}
		}

		/* none available, so resize and try again */

		send_bitset.resize (send_bitset.size() + 16, false);
	}
}

uint32_t
Session::next_aux_send_id ()
{
	/* this doesn't really loop forever. just think about it */

	while (true) {
		for (boost::dynamic_bitset<uint32_t>::size_type n = 0; n < aux_send_bitset.size(); ++n) {
			if (!aux_send_bitset[n]) {
				aux_send_bitset[n] = true;
				return n;

			}
		}

		/* none available, so resize and try again */

		aux_send_bitset.resize (aux_send_bitset.size() + 16, false);
	}
}

uint32_t
Session::next_return_id ()
{
	/* this doesn't really loop forever. just think about it */

	while (true) {
		for (boost::dynamic_bitset<uint32_t>::size_type n = 0; n < return_bitset.size(); ++n) {
			if (!return_bitset[n]) {
				return_bitset[n] = true;
				return n;

			}
		}

		/* none available, so resize and try again */

		return_bitset.resize (return_bitset.size() + 16, false);
	}
}

void
Session::mark_send_id (uint32_t id)
{
	if (id >= send_bitset.size()) {
		send_bitset.resize (id+16, false);
	}
	if (send_bitset[id]) {
		warning << string_compose (_("send ID %1 appears to be in use already"), id) << endmsg;
	}
	send_bitset[id] = true;
}

void
Session::mark_aux_send_id (uint32_t id)
{
	if (id >= aux_send_bitset.size()) {
		aux_send_bitset.resize (id+16, false);
	}
	if (aux_send_bitset[id]) {
		warning << string_compose (_("aux send ID %1 appears to be in use already"), id) << endmsg;
	}
	aux_send_bitset[id] = true;
}

void
Session::mark_return_id (uint32_t id)
{
	if (id >= return_bitset.size()) {
		return_bitset.resize (id+16, false);
	}
	if (return_bitset[id]) {
		warning << string_compose (_("return ID %1 appears to be in use already"), id) << endmsg;
	}
	return_bitset[id] = true;
}

void
Session::mark_insert_id (uint32_t id)
{
	if (id >= insert_bitset.size()) {
		insert_bitset.resize (id+16, false);
	}
	if (insert_bitset[id]) {
		warning << string_compose (_("insert ID %1 appears to be in use already"), id) << endmsg;
	}
	insert_bitset[id] = true;
}

void
Session::unmark_send_id (uint32_t id)
{
	if (id < send_bitset.size()) {
		send_bitset[id] = false;
	}
}

void
Session::unmark_aux_send_id (uint32_t id)
{
	if (id < aux_send_bitset.size()) {
		aux_send_bitset[id] = false;
	}
}

void
Session::unmark_return_id (uint32_t id)
{
	if (id < return_bitset.size()) {
		return_bitset[id] = false;
	}
}

void
Session::unmark_insert_id (uint32_t id)
{
	if (id < insert_bitset.size()) {
		insert_bitset[id] = false;
	}
}

void
Session::reset_native_file_format ()
{
	boost::shared_ptr<RouteList> rl = routes.reader ();

	for (RouteList::iterator i = rl->begin(); i != rl->end(); ++i) {
		boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
		if (tr) {
			/* don't save state as we do this, there's no point
			 */
			_state_of_the_state = StateOfTheState (_state_of_the_state|InCleanup);
			tr->reset_write_sources (false);
			_state_of_the_state = StateOfTheState (_state_of_the_state & ~InCleanup);
		}
	}
}

bool
Session::route_name_unique (string n) const
{
	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::const_iterator i = r->begin(); i != r->end(); ++i) {
		if ((*i)->name() == n) {
			return false;
		}
	}

	return true;
}

bool
Session::route_name_internal (string n) const
{
	if (auditioner && auditioner->name() == n) {
		return true;
	}

	if (_click_io && _click_io->name() == n) {
		return true;
	}

	return false;
}

int
Session::freeze_all (InterThreadInfo& itt)
{
	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {

		boost::shared_ptr<Track> t;

		if ((t = boost::dynamic_pointer_cast<Track>(*i)) != 0) {
			/* XXX this is wrong because itt.progress will keep returning to zero at the start
			   of every track.
			*/
			t->freeze_me (itt);
		}
	}

	return 0;
}

boost::shared_ptr<Region>
Session::write_one_track (Track& track, framepos_t start, framepos_t end,
			  bool /*overwrite*/, vector<boost::shared_ptr<Source> >& srcs,
			  InterThreadInfo& itt,
			  boost::shared_ptr<Processor> endpoint, bool include_endpoint,
			  bool for_export, bool for_freeze)
{
	boost::shared_ptr<Region> result;
	boost::shared_ptr<Playlist> playlist;
	boost::shared_ptr<Source> source;
	ChanCount diskstream_channels (track.n_channels());
	framepos_t position;
	framecnt_t this_chunk;
	framepos_t to_do;
	framepos_t latency_skip;
	BufferSet buffers;
	framepos_t len = end - start;
	bool need_block_size_reset = false;
	ChanCount const max_proc = track.max_processor_streams ();
	string legal_playlist_name;
	string possible_path;

	if (end <= start) {
		error << string_compose (_("Cannot write a range where end <= start (e.g. %1 <= %2)"),
					 end, start) << endmsg;
		return result;
	}

	diskstream_channels = track.bounce_get_output_streams (diskstream_channels, endpoint,
			include_endpoint, for_export, for_freeze);

	if (diskstream_channels.n(track.data_type()) < 1) {
		error << _("Cannot write a range with no data.") << endmsg;
		return result;
	}

	// block all process callback handling

	block_processing ();

	{
		// synchronize with AudioEngine::process_callback()
		// make sure processing is not currently running
		// and processing_blocked() is honored before
		// acquiring thread buffers
		Glib::Threads::Mutex::Lock lm (_engine.process_lock());
	}

	_bounce_processing_active = true;

	/* call tree *MUST* hold route_lock */

	if ((playlist = track.playlist()) == 0) {
		goto out;
	}

	legal_playlist_name = legalize_for_path (playlist->name());

	for (uint32_t chan_n = 0; chan_n < diskstream_channels.n(track.data_type()); ++chan_n) {

		string base_name = string_compose ("%1-%2-bounce", playlist->name(), chan_n);
		string path = ((track.data_type() == DataType::AUDIO)
		               ? new_audio_source_path (legal_playlist_name, diskstream_channels.n_audio(), chan_n, false, true)
		               : new_midi_source_path (legal_playlist_name));

		if (path.empty()) {
			goto out;
		}

		try {
			source = SourceFactory::createWritable (track.data_type(), *this, path, false, frame_rate());
		}

		catch (failed_constructor& err) {
			error << string_compose (_("cannot create new file \"%1\" for %2"), path, track.name()) << endmsg;
			goto out;
		}

		srcs.push_back (source);
	}

	/* tell redirects that care that we are about to use a much larger
	 * blocksize. this will flush all plugins too, so that they are ready
	 * to be used for this process.
	 */

	need_block_size_reset = true;
	track.set_block_size (bounce_chunk_size);
	_engine.main_thread()->get_buffers ();

	position = start;
	to_do = len;
	latency_skip = track.bounce_get_latency (endpoint, include_endpoint, for_export, for_freeze);

	/* create a set of reasonably-sized buffers */
	for (DataType::iterator t = DataType::begin(); t != DataType::end(); ++t) {
		buffers.ensure_buffers(*t, max_proc.get(*t), bounce_chunk_size);
	}
	buffers.set_count (max_proc);

	for (vector<boost::shared_ptr<Source> >::iterator src = srcs.begin(); src != srcs.end(); ++src) {
		boost::shared_ptr<AudioFileSource> afs = boost::dynamic_pointer_cast<AudioFileSource>(*src);
		boost::shared_ptr<MidiSource> ms;
		if (afs) {
			afs->prepare_for_peakfile_writes ();
		} else if ((ms = boost::dynamic_pointer_cast<MidiSource>(*src))) {
			Source::Lock lock(ms->mutex());
			ms->mark_streaming_write_started(lock);
		}
	}

	while (to_do && !itt.cancel) {

		this_chunk = min (to_do, bounce_chunk_size);

		if (track.export_stuff (buffers, start, this_chunk, endpoint, include_endpoint, for_export, for_freeze)) {
			goto out;
		}

		start += this_chunk;
		to_do -= this_chunk;
		itt.progress = (float) (1.0 - ((double) to_do / len));

		if (latency_skip >= bounce_chunk_size) {
			latency_skip -= bounce_chunk_size;
			continue;
		}

		const framecnt_t current_chunk = this_chunk - latency_skip;

		uint32_t n = 0;
		for (vector<boost::shared_ptr<Source> >::iterator src=srcs.begin(); src != srcs.end(); ++src, ++n) {
			boost::shared_ptr<AudioFileSource> afs = boost::dynamic_pointer_cast<AudioFileSource>(*src);
			boost::shared_ptr<MidiSource> ms;

			if (afs) {
				if (afs->write (buffers.get_audio(n).data(latency_skip), current_chunk) != current_chunk) {
					goto out;
				}
			} else if ((ms = boost::dynamic_pointer_cast<MidiSource>(*src))) {
				Source::Lock lock(ms->mutex());

				const MidiBuffer& buf = buffers.get_midi(0);
				for (MidiBuffer::const_iterator i = buf.begin(); i != buf.end(); ++i) {
					Evoral::Event<framepos_t> ev = *i;
					ev.set_time(ev.time() - position);
					ms->append_event_frames(lock, ev, ms->timeline_position());
				}
			}
		}
		latency_skip = 0;
	}

	/* post-roll, pick up delayed processor output */
	latency_skip = track.bounce_get_latency (endpoint, include_endpoint, for_export, for_freeze);

	while (latency_skip && !itt.cancel) {
		this_chunk = min (latency_skip, bounce_chunk_size);
		latency_skip -= this_chunk;

		buffers.silence (this_chunk, 0);
		track.bounce_process (buffers, start, this_chunk, endpoint, include_endpoint, for_export, for_freeze);

		uint32_t n = 0;
		for (vector<boost::shared_ptr<Source> >::iterator src=srcs.begin(); src != srcs.end(); ++src, ++n) {
			boost::shared_ptr<AudioFileSource> afs = boost::dynamic_pointer_cast<AudioFileSource>(*src);

			if (afs) {
				if (afs->write (buffers.get_audio(n).data(), this_chunk) != this_chunk) {
					goto out;
				}
			}
		}
	}

	if (!itt.cancel) {

		time_t now;
		struct tm* xnow;
		time (&now);
		xnow = localtime (&now);

		for (vector<boost::shared_ptr<Source> >::iterator src=srcs.begin(); src != srcs.end(); ++src) {
			boost::shared_ptr<AudioFileSource> afs = boost::dynamic_pointer_cast<AudioFileSource>(*src);
			boost::shared_ptr<MidiSource> ms;

			if (afs) {
				afs->update_header (position, *xnow, now);
				afs->flush_header ();
			} else if ((ms = boost::dynamic_pointer_cast<MidiSource>(*src))) {
				Source::Lock lock(ms->mutex());
				ms->mark_streaming_write_completed(lock);
			}
		}

		/* construct a region to represent the bounced material */

		PropertyList plist;

		plist.add (Properties::start, 0);
		plist.add (Properties::length, srcs.front()->length(srcs.front()->timeline_position()));
		plist.add (Properties::name, region_name_from_path (srcs.front()->name(), true));

		result = RegionFactory::create (srcs, plist);

	}

  out:
	if (!result) {
		for (vector<boost::shared_ptr<Source> >::iterator src = srcs.begin(); src != srcs.end(); ++src) {
			(*src)->mark_for_remove ();
			(*src)->drop_references ();
		}

	} else {
		for (vector<boost::shared_ptr<Source> >::iterator src = srcs.begin(); src != srcs.end(); ++src) {
			boost::shared_ptr<AudioFileSource> afs = boost::dynamic_pointer_cast<AudioFileSource>(*src);

			if (afs)
				afs->done_with_peakfile_writes ();
		}
	}

	_bounce_processing_active = false;

	if (need_block_size_reset) {
		_engine.main_thread()->drop_buffers ();
		track.set_block_size (get_block_size());
	}

	unblock_processing ();

	return result;
}

gain_t*
Session::gain_automation_buffer() const
{
	return ProcessThread::gain_automation_buffer ();
}

gain_t*
Session::trim_automation_buffer() const
{
	return ProcessThread::trim_automation_buffer ();
}

gain_t*
Session::send_gain_automation_buffer() const
{
	return ProcessThread::send_gain_automation_buffer ();
}

pan_t**
Session::pan_automation_buffer() const
{
	return ProcessThread::pan_automation_buffer ();
}

BufferSet&
Session::get_silent_buffers (ChanCount count)
{
	return ProcessThread::get_silent_buffers (count);
}

BufferSet&
Session::get_scratch_buffers (ChanCount count, bool silence)
{
	return ProcessThread::get_scratch_buffers (count, silence);
}

BufferSet&
Session::get_route_buffers (ChanCount count, bool silence)
{
	return ProcessThread::get_route_buffers (count, silence);
}


BufferSet&
Session::get_mix_buffers (ChanCount count)
{
	return ProcessThread::get_mix_buffers (count);
}

uint32_t
Session::ntracks () const
{
	uint32_t n = 0;
	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::const_iterator i = r->begin(); i != r->end(); ++i) {
		if (boost::dynamic_pointer_cast<Track> (*i)) {
			++n;
		}
	}

	return n;
}

uint32_t
Session::nbusses () const
{
	uint32_t n = 0;
	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::const_iterator i = r->begin(); i != r->end(); ++i) {
		if (boost::dynamic_pointer_cast<Track>(*i) == 0) {
			++n;
		}
	}

	return n;
}

void
Session::add_automation_list(AutomationList *al)
{
	automation_lists[al->id()] = al;
}

/** @return true if there is at least one record-enabled track, otherwise false */
bool
Session::have_rec_enabled_track () const
{
	return g_atomic_int_get (const_cast<gint*>(&_have_rec_enabled_track)) == 1;
}

bool
Session::have_rec_disabled_track () const
{
    return g_atomic_int_get (const_cast<gint*>(&_have_rec_disabled_track)) == 1;
}

/** Update the state of our rec-enabled tracks flag */
void
Session::update_route_record_state ()
{
	boost::shared_ptr<RouteList> rl = routes.reader ();
	RouteList::iterator i = rl->begin();
	while (i != rl->end ()) {

		boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
		if (tr && tr->record_enabled ()) {
			break;
		}

		++i;
	}

	int const old = g_atomic_int_get (&_have_rec_enabled_track);

	g_atomic_int_set (&_have_rec_enabled_track, i != rl->end () ? 1 : 0);

	if (g_atomic_int_get (&_have_rec_enabled_track) != old) {
		RecordStateChanged (); /* EMIT SIGNAL */
	}

	for (i = rl->begin(); i != rl->end (); ++i) {
		boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
		if (tr && !tr->record_enabled ()) {
			break;
		}
	}

	g_atomic_int_set (&_have_rec_disabled_track, i != rl->end () ? 1 : 0);

	bool record_arm_state_changed = (old != g_atomic_int_get (&_have_rec_enabled_track) );

	if (record_status() == Recording && record_arm_state_changed ) {
		RecordArmStateChanged ();
	}

}

void
Session::listen_position_changed ()
{
	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		(*i)->listen_position_changed ();
	}
}

void
Session::solo_control_mode_changed ()
{
	/* cancel all solo or all listen when solo control mode changes */

	if (soloing()) {
		set_solo (get_routes(), false);
	} else if (listening()) {
		set_listen (get_routes(), false);
	}
}

/** Called when a property of one of our route groups changes */
void
Session::route_group_property_changed (RouteGroup* rg)
{
	RouteGroupPropertyChanged (rg); /* EMIT SIGNAL */
}

/** Called when a route is added to one of our route groups */
void
Session::route_added_to_route_group (RouteGroup* rg, boost::weak_ptr<Route> r)
{
	RouteAddedToRouteGroup (rg, r);
}

/** Called when a route is removed from one of our route groups */
void
Session::route_removed_from_route_group (RouteGroup* rg, boost::weak_ptr<Route> r)
{
	update_route_record_state ();
	RouteRemovedFromRouteGroup (rg, r); /* EMIT SIGNAL */
}

boost::shared_ptr<RouteList>
Session::get_tracks () const
{
	boost::shared_ptr<RouteList> rl = routes.reader ();
	boost::shared_ptr<RouteList> tl (new RouteList);

	for (RouteList::const_iterator r = rl->begin(); r != rl->end(); ++r) {
		if (boost::dynamic_pointer_cast<Track> (*r)) {
			if (!(*r)->is_auditioner()) {
				tl->push_back (*r);
			}
		}
	}
	return tl;
}

boost::shared_ptr<RouteList>
Session::get_routes_with_regions_at (framepos_t const p) const
{
	boost::shared_ptr<RouteList> r = routes.reader ();
	boost::shared_ptr<RouteList> rl (new RouteList);

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
		if (!tr) {
			continue;
		}

		boost::shared_ptr<Playlist> pl = tr->playlist ();
		if (!pl) {
			continue;
		}

		if (pl->has_region_at (p)) {
			rl->push_back (*i);
		}
	}

	return rl;
}

void
Session::goto_end ()
{
	if (_session_range_location) {
		request_locate (_session_range_location->end(), false);
	} else {
		request_locate (0, false);
	}
}

void
Session::goto_start ()
{
	if (_session_range_location) {
		request_locate (_session_range_location->start(), false);
	} else {
		request_locate (0, false);
	}
}

framepos_t
Session::current_start_frame () const
{
	return _session_range_location ? _session_range_location->start() : 0;
}

framepos_t
Session::current_end_frame () const
{
	return _session_range_location ? _session_range_location->end() : 0;
}

void
Session::set_session_range_location (framepos_t start, framepos_t end)
{
	_session_range_location = new Location (*this, start, end, _("session"), Location::IsSessionRange);
	_locations->add (_session_range_location);
}

void
Session::step_edit_status_change (bool yn)
{
	bool send = false;

	bool val = false;
	if (yn) {
		send = (_step_editors == 0);
		val = true;

		_step_editors++;
	} else {
		send = (_step_editors == 1);
		val = false;

		if (_step_editors > 0) {
			_step_editors--;
		}
	}

	if (send) {
		StepEditStatusChange (val);
	}
}


void
Session::start_time_changed (framepos_t old)
{
	/* Update the auto loop range to match the session range
	   (unless the auto loop range has been changed by the user)
	*/

	Location* s = _locations->session_range_location ();
	if (s == 0) {
		return;
	}

	Location* l = _locations->auto_loop_location ();

	if (l && l->start() == old) {
		l->set_start (s->start(), true);
	}
}

void
Session::end_time_changed (framepos_t old)
{
	/* Update the auto loop range to match the session range
	   (unless the auto loop range has been changed by the user)
	*/

	Location* s = _locations->session_range_location ();
	if (s == 0) {
		return;
	}

	Location* l = _locations->auto_loop_location ();

	if (l && l->end() == old) {
		l->set_end (s->end(), true);
	}
}

std::vector<std::string>
Session::source_search_path (DataType type) const
{
	Searchpath sp;

	if (session_dirs.size() == 1) {
		switch (type) {
		case DataType::AUDIO:
			sp.push_back (_session_dir->sound_path());
			break;
		case DataType::MIDI:
			sp.push_back (_session_dir->midi_path());
			break;
		}
	} else {
		for (vector<space_and_path>::const_iterator i = session_dirs.begin(); i != session_dirs.end(); ++i) {
			SessionDirectory sdir (i->path);
			switch (type) {
			case DataType::AUDIO:
				sp.push_back (sdir.sound_path());
				break;
			case DataType::MIDI:
				sp.push_back (sdir.midi_path());
				break;
			}
		}
	}

	if (type == DataType::AUDIO) {
		const string sound_path_2X = _session_dir->sound_path_2X();
		if (Glib::file_test (sound_path_2X, Glib::FILE_TEST_EXISTS|Glib::FILE_TEST_IS_DIR)) {
			if (find (sp.begin(), sp.end(), sound_path_2X) == sp.end()) {
				sp.push_back (sound_path_2X);
			}
		}
	}

	// now check the explicit (possibly user-specified) search path

	switch (type) {
	case DataType::AUDIO:
		sp += Searchpath(config.get_audio_search_path ());
		break;
	case DataType::MIDI:
		sp += Searchpath(config.get_midi_search_path ());
		break;
	}

	return sp;
}

void
Session::ensure_search_path_includes (const string& path, DataType type)
{
	Searchpath sp;

	if (path == ".") {
		return;
	}

	switch (type) {
	case DataType::AUDIO:
		sp += Searchpath(config.get_audio_search_path ());
		break;
	case DataType::MIDI:
		sp += Searchpath (config.get_midi_search_path ());
		break;
	}

	for (vector<std::string>::iterator i = sp.begin(); i != sp.end(); ++i) {
		/* No need to add this new directory if it has the same inode as
		   an existing one; checking inode rather than name prevents duplicated
		   directories when we are using symlinks.

		   On Windows, I think we could just do if (*i == path) here.
		*/
		if (PBD::equivalent_paths (*i, path)) {
			return;
		}
	}

	sp += path;

	switch (type) {
	case DataType::AUDIO:
		config.set_audio_search_path (sp.to_string());
		break;
	case DataType::MIDI:
		config.set_midi_search_path (sp.to_string());
		break;
	}
}

void
Session::remove_dir_from_search_path (const string& dir, DataType type)
{
	Searchpath sp;

	switch (type) {
	case DataType::AUDIO:
		sp = Searchpath(config.get_audio_search_path ());
		break;
	case DataType::MIDI:
		sp = Searchpath (config.get_midi_search_path ());
		break;
	}

	sp -= dir;

	switch (type) {
	case DataType::AUDIO:
		config.set_audio_search_path (sp.to_string());
		break;
	case DataType::MIDI:
		config.set_midi_search_path (sp.to_string());
		break;
	}

}

boost::shared_ptr<Speakers>
Session::get_speakers()
{
	return _speakers;
}

list<string>
Session::unknown_processors () const
{
	list<string> p;

	boost::shared_ptr<RouteList> r = routes.reader ();
	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		list<string> t = (*i)->unknown_processors ();
		copy (t.begin(), t.end(), back_inserter (p));
	}

	p.sort ();
	p.unique ();

	return p;
}

void
Session::update_latency (bool playback)
{
	DEBUG_TRACE (DEBUG::Latency, string_compose ("JACK latency callback: %1\n", (playback ? "PLAYBACK" : "CAPTURE")));

	if ((_state_of_the_state & (InitialConnecting|Deletion)) || _adding_routes_in_progress) {
		return;
	}

	boost::shared_ptr<RouteList> r = routes.reader ();
	framecnt_t max_latency = 0;

	if (playback) {
		/* reverse the list so that we work backwards from the last route to run to the first */
                RouteList* rl = routes.reader().get();
                r.reset (new RouteList (*rl));
		reverse (r->begin(), r->end());
	}

	/* compute actual latency values for the given direction and store them all in per-port
	   structures. this will also publish the same values (to JACK) so that computation of latency
	   for routes can consistently use public latency values.
	*/

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		max_latency = max (max_latency, (*i)->set_private_port_latencies (playback));
	}

        /* because we latency compensate playback, our published playback latencies should
           be the same for all output ports - all material played back by ardour has
           the same latency, whether its caused by plugins or by latency compensation. since
           these may differ from the values computed above, reset all playback port latencies
           to the same value.
        */

        DEBUG_TRACE (DEBUG::Latency, string_compose ("Set public port latencies to %1\n", max_latency));

        for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
                (*i)->set_public_port_latencies (max_latency, playback);
        }

	if (playback) {

		post_playback_latency ();

	} else {

		post_capture_latency ();
	}

	DEBUG_TRACE (DEBUG::Latency, "JACK latency callback: DONE\n");
}

void
Session::post_playback_latency ()
{
	set_worst_playback_latency ();

	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		if (!(*i)->is_auditioner() && ((*i)->active())) {
			_worst_track_latency = max (_worst_track_latency, (*i)->update_signal_latency ());
		}
	}

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		(*i)->set_latency_compensation (_worst_track_latency);
	}
}

void
Session::post_capture_latency ()
{
	set_worst_capture_latency ();

	/* reflect any changes in capture latencies into capture offsets
	 */

	boost::shared_ptr<RouteList> rl = routes.reader();
	for (RouteList::iterator i = rl->begin(); i != rl->end(); ++i) {
		boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
		if (tr) {
			tr->set_capture_offset ();
		}
	}
}

void
Session::initialize_latencies ()
{
        {
                Glib::Threads::Mutex::Lock lm (_engine.process_lock());
                update_latency (false);
                update_latency (true);
        }

        set_worst_io_latencies ();
}

void
Session::set_worst_io_latencies ()
{
	set_worst_playback_latency ();
	set_worst_capture_latency ();
}

void
Session::set_worst_playback_latency ()
{
	if (_state_of_the_state & (InitialConnecting|Deletion)) {
		return;
	}

	_worst_output_latency = 0;

	if (!_engine.connected()) {
		return;
	}

	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		_worst_output_latency = max (_worst_output_latency, (*i)->output()->latency());
	}

	DEBUG_TRACE (DEBUG::Latency, string_compose ("Worst output latency: %1\n", _worst_output_latency));
}

void
Session::set_worst_capture_latency ()
{
	if (_state_of_the_state & (InitialConnecting|Deletion)) {
		return;
	}

	_worst_input_latency = 0;

	if (!_engine.connected()) {
		return;
	}

	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		_worst_input_latency = max (_worst_input_latency, (*i)->input()->latency());
	}

        DEBUG_TRACE (DEBUG::Latency, string_compose ("Worst input latency: %1\n", _worst_input_latency));
}

void
Session::update_latency_compensation (bool force_whole_graph)
{
	bool some_track_latency_changed = false;

	if (_state_of_the_state & (InitialConnecting|Deletion)) {
		return;
	}

	DEBUG_TRACE(DEBUG::Latency, "---------------------------- update latency compensation\n\n");

	_worst_track_latency = 0;

	boost::shared_ptr<RouteList> r = routes.reader ();

	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		if (!(*i)->is_auditioner() && ((*i)->active())) {
			framecnt_t tl;
			if ((*i)->signal_latency () != (tl = (*i)->update_signal_latency ())) {
				some_track_latency_changed = true;
			}
			_worst_track_latency = max (tl, _worst_track_latency);
		}
	}

	DEBUG_TRACE (DEBUG::Latency, string_compose ("worst signal processing latency: %1 (changed ? %2)\n", _worst_track_latency,
	                                             (some_track_latency_changed ? "yes" : "no")));

	DEBUG_TRACE(DEBUG::Latency, "---------------------------- DONE update latency compensation\n\n");

	if (some_track_latency_changed || force_whole_graph)  {
		_engine.update_latencies ();
	}


	for (RouteList::iterator i = r->begin(); i != r->end(); ++i) {
		boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
		if (!tr) {
			continue;
		}
		tr->set_capture_offset ();
	}
}

char
Session::session_name_is_legal (const string& path)
{
	char illegal_chars[] = { '/', '\\', ':', ';', '\0' };

	for (int i = 0; illegal_chars[i]; ++i) {
		if (path.find (illegal_chars[i]) != string::npos) {
			return illegal_chars[i];
		}
	}

	return 0;
}

uint32_t
Session::next_control_id () const
{
	int subtract = 0;

	/* the monitor bus remote ID is in a different
	 * "namespace" than regular routes. its existence doesn't
	 * affect normal (low) numbered routes.
	 */

	if (_monitor_out) {
		subtract++;
	}

	/* the same about masterbus in Waves Tracks */

	if (Profile->get_trx() && _master_out) {
		subtract++;
	}

	return nroutes() - subtract;
}

void
Session::notify_remote_id_change ()
{
	if (deletion_in_progress()) {
		return;
	}

	switch (Config->get_remote_model()) {
	case MixerOrdered:
		Route::RemoteControlIDChange (); /* EMIT SIGNAL */
		break;
	default:
		break;
	}

#ifdef USE_TRACKS_CODE_FEATURES
		/* Waves Tracks: for Waves Tracks session it's required to reconnect their IOs
		 * if track order has been changed by user
		 */
		reconnect_existing_routes(true, true);
#endif

}

void
Session::sync_order_keys ()
{
	if (deletion_in_progress()) {
		return;
	}

	/* tell everyone that something has happened to the sort keys
	   and let them sync up with the change(s)
	   this will give objects that manage the sort order keys the
	   opportunity to keep them in sync if they wish to.
	*/

	DEBUG_TRACE (DEBUG::OrderKeys, "Sync Order Keys.\n");

	reassign_track_numbers();

	Route::SyncOrderKeys (); /* EMIT SIGNAL */

	DEBUG_TRACE (DEBUG::OrderKeys, "\tsync done\n");
}

bool
Session::operation_in_progress (GQuark op) const
{
	return (find (_current_trans_quarks.begin(), _current_trans_quarks.end(), op) != _current_trans_quarks.end());
}

boost::shared_ptr<Port>
Session::ltc_input_port () const
{
	return _ltc_input->nth (0);
}

boost::shared_ptr<Port>
Session::ltc_output_port () const
{
	return _ltc_output->nth (0);
}

void
Session::reconnect_ltc_input ()
{
	if (_ltc_input) {

		string src = Config->get_ltc_source_port();

		_ltc_input->disconnect (this);

		if (src != _("None") && !src.empty())  {
			_ltc_input->nth (0)->connect (src);
		}

		if ( ARDOUR::Profile->get_trx () ) {
			// Tracks need this signal to update timecode_source_dropdown
			MtcOrLtcInputPortChanged (); //emit signal
		}
	}
}

void
Session::reconnect_ltc_output ()
{
	if (_ltc_output) {

		string src = Config->get_ltc_output_port();

		_ltc_output->disconnect (this);

		if (src != _("None") && !src.empty())  {
			_ltc_output->nth (0)->connect (src);
		}
	}
}

void
Session::set_range_selection (framepos_t start, framepos_t end)
{
	_range_selection = Evoral::Range<framepos_t> (start, end);
#ifdef USE_TRACKS_CODE_FEATURES
	follow_playhead_priority ();
#endif
}

void
Session::set_object_selection (framepos_t start, framepos_t end)
{
	_object_selection = Evoral::Range<framepos_t> (start, end);
#ifdef USE_TRACKS_CODE_FEATURES
	follow_playhead_priority ();
#endif
}

void
Session::clear_range_selection ()
{
	_range_selection = Evoral::Range<framepos_t> (-1,-1);
#ifdef USE_TRACKS_CODE_FEATURES
	follow_playhead_priority ();
#endif
}

void
Session::clear_object_selection ()
{
	_object_selection = Evoral::Range<framepos_t> (-1,-1);
#ifdef USE_TRACKS_CODE_FEATURES
	follow_playhead_priority ();
#endif
}
