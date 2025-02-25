/*
  Copyright (C) 1999-2013 Paul Davis

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


#ifdef WAF_BUILD
#include "libardour-config.h"
#endif

#include <stdint.h>

#include <algorithm>
#include <string>
#include <cerrno>
#include <cstdio> /* snprintf(3) ... grrr */
#include <cmath>
#include <unistd.h>
#include <climits>
#include <signal.h>
#include <sys/time.h>

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#ifdef __APPLE__
#include <sys/param.h>
#include <sys/mount.h>
#endif

#ifdef HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif

#include <glib.h>
#include "pbd/gstdio_compat.h"

#include <glibmm.h>
#include <glibmm/threads.h>
#include <glibmm/fileutils.h>

#include <boost/algorithm/string.hpp>

#include "midi++/mmc.h"
#include "midi++/port.h"

#include "evoral/SMF.hpp"

#include "pbd/boost_debug.h"
#include "pbd/basename.h"
#include "pbd/controllable_descriptor.h"
#include "pbd/debug.h"
#include "pbd/enumwriter.h"
#include "pbd/error.h"
#include "pbd/file_utils.h"
#include "pbd/pathexpand.h"
#include "pbd/pthread_utils.h"
#include "pbd/stacktrace.h"
#include "pbd/convert.h"
#include "pbd/localtime_r.h"

#include "ardour/amp.h"
#include "ardour/async_midi_port.h"
#include "ardour/audio_diskstream.h"
#include "ardour/audio_track.h"
#include "ardour/audioengine.h"
#include "ardour/audiofilesource.h"
#include "ardour/audioregion.h"
#include "ardour/automation_control.h"
#include "ardour/butler.h"
#include "ardour/control_protocol_manager.h"
#include "ardour/directory_names.h"
#include "ardour/filename_extensions.h"
#include "ardour/graph.h"
#include "ardour/location.h"
#include "ardour/midi_model.h"
#include "ardour/midi_patch_manager.h"
#include "ardour/midi_region.h"
#include "ardour/midi_scene_changer.h"
#include "ardour/midi_source.h"
#include "ardour/midi_track.h"
#include "ardour/pannable.h"
#include "ardour/playlist_factory.h"
#include "ardour/playlist_source.h"
#include "ardour/port.h"
#include "ardour/processor.h"
#include "ardour/profile.h"
#include "ardour/proxy_controllable.h"
#include "ardour/recent_sessions.h"
#include "ardour/region_factory.h"
#include "ardour/route_group.h"
#include "ardour/send.h"
#include "ardour/session.h"
#include "ardour/session_directory.h"
#include "ardour/session_metadata.h"
#include "ardour/session_playlists.h"
#include "ardour/session_state_utils.h"
#include "ardour/silentfilesource.h"
#include "ardour/sndfilesource.h"
#include "ardour/source_factory.h"
#include "ardour/speakers.h"
#include "ardour/template_utils.h"
#include "ardour/tempo.h"
#include "ardour/ticker.h"
#include "ardour/user_bundle.h"

#include "control_protocol/control_protocol.h"

#include "i18n.h"
#include <locale.h>

using namespace std;
using namespace ARDOUR;
using namespace PBD;

#define DEBUG_UNDO_HISTORY(msg) DEBUG_TRACE (PBD::DEBUG::UndoHistory, string_compose ("%1: %2\n", __LINE__, msg));

void
Session::pre_engine_init (string fullpath)
{
	if (fullpath.empty()) {
		destroy ();
		throw failed_constructor();
	}

	/* discover canonical fullpath */

	_path = canonical_path(fullpath);

	/* is it new ? */
	if (Profile->get_trx() ) {
		// Waves TracksLive has a usecase of session replacement with a new one.
		// We should check session state file (<session_name>.ardour) existance
		// to determine if the session is new or not

		string full_session_name = Glib::build_filename( fullpath, _name );
		full_session_name += statefile_suffix;

		_is_new = !Glib::file_test (full_session_name, Glib::FileTest (G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR));
	} else {
		_is_new = !Glib::file_test (_path, Glib::FileTest (G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR));
	}

	/* finish initialization that can't be done in a normal C++ constructor
	   definition.
	*/

	timerclear (&last_mmc_step);
	g_atomic_int_set (&processing_prohibited, 0);
	g_atomic_int_set (&_record_status, Disabled);
	g_atomic_int_set (&_playback_load, 100);
	g_atomic_int_set (&_capture_load, 100);
	set_next_event ();
	_all_route_group->set_active (true, this);
	interpolation.add_channel_to (0, 0);

	if (config.get_use_video_sync()) {
		waiting_for_sync_offset = true;
	} else {
		waiting_for_sync_offset = false;
	}

	last_rr_session_dir = session_dirs.begin();

	set_history_depth (Config->get_history_depth());

        /* default: assume simple stereo speaker configuration */

        _speakers->setup_default_speakers (2);

        _solo_cut_control.reset (new ProxyControllable (_("solo cut control (dB)"), PBD::Controllable::GainLike,
                                                        boost::bind (&RCConfiguration::set_solo_mute_gain, Config, _1),
                                                        boost::bind (&RCConfiguration::get_solo_mute_gain, Config)));
        add_controllable (_solo_cut_control);

	/* These are all static "per-class" signals */

	SourceFactory::SourceCreated.connect_same_thread (*this, boost::bind (&Session::add_source, this, _1));
	PlaylistFactory::PlaylistCreated.connect_same_thread (*this, boost::bind (&Session::add_playlist, this, _1, _2));
	AutomationList::AutomationListCreated.connect_same_thread (*this, boost::bind (&Session::add_automation_list, this, _1));
	Controllable::Destroyed.connect_same_thread (*this, boost::bind (&Session::remove_controllable, this, _1));
	IO::PortCountChanged.connect_same_thread (*this, boost::bind (&Session::ensure_buffers, this, _1));

	/* stop IO objects from doing stuff until we're ready for them */

	Delivery::disable_panners ();
	IO::disable_connecting ();
}

int
Session::post_engine_init ()
{
	BootMessage (_("Set block size and sample rate"));

	set_block_size (_engine.samples_per_cycle());
	set_frame_rate (_engine.sample_rate());

	BootMessage (_("Using configuration"));

	_midi_ports = new MidiPortManager;

	MIDISceneChanger* msc;

	_scene_changer = msc = new MIDISceneChanger (*this);
	msc->set_input_port (scene_input_port());
	msc->set_output_port (scene_out());

	boost::function<framecnt_t(void)> timer_func (boost::bind (&Session::audible_frame, this));
	boost::dynamic_pointer_cast<AsyncMIDIPort>(scene_in())->set_timer (timer_func);

	setup_midi_machine_control ();

	if (_butler->start_thread()) {
		return -1;
	}

	if (start_midi_thread ()) {
		return -1;
	}

	setup_click_sounds (0);
	setup_midi_control ();

	_engine.Halted.connect_same_thread (*this, boost::bind (&Session::engine_halted, this));
	_engine.Xrun.connect_same_thread (*this, boost::bind (&Session::xrun_recovery, this));

	try {
		/* tempo map requires sample rate knowledge */

		delete _tempo_map;
		_tempo_map = new TempoMap (_current_frame_rate);
		_tempo_map->PropertyChanged.connect_same_thread (*this, boost::bind (&Session::tempo_map_changed, this, _1));

		/* MidiClock requires a tempo map */

		midi_clock = new MidiClockTicker ();
		midi_clock->set_session (this);

		/* crossfades require sample rate knowledge */

		SndFileSource::setup_standard_crossfades (*this, frame_rate());
		_engine.GraphReordered.connect_same_thread (*this, boost::bind (&Session::graph_reordered, this));

		AudioDiskstream::allocate_working_buffers();
		refresh_disk_space ();

		/* we're finally ready to call set_state() ... all objects have
		 * been created, the engine is running.
		 */

		if (state_tree) {
			if (set_state (*state_tree->root(), Stateful::loading_state_version)) {
				return -1;
			}
		} else {
			// set_state() will call setup_raid_path(), but if it's a new session we need
			// to call setup_raid_path() here.
			setup_raid_path (_path);
		}

		/* ENGINE */

		boost::function<void (std::string)> ff (boost::bind (&Session::config_changed, this, _1, false));
		boost::function<void (std::string)> ft (boost::bind (&Session::config_changed, this, _1, true));

		Config->map_parameters (ff);
		config.map_parameters (ft);
                _butler->map_parameters ();

		/* Reset all panners */

		Delivery::reset_panners ();

		/* this will cause the CPM to instantiate any protocols that are in use
		 * (or mandatory), which will pass it this Session, and then call
		 * set_state() on each instantiated protocol to match stored state.
		 */

		ControlProtocolManager::instance().set_session (this);

		/* This must be done after the ControlProtocolManager set_session above,
		   as it will set states for ports which the ControlProtocolManager creates.
		*/

		// XXX set state of MIDI::Port's
		// MidiPortManager::instance()->set_port_states (Config->midi_port_states ());

		/* And this must be done after the MIDI::Manager::set_port_states as
		 * it will try to make connections whose details are loaded by set_port_states.
		 */

		hookup_io ();

		/* Let control protocols know that we are now all connected, so they
		 * could start talking to surfaces if they want to.
		 */

		ControlProtocolManager::instance().midi_connectivity_established ();

		if (_is_new && !no_auto_connect()) {
			Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock());
			auto_connect_master_bus ();
		}

		_state_of_the_state = StateOfTheState (_state_of_the_state & ~(CannotSave|Dirty));

		/* update latencies */

		initialize_latencies ();

		_locations->added.connect_same_thread (*this, boost::bind (&Session::location_added, this, _1));
		_locations->removed.connect_same_thread (*this, boost::bind (&Session::location_removed, this, _1));
		_locations->changed.connect_same_thread (*this, boost::bind (&Session::locations_changed, this));

	} catch (AudioEngine::PortRegistrationFailure& err) {
		/* handle this one in a different way than all others, so that its clear what happened */
		error << err.what() << endmsg;
		return -1;
	} catch (...) {
		return -1;
	}

	BootMessage (_("Reset Remote Controls"));

	// send_full_time_code (0);
	_engine.transport_locate (0);

	send_immediate_mmc (MIDI::MachineControlCommand (MIDI::MachineControl::cmdMmcReset));
	send_immediate_mmc (MIDI::MachineControlCommand (Timecode::Time ()));

	MIDI::Name::MidiPatchManager::instance().set_session (this);

	ltc_tx_initialize();
	/* initial program change will be delivered later; see ::config_changed() */

	_state_of_the_state = Clean;

	Port::set_connecting_blocked (false);

	DirtyChanged (); /* EMIT SIGNAL */

	if (_is_new) {
		save_state ("");
	} else if (state_was_pending) {
		save_state ("");
		remove_pending_capture_state ();
		state_was_pending = false;
	}

	/* Now, finally, we can fill the playback buffers */

	BootMessage (_("Filling playback buffers"));

	boost::shared_ptr<RouteList> rl = routes.reader();
	for (RouteList::iterator r = rl->begin(); r != rl->end(); ++r) {
		boost::shared_ptr<Track> trk = boost::dynamic_pointer_cast<Track> (*r);
		if (trk && !trk->hidden()) {
			trk->seek (_transport_frame, true);
		}
	}

	return 0;
}

void
Session::session_loaded ()
{
	SessionLoaded();

	_state_of_the_state = Clean;

	DirtyChanged (); /* EMIT SIGNAL */

	if (_is_new) {
		save_state ("");
	} else if (state_was_pending) {
		save_state ("");
		remove_pending_capture_state ();
		state_was_pending = false;
	}

	/* Now, finally, we can fill the playback buffers */

	BootMessage (_("Filling playback buffers"));
	force_locate (_transport_frame, false);
}

string
Session::raid_path () const
{
	Searchpath raid_search_path;

	for (vector<space_and_path>::const_iterator i = session_dirs.begin(); i != session_dirs.end(); ++i) {
		raid_search_path += (*i).path;
	}

	return raid_search_path.to_string ();
}

void
Session::setup_raid_path (string path)
{
	if (path.empty()) {
		return;
	}

	space_and_path sp;
	string fspath;

	session_dirs.clear ();

	Searchpath search_path(path);
	Searchpath sound_search_path;
	Searchpath midi_search_path;

	for (Searchpath::const_iterator i = search_path.begin(); i != search_path.end(); ++i) {
		sp.path = *i;
		sp.blocks = 0; // not needed
		session_dirs.push_back (sp);

		SessionDirectory sdir(sp.path);

		sound_search_path += sdir.sound_path ();
		midi_search_path += sdir.midi_path ();
	}

	// reset the round-robin soundfile path thingie
	last_rr_session_dir = session_dirs.begin();
}

bool
Session::path_is_within_session (const std::string& path)
{
	for (vector<space_and_path>::const_iterator i = session_dirs.begin(); i != session_dirs.end(); ++i) {
		if (PBD::path_is_within (i->path, path)) {
			return true;
		}
	}
	return false;
}

int
Session::ensure_subdirs ()
{
	string dir;

	dir = session_directory().peak_path();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session peakfile folder \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	dir = session_directory().sound_path();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session sounds dir \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	dir = session_directory().midi_path();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session midi dir \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	dir = session_directory().dead_path();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session dead sounds folder \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	dir = session_directory().export_path();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session export folder \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	dir = analysis_dir ();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session analysis folder \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	dir = plugins_dir ();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session plugins folder \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	dir = externals_dir ();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session externals folder \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	return 0;
}

/** @param session_template directory containing session template, or empty.
 *  Caller must not hold process lock.
 */
int
Session::create (const string& session_template, BusProfile* bus_profile)
{
	if (g_mkdir_with_parents (_path.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session folder \"%1\" (%2)"), _path, strerror (errno)) << endmsg;
		return -1;
	}

	if (ensure_subdirs ()) {
		return -1;
	}

	_writable = exists_and_writable (_path);

	if (!session_template.empty()) {
		string in_path = (ARDOUR::Profile->get_trx () ? session_template : session_template_dir_to_file (session_template));

		FILE* in = g_fopen (in_path.c_str(), "rb");

		if (in) {
			/* no need to call legalize_for_path() since the string
			 * in session_template is already a legal path name
			 */
			string out_path = Glib::build_filename (_session_dir->root_path(), _name + statefile_suffix);

			FILE* out = g_fopen (out_path.c_str(), "wb");

			if (out) {
				char buf[1024];
				stringstream new_session;

				while (!feof (in)) {
					size_t charsRead = fread (buf, sizeof(char), 1024, in);

					if (ferror (in)) {
						error << string_compose (_("Error reading session template file %1 (%2)"), in_path, strerror (errno)) << endmsg;
						fclose (in);
						fclose (out);
						return -1;
					}
					if (charsRead == 0) {
						break;
					}
					new_session.write (buf, charsRead);
				}
				fclose (in);

				string file_contents = new_session.str();
				size_t writeSize = file_contents.length();
				if (fwrite (file_contents.c_str(), sizeof(char), writeSize, out) != writeSize) {
					error << string_compose (_("Error writing session template file %1 (%2)"), out_path, strerror (errno)) << endmsg;
					fclose (out);
					return -1;
				}
				fclose (out);

				_is_new = false;

				if (!ARDOUR::Profile->get_trx()) {
					/* Copy plugin state files from template to new session */
					std::string template_plugins = Glib::build_filename (session_template, X_("plugins"));
					copy_recurse (template_plugins, plugins_dir ());
				}

				return 0;

			} else {
				error << string_compose (_("Could not open %1 for writing session template"), out_path)
					<< endmsg;
				fclose(in);
				return -1;
			}

		} else {
			error << string_compose (_("Could not open session template %1 for reading"), in_path)
				<< endmsg;
			return -1;
		}

	}

	if (Profile->get_trx()) {

		/* set initial start + end point : ARDOUR::Session::session_end_shift long.
		   Remember that this is a brand new session. Sessions
		   loaded from saved state will get this range from the saved state.
		*/

		set_session_range_location (0, 0);

		/* Initial loop location, from absolute zero, length 10 seconds  */

		Location* loc = new Location (*this, 0, 10.0 * _engine.sample_rate(), _("Loop"),  Location::IsAutoLoop);
		_locations->add (loc, true);
		set_auto_loop_location (loc);
	}

	_state_of_the_state = Clean;

        /* set up Master Out and Control Out if necessary */

        if (bus_profile) {

		RouteList rl;
                ChanCount count(DataType::AUDIO, bus_profile->master_out_channels);

                // Waves Tracks: always create master bus for Tracks
                if (ARDOUR::Profile->get_trx() || bus_profile->master_out_channels) {
	                boost::shared_ptr<Route> r (new Route (*this, _("Master"), Route::MasterOut, DataType::AUDIO));
                        if (r->init ()) {
                                return -1;
                        }
#ifdef BOOST_SP_ENABLE_DEBUG_HOOKS
			// boost_debug_shared_ptr_mark_interesting (r.get(), "Route");
#endif
			{
				Glib::Threads::Mutex::Lock lm (AudioEngine::instance()->process_lock ());
				r->input()->ensure_io (count, false, this);
				r->output()->ensure_io (count, false, this);
			}

			rl.push_back (r);

		} else {
			/* prohibit auto-connect to master, because there isn't one */
			bus_profile->output_ac = AutoConnectOption (bus_profile->output_ac & ~AutoConnectMaster);
		}

		if (!rl.empty()) {
			add_routes (rl, false, false, false);
		}

		// Waves Tracks: Skip this. Always use autoconnection for Tracks
		if (!ARDOUR::Profile->get_trx()) {

			/* this allows the user to override settings with an environment variable.
			 */

			if (no_auto_connect()) {
				bus_profile->input_ac = AutoConnectOption (0);
				bus_profile->output_ac = AutoConnectOption (0);
			}

			Config->set_input_auto_connect (bus_profile->input_ac);
			Config->set_output_auto_connect (bus_profile->output_ac);
		}
        }

	if (Config->get_use_monitor_bus() && bus_profile) {
		add_monitor_section ();
	}

	return 0;
}

void
Session::maybe_write_autosave()
{
        if (dirty() && record_status() != Recording) {
                save_state("", true);
        }
}

void
Session::remove_pending_capture_state ()
{
	std::string pending_state_file_path(_session_dir->root_path());

	pending_state_file_path = Glib::build_filename (pending_state_file_path, legalize_for_path (_current_snapshot_name) + pending_suffix);

	if (!Glib::file_test (pending_state_file_path, Glib::FILE_TEST_EXISTS)) return;

	if (g_remove (pending_state_file_path.c_str()) != 0) {
		error << string_compose(_("Could not remove pending capture state at path \"%1\" (%2)"),
				pending_state_file_path, g_strerror (errno)) << endmsg;
	}
}

/** Rename a state file.
 *  @param old_name Old snapshot name.
 *  @param new_name New snapshot name.
 */
void
Session::rename_state (string old_name, string new_name)
{
	if (old_name == _current_snapshot_name || old_name == _name) {
		/* refuse to rename the current snapshot or the "main" one */
		return;
	}

	const string old_xml_filename = legalize_for_path (old_name) + statefile_suffix;
	const string new_xml_filename = legalize_for_path (new_name) + statefile_suffix;

	const std::string old_xml_path(Glib::build_filename (_session_dir->root_path(), old_xml_filename));
	const std::string new_xml_path(Glib::build_filename (_session_dir->root_path(), new_xml_filename));

	if (::g_rename (old_xml_path.c_str(), new_xml_path.c_str()) != 0) {
		error << string_compose(_("could not rename snapshot %1 to %2 (%3)"),
				old_name, new_name, g_strerror(errno)) << endmsg;
	}
}

/** Remove a state file.
 *  @param snapshot_name Snapshot name.
 */
void
Session::remove_state (string snapshot_name)
{
	if (!_writable || snapshot_name == _current_snapshot_name || snapshot_name == _name) {
		// refuse to remove the current snapshot or the "main" one
		return;
	}

	std::string xml_path(_session_dir->root_path());

	xml_path = Glib::build_filename (xml_path, legalize_for_path (snapshot_name) + statefile_suffix);

	if (!create_backup_file (xml_path)) {
		// don't remove it if a backup can't be made
		// create_backup_file will log the error.
		return;
	}

	// and delete it
	if (g_remove (xml_path.c_str()) != 0) {
		error << string_compose(_("Could not remove session file at path \"%1\" (%2)"),
				xml_path, g_strerror (errno)) << endmsg;
	}
}

/** @param snapshot_name Name to save under, without .ardour / .pending prefix */
int
Session::save_state (string snapshot_name, bool pending, bool switch_to_snapshot, bool template_only)
{
	XMLTree tree;
	std::string xml_path(_session_dir->root_path());

	/* prevent concurrent saves from different threads */

	Glib::Threads::Mutex::Lock lm (save_state_lock);

	if (!_writable || (_state_of_the_state & CannotSave)) {
		return 1;
	}

	if (g_atomic_int_get(&_suspend_save)) {
		_save_queued = true;
		return 1;
	}
	_save_queued = false;

	if (!_engine.connected ()) {
		error << string_compose (_("the %1 audio engine is not connected and state saving would lose all I/O connections. Session not saved"),
                                         PROGRAM_NAME)
		      << endmsg;
		return 1;
	}

	/* tell sources we're saving first, in case they write out to a new file
	 * which should be saved with the state rather than the old one */
	for (SourceMap::const_iterator i = sources.begin(); i != sources.end(); ++i) {
		try {
			i->second->session_saved();
		} catch (Evoral::SMF::FileError& e) {
			error << string_compose ("Could not write to MIDI file %1; MIDI data not saved.", e.file_name ()) << endmsg;
		}
	}

	SessionSaveUnderway (); /* EMIT SIGNAL */

	if (template_only) {
		tree.set_root (&get_template());
	} else {
		tree.set_root (&get_state());
	}

	if (snapshot_name.empty()) {
		snapshot_name = _current_snapshot_name;
	} else if (switch_to_snapshot) {
                _current_snapshot_name = snapshot_name;
        }

	if (!pending) {

		/* proper save: use statefile_suffix (.ardour in English) */

		xml_path = Glib::build_filename (xml_path, legalize_for_path (snapshot_name) + statefile_suffix);

		/* make a backup copy of the old file */

		if (Glib::file_test (xml_path, Glib::FILE_TEST_EXISTS) && !create_backup_file (xml_path)) {
			// create_backup_file will log the error
			return -1;
		}

	} else {

		/* pending save: use pending_suffix (.pending in English) */
		xml_path = Glib::build_filename (xml_path, legalize_for_path (snapshot_name) + pending_suffix);
	}

	std::string tmp_path(_session_dir->root_path());
	tmp_path = Glib::build_filename (tmp_path, legalize_for_path (snapshot_name) + temp_suffix);

	cerr << "actually writing state to " << tmp_path << endl;

	if (!tree.write (tmp_path)) {
		error << string_compose (_("state could not be saved to %1"), tmp_path) << endmsg;
		if (g_remove (tmp_path.c_str()) != 0) {
			error << string_compose(_("Could not remove temporary session file at path \"%1\" (%2)"),
					tmp_path, g_strerror (errno)) << endmsg;
		}
		return -1;

	} else {

		cerr << "renaming state to " << xml_path << endl;

		if (::g_rename (tmp_path.c_str(), xml_path.c_str()) != 0) {
			error << string_compose (_("could not rename temporary session file %1 to %2 (%3)"),
					tmp_path, xml_path, g_strerror(errno)) << endmsg;
			if (g_remove (tmp_path.c_str()) != 0) {
				error << string_compose(_("Could not remove temporary session file at path \"%1\" (%2)"),
						tmp_path, g_strerror (errno)) << endmsg;
			}
			return -1;
		}
	}

	if (!pending) {

		save_history (snapshot_name);

		bool was_dirty = dirty();

		_state_of_the_state = StateOfTheState (_state_of_the_state & ~Dirty);

		if (was_dirty) {
			DirtyChanged (); /* EMIT SIGNAL */
		}

		StateSaved (snapshot_name); /* EMIT SIGNAL */
	}

	return 0;
}

int
Session::restore_state (string snapshot_name)
{
	if (load_state (snapshot_name) == 0) {
		set_state (*state_tree->root(), Stateful::loading_state_version);
	}

	return 0;
}

int
Session::load_state (string snapshot_name)
{
	delete state_tree;
	state_tree = 0;

	state_was_pending = false;

	/* check for leftover pending state from a crashed capture attempt */

	std::string xmlpath(_session_dir->root_path());
	xmlpath = Glib::build_filename (xmlpath, legalize_for_path (snapshot_name) + pending_suffix);

	if (Glib::file_test (xmlpath, Glib::FILE_TEST_EXISTS)) {

		/* there is pending state from a crashed capture attempt */

                boost::optional<int> r = AskAboutPendingState();
		if (r.get_value_or (1)) {
			state_was_pending = true;
		}
	}

	if (!state_was_pending) {
		xmlpath = Glib::build_filename (_session_dir->root_path(), snapshot_name);
	}

	if (!Glib::file_test (xmlpath, Glib::FILE_TEST_EXISTS)) {
		xmlpath = Glib::build_filename (_session_dir->root_path(), legalize_for_path (snapshot_name) + statefile_suffix);
		if (!Glib::file_test (xmlpath, Glib::FILE_TEST_EXISTS)) {
                        error << string_compose(_("%1: session file \"%2\" doesn't exist!"), _name, xmlpath) << endmsg;
                        return 1;
                }
        }

	state_tree = new XMLTree;

	set_dirty();

	_writable = exists_and_writable (xmlpath) && exists_and_writable(Glib::path_get_dirname(xmlpath));

	if (!state_tree->read (xmlpath)) {
		error << string_compose(_("Could not understand session file %1"), xmlpath) << endmsg;
		delete state_tree;
		state_tree = 0;
		return -1;
	}

	XMLNode& root (*state_tree->root());

	if (root.name() != X_("Session")) {
		error << string_compose (_("Session file %1 is not a session"), xmlpath) << endmsg;
		delete state_tree;
		state_tree = 0;
		return -1;
	}

	const XMLProperty* prop;

	if ((prop = root.property ("version")) == 0) {
		/* no version implies very old version of Ardour */
		Stateful::loading_state_version = 1000;
	} else {
		if (prop->value().find ('.') != string::npos) {
			/* old school version format */
			if (prop->value()[0] == '2') {
				Stateful::loading_state_version = 2000;
			} else {
				Stateful::loading_state_version = 3000;
			}
		} else {
			Stateful::loading_state_version = atoi (prop->value());
		}
	}

	if (Stateful::loading_state_version < CURRENT_SESSION_FILE_VERSION && _writable) {

		std::string backup_path(_session_dir->root_path());
		std::string backup_filename = string_compose ("%1-%2%3", legalize_for_path (snapshot_name), Stateful::loading_state_version, statefile_suffix);
		backup_path = Glib::build_filename (backup_path, backup_filename);

		// only create a backup for a given statefile version once

		if (!Glib::file_test (backup_path, Glib::FILE_TEST_EXISTS)) {

			VersionMismatch (xmlpath, backup_path);

			if (!copy_file (xmlpath, backup_path)) {;
				return -1;
			}
		}
	}

	return 0;
}

int
Session::load_options (const XMLNode& node)
{
	LocaleGuard lg (X_("C"));
	config.set_variables (node);
	return 0;
}

bool
Session::save_default_options ()
{
	return config.save_state();
}

XMLNode&
Session::get_state()
{
	return state(true);
}

XMLNode&
Session::get_template()
{
	/* if we don't disable rec-enable, diskstreams
	   will believe they need to store their capture
	   sources in their state node.
	*/

	disable_record (false);

	return state(false);
}

XMLNode&
Session::state (bool full_state)
{
	XMLNode* node = new XMLNode("Session");
	XMLNode* child;

	char buf[16];
	snprintf(buf, sizeof(buf), "%d", CURRENT_SESSION_FILE_VERSION);
	node->add_property("version", buf);

	/* store configuration settings */

	if (full_state) {

		node->add_property ("name", _name);
		snprintf (buf, sizeof (buf), "%" PRId64, _nominal_frame_rate);
		node->add_property ("sample-rate", buf);

		if (session_dirs.size() > 1) {

			string p;

			vector<space_and_path>::iterator i = session_dirs.begin();
			vector<space_and_path>::iterator next;

			++i; /* skip the first one */
			next = i;
			++next;

			while (i != session_dirs.end()) {

				p += (*i).path;

				if (next != session_dirs.end()) {
					p += G_SEARCHPATH_SEPARATOR;
				} else {
					break;
				}

				++next;
				++i;
			}

			child = node->add_child ("Path");
			child->add_content (p);
		}
	}

	/* save the ID counter */

	snprintf (buf, sizeof (buf), "%" PRIu64, ID::counter());
	node->add_property ("id-counter", buf);

	/* save the event ID counter */

	snprintf (buf, sizeof (buf), "%d", Evoral::event_id_counter());
	node->add_property ("event-counter", buf);

	/* various options */

	list<XMLNode*> midi_port_nodes = _midi_ports->get_midi_port_states();
	if (!midi_port_nodes.empty()) {
		XMLNode* midi_port_stuff = new XMLNode ("MIDIPorts");
		for (list<XMLNode*>::const_iterator n = midi_port_nodes.begin(); n != midi_port_nodes.end(); ++n) {
			midi_port_stuff->add_child_nocopy (**n);
		}
		node->add_child_nocopy (*midi_port_stuff);
	}

	node->add_child_nocopy (config.get_variables ());

	node->add_child_nocopy (ARDOUR::SessionMetadata::Metadata()->get_state());

	child = node->add_child ("Sources");

	if (full_state) {
		Glib::Threads::Mutex::Lock sl (source_lock);

		for (SourceMap::iterator siter = sources.begin(); siter != sources.end(); ++siter) {

			/* Don't save information about non-file Sources, or
			 * about non-destructive file sources that are empty
			 * and unused by any regions.
                        */

			boost::shared_ptr<FileSource> fs;

			if ((fs = boost::dynamic_pointer_cast<FileSource> (siter->second)) != 0) {

				if (!fs->destructive()) {
					if (fs->empty() && !fs->used()) {
						continue;
					}
				}

				child->add_child_nocopy (siter->second->get_state());
			}
		}
	}

	child = node->add_child ("Regions");

	if (full_state) {
		Glib::Threads::Mutex::Lock rl (region_lock);
                const RegionFactory::RegionMap& region_map (RegionFactory::all_regions());
                for (RegionFactory::RegionMap::const_iterator i = region_map.begin(); i != region_map.end(); ++i) {
                        boost::shared_ptr<Region> r = i->second;
                        /* only store regions not attached to playlists */
                        if (r->playlist() == 0) {
				if (boost::dynamic_pointer_cast<AudioRegion>(r)) {
					child->add_child_nocopy ((boost::dynamic_pointer_cast<AudioRegion>(r))->get_basic_state ());
				} else {
					child->add_child_nocopy (r->get_state ());
				}
                        }
                }

		RegionFactory::CompoundAssociations& cassocs (RegionFactory::compound_associations());

		if (!cassocs.empty()) {
			XMLNode* ca = node->add_child (X_("CompoundAssociations"));

			for (RegionFactory::CompoundAssociations::iterator i = cassocs.begin(); i != cassocs.end(); ++i) {
				char buf[64];
				XMLNode* can = new XMLNode (X_("CompoundAssociation"));
				i->first->id().print (buf, sizeof (buf));
				can->add_property (X_("copy"), buf);
				i->second->id().print (buf, sizeof (buf));
				can->add_property (X_("original"), buf);
				ca->add_child_nocopy (*can);
			}
		}
	}



	if (full_state) {

		if (_locations) {
			node->add_child_nocopy (_locations->get_state());
		}
	} else {
		Locations loc (*this);
		// for a template, just create a new Locations, populate it
		// with the default start and end, and get the state for that.
		Location* range = new Location (*this, 0, 0, _("session"), Location::IsSessionRange);
		range->set (max_framepos, 0);
		loc.add (range);
		XMLNode& locations_state = loc.get_state();

		if (ARDOUR::Profile->get_trx() && _locations) {
			// For tracks we need stored the Auto Loop Range and all MIDI markers.
			for (Locations::LocationList::const_iterator i = _locations->list ().begin (); i != _locations->list ().end (); ++i) {
				if ((*i)->is_mark () || (*i)->is_auto_loop ()) {
					locations_state.add_child_nocopy ((*i)->get_state ());
				}
			}
		}
		node->add_child_nocopy (locations_state);
	}

	child = node->add_child ("Bundles");
	{
		boost::shared_ptr<BundleList> bundles = _bundles.reader ();
		for (BundleList::iterator i = bundles->begin(); i != bundles->end(); ++i) {
			boost::shared_ptr<UserBundle> b = boost::dynamic_pointer_cast<UserBundle> (*i);
			if (b) {
				child->add_child_nocopy (b->get_state());
			}
		}
	}

	child = node->add_child ("Routes");
	{
		boost::shared_ptr<RouteList> r = routes.reader ();

		RoutePublicOrderSorter cmp;
		RouteList public_order (*r);
		public_order.sort (cmp);

		/* the sort should have put control outs first */

		if (_monitor_out) {
			assert (_monitor_out == public_order.front());
		}

		for (RouteList::iterator i = public_order.begin(); i != public_order.end(); ++i) {
			if (!(*i)->is_auditioner()) {
				if (full_state) {
					child->add_child_nocopy ((*i)->get_state());
				} else {
					child->add_child_nocopy ((*i)->get_template());
				}
			}
		}
	}

	playlists->add_state (node, full_state);

	child = node->add_child ("RouteGroups");
	for (list<RouteGroup *>::iterator i = _route_groups.begin(); i != _route_groups.end(); ++i) {
		child->add_child_nocopy ((*i)->get_state());
	}

	if (_click_io) {
		XMLNode* gain_child = node->add_child ("Click");
		gain_child->add_child_nocopy (_click_io->state (full_state));
		gain_child->add_child_nocopy (_click_gain->state (full_state));
	}

	if (_ltc_input) {
		XMLNode* ltc_input_child = node->add_child ("LTC-In");
		ltc_input_child->add_child_nocopy (_ltc_input->state (full_state));
	}

	if (_ltc_input) {
		XMLNode* ltc_output_child = node->add_child ("LTC-Out");
		ltc_output_child->add_child_nocopy (_ltc_output->state (full_state));
	}

        node->add_child_nocopy (_speakers->get_state());
	node->add_child_nocopy (_tempo_map->get_state());
	node->add_child_nocopy (get_control_protocol_state());

	if (_extra_xml) {
		node->add_child_copy (*_extra_xml);
	}

	return *node;
}

XMLNode&
Session::get_control_protocol_state ()
{
	ControlProtocolManager& cpm (ControlProtocolManager::instance());
	return cpm.get_state();
}

int
Session::set_state (const XMLNode& node, int version)
{
	XMLNodeList nlist;
	XMLNode* child;
	const XMLProperty* prop;
	int ret = -1;

	_state_of_the_state = StateOfTheState (_state_of_the_state|CannotSave);

	if (node.name() != X_("Session")) {
		fatal << _("programming error: Session: incorrect XML node sent to set_state()") << endmsg;
		goto out;
	}

	if ((prop = node.property ("name")) != 0) {
		_name = prop->value ();
	}

	if ((prop = node.property (X_("sample-rate"))) != 0) {

		_nominal_frame_rate = atoi (prop->value());

		if (_nominal_frame_rate != _current_frame_rate) {
                        boost::optional<int> r = AskAboutSampleRateMismatch (_nominal_frame_rate, _current_frame_rate);
			if (r.get_value_or (0)) {
				goto out;
			}
		}
	}

	setup_raid_path(_session_dir->root_path());

	if ((prop = node.property (X_("id-counter"))) != 0) {
		uint64_t x;
		sscanf (prop->value().c_str(), "%" PRIu64, &x);
		ID::init_counter (x);
	} else {
		/* old sessions used a timebased counter, so fake
		   the startup ID counter based on a standard
		   timestamp.
		*/
		time_t now;
		time (&now);
		ID::init_counter (now);
	}

        if ((prop = node.property (X_("event-counter"))) != 0) {
                Evoral::init_event_id_counter (atoi (prop->value()));
        }


	if ((child = find_named_node (node, "MIDIPorts")) != 0) {
		_midi_ports->set_midi_port_states (child->children());
	}

	IO::disable_connecting ();

	Stateful::save_extra_xml (node);

	if (((child = find_named_node (node, "Options")) != 0)) { /* old style */
		load_options (*child);
	} else if ((child = find_named_node (node, "Config")) != 0) { /* new style */
		load_options (*child);
	} else {
		error << _("Session: XML state has no options section") << endmsg;
	}

	if (version >= 3000) {
		if ((child = find_named_node (node, "Metadata")) == 0) {
			warning << _("Session: XML state has no metadata section") << endmsg;
		} else if ( ARDOUR::SessionMetadata::Metadata()->set_state (*child, version) ) {
			goto out;
		}
	}

        if ((child = find_named_node (node, X_("Speakers"))) != 0) {
                _speakers->set_state (*child, version);
        }

	if ((child = find_named_node (node, "Sources")) == 0) {
		error << _("Session: XML state has no sources section") << endmsg;
		goto out;
	} else if (load_sources (*child)) {
		goto out;
	}

	if ((child = find_named_node (node, "TempoMap")) == 0) {
		error << _("Session: XML state has no Tempo Map section") << endmsg;
		goto out;
	} else if (_tempo_map->set_state (*child, version)) {
		goto out;
	}

	if ((child = find_named_node (node, "Locations")) == 0) {
		error << _("Session: XML state has no locations section") << endmsg;
		goto out;
	} else if (_locations->set_state (*child, version)) {
		goto out;
	}

	locations_changed ();

	if (_session_range_location) {
		AudioFileSource::set_header_position_offset (_session_range_location->start());
	}

	if ((child = find_named_node (node, "Regions")) == 0) {
		error << _("Session: XML state has no Regions section") << endmsg;
		goto out;
	} else if (load_regions (*child)) {
		goto out;
	}

	if ((child = find_named_node (node, "Playlists")) == 0) {
		error << _("Session: XML state has no playlists section") << endmsg;
		goto out;
	} else if (playlists->load (*this, *child)) {
		goto out;
	}

	if ((child = find_named_node (node, "UnusedPlaylists")) == 0) {
		// this is OK
	} else if (playlists->load_unused (*this, *child)) {
		goto out;
	}

	if ((child = find_named_node (node, "CompoundAssociations")) != 0) {
		if (load_compounds (*child)) {
			goto out;
		}
	}

	if (version >= 3000) {
		if ((child = find_named_node (node, "Bundles")) == 0) {
			warning << _("Session: XML state has no bundles section") << endmsg;
			//goto out;
		} else {
			/* We can't load Bundles yet as they need to be able
			   to convert from port names to Port objects, which can't happen until
			   later */
			_bundle_xml_node = new XMLNode (*child);
		}
	}

	if (version < 3000) {
		if ((child = find_named_node (node, X_("DiskStreams"))) == 0) {
			error << _("Session: XML state has no diskstreams section") << endmsg;
			goto out;
		} else if (load_diskstreams_2X (*child, version)) {
			goto out;
		}
	}

	if ((child = find_named_node (node, "Routes")) == 0) {
		error << _("Session: XML state has no routes section") << endmsg;
		goto out;
	} else if (load_routes (*child, version)) {
		goto out;
	}

	/* our diskstreams list is no longer needed as they are now all owned by their Route */
	_diskstreams_2X.clear ();

	if (version >= 3000) {

		if ((child = find_named_node (node, "RouteGroups")) == 0) {
			error << _("Session: XML state has no route groups section") << endmsg;
			goto out;
		} else if (load_route_groups (*child, version)) {
			goto out;
		}

	} else if (version < 3000) {

		if ((child = find_named_node (node, "EditGroups")) == 0) {
			error << _("Session: XML state has no edit groups section") << endmsg;
			goto out;
		} else if (load_route_groups (*child, version)) {
			goto out;
		}

		if ((child = find_named_node (node, "MixGroups")) == 0) {
			error << _("Session: XML state has no mix groups section") << endmsg;
			goto out;
		} else if (load_route_groups (*child, version)) {
			goto out;
		}
	}

	if ((child = find_named_node (node, "Click")) == 0) {
		warning << _("Session: XML state has no click section") << endmsg;
	} else if (_click_io) {
		setup_click_state (&node);
	}

	if ((child = find_named_node (node, ControlProtocolManager::state_node_name)) != 0) {
		ControlProtocolManager::instance().set_state (*child, version);
	}

	update_route_record_state ();

	/* here beginneth the second phase ... */

	StateReady (); /* EMIT SIGNAL */

	delete state_tree;
	state_tree = 0;
	return 0;

  out:
	delete state_tree;
	state_tree = 0;
	return ret;
}

int
Session::load_routes (const XMLNode& node, int version)
{
	XMLNodeList nlist;
	XMLNodeConstIterator niter;
	RouteList new_routes;

	nlist = node.children();

	set_dirty();

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {

		boost::shared_ptr<Route> route;
		if (version < 3000) {
			route = XMLRouteFactory_2X (**niter, version);
		} else {
			route = XMLRouteFactory (**niter, version);
		}

		if (route == 0) {
			error << _("Session: cannot create Route from XML description.") << endmsg;
			return -1;
		}

		BootMessage (string_compose (_("Loaded track/bus %1"), route->name()));

		new_routes.push_back (route);
	}

	BootMessage (_("Tracks/busses loaded;  Adding to Session"));

	add_routes (new_routes, false, false, false);

	BootMessage (_("Finished adding tracks/busses"));

	return 0;
}

boost::shared_ptr<Route>
Session::XMLRouteFactory (const XMLNode& node, int version)
{
	boost::shared_ptr<Route> ret;

	if (node.name() != "Route") {
		return ret;
	}

	XMLNode* ds_child = find_named_node (node, X_("Diskstream"));

	DataType type = DataType::AUDIO;
	const XMLProperty* prop = node.property("default-type");

	if (prop) {
		type = DataType (prop->value());
	}

	assert (type != DataType::NIL);

	if (ds_child) {

		boost::shared_ptr<Track> track;

                if (type == DataType::AUDIO) {
                        track.reset (new AudioTrack (*this, X_("toBeResetFroXML")));
                } else {
                        track.reset (new MidiTrack (*this, X_("toBeResetFroXML")));
                }

                if (track->init()) {
                        return ret;
                }

                if (track->set_state (node, version)) {
                        return ret;
                }

#ifdef BOOST_SP_ENABLE_DEBUG_HOOKS
                // boost_debug_shared_ptr_mark_interesting (track.get(), "Track");
#endif
                ret = track;

	} else {
		enum Route::Flag flags = Route::Flag(0);
		const XMLProperty* prop = node.property("flags");
		if (prop) {
			flags = Route::Flag (string_2_enum (prop->value(), flags));
		}

		boost::shared_ptr<Route> r (new Route (*this, X_("toBeResetFroXML"), flags));

                if (r->init () == 0 && r->set_state (node, version) == 0) {
#ifdef BOOST_SP_ENABLE_DEBUG_HOOKS
                        // boost_debug_shared_ptr_mark_interesting (r.get(), "Route");
#endif
                        ret = r;
                }
	}

	return ret;
}

boost::shared_ptr<Route>
Session::XMLRouteFactory_2X (const XMLNode& node, int version)
{
	boost::shared_ptr<Route> ret;

	if (node.name() != "Route") {
		return ret;
	}

	XMLProperty const * ds_prop = node.property (X_("diskstream-id"));
	if (!ds_prop) {
		ds_prop = node.property (X_("diskstream"));
	}

	DataType type = DataType::AUDIO;
	const XMLProperty* prop = node.property("default-type");

	if (prop) {
		type = DataType (prop->value());
	}

	assert (type != DataType::NIL);

	if (ds_prop) {

		list<boost::shared_ptr<Diskstream> >::iterator i = _diskstreams_2X.begin ();
		while (i != _diskstreams_2X.end() && (*i)->id() != ds_prop->value()) {
			++i;
		}

		if (i == _diskstreams_2X.end()) {
			error << _("Could not find diskstream for route") << endmsg;
			return boost::shared_ptr<Route> ();
		}

		boost::shared_ptr<Track> track;

                if (type == DataType::AUDIO) {
                        track.reset (new AudioTrack (*this, X_("toBeResetFroXML")));
                } else {
                        track.reset (new MidiTrack (*this, X_("toBeResetFroXML")));
                }

                if (track->init()) {
                        return ret;
                }

                if (track->set_state (node, version)) {
                        return ret;
                }

		track->set_diskstream (*i);

#ifdef BOOST_SP_ENABLE_DEBUG_HOOKS
                // boost_debug_shared_ptr_mark_interesting (track.get(), "Track");
#endif
                ret = track;

	} else {
		enum Route::Flag flags = Route::Flag(0);
		const XMLProperty* prop = node.property("flags");
		if (prop) {
			flags = Route::Flag (string_2_enum (prop->value(), flags));
		}

		boost::shared_ptr<Route> r (new Route (*this, X_("toBeResetFroXML"), flags));

                if (r->init () == 0 && r->set_state (node, version) == 0) {
#ifdef BOOST_SP_ENABLE_DEBUG_HOOKS
                        // boost_debug_shared_ptr_mark_interesting (r.get(), "Route");
#endif
                        ret = r;
                }
	}

	return ret;
}

int
Session::load_regions (const XMLNode& node)
{
	XMLNodeList nlist;
	XMLNodeConstIterator niter;
	boost::shared_ptr<Region> region;

	nlist = node.children();

	set_dirty();

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
		if ((region = XMLRegionFactory (**niter, false)) == 0) {
			error << _("Session: cannot create Region from XML description.");
			const XMLProperty *name = (**niter).property("name");

			if (name) {
				error << " " << string_compose (_("Can not load state for region '%1'"), name->value());
			}

			error << endmsg;
		}
	}

	return 0;
}

int
Session::load_compounds (const XMLNode& node)
{
	XMLNodeList calist = node.children();
	XMLNodeConstIterator caiter;
	XMLProperty *caprop;

	for (caiter = calist.begin(); caiter != calist.end(); ++caiter) {
		XMLNode* ca = *caiter;
		ID orig_id;
		ID copy_id;

		if ((caprop = ca->property (X_("original"))) == 0) {
			continue;
		}
		orig_id = caprop->value();

		if ((caprop = ca->property (X_("copy"))) == 0) {
			continue;
		}
		copy_id = caprop->value();

		boost::shared_ptr<Region> orig = RegionFactory::region_by_id (orig_id);
		boost::shared_ptr<Region> copy = RegionFactory::region_by_id (copy_id);

		if (!orig || !copy) {
			warning << string_compose (_("Regions in compound description not found (ID's %1 and %2): ignored"),
						   orig_id, copy_id)
				<< endmsg;
			continue;
		}

		RegionFactory::add_compound_association (orig, copy);
	}

	return 0;
}

void
Session::load_nested_sources (const XMLNode& node)
{
	XMLNodeList nlist;
	XMLNodeConstIterator niter;

	nlist = node.children();

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
		if ((*niter)->name() == "Source") {

			/* it may already exist, so don't recreate it unnecessarily
			 */

			XMLProperty* prop = (*niter)->property (X_("id"));
			if (!prop) {
				error << _("Nested source has no ID info in session file! (ignored)") << endmsg;
				continue;
			}

			ID source_id (prop->value());

			if (!source_by_id (source_id)) {

				try {
					SourceFactory::create (*this, **niter, true);
				}
				catch (failed_constructor& err) {
					error << string_compose (_("Cannot reconstruct nested source for region %1"), name()) << endmsg;
				}
			}
		}
	}
}

boost::shared_ptr<Region>
Session::XMLRegionFactory (const XMLNode& node, bool full)
{
	const XMLProperty* type = node.property("type");

	try {

		const XMLNodeList& nlist = node.children();

		for (XMLNodeConstIterator niter = nlist.begin(); niter != nlist.end(); ++niter) {
			XMLNode *child = (*niter);
			if (child->name() == "NestedSource") {
				load_nested_sources (*child);
			}
		}

                if (!type || type->value() == "audio") {
                        return boost::shared_ptr<Region>(XMLAudioRegionFactory (node, full));
                } else if (type->value() == "midi") {
                        return boost::shared_ptr<Region>(XMLMidiRegionFactory (node, full));
                }

	} catch (failed_constructor& err) {
		return boost::shared_ptr<Region> ();
	}

	return boost::shared_ptr<Region> ();
}

boost::shared_ptr<AudioRegion>
Session::XMLAudioRegionFactory (const XMLNode& node, bool /*full*/)
{
	const XMLProperty* prop;
	boost::shared_ptr<Source> source;
	boost::shared_ptr<AudioSource> as;
	SourceList sources;
	SourceList master_sources;
	uint32_t nchans = 1;
	char buf[128];

	if (node.name() != X_("Region")) {
		return boost::shared_ptr<AudioRegion>();
	}

	if ((prop = node.property (X_("channels"))) != 0) {
		nchans = atoi (prop->value().c_str());
	}

	if ((prop = node.property ("name")) == 0) {
		cerr << "no name for this region\n";
		abort ();
	}

	if ((prop = node.property (X_("source-0"))) == 0) {
		if ((prop = node.property ("source")) == 0) {
			error << _("Session: XMLNode describing a AudioRegion is incomplete (no source)") << endmsg;
			return boost::shared_ptr<AudioRegion>();
		}
	}

	PBD::ID s_id (prop->value());

	if ((source = source_by_id (s_id)) == 0) {
		error << string_compose(_("Session: XMLNode describing a AudioRegion references an unknown source id =%1"), s_id) << endmsg;
		return boost::shared_ptr<AudioRegion>();
	}

	as = boost::dynamic_pointer_cast<AudioSource>(source);
	if (!as) {
		error << string_compose(_("Session: XMLNode describing a AudioRegion references a non-audio source id =%1"), s_id) << endmsg;
		return boost::shared_ptr<AudioRegion>();
	}

	sources.push_back (as);

	/* pickup other channels */

	for (uint32_t n=1; n < nchans; ++n) {
		snprintf (buf, sizeof(buf), X_("source-%d"), n);
		if ((prop = node.property (buf)) != 0) {

			PBD::ID id2 (prop->value());

			if ((source = source_by_id (id2)) == 0) {
				error << string_compose(_("Session: XMLNode describing a AudioRegion references an unknown source id =%1"), id2) << endmsg;
				return boost::shared_ptr<AudioRegion>();
			}

			as = boost::dynamic_pointer_cast<AudioSource>(source);
			if (!as) {
				error << string_compose(_("Session: XMLNode describing a AudioRegion references a non-audio source id =%1"), id2) << endmsg;
				return boost::shared_ptr<AudioRegion>();
			}
			sources.push_back (as);
		}
	}

	for (uint32_t n = 0; n < nchans; ++n) {
		snprintf (buf, sizeof(buf), X_("master-source-%d"), n);
		if ((prop = node.property (buf)) != 0) {

			PBD::ID id2 (prop->value());

			if ((source = source_by_id (id2)) == 0) {
				error << string_compose(_("Session: XMLNode describing a AudioRegion references an unknown source id =%1"), id2) << endmsg;
				return boost::shared_ptr<AudioRegion>();
			}

			as = boost::dynamic_pointer_cast<AudioSource>(source);
			if (!as) {
				error << string_compose(_("Session: XMLNode describing a AudioRegion references a non-audio source id =%1"), id2) << endmsg;
				return boost::shared_ptr<AudioRegion>();
			}
			master_sources.push_back (as);
		}
	}

	try {
		boost::shared_ptr<AudioRegion> region (boost::dynamic_pointer_cast<AudioRegion> (RegionFactory::create (sources, node)));

		/* a final detail: this is the one and only place that we know how long missing files are */

		if (region->whole_file()) {
			for (SourceList::iterator sx = sources.begin(); sx != sources.end(); ++sx) {
				boost::shared_ptr<SilentFileSource> sfp = boost::dynamic_pointer_cast<SilentFileSource> (*sx);
				if (sfp) {
					sfp->set_length (region->length());
				}
			}
		}

		if (!master_sources.empty()) {
			if (master_sources.size() != nchans) {
				error << _("Session: XMLNode describing an AudioRegion is missing some master sources; ignored") << endmsg;
			} else {
				region->set_master_sources (master_sources);
			}
		}

		return region;

	}

	catch (failed_constructor& err) {
		return boost::shared_ptr<AudioRegion>();
	}
}

boost::shared_ptr<MidiRegion>
Session::XMLMidiRegionFactory (const XMLNode& node, bool /*full*/)
{
	const XMLProperty* prop;
	boost::shared_ptr<Source> source;
	boost::shared_ptr<MidiSource> ms;
	SourceList sources;

	if (node.name() != X_("Region")) {
		return boost::shared_ptr<MidiRegion>();
	}

	if ((prop = node.property ("name")) == 0) {
		cerr << "no name for this region\n";
		abort ();
	}

	if ((prop = node.property (X_("source-0"))) == 0) {
		if ((prop = node.property ("source")) == 0) {
			error << _("Session: XMLNode describing a MidiRegion is incomplete (no source)") << endmsg;
			return boost::shared_ptr<MidiRegion>();
		}
	}

	PBD::ID s_id (prop->value());

	if ((source = source_by_id (s_id)) == 0) {
		error << string_compose(_("Session: XMLNode describing a MidiRegion references an unknown source id =%1"), s_id) << endmsg;
		return boost::shared_ptr<MidiRegion>();
	}

	ms = boost::dynamic_pointer_cast<MidiSource>(source);
	if (!ms) {
		error << string_compose(_("Session: XMLNode describing a MidiRegion references a non-midi source id =%1"), s_id) << endmsg;
		return boost::shared_ptr<MidiRegion>();
	}

	sources.push_back (ms);

	try {
		boost::shared_ptr<MidiRegion> region (boost::dynamic_pointer_cast<MidiRegion> (RegionFactory::create (sources, node)));
		/* a final detail: this is the one and only place that we know how long missing files are */

		if (region->whole_file()) {
			for (SourceList::iterator sx = sources.begin(); sx != sources.end(); ++sx) {
				boost::shared_ptr<SilentFileSource> sfp = boost::dynamic_pointer_cast<SilentFileSource> (*sx);
				if (sfp) {
					sfp->set_length (region->length());
				}
			}
		}

		return region;
	}

	catch (failed_constructor& err) {
		return boost::shared_ptr<MidiRegion>();
	}
}

XMLNode&
Session::get_sources_as_xml ()

{
	XMLNode* node = new XMLNode (X_("Sources"));
	Glib::Threads::Mutex::Lock lm (source_lock);

	for (SourceMap::iterator i = sources.begin(); i != sources.end(); ++i) {
		node->add_child_nocopy (i->second->get_state());
	}

	return *node;
}

void
Session::reset_write_sources (bool mark_write_complete, bool force)
{
	boost::shared_ptr<RouteList> rl = routes.reader();
	for (RouteList::iterator i = rl->begin(); i != rl->end(); ++i) {
		boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
		if (tr) {
			_state_of_the_state = StateOfTheState (_state_of_the_state|InCleanup);
			tr->reset_write_sources(mark_write_complete, force);
			_state_of_the_state = StateOfTheState (_state_of_the_state & ~InCleanup);
		}
	}
}

int
Session::load_sources (const XMLNode& node)
{
	XMLNodeList nlist;
	XMLNodeConstIterator niter;
	boost::shared_ptr<Source> source; /* don't need this but it stops some
					   * versions of gcc complaining about
					   * discarded return values.
					   */

	nlist = node.children();

	set_dirty();

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
          retry:
		try {
			if ((source = XMLSourceFactory (**niter)) == 0) {
				error << _("Session: cannot create Source from XML description.") << endmsg;
			}

		} catch (MissingSource& err) {

                        int user_choice;

			if (err.type == DataType::MIDI && Glib::path_is_absolute (err.path)) {
				error << string_compose (_("A external MIDI file is missing. %1 cannot currently recover from missing external MIDI files"),
							 PROGRAM_NAME) << endmsg;
				return -1;
			}

                        if (!no_questions_about_missing_files) {
				user_choice = MissingFile (this, err.path, err.type).get_value_or (-1);
			} else {
                                user_choice = -2;
                        }

                        switch (user_choice) {
                        case 0:
                                /* user added a new search location, so try again */
                                goto retry;


                        case 1:
                                /* user asked to quit the entire session load
                                 */
                                return -1;

                        case 2:
                                no_questions_about_missing_files = true;
                                goto retry;

                        case 3:
                                no_questions_about_missing_files = true;
                                /* fallthru */

                        case -1:
                        default:
				switch (err.type) {

				case DataType::AUDIO:
					source = SourceFactory::createSilent (*this, **niter, max_framecnt, _current_frame_rate);
					break;

				case DataType::MIDI:
					/* The MIDI file is actually missing so
					 * just create a new one in the same
					 * location. Do not announce its
					 */
					string fullpath;

					if (!Glib::path_is_absolute (err.path)) {
						fullpath = Glib::build_filename (source_search_path (DataType::MIDI).front(), err.path);
					} else {
						/* this should be an unrecoverable error: we would be creating a MIDI file outside
						   the session tree.
						*/
						return -1;
					}
					/* Note that we do not announce the source just yet - we need to reset its ID before we do that */
					source = SourceFactory::createWritable (DataType::MIDI, *this, fullpath, false, _current_frame_rate, false, false);
					/* reset ID to match the missing one */
					source->set_id (**niter);
					/* Now we can announce it */
					SourceFactory::SourceCreated (source);
					break;
				}
                                break;
                        }
		}
	}

	return 0;
}

boost::shared_ptr<Source>
Session::XMLSourceFactory (const XMLNode& node)
{
	if (node.name() != "Source") {
		return boost::shared_ptr<Source>();
	}

	try {
		/* note: do peak building in another thread when loading session state */
		return SourceFactory::create (*this, node, true);
	}

	catch (failed_constructor& err) {
		error << string_compose (_("Found a sound file that cannot be used by %1. Talk to the programmers."), PROGRAM_NAME) << endmsg;
		return boost::shared_ptr<Source>();
	}
}

int
Session::save_template (string template_name)
{
	if ((_state_of_the_state & CannotSave) || template_name.empty ()) {
		return -1;
	}

	bool absolute_path = Glib::path_is_absolute (template_name);

	/* directory to put the template in */
	std::string template_dir_path;

	if (!absolute_path) {
		std::string user_template_dir(user_template_directory());

		if (g_mkdir_with_parents (user_template_dir.c_str(), 0755) != 0) {
			error << string_compose(_("Could not create templates directory \"%1\" (%2)"),
					user_template_dir, g_strerror (errno)) << endmsg;
			return -1;
		}

		template_dir_path = Glib::build_filename (user_template_dir, template_name);
	} else {
		template_dir_path = template_name;
	}

	if (!ARDOUR::Profile->get_trx()) {
		if (Glib::file_test (template_dir_path, Glib::FILE_TEST_EXISTS)) {
			warning << string_compose(_("Template \"%1\" already exists - new version not created"),
									  template_dir_path) << endmsg;
			return -1;
		}

		if (g_mkdir_with_parents (template_dir_path.c_str(), 0755) != 0) {
			error << string_compose(_("Could not create directory for Session template\"%1\" (%2)"),
									template_dir_path, g_strerror (errno)) << endmsg;
			return -1;
		}
	}

	/* file to write */
	std::string template_file_path;

	if (ARDOUR::Profile->get_trx()) {
		template_file_path = template_name;
	} else {
		if (absolute_path) {
			template_file_path = Glib::build_filename (template_dir_path, Glib::path_get_basename (template_dir_path) + template_suffix);
		} else {
			template_file_path = Glib::build_filename (template_dir_path, template_name + template_suffix);
		}
	}

	SessionSaveUnderway (); /* EMIT SIGNAL */

	XMLTree tree;

	tree.set_root (&get_template());
	if (!tree.write (template_file_path)) {
		error << _("template not saved") << endmsg;
		return -1;
	}

	if (!ARDOUR::Profile->get_trx()) {
		/* copy plugin state directory */

		std::string template_plugin_state_path (Glib::build_filename (template_dir_path, X_("plugins")));

		if (g_mkdir_with_parents (template_plugin_state_path.c_str(), 0755) != 0) {
			error << string_compose(_("Could not create directory for Session template plugin state\"%1\" (%2)"),
									template_plugin_state_path, g_strerror (errno)) << endmsg;
			return -1;
		}
		copy_files (plugins_dir(), template_plugin_state_path);
	}

	store_recent_templates (template_file_path);

	return 0;
}

void
Session::refresh_disk_space ()
{
#if __APPLE__ || (HAVE_SYS_VFS_H && HAVE_SYS_STATVFS_H)

	Glib::Threads::Mutex::Lock lm (space_lock);

	/* get freespace on every FS that is part of the session path */

	_total_free_4k_blocks = 0;
	_total_free_4k_blocks_uncertain = false;

	for (vector<space_and_path>::iterator i = session_dirs.begin(); i != session_dirs.end(); ++i) {

		struct statfs statfsbuf;
		statfs (i->path.c_str(), &statfsbuf);

		double const scale = statfsbuf.f_bsize / 4096.0;

		/* See if this filesystem is read-only */
		struct statvfs statvfsbuf;
		statvfs (i->path.c_str(), &statvfsbuf);

		/* f_bavail can be 0 if it is undefined for whatever
		   filesystem we are looking at; Samba shares mounted
		   via GVFS are an example of this.
		*/
		if (statfsbuf.f_bavail == 0) {
			/* block count unknown */
			i->blocks = 0;
			i->blocks_unknown = true;
		} else if (statvfsbuf.f_flag & ST_RDONLY) {
			/* read-only filesystem */
			i->blocks = 0;
			i->blocks_unknown = false;
		} else {
			/* read/write filesystem with known space */
			i->blocks = (uint32_t) floor (statfsbuf.f_bavail * scale);
			i->blocks_unknown = false;
		}

		_total_free_4k_blocks += i->blocks;
		if (i->blocks_unknown) {
			_total_free_4k_blocks_uncertain = true;
		}
	}
#elif defined PLATFORM_WINDOWS
	vector<string> scanned_volumes;
	vector<string>::iterator j;
	vector<space_and_path>::iterator i;
    DWORD nSectorsPerCluster, nBytesPerSector,
          nFreeClusters, nTotalClusters;
    char disk_drive[4];
	bool volume_found;

	_total_free_4k_blocks = 0;

	for (i = session_dirs.begin(); i != session_dirs.end(); i++) {
		strncpy (disk_drive, (*i).path.c_str(), 3);
		disk_drive[3] = 0;
		strupr(disk_drive);

		volume_found = false;
		if (0 != (GetDiskFreeSpace(disk_drive, &nSectorsPerCluster, &nBytesPerSector, &nFreeClusters, &nTotalClusters)))
		{
			int64_t nBytesPerCluster = nBytesPerSector * nSectorsPerCluster;
			int64_t nFreeBytes = nBytesPerCluster * (int64_t)nFreeClusters;
			i->blocks = (uint32_t)(nFreeBytes / 4096);

			for (j = scanned_volumes.begin(); j != scanned_volumes.end(); j++) {
				if (0 == j->compare(disk_drive)) {
					volume_found = true;
					break;
				}
			}

			if (!volume_found) {
				scanned_volumes.push_back(disk_drive);
				_total_free_4k_blocks += i->blocks;
			}
		}
	}

	if (0 == _total_free_4k_blocks) {
		strncpy (disk_drive, path().c_str(), 3);
		disk_drive[3] = 0;

		if (0 != (GetDiskFreeSpace(disk_drive, &nSectorsPerCluster, &nBytesPerSector, &nFreeClusters, &nTotalClusters)))
		{
			int64_t nBytesPerCluster = nBytesPerSector * nSectorsPerCluster;
			int64_t nFreeBytes = nBytesPerCluster * (int64_t)nFreeClusters;
			_total_free_4k_blocks = (uint32_t)(nFreeBytes / 4096);
		}
	}
#endif
}

string
Session::get_best_session_directory_for_new_audio ()
{
	vector<space_and_path>::iterator i;
	string result = _session_dir->root_path();

	/* handle common case without system calls */

	if (session_dirs.size() == 1) {
		return result;
	}

	/* OK, here's the algorithm we're following here:

	We want to select which directory to use for
	the next file source to be created. Ideally,
	we'd like to use a round-robin process so as to
	get maximum performance benefits from splitting
	the files across multiple disks.

	However, in situations without much diskspace, an
	RR approach may end up filling up a filesystem
	with new files while others still have space.
	Its therefore important to pay some attention to
	the freespace in the filesystem holding each
	directory as well. However, if we did that by
	itself, we'd keep creating new files in the file
	system with the most space until it was as full
	as all others, thus negating any performance
	benefits of this RAID-1 like approach.

	So, we use a user-configurable space threshold. If
	there are at least 2 filesystems with more than this
	much space available, we use RR selection between them.
	If not, then we pick the filesystem with the most space.

	This gets a good balance between the two
	approaches.
	*/

	refresh_disk_space ();

	int free_enough = 0;

	for (i = session_dirs.begin(); i != session_dirs.end(); ++i) {
		if ((*i).blocks * 4096 >= Config->get_disk_choice_space_threshold()) {
			free_enough++;
		}
	}

	if (free_enough >= 2) {
		/* use RR selection process, ensuring that the one
		   picked works OK.
		*/

		i = last_rr_session_dir;

		do {
			if (++i == session_dirs.end()) {
				i = session_dirs.begin();
			}

			if ((*i).blocks * 4096 >= Config->get_disk_choice_space_threshold()) {
				SessionDirectory sdir(i->path);
				if (sdir.create ()) {
					result = (*i).path;
					last_rr_session_dir = i;
					return result;
				}
			}

		} while (i != last_rr_session_dir);

	} else {

		/* pick FS with the most freespace (and that
		   seems to actually work ...)
		*/

		vector<space_and_path> sorted;
		space_and_path_ascending_cmp cmp;

		sorted = session_dirs;
		sort (sorted.begin(), sorted.end(), cmp);

		for (i = sorted.begin(); i != sorted.end(); ++i) {
			SessionDirectory sdir(i->path);
			if (sdir.create ()) {
				result = (*i).path;
				last_rr_session_dir = i;
				return result;
			}
		}
	}

	return result;
}

string
Session::automation_dir () const
{
	return Glib::build_filename (_path, automation_dir_name);
}

string
Session::analysis_dir () const
{
	return Glib::build_filename (_path, analysis_dir_name);
}

string
Session::plugins_dir () const
{
	return Glib::build_filename (_path, plugins_dir_name);
}

string
Session::externals_dir () const
{
	return Glib::build_filename (_path, externals_dir_name);
}

int
Session::load_bundles (XMLNode const & node)
{
	XMLNodeList nlist = node.children();
	XMLNodeConstIterator niter;

	set_dirty();

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
		if ((*niter)->name() == "InputBundle") {
			add_bundle (boost::shared_ptr<UserBundle> (new UserBundle (**niter, true)));
		} else if ((*niter)->name() == "OutputBundle") {
			add_bundle (boost::shared_ptr<UserBundle> (new UserBundle (**niter, false)));
		} else {
			error << string_compose(_("Unknown node \"%1\" found in Bundles list from session file"), (*niter)->name()) << endmsg;
			return -1;
		}
	}

	return 0;
}

int
Session::load_route_groups (const XMLNode& node, int version)
{
	XMLNodeList nlist = node.children();
	XMLNodeConstIterator niter;

	set_dirty ();

	if (version >= 3000) {

		for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
			if ((*niter)->name() == "RouteGroup") {
				RouteGroup* rg = new RouteGroup (*this, "");
				add_route_group (rg);
				rg->set_state (**niter, version);
			}
		}

	} else if (version < 3000) {

		for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
			if ((*niter)->name() == "EditGroup" || (*niter)->name() == "MixGroup") {
				RouteGroup* rg = new RouteGroup (*this, "");
				add_route_group (rg);
				rg->set_state (**niter, version);
			}
		}
	}

	return 0;
}

static bool
state_file_filter (const string &str, void* /*arg*/)
{
	return (str.length() > strlen(statefile_suffix) &&
		str.find (statefile_suffix) == (str.length() - strlen (statefile_suffix)));
}

static string
remove_end(string state)
{
	string statename(state);

	string::size_type start,end;
	if ((start = statename.find_last_of (G_DIR_SEPARATOR)) != string::npos) {
		statename = statename.substr (start+1);
	}

	if ((end = statename.rfind(statefile_suffix)) == string::npos) {
		end = statename.length();
	}

	return string(statename.substr (0, end));
}

vector<string>
Session::possible_states (string path)
{
	vector<string> states;
	find_files_matching_filter (states, path, state_file_filter, 0, false, false);

	transform(states.begin(), states.end(), states.begin(), remove_end);

	sort (states.begin(), states.end());

	return states;
}

vector<string>
Session::possible_states () const
{
	return possible_states(_path);
}

void
Session::add_route_group (RouteGroup* g)
{
	_route_groups.push_back (g);
	route_group_added (g); /* EMIT SIGNAL */

	g->RouteAdded.connect_same_thread (*this, boost::bind (&Session::route_added_to_route_group, this, _1, _2));
	g->RouteRemoved.connect_same_thread (*this, boost::bind (&Session::route_removed_from_route_group, this, _1, _2));
	g->PropertyChanged.connect_same_thread (*this, boost::bind (&Session::route_group_property_changed, this, g));

	set_dirty ();
}

void
Session::remove_route_group (RouteGroup& rg)
{
	list<RouteGroup*>::iterator i;

	if ((i = find (_route_groups.begin(), _route_groups.end(), &rg)) != _route_groups.end()) {
		_route_groups.erase (i);
		delete &rg;

		route_group_removed (); /* EMIT SIGNAL */
	}
}

/** Set a new order for our route groups, without adding or removing any.
 *  @param groups Route group list in the new order.
 */
void
Session::reorder_route_groups (list<RouteGroup*> groups)
{
	_route_groups = groups;

	route_groups_reordered (); /* EMIT SIGNAL */
	set_dirty ();
}


RouteGroup *
Session::route_group_by_name (string name)
{
	list<RouteGroup *>::iterator i;

	for (i = _route_groups.begin(); i != _route_groups.end(); ++i) {
		if ((*i)->name() == name) {
			return* i;
		}
	}
	return 0;
}

RouteGroup&
Session::all_route_group() const
{
        return *_all_route_group;
}

void
Session::add_commands (vector<Command*> const & cmds)
{
	for (vector<Command*>::const_iterator i = cmds.begin(); i != cmds.end(); ++i) {
		add_command (*i);
	}
}

void
Session::add_command (Command* const cmd)
{
	assert (_current_trans);
	DEBUG_UNDO_HISTORY (
	    string_compose ("Current Undo Transaction %1, adding command: %2",
	                    _current_trans->name (),
	                    cmd->name ()));
	_current_trans->add_command (cmd);
}
void
Session::begin_reversible_command (const string& name)
{
	begin_reversible_command (g_quark_from_string (name.c_str ()));
}

/** Begin a reversible command using a GQuark to identify it.
 *  begin_reversible_command() and commit_reversible_command() calls may be nested,
 *  but there must be as many begin...()s as there are commit...()s.
 */
void
Session::begin_reversible_command (GQuark q)
{
	/* If nested begin/commit pairs are used, we create just one UndoTransaction
	   to hold all the commands that are committed.  This keeps the order of
	   commands correct in the history.
	*/

	if (_current_trans == 0) {
		DEBUG_UNDO_HISTORY (string_compose (
		    "Begin Reversible Command, new transaction: %1", g_quark_to_string (q)));

		/* start a new transaction */
		assert (_current_trans_quarks.empty ());
		_current_trans = new UndoTransaction();
		_current_trans->set_name (g_quark_to_string (q));
	} else {
		DEBUG_UNDO_HISTORY (
		    string_compose ("Begin Reversible Command, current transaction: %1",
		                    _current_trans->name ()));
	}

	_current_trans_quarks.push_front (q);
}

void
Session::abort_reversible_command ()
{
	if (_current_trans != 0) {
		DEBUG_UNDO_HISTORY (
		    string_compose ("Abort Reversible Command: %1", _current_trans->name ()));
		_current_trans->clear();
		delete _current_trans;
		_current_trans = 0;
		_current_trans_quarks.clear();
	}
}

void
Session::commit_reversible_command (Command *cmd)
{
	assert (_current_trans);
	assert (!_current_trans_quarks.empty ());

	struct timeval now;

	if (cmd) {
		DEBUG_UNDO_HISTORY (
		    string_compose ("Current Undo Transaction %1, adding command: %2",
		                    _current_trans->name (),
		                    cmd->name ()));
		_current_trans->add_command (cmd);
	}

	DEBUG_UNDO_HISTORY (
	    string_compose ("Commit Reversible Command, current transaction: %1",
	                    _current_trans->name ()));

	_current_trans_quarks.pop_front ();

	if (!_current_trans_quarks.empty ()) {
		DEBUG_UNDO_HISTORY (
		    string_compose ("Commit Reversible Command, transaction is not "
		                    "top-level, current transaction: %1",
		                    _current_trans->name ()));
		/* the transaction we're committing is not the top-level one */
		return;
	}

	if (_current_trans->empty()) {
		/* no commands were added to the transaction, so just get rid of it */
		DEBUG_UNDO_HISTORY (
		    string_compose ("Commit Reversible Command, No commands were "
		                    "added to current transaction: %1",
		                    _current_trans->name ()));
		delete _current_trans;
		_current_trans = 0;
		return;
	}

	gettimeofday (&now, 0);
	_current_trans->set_timestamp (now);

	_history.add (_current_trans);
	_current_trans = 0;
}

static bool
accept_all_audio_files (const string& path, void* /*arg*/)
{
        if (!Glib::file_test (path, Glib::FILE_TEST_IS_REGULAR)) {
                return false;
        }

        if (!AudioFileSource::safe_audio_file_extension (path)) {
                return false;
        }

        return true;
}

static bool
accept_all_midi_files (const string& path, void* /*arg*/)
{
        if (!Glib::file_test (path, Glib::FILE_TEST_IS_REGULAR)) {
                return false;
        }

	return ((path.length() > 4 && path.find (".mid") != (path.length() - 4)) ||
                (path.length() > 4 && path.find (".smf") != (path.length() - 4)) ||
                (path.length() > 5 && path.find (".midi") != (path.length() - 5)));
}

static bool
accept_all_state_files (const string& path, void* /*arg*/)
{
	if (!Glib::file_test (path, Glib::FILE_TEST_IS_REGULAR)) {
		return false;
	}

	std::string const statefile_ext (statefile_suffix);
	if (path.length() >= statefile_ext.length()) {
		return (0 == path.compare (path.length() - statefile_ext.length(), statefile_ext.length(), statefile_ext));
	} else {
		return false;
	}
}

int
Session::find_all_sources (string path, set<string>& result)
{
	XMLTree tree;
	XMLNode* node;

	if (!tree.read (path)) {
		return -1;
	}

	if ((node = find_named_node (*tree.root(), "Sources")) == 0) {
		return -2;
	}

	XMLNodeList nlist;
	XMLNodeConstIterator niter;

	nlist = node->children();

	set_dirty();

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {

		XMLProperty* prop;

		if ((prop = (*niter)->property (X_("type"))) == 0) {
			continue;
		}

		DataType type (prop->value());

		if ((prop = (*niter)->property (X_("name"))) == 0) {
			continue;
		}

		if (Glib::path_is_absolute (prop->value())) {
			/* external file, ignore */
			continue;
		}

		string found_path;
		bool is_new;
		uint16_t chan;

		if (FileSource::find (*this, type, prop->value(), true, is_new, chan, found_path)) {
			result.insert (found_path);
		}
	}

	return 0;
}

int
Session::find_all_sources_across_snapshots (set<string>& result, bool exclude_this_snapshot)
{
	vector<string> state_files;
	string ripped;
	string this_snapshot_path;

	result.clear ();

	ripped = _path;

	if (ripped[ripped.length()-1] == G_DIR_SEPARATOR) {
		ripped = ripped.substr (0, ripped.length() - 1);
	}

	find_files_matching_filter (state_files, ripped, accept_all_state_files, (void *) 0, true, true);

	if (state_files.empty()) {
		/* impossible! */
		return 0;
	}

	this_snapshot_path = _path;
	this_snapshot_path += legalize_for_path (_current_snapshot_name);
	this_snapshot_path += statefile_suffix;

	for (vector<string>::iterator i = state_files.begin(); i != state_files.end(); ++i) {

		if (exclude_this_snapshot && *i == this_snapshot_path) {
			continue;
		}

		if (find_all_sources (*i, result) < 0) {
			return -1;
		}
	}

	return 0;
}

struct RegionCounter {
    typedef std::map<PBD::ID,boost::shared_ptr<AudioSource> > AudioSourceList;
    AudioSourceList::iterator iter;
    boost::shared_ptr<Region> region;
    uint32_t count;

    RegionCounter() : count (0) {}
};

int
Session::ask_about_playlist_deletion (boost::shared_ptr<Playlist> p)
{
        boost::optional<int> r = AskAboutPlaylistDeletion (p);
	return r.get_value_or (1);
}

void
Session::cleanup_regions ()
{
	bool removed = false;
	const RegionFactory::RegionMap& regions (RegionFactory::regions());

	for (RegionFactory::RegionMap::const_iterator i = regions.begin(); i != regions.end();) {

		uint32_t used = playlists->region_use_count (i->second);

		if (used == 0 && !i->second->automatic ()) {
			boost::weak_ptr<Region> w = i->second;
			++i;
			removed = true;
			RegionFactory::map_remove (w);
		} else {
			++i;
		}
	}

	if (removed) {
		// re-check to remove parent references of compound regions
		for (RegionFactory::RegionMap::const_iterator i = regions.begin(); i != regions.end();) {
			if (!(i->second->whole_file() && i->second->max_source_level() > 0)) {
				++i;
				continue;
			}
			assert(boost::dynamic_pointer_cast<PlaylistSource>(i->second->source (0)) != 0);
			if (0 == playlists->region_use_count (i->second)) {
				boost::weak_ptr<Region> w = i->second;
				++i;
				RegionFactory::map_remove (w);
			} else {
				++i;
			}
		}
	}

	/* dump the history list */
	_history.clear ();

	save_state ("");
}

bool
Session::can_cleanup_peakfiles () const
{
	if (deletion_in_progress()) {
		return false;
	}
	if (!_writable || (_state_of_the_state & CannotSave)) {
		warning << _("Cannot cleanup peak-files for read-only session.") << endmsg;
		return false;
	}
        if (record_status() == Recording) {
		error << _("Cannot cleanup peak-files while recording") << endmsg;
		return false;
	}
	return true;
}

int
Session::cleanup_peakfiles ()
{
	Glib::Threads::Mutex::Lock lm (peak_cleanup_lock, Glib::Threads::TRY_LOCK);
	if (!lm.locked()) {
		return -1;
	}

	assert (can_cleanup_peakfiles ());
	assert (!peaks_cleanup_in_progres());

	_state_of_the_state = StateOfTheState (_state_of_the_state | PeakCleanup);

	int timeout = 5000; // 5 seconds
	while (!SourceFactory::files_with_peaks.empty()) {
		Glib::usleep (1000);
		if (--timeout < 0) {
			warning << _("Timeout waiting for peak-file creation to terminate before cleanup, please try again later.") << endmsg;
			_state_of_the_state = StateOfTheState (_state_of_the_state & (~PeakCleanup));
			return -1;
		}
	}

	for (SourceMap::iterator i = sources.begin(); i != sources.end(); ++i) {
		boost::shared_ptr<AudioSource> as;
		if ((as = boost::dynamic_pointer_cast<AudioSource> (i->second)) != 0) {
			as->close_peakfile();
		}
	}

	PBD::clear_directory (session_directory().peak_path());

	_state_of_the_state = StateOfTheState (_state_of_the_state & (~PeakCleanup));

	for (SourceMap::iterator i = sources.begin(); i != sources.end(); ++i) {
		boost::shared_ptr<AudioSource> as;
		if ((as = boost::dynamic_pointer_cast<AudioSource> (i->second)) != 0) {
			SourceFactory::setup_peakfile(as, true);
		}
	}
	return 0;
}

int
Session::cleanup_sources (CleanupReport& rep)
{
	// FIXME: needs adaptation to midi

	vector<boost::shared_ptr<Source> > dead_sources;
	string audio_path;
	string midi_path;
	vector<string> candidates;
	vector<string> unused;
	set<string> all_sources;
	bool used;
	string spath;
	int ret = -1;
	string tmppath1;
	string tmppath2;
	Searchpath asp;
	Searchpath msp;

	_state_of_the_state = (StateOfTheState) (_state_of_the_state | InCleanup);

	/* this is mostly for windows which doesn't allow file
	 * renaming if the file is in use. But we don't special
	 * case it because we need to know if this causes
	 * problems, and the easiest way to notice that is to
	 * keep it in place for all platforms.
	 */

	request_stop (false);
	_butler->summon ();
	_butler->wait_until_finished ();

	/* consider deleting all unused playlists */

	if (playlists->maybe_delete_unused (boost::bind (Session::ask_about_playlist_deletion, _1))) {
		ret = 0;
		goto out;
	}

        /* sync the "all regions" property of each playlist with its current state
         */

        playlists->sync_all_regions_with_regions ();

	/* find all un-used sources */

	rep.paths.clear ();
	rep.space = 0;

	for (SourceMap::iterator i = sources.begin(); i != sources.end(); ) {

		SourceMap::iterator tmp;

		tmp = i;
		++tmp;

		/* do not bother with files that are zero size, otherwise we remove the current "nascent"
		   capture files.
		*/

		if (!i->second->used() && (i->second->length(i->second->timeline_position() > 0))) {
			dead_sources.push_back (i->second);
			i->second->drop_references ();
		}

		i = tmp;
	}

	/* build a list of all the possible audio directories for the session */

	for (vector<space_and_path>::const_iterator i = session_dirs.begin(); i != session_dirs.end(); ++i) {
		SessionDirectory sdir ((*i).path);
		asp += sdir.sound_path();
	}
	audio_path += asp.to_string();


	/* build a list of all the possible midi directories for the session */

	for (vector<space_and_path>::const_iterator i = session_dirs.begin(); i != session_dirs.end(); ++i) {
		SessionDirectory sdir ((*i).path);
		msp += sdir.midi_path();
	}
	midi_path += msp.to_string();

	find_files_matching_filter (candidates, audio_path, accept_all_audio_files, (void *) 0, true, true);
	find_files_matching_filter (candidates, midi_path, accept_all_midi_files, (void *) 0, true, true);

	/* find all sources, but don't use this snapshot because the
	   state file on disk still references sources we may have already
	   dropped.
	*/

	find_all_sources_across_snapshots (all_sources, true);

	/*  add our current source list
	 */

	for (SourceMap::iterator i = sources.begin(); i != sources.end(); ) {
		boost::shared_ptr<FileSource> fs;
                SourceMap::iterator tmp = i;
                ++tmp;

		if ((fs = boost::dynamic_pointer_cast<FileSource> (i->second)) != 0) {

			/* this is mostly for windows which doesn't allow file
			 * renaming if the file is in use. But we don't special
			 * case it because we need to know if this causes
			 * problems, and the easiest way to notice that is to
			 * keep it in place for all platforms.
			 */

			fs->close ();

			if (!fs->is_stub()) {

				if (playlists->source_use_count (fs) != 0) {
					all_sources.insert (fs->path());
				} else {

					/* we might not remove this source from disk, because it may be used
					   by other snapshots, but its not being used in this version
					   so lets get rid of it now, along with any representative regions
					   in the region list.
					*/

					RegionFactory::remove_regions_using_source (i->second);

					// also remove source from all_sources

					for (set<string>::iterator j = all_sources.begin(); j != all_sources.end(); ++j) {
						spath = Glib::path_get_basename (*j);
						if (spath == i->second->name()) {
							all_sources.erase (j);
							break;
						}
					}

					sources.erase (i);
				}
			}
		}

                i = tmp;
	}

	for (vector<string>::iterator x = candidates.begin(); x != candidates.end(); ++x) {

		used = false;
		spath = *x;

		for (set<string>::iterator i = all_sources.begin(); i != all_sources.end(); ++i) {

			tmppath1 = canonical_path (spath);
			tmppath2 = canonical_path ((*i));

			if (tmppath1 == tmppath2) {
				used = true;
				break;
			}
		}

		if (!used) {
			unused.push_back (spath);
		}
	}

	/* now try to move all unused files into the "dead" directory(ies) */

	for (vector<string>::iterator x = unused.begin(); x != unused.end(); ++x) {
		GStatBuf statbuf;

		string newpath;

		/* don't move the file across filesystems, just
		   stick it in the `dead_dir_name' directory
		   on whichever filesystem it was already on.
		*/

		if ((*x).find ("/sounds/") != string::npos) {

			/* old school, go up 1 level */

			newpath = Glib::path_get_dirname (*x);      // "sounds"
			newpath = Glib::path_get_dirname (newpath); // "session-name"

		} else {

			/* new school, go up 4 levels */

			newpath = Glib::path_get_dirname (*x);      // "audiofiles" or "midifiles"
			newpath = Glib::path_get_dirname (newpath); // "session-name"
			newpath = Glib::path_get_dirname (newpath); // "interchange"
			newpath = Glib::path_get_dirname (newpath); // "session-dir"
		}

		newpath = Glib::build_filename (newpath, dead_dir_name);

		if (g_mkdir_with_parents (newpath.c_str(), 0755) < 0) {
			error << string_compose(_("Session: cannot create dead file folder \"%1\" (%2)"), newpath, strerror (errno)) << endmsg;
			return -1;
		}

		newpath = Glib::build_filename (newpath, Glib::path_get_basename ((*x)));

		if (Glib::file_test (newpath, Glib::FILE_TEST_EXISTS)) {

			/* the new path already exists, try versioning */

			char buf[PATH_MAX+1];
			int version = 1;
			string newpath_v;

			snprintf (buf, sizeof (buf), "%s.%d", newpath.c_str(), version);
			newpath_v = buf;

			while (Glib::file_test (newpath_v.c_str(), Glib::FILE_TEST_EXISTS) && version < 999) {
				snprintf (buf, sizeof (buf), "%s.%d", newpath.c_str(), ++version);
				newpath_v = buf;
			}

			if (version == 999) {
				error << string_compose (_("there are already 1000 files with names like %1; versioning discontinued"),
						  newpath)
				      << endmsg;
			} else {
				newpath = newpath_v;
			}

		} else {

			/* it doesn't exist, or we can't read it or something */

		}

		g_stat ((*x).c_str(), &statbuf);

		if (::rename ((*x).c_str(), newpath.c_str()) != 0) {
			error << string_compose (_("cannot rename unused file source from %1 to %2 (%3)"),
					  (*x), newpath, strerror (errno))
			      << endmsg;
			goto out;
		}

		/* see if there an easy to find peakfile for this file, and remove it.
		 */

                string base = Glib::path_get_basename (*x);
                base += "%A"; /* this is what we add for the channel suffix of all native files,
                                 or for the first channel of embedded files. it will miss
                                 some peakfiles for other channels
                              */
		string peakpath = construct_peak_filepath (base);

		if (Glib::file_test (peakpath.c_str(), Glib::FILE_TEST_EXISTS)) {
			if (::g_unlink (peakpath.c_str()) != 0) {
				error << string_compose (_("cannot remove peakfile %1 for %2 (%3)"),
                                                         peakpath, _path, strerror (errno))
				      << endmsg;
				/* try to back out */
				::rename (newpath.c_str(), _path.c_str());
				goto out;
			}
		}

		rep.paths.push_back (*x);
		rep.space += statbuf.st_size;
	}

	/* dump the history list */

	_history.clear ();

	/* save state so we don't end up a session file
	   referring to non-existent sources.
	*/

	save_state ("");
	ret = 0;

  out:
	_state_of_the_state = (StateOfTheState) (_state_of_the_state & ~InCleanup);

	return ret;
}

int
Session::cleanup_trash_sources (CleanupReport& rep)
{
	// FIXME: needs adaptation for MIDI

	vector<space_and_path>::iterator i;
	string dead_dir;

	rep.paths.clear ();
	rep.space = 0;

	for (i = session_dirs.begin(); i != session_dirs.end(); ++i) {

		dead_dir = Glib::build_filename ((*i).path, dead_dir_name);

                clear_directory (dead_dir, &rep.space, &rep.paths);
	}

	return 0;
}

void
Session::set_dirty ()
{
	/* never mark session dirty during loading */

	if (_state_of_the_state & Loading) {
		return;
	}

	bool was_dirty = dirty();

	_state_of_the_state = StateOfTheState (_state_of_the_state | Dirty);


	if (!was_dirty) {
		DirtyChanged(); /* EMIT SIGNAL */
	}
}


void
Session::set_clean ()
{
	bool was_dirty = dirty();

	_state_of_the_state = Clean;


	if (was_dirty) {
		DirtyChanged(); /* EMIT SIGNAL */
	}
}

void
Session::set_deletion_in_progress ()
{
	_state_of_the_state = StateOfTheState (_state_of_the_state | Deletion);
}

void
Session::clear_deletion_in_progress ()
{
	_state_of_the_state = StateOfTheState (_state_of_the_state & (~Deletion));
}

void
Session::add_controllable (boost::shared_ptr<Controllable> c)
{
	/* this adds a controllable to the list managed by the Session.
	   this is a subset of those managed by the Controllable class
	   itself, and represents the only ones whose state will be saved
	   as part of the session.
	*/

	Glib::Threads::Mutex::Lock lm (controllables_lock);
	controllables.insert (c);
}

struct null_deleter { void operator()(void const *) const {} };

void
Session::remove_controllable (Controllable* c)
{
	if (_state_of_the_state & Deletion) {
		return;
	}

	Glib::Threads::Mutex::Lock lm (controllables_lock);

	Controllables::iterator x = controllables.find (boost::shared_ptr<Controllable>(c, null_deleter()));

	if (x != controllables.end()) {
		controllables.erase (x);
	}
}

boost::shared_ptr<Controllable>
Session::controllable_by_id (const PBD::ID& id)
{
	Glib::Threads::Mutex::Lock lm (controllables_lock);

	for (Controllables::iterator i = controllables.begin(); i != controllables.end(); ++i) {
		if ((*i)->id() == id) {
			return *i;
		}
	}

	return boost::shared_ptr<Controllable>();
}

boost::shared_ptr<Controllable>
Session::controllable_by_descriptor (const ControllableDescriptor& desc)
{
	boost::shared_ptr<Controllable> c;
	boost::shared_ptr<Route> r;

	switch (desc.top_level_type()) {
	case ControllableDescriptor::NamedRoute:
	{
		std::string str = desc.top_level_name();
		if (str == "Master" || str == "master") {
			r = _master_out;
		} else if (str == "control" || str == "listen") {
			r = _monitor_out;
		} else {
			r = route_by_name (desc.top_level_name());
		}
		break;
	}

	case ControllableDescriptor::RemoteControlID:
		r = route_by_remote_id (desc.rid());
		break;
	}

	if (!r) {
		return c;
	}

	switch (desc.subtype()) {
	case ControllableDescriptor::Gain:
		c = r->gain_control ();
		break;

	case ControllableDescriptor::Trim:
		c = r->trim()->gain_control ();
		break;

	case ControllableDescriptor::Solo:
                c = r->solo_control();
		break;

	case ControllableDescriptor::Mute:
		c = r->mute_control();
		break;

	case ControllableDescriptor::Recenable:
	{
		boost::shared_ptr<Track> t = boost::dynamic_pointer_cast<Track>(r);

		if (t) {
			c = t->rec_enable_control ();
		}
		break;
	}

	case ControllableDescriptor::PanDirection:
        {
                c = r->pannable()->pan_azimuth_control;
		break;
        }

	case ControllableDescriptor::PanWidth:
        {
                c = r->pannable()->pan_width_control;
		break;
        }

	case ControllableDescriptor::PanElevation:
        {
                c = r->pannable()->pan_elevation_control;
		break;
        }

	case ControllableDescriptor::Balance:
		/* XXX simple pan control */
		break;

	case ControllableDescriptor::PluginParameter:
	{
		uint32_t plugin = desc.target (0);
		uint32_t parameter_index = desc.target (1);

		/* revert to zero based counting */

		if (plugin > 0) {
			--plugin;
		}

		if (parameter_index > 0) {
			--parameter_index;
		}

		boost::shared_ptr<Processor> p = r->nth_plugin (plugin);

		if (p) {
			c = boost::dynamic_pointer_cast<ARDOUR::AutomationControl>(
				p->control(Evoral::Parameter(PluginAutomation, 0, parameter_index)));
		}
		break;
	}

	case ControllableDescriptor::SendGain:
	{
		uint32_t send = desc.target (0);

		/* revert to zero-based counting */

		if (send > 0) {
			--send;
		}

		boost::shared_ptr<Processor> p = r->nth_send (send);

		if (p) {
			boost::shared_ptr<Send> s = boost::dynamic_pointer_cast<Send>(p);
			boost::shared_ptr<Amp> a = s->amp();

			if (a) {
				c = s->amp()->gain_control();
			}
		}
		break;
	}

	default:
		/* relax and return a null pointer */
		break;
	}

	return c;
}

void
Session::add_instant_xml (XMLNode& node, bool write_to_config)
{
	if (_writable) {
		Stateful::add_instant_xml (node, _path);
	}

	if (write_to_config) {
		Config->add_instant_xml (node);
	}
}

XMLNode*
Session::instant_xml (const string& node_name)
{
	return Stateful::instant_xml (node_name, _path);
}

int
Session::save_history (string snapshot_name)
{
	XMLTree tree;

	if (!_writable) {
	        return 0;
	}

	if (!Config->get_save_history() || Config->get_saved_history_depth() < 0 ||
	    (_history.undo_depth() == 0 && _history.redo_depth() == 0)) {
		return 0;
	}

	if (snapshot_name.empty()) {
		snapshot_name = _current_snapshot_name;
	}

	const string history_filename = legalize_for_path (snapshot_name) + history_suffix;
	const string backup_filename = history_filename + backup_suffix;
	const std::string xml_path(Glib::build_filename (_session_dir->root_path(), history_filename));
	const std::string backup_path(Glib::build_filename (_session_dir->root_path(), backup_filename));

	if (Glib::file_test (xml_path, Glib::FILE_TEST_EXISTS)) {
		if (::g_rename (xml_path.c_str(), backup_path.c_str()) != 0) {
			error << _("could not backup old history file, current history not saved") << endmsg;
			return -1;
		}
	}

	tree.set_root (&_history.get_state (Config->get_saved_history_depth()));

	if (!tree.write (xml_path))
	{
		error << string_compose (_("history could not be saved to %1"), xml_path) << endmsg;

		if (g_remove (xml_path.c_str()) != 0) {
			error << string_compose(_("Could not remove history file at path \"%1\" (%2)"),
					xml_path, g_strerror (errno)) << endmsg;
		}
		if (::g_rename (backup_path.c_str(), xml_path.c_str()) != 0) {
			error << string_compose (_("could not restore history file from backup %1 (%2)"),
					backup_path, g_strerror (errno)) << endmsg;
		}

		return -1;
	}

	return 0;
}

int
Session::restore_history (string snapshot_name)
{
	XMLTree tree;

	if (snapshot_name.empty()) {
		snapshot_name = _current_snapshot_name;
	}

	const std::string xml_filename = legalize_for_path (snapshot_name) + history_suffix;
	const std::string xml_path(Glib::build_filename (_session_dir->root_path(), xml_filename));

	info << "Loading history from " << xml_path << endmsg;

	if (!Glib::file_test (xml_path, Glib::FILE_TEST_EXISTS)) {
		info << string_compose (_("%1: no history file \"%2\" for this session."),
				_name, xml_path) << endmsg;
		return 1;
	}

	if (!tree.read (xml_path)) {
		error << string_compose (_("Could not understand session history file \"%1\""),
				xml_path) << endmsg;
		return -1;
	}

	// replace history
	_history.clear();

	for (XMLNodeConstIterator it  = tree.root()->children().begin(); it != tree.root()->children().end(); it++) {

		XMLNode *t = *it;
		UndoTransaction* ut = new UndoTransaction ();
		struct timeval tv;

		ut->set_name(t->property("name")->value());
		stringstream ss(t->property("tv-sec")->value());
		ss >> tv.tv_sec;
		ss.str(t->property("tv-usec")->value());
		ss >> tv.tv_usec;
		ut->set_timestamp(tv);

		for (XMLNodeConstIterator child_it  = t->children().begin();
				child_it != t->children().end(); child_it++)
		{
			XMLNode *n = *child_it;
			Command *c;

			if (n->name() == "MementoCommand" ||
					n->name() == "MementoUndoCommand" ||
					n->name() == "MementoRedoCommand") {

				if ((c = memento_command_factory(n))) {
					ut->add_command(c);
				}

			} else if (n->name() == "NoteDiffCommand") {
				PBD::ID id (n->property("midi-source")->value());
				boost::shared_ptr<MidiSource> midi_source =
					boost::dynamic_pointer_cast<MidiSource, Source>(source_by_id(id));
				if (midi_source) {
					ut->add_command (new MidiModel::NoteDiffCommand(midi_source->model(), *n));
				} else {
					error << _("Failed to downcast MidiSource for NoteDiffCommand") << endmsg;
				}

			} else if (n->name() == "SysExDiffCommand") {

				PBD::ID id (n->property("midi-source")->value());
				boost::shared_ptr<MidiSource> midi_source =
					boost::dynamic_pointer_cast<MidiSource, Source>(source_by_id(id));
				if (midi_source) {
					ut->add_command (new MidiModel::SysExDiffCommand (midi_source->model(), *n));
				} else {
					error << _("Failed to downcast MidiSource for SysExDiffCommand") << endmsg;
				}

			} else if (n->name() == "PatchChangeDiffCommand") {

				PBD::ID id (n->property("midi-source")->value());
				boost::shared_ptr<MidiSource> midi_source =
					boost::dynamic_pointer_cast<MidiSource, Source>(source_by_id(id));
				if (midi_source) {
					ut->add_command (new MidiModel::PatchChangeDiffCommand (midi_source->model(), *n));
				} else {
					error << _("Failed to downcast MidiSource for PatchChangeDiffCommand") << endmsg;
				}

			} else if (n->name() == "StatefulDiffCommand") {
				if ((c = stateful_diff_command_factory (n))) {
					ut->add_command (c);
				}
			} else {
				error << string_compose(_("Couldn't figure out how to make a Command out of a %1 XMLNode."), n->name()) << endmsg;
			}
		}

		_history.add (ut);
	}

	return 0;
}

void
Session::config_changed (std::string p, bool ours)
{
	if (ours) {
		set_dirty ();
	}

	if (p == "seamless-loop") {

	} else if (p == "rf-speed") {

	} else if (p == "auto-loop") {

	} else if (p == "auto-input") {

		if (Config->get_monitoring_model() == HardwareMonitoring && transport_rolling()) {
			/* auto-input only makes a difference if we're rolling */
                        set_track_monitor_input_status (!config.get_auto_input());
		}

	} else if (p == "punch-in") {

		Location* location;

		if ((location = _locations->auto_punch_location()) != 0) {

			if (config.get_punch_in ()) {
				replace_event (SessionEvent::PunchIn, location->start());
			} else {
				remove_event (location->start(), SessionEvent::PunchIn);
			}
		}

	} else if (p == "punch-out") {

		Location* location;

		if ((location = _locations->auto_punch_location()) != 0) {

			if (config.get_punch_out()) {
				replace_event (SessionEvent::PunchOut, location->end());
			} else {
				clear_events (SessionEvent::PunchOut);
			}
		}

	} else if (p == "edit-mode") {

		Glib::Threads::Mutex::Lock lm (playlists->lock);

		for (SessionPlaylists::List::iterator i = playlists->playlists.begin(); i != playlists->playlists.end(); ++i) {
			(*i)->set_edit_mode (Config->get_edit_mode ());
		}

	} else if (p == "use-video-sync") {

		waiting_for_sync_offset = config.get_use_video_sync();

	} else if (p == "mmc-control") {

		//poke_midi_thread ();

	} else if (p == "mmc-device-id" || p == "mmc-receive-id" || p == "mmc-receive-device-id") {

		_mmc->set_receive_device_id (Config->get_mmc_receive_device_id());

	} else if (p == "mmc-send-id" || p == "mmc-send-device-id") {

		_mmc->set_send_device_id (Config->get_mmc_send_device_id());

	} else if (p == "midi-control") {

		//poke_midi_thread ();

	} else if (p == "raid-path") {

		setup_raid_path (config.get_raid_path());

	} else if (p == "timecode-format") {

		sync_time_vars ();

	} else if (p == "video-pullup") {

		sync_time_vars ();

	} else if (p == "seamless-loop") {

		if (play_loop && transport_rolling()) {
			// to reset diskstreams etc
			request_play_loop (true);
		}

	} else if (p == "rf-speed") {

		cumulative_rf_motion = 0;
		reset_rf_scale (0);

	} else if (p == "click-sound") {

		setup_click_sounds (1);

	} else if (p == "click-emphasis-sound") {

		setup_click_sounds (-1);

	} else if (p == "clicking") {

		if (Config->get_clicking()) {
			if (_click_io && click_data) { // don't require emphasis data
				_clicking = true;
			}
		} else {
			_clicking = false;
		}

	} else if (p == "click-gain") {

		if (_click_gain) {
			_click_gain->set_gain (Config->get_click_gain(), this);
		}

	} else if (p == "send-mtc") {

		if (Config->get_send_mtc ()) {
			/* mark us ready to send */
			next_quarter_frame_to_send = 0;
		}

	} else if (p == "send-mmc") {

		_mmc->enable_send (Config->get_send_mmc ());

	} else if (p == "midi-feedback") {

		session_midi_feedback = Config->get_midi_feedback();

	} else if (p == "jack-time-master") {

		engine().reset_timebase ();

	} else if (p == "native-file-header-format") {

		if (!first_file_header_format_reset) {
			reset_native_file_format ();
		}

		first_file_header_format_reset = false;

	} else if (p == "native-file-data-format") {

		if (!first_file_data_format_reset) {
			reset_native_file_format ();
		}

		first_file_data_format_reset = false;

	} else if (p == "external-sync") {
		if (!config.get_external_sync()) {
			drop_sync_source ();
		} else {
			switch_to_sync_source (Config->get_sync_source());
		}
	}  else if (p == "denormal-model") {
		setup_fpu ();
	} else if (p == "history-depth") {
		set_history_depth (Config->get_history_depth());
	} else if (p == "remote-model") {
		/* XXX DO SOMETHING HERE TO TELL THE GUI THAT WE NEED
		   TO SET REMOTE ID'S
		*/
	} else if (p == "initial-program-change") {

		if (_mmc->output_port() && Config->get_initial_program_change() >= 0) {
			MIDI::byte buf[2];

			buf[0] = MIDI::program; // channel zero by default
			buf[1] = (Config->get_initial_program_change() & 0x7f);

			_mmc->output_port()->midimsg (buf, sizeof (buf), 0);
		}
	} else if (p == "solo-mute-override") {
		// catch_up_on_solo_mute_override ();
	} else if (p == "listen-position" || p == "pfl-position") {
		listen_position_changed ();
	} else if (p == "solo-control-is-listen-control") {
		solo_control_mode_changed ();
	} else if (p == "solo-mute-gain") {
		_solo_cut_control->Changed();
	} else if (p == "timecode-offset" || p == "timecode-offset-negative") {
		last_timecode_valid = false;
	} else if (p == "playback-buffer-seconds") {
		AudioSource::allocate_working_buffers (frame_rate());
	} else if (p == "ltc-source-port") {
		reconnect_ltc_input ();
	} else if (p == "ltc-sink-port") {
		reconnect_ltc_output ();
	} else if (p == "timecode-generator-offset") {
		ltc_tx_parse_offset();
	} else if (p == "auto-return-target-list") {
		follow_playhead_priority ();
	}

	set_dirty ();
}

void
Session::set_history_depth (uint32_t d)
{
	_history.set_depth (d);
}

int
Session::load_diskstreams_2X (XMLNode const & node, int)
{
        XMLNodeList          clist;
        XMLNodeConstIterator citer;

        clist = node.children();

        for (citer = clist.begin(); citer != clist.end(); ++citer) {

                try {
                        /* diskstreams added automatically by DiskstreamCreated handler */
                        if ((*citer)->name() == "AudioDiskstream" || (*citer)->name() == "DiskStream") {
				boost::shared_ptr<AudioDiskstream> dsp (new AudioDiskstream (*this, **citer));
				_diskstreams_2X.push_back (dsp);
                        } else {
                                error << _("Session: unknown diskstream type in XML") << endmsg;
                        }
                }

                catch (failed_constructor& err) {
                        error << _("Session: could not load diskstream via XML state") << endmsg;
                        return -1;
                }
        }

        return 0;
}

/** Connect things to the MMC object */
void
Session::setup_midi_machine_control ()
{
	_mmc = new MIDI::MachineControl;
	_mmc->set_ports (_midi_ports->mmc_input_port(), _midi_ports->mmc_output_port());

	_mmc->Play.connect_same_thread (*this, boost::bind (&Session::mmc_deferred_play, this, _1));
	_mmc->DeferredPlay.connect_same_thread (*this, boost::bind (&Session::mmc_deferred_play, this, _1));
	_mmc->Stop.connect_same_thread (*this, boost::bind (&Session::mmc_stop, this, _1));
	_mmc->FastForward.connect_same_thread (*this, boost::bind (&Session::mmc_fast_forward, this, _1));
	_mmc->Rewind.connect_same_thread (*this, boost::bind (&Session::mmc_rewind, this, _1));
	_mmc->Pause.connect_same_thread (*this, boost::bind (&Session::mmc_pause, this, _1));
	_mmc->RecordPause.connect_same_thread (*this, boost::bind (&Session::mmc_record_pause, this, _1));
	_mmc->RecordStrobe.connect_same_thread (*this, boost::bind (&Session::mmc_record_strobe, this, _1));
	_mmc->RecordExit.connect_same_thread (*this, boost::bind (&Session::mmc_record_exit, this, _1));
	_mmc->Locate.connect_same_thread (*this, boost::bind (&Session::mmc_locate, this, _1, _2));
	_mmc->Step.connect_same_thread (*this, boost::bind (&Session::mmc_step, this, _1, _2));
	_mmc->Shuttle.connect_same_thread (*this, boost::bind (&Session::mmc_shuttle, this, _1, _2, _3));
	_mmc->TrackRecordStatusChange.connect_same_thread (*this, boost::bind (&Session::mmc_record_enable, this, _1, _2, _3));

	/* also handle MIDI SPP because its so common */

	_mmc->SPPStart.connect_same_thread (*this, boost::bind (&Session::spp_start, this));
	_mmc->SPPContinue.connect_same_thread (*this, boost::bind (&Session::spp_continue, this));
	_mmc->SPPStop.connect_same_thread (*this, boost::bind (&Session::spp_stop, this));
}

boost::shared_ptr<Controllable>
Session::solo_cut_control() const
{
        /* the solo cut control is a bit of an anomaly, at least as of Febrary 2011. There are no other
           controls in Ardour that currently get presented to the user in the GUI that require
           access as a Controllable and are also NOT owned by some SessionObject (e.g. Route, or MonitorProcessor).

           its actually an RCConfiguration parameter, so we use a ProxyControllable to wrap
           it up as a Controllable. Changes to the Controllable will just map back to the RCConfiguration
           parameter.
        */

        return _solo_cut_control;
}

int
Session::rename (const std::string& new_name)
{
	string legal_name = legalize_for_path (new_name);
	string new_path;
	string oldstr;
	string newstr;
	bool first = true;

	string const old_sources_root = _session_dir->sources_root();

	if (!_writable || (_state_of_the_state & CannotSave)) {
		error << _("Cannot rename read-only session.") << endmsg;
		return 0; // don't show "messed up" warning
	}
        if (record_status() == Recording) {
		error << _("Cannot rename session while recording") << endmsg;
		return 0; // don't show "messed up" warning
	}

	StateProtector stp (this);

	/* Rename:

	 * session directory
	 * interchange subdirectory
	 * session file
	 * session history

	 * Backup files are left unchanged and not renamed.
	 */

	/* Windows requires that we close all files before attempting the
	 * rename. This works on other platforms, but isn't necessary there.
	 * Leave it in place for all platforms though, since it may help
	 * catch issues that could arise if the way Source files work ever
	 * change (since most developers are not using Windows).
	 */

	for (SourceMap::const_iterator i = sources.begin(); i != sources.end(); ++i) {
		boost::shared_ptr<FileSource> fs = boost::dynamic_pointer_cast<FileSource> (i->second);
		if (fs) {
			fs->close ();
		}
	}

	/* pass one: not 100% safe check that the new directory names don't
	 * already exist ...
	 */

	for (vector<space_and_path>::const_iterator i = session_dirs.begin(); i != session_dirs.end(); ++i) {

		oldstr = (*i).path;

		/* this is a stupid hack because Glib::path_get_dirname() is
		 * lexical-only, and so passing it /a/b/c/ gives a different
		 * result than passing it /a/b/c ...
		 */

		if (oldstr[oldstr.length()-1] == G_DIR_SEPARATOR) {
			oldstr = oldstr.substr (0, oldstr.length() - 1);
		}

		string base = Glib::path_get_dirname (oldstr);

		newstr = Glib::build_filename (base, legal_name);

		cerr << "Looking for " << newstr << endl;

		if (Glib::file_test (newstr, Glib::FILE_TEST_EXISTS)) {
			cerr << " exists\n";
			return -1;
		}
	}

	/* Session dirs */

	first = true;

	for (vector<space_and_path>::iterator i = session_dirs.begin(); i != session_dirs.end(); ++i) {

		vector<string> v;

		oldstr = (*i).path;

		/* this is a stupid hack because Glib::path_get_dirname() is
		 * lexical-only, and so passing it /a/b/c/ gives a different
		 * result than passing it /a/b/c ...
		 */

		if (oldstr[oldstr.length()-1] == G_DIR_SEPARATOR) {
			oldstr = oldstr.substr (0, oldstr.length() - 1);
		}

		string base = Glib::path_get_dirname (oldstr);
		newstr = Glib::build_filename (base, legal_name);

		cerr << "for " << oldstr << " new dir = " << newstr << endl;

		cerr << "Rename " << oldstr << " => " << newstr << endl;
		if (::g_rename (oldstr.c_str(), newstr.c_str()) != 0) {
			cerr << string_compose (_("renaming %s as %2 failed (%3)"), oldstr, newstr, g_strerror (errno)) << endl;
			error << string_compose (_("renaming %s as %2 failed (%3)"), oldstr, newstr, g_strerror (errno)) << endmsg;
			return 1;
		}

		/* Reset path in "session dirs" */

		(*i).path = newstr;
		(*i).blocks = 0;

		/* reset primary SessionDirectory object */

		if (first) {
			(*_session_dir) = newstr;
			new_path = newstr;
			first = false;
		}

		/* now rename directory below session_dir/interchange */

		string old_interchange_dir;
		string new_interchange_dir;

		/* use newstr here because we renamed the path
		 * (folder/directory) that used to be oldstr to newstr above
		 */

		v.push_back (newstr);
		v.push_back (interchange_dir_name);
		v.push_back (Glib::path_get_basename (oldstr));

		old_interchange_dir = Glib::build_filename (v);

		v.clear ();
		v.push_back (newstr);
		v.push_back (interchange_dir_name);
		v.push_back (legal_name);

		new_interchange_dir = Glib::build_filename (v);

		cerr << "Rename " << old_interchange_dir << " => " << new_interchange_dir << endl;

		if (::g_rename (old_interchange_dir.c_str(), new_interchange_dir.c_str()) != 0) {
			cerr << string_compose (_("renaming %s as %2 failed (%3)"),
			                         old_interchange_dir, new_interchange_dir,
						 g_strerror (errno))
			      << endl;
			error << string_compose (_("renaming %s as %2 failed (%3)"),
						 old_interchange_dir, new_interchange_dir,
						 g_strerror (errno))
			      << endmsg;
			return 1;
		}
	}

	/* state file */

	oldstr = Glib::build_filename (new_path, _current_snapshot_name + statefile_suffix);
	newstr= Glib::build_filename (new_path, legal_name + statefile_suffix);

	cerr << "Rename " << oldstr << " => " << newstr << endl;

	if (::g_rename (oldstr.c_str(), newstr.c_str()) != 0) {
		cerr << string_compose (_("renaming %1 as %2 failed (%3)"), oldstr, newstr, g_strerror (errno)) << endl;
		error << string_compose (_("renaming %1 as %2 failed (%3)"), oldstr, newstr, g_strerror (errno)) << endmsg;
		return 1;
	}

	/* history file */

	oldstr = Glib::build_filename (new_path, _current_snapshot_name) + history_suffix;

	if (Glib::file_test (oldstr, Glib::FILE_TEST_EXISTS))  {
		newstr = Glib::build_filename (new_path, legal_name) + history_suffix;

		cerr << "Rename " << oldstr << " => " << newstr << endl;

		if (::g_rename (oldstr.c_str(), newstr.c_str()) != 0) {
			cerr << string_compose (_("renaming %1 as %2 failed (%3)"), oldstr, newstr, g_strerror (errno)) << endl;
			error << string_compose (_("renaming %1 as %2 failed (%3)"), oldstr, newstr, g_strerror (errno)) << endmsg;
			return 1;
		}
	}

	/* remove old name from recent sessions */
	remove_recent_sessions (_path);
	_path = new_path;

	/* update file source paths */

	for (SourceMap::iterator i = sources.begin(); i != sources.end(); ++i) {
		boost::shared_ptr<FileSource> fs = boost::dynamic_pointer_cast<FileSource> (i->second);
		if (fs) {
			string p = fs->path ();
			boost::replace_all (p, old_sources_root, _session_dir->sources_root());
			fs->set_path (p);
			SourceFactory::setup_peakfile(i->second, true);
		}
	}

	_current_snapshot_name = new_name;
	_name = new_name;

	set_dirty ();

	/* save state again to get everything just right */

	save_state (_current_snapshot_name);

	/* add to recent sessions */

	store_recent_sessions (new_name, _path);

	return 0;
}

int
Session::get_session_info_from_path (XMLTree& tree, const string& xmlpath)
{
	if (!Glib::file_test (xmlpath, Glib::FILE_TEST_EXISTS)) {
		return -1;
        }

	if (!tree.read (xmlpath)) {
		return -1;
	}

	return 0;
}

int
Session::get_info_from_path (const string& xmlpath, float& sample_rate, SampleFormat& data_format)
{
	XMLTree tree;
	bool found_sr = false;
	bool found_data_format = false;

	if (get_session_info_from_path (tree, xmlpath)) {
		return -1;
	}

	/* sample rate */

	const XMLProperty* prop;
	if ((prop = tree.root()->property (X_("sample-rate"))) != 0) {
		sample_rate = atoi (prop->value());
		found_sr = true;
	}

	const XMLNodeList& children (tree.root()->children());
	for (XMLNodeList::const_iterator c = children.begin(); c != children.end(); ++c) {
		const XMLNode* child = *c;
		if (child->name() == "Config") {
			const XMLNodeList& options (child->children());
			for (XMLNodeList::const_iterator oc = options.begin(); oc != options.end(); ++oc) {
				const XMLNode* option = *oc;
				const XMLProperty* name = option->property("name");

				if (!name) {
					continue;
				}

				if (name->value() == "native-file-data-format") {
					const XMLProperty* value = option->property ("value");
					if (value) {
						SampleFormat fmt = (SampleFormat) string_2_enum (option->property ("value")->value(), fmt);
						data_format = fmt;
						found_data_format = true;
						break;
					}
				}
			}
		}
		if (found_data_format) {
			break;
		}
	}

	return !(found_sr && found_data_format); // zero if they are both found
}

typedef std::vector<boost::shared_ptr<FileSource> > SeveralFileSources;
typedef std::map<std::string,SeveralFileSources> SourcePathMap;

int
Session::bring_all_sources_into_session (boost::function<void(uint32_t,uint32_t,string)> callback)
{
	uint32_t total = 0;
	uint32_t n = 0;
	SourcePathMap source_path_map;
	string new_path;
	boost::shared_ptr<AudioFileSource> afs;
	int ret = 0;

	{

		Glib::Threads::Mutex::Lock lm (source_lock);

		cerr << " total sources = " << sources.size();

		for (SourceMap::const_iterator i = sources.begin(); i != sources.end(); ++i) {
			boost::shared_ptr<FileSource> fs = boost::dynamic_pointer_cast<FileSource> (i->second);

			if (!fs) {
				continue;
			}

			if (fs->within_session()) {
				continue;
			}

			if (source_path_map.find (fs->path()) != source_path_map.end()) {
				source_path_map[fs->path()].push_back (fs);
			} else {
				SeveralFileSources v;
				v.push_back (fs);
				source_path_map.insert (make_pair (fs->path(), v));
			}

			total++;
		}

		cerr << " fsources = " << total << endl;

		for (SourcePathMap::iterator i = source_path_map.begin(); i != source_path_map.end(); ++i) {

			/* tell caller where we are */

			string old_path = i->first;

			callback (n, total, old_path);

			cerr << old_path << endl;

			new_path.clear ();

			switch (i->second.front()->type()) {
			case DataType::AUDIO:
				new_path = new_audio_source_path_for_embedded (old_path);
				break;

			case DataType::MIDI:
				/* XXX not implemented yet */
				break;
			}

			if (new_path.empty()) {
				continue;
			}

			cerr << "Move " << old_path << " => " << new_path << endl;

			if (!copy_file (old_path, new_path)) {
				cerr << "failed !\n";
				ret = -1;
			}

			/* make sure we stop looking in the external
			   dir/folder. Remember, this is an all-or-nothing
			   operations, it doesn't merge just some files.
			*/
			remove_dir_from_search_path (Glib::path_get_dirname (old_path), i->second.front()->type());

			for (SeveralFileSources::iterator f = i->second.begin(); f != i->second.end(); ++f) {
				(*f)->set_path (new_path);
			}
		}
	}

	save_state ("", false, false);

	return ret;
}

static
bool accept_all_files (string const &, void *)
{
	return true;
}

void
Session::save_as_bring_callback (uint32_t,uint32_t,string)
{
	/* It would be good if this did something useful vis-a-vis save-as, but the arguments doesn't provide the correct information right now to do this.
	*/
}

static string
make_new_media_path (string old_path, string new_session_folder, string new_session_path)
{
	/* typedir is the "midifiles" or "audiofiles" etc. part of the path. */

	string typedir = Glib::path_get_basename (Glib::path_get_dirname (old_path));
	vector<string> v;
	v.push_back (new_session_folder); /* full path */
	v.push_back (interchange_dir_name);
	v.push_back (new_session_path);   /* just one directory/folder */
	v.push_back (typedir);
	v.push_back (Glib::path_get_basename (old_path));

	return Glib::build_filename (v);
}

int
Session::save_as (SaveAs& saveas)
{
	vector<string> files;
	string current_folder = Glib::path_get_dirname (_path);
	string new_folder = legalize_for_path (saveas.new_name);
	string to_dir = Glib::build_filename (saveas.new_parent_folder, new_folder);
	int64_t total_bytes = 0;
	int64_t copied = 0;
	int64_t cnt = 0;
	int64_t all = 0;
	int32_t internal_file_cnt = 0;

	vector<string> do_not_copy_extensions;
	do_not_copy_extensions.push_back (statefile_suffix);
	do_not_copy_extensions.push_back (pending_suffix);
	do_not_copy_extensions.push_back (backup_suffix);
	do_not_copy_extensions.push_back (temp_suffix);
	do_not_copy_extensions.push_back (history_suffix);

	/* get total size */

	for (vector<space_and_path>::const_iterator sd = session_dirs.begin(); sd != session_dirs.end(); ++sd) {

		/* need to clear this because
		 * find_files_matching_filter() is cumulative
		 */

		files.clear ();

		find_files_matching_filter (files, (*sd).path, accept_all_files, 0, false, true, true);

		all += files.size();

		for (vector<string>::iterator i = files.begin(); i != files.end(); ++i) {
			GStatBuf gsb;
			g_stat ((*i).c_str(), &gsb);
			total_bytes += gsb.st_size;
		}
	}

	/* save old values so we can switch back if we are not switching to the new session */

	string old_path = _path;
	string old_name = _name;
	string old_snapshot = _current_snapshot_name;
	string old_sd = _session_dir->root_path();
	vector<string> old_search_path[DataType::num_types];
	string old_config_search_path[DataType::num_types];

	old_search_path[DataType::AUDIO] = source_search_path (DataType::AUDIO);
	old_search_path[DataType::MIDI] = source_search_path (DataType::MIDI);
	old_config_search_path[DataType::AUDIO]  = config.get_audio_search_path ();
	old_config_search_path[DataType::MIDI]  = config.get_midi_search_path ();

	/* switch session directory */

	(*_session_dir) = to_dir;

	/* create new tree */

	if (!_session_dir->create()) {
		saveas.failure_message = string_compose (_("Cannot create new session folder %1"), to_dir);
		return -1;
	}

	try {
		/* copy all relevant files. Find each location in session_dirs,
		 * and copy files from there to target.
		 */

		for (vector<space_and_path>::const_iterator sd = session_dirs.begin(); sd != session_dirs.end(); ++sd) {

			/* need to clear this because
			 * find_files_matching_filter() is cumulative
			 */

			files.clear ();

			const size_t prefix_len = (*sd).path.size();

			/* Work just on the files within this session dir */

			find_files_matching_filter (files, (*sd).path, accept_all_files, 0, false, true, true);

			/* add dir separator to protect against collisions with
			 * track names (e.g. track named "audiofiles" or
			 * "analysis".
			 */

			static const std::string audiofile_dir_string = string (sound_dir_name) + G_DIR_SEPARATOR;
			static const std::string midifile_dir_string = string (midi_dir_name) + G_DIR_SEPARATOR;
			static const std::string analysis_dir_string = analysis_dir() + G_DIR_SEPARATOR;

			/* copy all the files. Handling is different for media files
			   than others because of the *silly* subtree we have below the interchange
			   folder. That really was a bad idea, but I'm not fixing it as part of
			   implementing ::save_as().
			*/

			for (vector<string>::iterator i = files.begin(); i != files.end(); ++i) {

				std::string from = *i;

#ifdef __APPLE__
				string filename = Glib::path_get_basename (from);
				std::transform (filename.begin(), filename.end(), filename.begin(), ::toupper);
				if (filename == ".DS_STORE") {
					continue;
				}
#endif

				if (from.find (audiofile_dir_string) != string::npos) {

					/* audio file: only copy if asked */

					if (saveas.include_media && saveas.copy_media) {

						string to = make_new_media_path (*i, to_dir, new_folder);

						info << "media file copying from " << from << " to " << to << endmsg;

						if (!copy_file (from, to)) {
							throw Glib::FileError (Glib::FileError::IO_ERROR,
												   string_compose(_("\ncopying \"%1\" failed !"), from));
						}
					}

					/* we found media files inside the session folder */

					internal_file_cnt++;

				} else if (from.find (midifile_dir_string) != string::npos) {

					/* midi file: always copy unless
					 * creating an empty new session
					 */

					if (saveas.include_media) {

						string to = make_new_media_path (*i, to_dir, new_folder);

						info << "media file copying from " << from << " to " << to << endmsg;

						if (!copy_file (from, to)) {
							throw Glib::FileError (Glib::FileError::IO_ERROR, "copy failed");
						}
					}

					/* we found media files inside the session folder */

					internal_file_cnt++;

				} else if (from.find (analysis_dir_string) != string::npos) {

					/*  make sure analysis dir exists in
					 *  new session folder, but we're not
					 *  copying analysis files here, see
					 *  below
					 */

					(void) g_mkdir_with_parents (analysis_dir().c_str(), 775);
					continue;

				} else {

					/* normal non-media file. Don't copy state, history, etc.
					 */

					bool do_copy = true;

					for (vector<string>::iterator v = do_not_copy_extensions.begin(); v != do_not_copy_extensions.end(); ++v) {
						if ((from.length() > (*v).length()) && (from.find (*v) == from.length() - (*v).length())) {
							/* end of filename matches extension, do not copy file */
							do_copy = false;
							break;
						}
					}

					if (!saveas.copy_media && from.find (peakfile_suffix) != string::npos) {
						/* don't copy peakfiles if
						 * we're not copying media
						 */
						do_copy = false;
					}

					if (do_copy) {
						string to = Glib::build_filename (to_dir, from.substr (prefix_len));

						info << "attempting to make directory/folder " << to << endmsg;

						if (g_mkdir_with_parents (Glib::path_get_dirname (to).c_str(), 0755)) {
							throw Glib::FileError (Glib::FileError::IO_ERROR, "cannot create required directory");
						}

						info << "attempting to copy " << from << " to " << to << endmsg;

						if (!copy_file (from, to)) {
							throw Glib::FileError (Glib::FileError::IO_ERROR,
												   string_compose(_("\ncopying \"%1\" failed !"), from));
						}
					}
				}

				/* measure file size even if we're not going to copy so that our Progress
				   signals are correct, since we included these do-not-copy files
				   in the computation of the total size and file count.
				*/

				GStatBuf gsb;
				g_stat (from.c_str(), &gsb);
				copied += gsb.st_size;
				cnt++;

				double fraction = (double) copied / total_bytes;

				bool keep_going = true;

				if (saveas.copy_media) {

					/* no need or expectation of this if
					 * media is not being copied, because
					 * it will be fast(ish).
					 */

					/* tell someone "X percent, file M of N"; M is one-based */

					boost::optional<bool> res = saveas.Progress (fraction, cnt, all);

					if (res) {
						keep_going = *res;
					}
				}

				if (!keep_going) {
					throw Glib::FileError (Glib::FileError::FAILED, "copy cancelled");
				}
			}

		}

		/* copy optional folders, if any */

		string old = plugins_dir ();
		if (Glib::file_test (old, Glib::FILE_TEST_EXISTS)) {
			string newdir = Glib::build_filename (to_dir, Glib::path_get_basename (old));
			copy_files (old, newdir);
		}

		old = externals_dir ();
		if (Glib::file_test (old, Glib::FILE_TEST_EXISTS)) {
			string newdir = Glib::build_filename (to_dir, Glib::path_get_basename (old));
			copy_files (old, newdir);
		}

		old = automation_dir ();
		if (Glib::file_test (old, Glib::FILE_TEST_EXISTS)) {
			string newdir = Glib::build_filename (to_dir, Glib::path_get_basename (old));
			copy_files (old, newdir);
		}

		if (saveas.include_media) {

			if (saveas.copy_media) {
#ifndef PLATFORM_WINDOWS
				/* There are problems with analysis files on
				 * Windows, because they used a colon in their
				 * names as late as 4.0. Colons are not legal
				 * under Windows even if NTFS allows them.
				 *
				 * This is a tricky problem to solve so for
				 * just don't copy these files. They will be
				 * regenerated as-needed anyway, subject to the
				 * existing issue that the filenames will be
				 * rejected by Windows, which is a separate
				 * problem (though related).
				 */

				/* only needed if we are copying media, since the
				 * analysis data refers to media data
				 */

				old = analysis_dir ();
				if (Glib::file_test (old, Glib::FILE_TEST_EXISTS)) {
					string newdir = Glib::build_filename (to_dir, "analysis");
					copy_files (old, newdir);
				}
#endif /* PLATFORM_WINDOWS */
			}
		}


		_path = to_dir;
		_current_snapshot_name = saveas.new_name;
		_name = saveas.new_name;

		if (saveas.include_media && !saveas.copy_media) {

			/* reset search paths of the new session (which we're pretending to be right now) to
			   include the original session search path, so we can still find all audio.
			*/

			if (internal_file_cnt) {
				for (vector<string>::iterator s = old_search_path[DataType::AUDIO].begin(); s != old_search_path[DataType::AUDIO].end(); ++s) {
					ensure_search_path_includes (*s, DataType::AUDIO);
					cerr << "be sure to include " << *s << "  for audio" << endl;
				}

				/* we do not do this for MIDI because we copy
				   all MIDI files if saveas.include_media is
				   true
				*/
			}
		}

		bool was_dirty = dirty ();

		save_state ("", false, false, !saveas.include_media);
		save_default_options ();

		if (saveas.copy_media && saveas.copy_external) {
			if (bring_all_sources_into_session (boost::bind (&Session::save_as_bring_callback, this, _1, _2, _3))) {
				throw Glib::FileError (Glib::FileError::NO_SPACE_LEFT, "consolidate failed");
			}
		}

		saveas.final_session_folder_name = _path;

		store_recent_sessions (_name, _path);

		if (!saveas.switch_to) {

			/* switch back to the way things were */

			_path = old_path;
			_name = old_name;
			_current_snapshot_name = old_snapshot;

			(*_session_dir) = old_sd;

			if (was_dirty) {
				set_dirty ();
			}

			if (internal_file_cnt) {
				/* reset these to their original values */
				config.set_audio_search_path (old_config_search_path[DataType::AUDIO]);
				config.set_midi_search_path (old_config_search_path[DataType::MIDI]);
			}

		} else {

			/* prune session dirs, and update disk space statistics
			 */

			space_and_path sp;
			sp.path = _path;
			session_dirs.clear ();
			session_dirs.push_back (sp);
			refresh_disk_space ();

			/* ensure that all existing tracks reset their current capture source paths
			 */
			reset_write_sources (true, true);

			/* the copying above was based on actually discovering files, not just iterating over the sources list.
			   But if we're going to switch to the new (copied) session, we need to change the paths in the sources also.
			*/

			for (SourceMap::const_iterator i = sources.begin(); i != sources.end(); ++i) {
				boost::shared_ptr<FileSource> fs = boost::dynamic_pointer_cast<FileSource> (i->second);

				if (!fs) {
					continue;
				}

				if (fs->within_session()) {
					string newpath = make_new_media_path (fs->path(), to_dir, new_folder);
					fs->set_path (newpath);
				}
			}
		}

	} catch (Glib::FileError& e) {

		saveas.failure_message = e.what();

		/* recursively remove all the directories */

		remove_directory (to_dir);

		/* return error */

		return -1;

	} catch (...) {

		saveas.failure_message = _("unknown reason");

		/* recursively remove all the directories */

		remove_directory (to_dir);

		/* return error */

		return -1;
	}

	return 0;
}
