/*
    Copyright (C) 2008-2012 Paul Davis
    Author: David Robillard

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

#include <string>
#include <vector>
#include <limits>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "pbd/gstdio_compat.h"
#include <glib/gprintf.h>
#include <glibmm.h>

#include <boost/utility.hpp>

#include "pbd/file_utils.h"
#include "pbd/stl_delete.h"
#include "pbd/compose.h"
#include "pbd/error.h"
#include "pbd/xml++.h"

#include "libardour-config.h"

#include "ardour/audio_buffer.h"
#include "ardour/audioengine.h"
#include "ardour/debug.h"
#include "ardour/lv2_plugin.h"
#include "ardour/session.h"
#include "ardour/tempo.h"
#include "ardour/types.h"
#include "ardour/utils.h"
#include "ardour/worker.h"
#include "ardour/search_paths.h"

#include "i18n.h"
#include <locale.h>

#include <lilv/lilv.h>

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/atom/forge.h"
#include "lv2/lv2plug.in/ns/ext/log/log.h"
#include "lv2/lv2plug.in/ns/ext/midi/midi.h"
#include "lv2/lv2plug.in/ns/ext/port-props/port-props.h"
#include "lv2/lv2plug.in/ns/ext/presets/presets.h"
#include "lv2/lv2plug.in/ns/ext/state/state.h"
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include "lv2/lv2plug.in/ns/ext/worker/worker.h"
#include "lv2/lv2plug.in/ns/ext/resize-port/resize-port.h"
#include "lv2/lv2plug.in/ns/extensions/ui/ui.h"
#include "lv2/lv2plug.in/ns/extensions/units/units.h"
#include "lv2/lv2plug.in/ns/ext/patch/patch.h"
#ifdef HAVE_LV2_1_2_0
#include "lv2/lv2plug.in/ns/ext/buf-size/buf-size.h"
#include "lv2/lv2plug.in/ns/ext/options/options.h"
#endif

#include "lv2_evbuf.h"

#ifdef HAVE_SUIL
#include <suil/suil.h>
#endif

// Compatibility for old LV2
#ifndef LV2_ATOM_CONTENTS_CONST
#define LV2_ATOM_CONTENTS_CONST(type, atom) \
	((const void*)((const uint8_t*)(atom) + sizeof(type)))
#endif
#ifndef LV2_ATOM_BODY_CONST
#define LV2_ATOM_BODY_CONST(atom) LV2_ATOM_CONTENTS_CONST(LV2_Atom, atom)
#endif
#ifndef LV2_PATCH__property
#define LV2_PATCH__property LV2_PATCH_PREFIX "property"
#endif
#ifndef LV2_PATCH__value
#define LV2_PATCH__value LV2_PATCH_PREFIX "value"
#endif
#ifndef LV2_PATCH__writable
#define LV2_PATCH__writable LV2_PATCH_PREFIX "writable"
#endif

/** The number of MIDI buffers that will fit in a UI/worker comm buffer.
    This needs to be roughly the number of cycles the UI will get around to
    actually processing the traffic.  Lower values are flakier but save memory.
*/
static const size_t NBUFS = 4;

using namespace std;
using namespace ARDOUR;
using namespace PBD;

class LV2World : boost::noncopyable {
public:
	LV2World ();
	~LV2World ();

	void load_bundled_plugins(bool verbose=false);

	LilvWorld* world;

	LilvNode* atom_AtomPort;
	LilvNode* atom_Chunk;
	LilvNode* atom_Sequence;
	LilvNode* atom_bufferType;
	LilvNode* atom_eventTransfer;
	LilvNode* atom_supports;
	LilvNode* ev_EventPort;
	LilvNode* ext_logarithmic;
	LilvNode* ext_notOnGUI;
	LilvNode* lv2_AudioPort;
	LilvNode* lv2_ControlPort;
	LilvNode* lv2_InputPort;
	LilvNode* lv2_OutputPort;
	LilvNode* lv2_enumeration;
	LilvNode* lv2_freewheeling;
	LilvNode* lv2_inPlaceBroken;
	LilvNode* lv2_integer;
	LilvNode* lv2_default;
	LilvNode* lv2_minimum;
	LilvNode* lv2_maximum;
	LilvNode* lv2_reportsLatency;
	LilvNode* lv2_sampleRate;
	LilvNode* lv2_toggled;
	LilvNode* midi_MidiEvent;
	LilvNode* rdfs_comment;
	LilvNode* rdfs_label;
	LilvNode* rdfs_range;
	LilvNode* rsz_minimumSize;
	LilvNode* time_Position;
	LilvNode* ui_GtkUI;
	LilvNode* ui_external;
	LilvNode* ui_externalkx;
	LilvNode* units_hz;
	LilvNode* units_db;
	LilvNode* units_unit;
	LilvNode* units_render;
	LilvNode* units_midiNote;
	LilvNode* patch_writable;
	LilvNode* patch_Message;
#ifdef HAVE_LV2_1_2_0
	LilvNode* bufz_powerOf2BlockLength;
	LilvNode* bufz_fixedBlockLength;
	LilvNode* bufz_nominalBlockLength;
#endif

private:
	bool _bundle_checked;
};

static LV2World _world;

/* worker extension */

/** Called by the plugin to schedule non-RT work. */
static LV2_Worker_Status
work_schedule(LV2_Worker_Schedule_Handle handle,
              uint32_t                   size,
              const void*                data)
{
	LV2Plugin* plugin = (LV2Plugin*)handle;
	if (plugin->session().engine().freewheeling()) {
		// Freewheeling, do the work immediately in this (audio) thread
		return (LV2_Worker_Status)plugin->work(size, data);
	} else {
		// Enqueue message for the worker thread
		return plugin->worker()->schedule(size, data) ?
			LV2_WORKER_SUCCESS : LV2_WORKER_ERR_UNKNOWN;
	}
}

/** Called by the plugin to respond to non-RT work. */
static LV2_Worker_Status
work_respond(LV2_Worker_Respond_Handle handle,
             uint32_t                  size,
             const void*               data)
{
	LV2Plugin* plugin = (LV2Plugin*)handle;
	if (plugin->session().engine().freewheeling()) {
		// Freewheeling, respond immediately in this (audio) thread
		return (LV2_Worker_Status)plugin->work_response(size, data);
	} else {
		// Enqueue response for the worker
		return plugin->worker()->respond(size, data) ?
			LV2_WORKER_SUCCESS : LV2_WORKER_ERR_UNKNOWN;
	}
}

/* log extension */

static int
log_vprintf(LV2_Log_Handle /*handle*/,
            LV2_URID       type,
            const char*    fmt,
            va_list        args)
{
	char* str = NULL;
	const int ret = g_vasprintf(&str, fmt, args);
	if (type == URIMap::instance().urids.log_Error) {
		error << str << endmsg;
	} else if (type == URIMap::instance().urids.log_Warning) {
		warning << str << endmsg;
	} else if (type == URIMap::instance().urids.log_Note) {
		info << str << endmsg;
	}
	// TODO: Toggleable log:Trace message support
	return ret;
}

static int
log_printf(LV2_Log_Handle handle,
           LV2_URID       type,
           const char*    fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	const int ret = log_vprintf(handle, type, fmt, args);
	va_end(args);
	return ret;
}

struct LV2Plugin::Impl {
	Impl() : plugin(0), ui(0), ui_type(0), name(0), author(0), instance(0)
	       , work_iface(0)
	       , opts_iface(0)
	       , state(0)
	       , block_length(0)
	       , options(0)
	{}

	/** Find the LV2 input port with the given designation.
	 * If found, bufptrs[port_index] will be set to bufptr.
	 */
	const LilvPort* designated_input (const char* uri, void** bufptrs[], void** bufptr);

	const LilvPlugin*            plugin;
	const LilvUI*                ui;
	const LilvNode*              ui_type;
	LilvNode*                    name;
	LilvNode*                    author;
	LilvInstance*                instance;
	const LV2_Worker_Interface*  work_iface;
	const LV2_Options_Interface* opts_iface;
	LilvState*                   state;
	LV2_Atom_Forge               forge;
	LV2_Atom_Forge               ui_forge;
	int32_t                      block_length;
	LV2_Options_Option*          options;
};

LV2Plugin::LV2Plugin (AudioEngine& engine,
                      Session&     session,
                      const void*  c_plugin,
                      framecnt_t   rate)
	: Plugin (engine, session)
	, Workee ()
	, _impl(new Impl())
	, _features(NULL)
	, _worker(NULL)
	, _insert_id("0")
	, _patch_port_in_index((uint32_t)-1)
	, _patch_port_out_index((uint32_t)-1)
	, _uri_map(URIMap::instance())
{
	init(c_plugin, rate);
}

LV2Plugin::LV2Plugin (const LV2Plugin& other)
	: Plugin (other)
	, Workee ()
	, _impl(new Impl())
	, _features(NULL)
	, _worker(NULL)
	, _insert_id(other._insert_id)
	, _patch_port_in_index((uint32_t)-1)
	, _patch_port_out_index((uint32_t)-1)
	, _uri_map(URIMap::instance())
{
	init(other._impl->plugin, other._sample_rate);

	for (uint32_t i = 0; i < parameter_count(); ++i) {
		_control_data[i] = other._shadow_data[i];
		_shadow_data[i]  = other._shadow_data[i];
	}
}

void
LV2Plugin::init(const void* c_plugin, framecnt_t rate)
{
	DEBUG_TRACE(DEBUG::LV2, "init\n");

	_impl->plugin           = (const LilvPlugin*)c_plugin;
	_impl->ui               = NULL;
	_impl->ui_type          = NULL;
	_to_ui                  = NULL;
	_from_ui                = NULL;
	_control_data           = 0;
	_shadow_data            = 0;
	_atom_ev_buffers        = 0;
	_ev_buffers             = 0;
	_bpm_control_port       = 0;
	_freewheel_control_port = 0;
	_latency_control_port   = 0;
	_next_cycle_start       = std::numeric_limits<framepos_t>::max();
	_next_cycle_speed       = 1.0;
	_seq_size               = _engine.raw_buffer_size(DataType::MIDI);
	_state_version          = 0;
	_was_activated          = false;
	_has_state_interface    = false;
	_impl->block_length     = _session.get_block_size();

	_instance_access_feature.URI = "http://lv2plug.in/ns/ext/instance-access";
	_data_access_feature.URI     = "http://lv2plug.in/ns/ext/data-access";
	_make_path_feature.URI       = LV2_STATE__makePath;
	_log_feature.URI             = LV2_LOG__log;
	_work_schedule_feature.URI   = LV2_WORKER__schedule;
	_work_schedule_feature.data  = NULL;
	_def_state_feature.URI       = LV2_STATE_PREFIX "loadDefaultState";  // Post LV2-1.2.0
	_def_state_feature.data      = NULL;

	const LilvPlugin* plugin = _impl->plugin;

	LilvNode* state_iface_uri = lilv_new_uri(_world.world, LV2_STATE__interface);
	LilvNode* state_uri       = lilv_new_uri(_world.world, LV2_STATE_URI);
	_has_state_interface =
		// What plugins should have (lv2:extensionData state:Interface)
		lilv_plugin_has_extension_data(plugin, state_iface_uri)
		// What some outdated/incorrect ones have
		|| lilv_plugin_has_feature(plugin, state_uri);
	lilv_node_free(state_uri);
	lilv_node_free(state_iface_uri);

	_features    = (LV2_Feature**)calloc(11, sizeof(LV2_Feature*));
	_features[0] = &_instance_access_feature;
	_features[1] = &_data_access_feature;
	_features[2] = &_make_path_feature;
	_features[3] = _uri_map.uri_map_feature();
	_features[4] = _uri_map.urid_map_feature();
	_features[5] = _uri_map.urid_unmap_feature();
	_features[6] = &_log_feature;

	unsigned n_features = 7;
#ifdef HAVE_LV2_1_2_0
	_features[n_features++] = &_def_state_feature;
#endif

	lv2_atom_forge_init(&_impl->forge, _uri_map.urid_map());
	lv2_atom_forge_init(&_impl->ui_forge, _uri_map.urid_map());

#ifdef HAVE_LV2_1_2_0
	LV2_URID atom_Int = _uri_map.uri_to_id(LV2_ATOM__Int);
	static const int32_t _min_block_length = 1;   // may happen during split-cycles
	static const int32_t _max_block_length = 8192; // max possible (with all engines and during export)
	/* Consider updating max-block-size whenever the buffersize changes.
	 * It requires re-instantiating the plugin (which is a non-realtime operation),
	 * so it should be done lightly and only for plugins that require it.
	 *
	 * given that the block-size can change at any time (split-cycles) ardour currently
	 * does not support plugins that require bufz_fixedBlockLength.
	 */
	LV2_Options_Option options[] = {
		{ LV2_OPTIONS_INSTANCE, 0, _uri_map.uri_to_id(LV2_BUF_SIZE__minBlockLength),
		  sizeof(int32_t), atom_Int, &_min_block_length },
		{ LV2_OPTIONS_INSTANCE, 0, _uri_map.uri_to_id(LV2_BUF_SIZE__maxBlockLength),
		  sizeof(int32_t), atom_Int, &_max_block_length },
		{ LV2_OPTIONS_INSTANCE, 0, _uri_map.uri_to_id(LV2_BUF_SIZE__sequenceSize),
		  sizeof(int32_t), atom_Int, &_seq_size },
		{ LV2_OPTIONS_INSTANCE, 0, _uri_map.uri_to_id("http://lv2plug.in/ns/ext/buf-size#nominalBlockLength"),
		  sizeof(int32_t), atom_Int, &_impl->block_length },
		{ LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL }
	};

	_impl->options = (LV2_Options_Option*) malloc (sizeof (options));
	memcpy ((void*) _impl->options, (void*) options, sizeof (options));

	_options_feature.URI    = LV2_OPTIONS__options;
	_options_feature.data   = _impl->options;
	_features[n_features++] = &_options_feature;
#endif

	LV2_State_Make_Path* make_path = (LV2_State_Make_Path*)malloc(
		sizeof(LV2_State_Make_Path));
	make_path->handle = this;
	make_path->path = &lv2_state_make_path;
	_make_path_feature.data = make_path;

	LV2_Log_Log* log = (LV2_Log_Log*)malloc(sizeof(LV2_Log_Log));
	log->handle  = this;
	log->printf  = &log_printf;
	log->vprintf = &log_vprintf;
	_log_feature.data = log;

	LilvNode* worker_schedule = lilv_new_uri(_world.world, LV2_WORKER__schedule);
	if (lilv_plugin_has_feature(plugin, worker_schedule)) {
		LV2_Worker_Schedule* schedule = (LV2_Worker_Schedule*)malloc(
			sizeof(LV2_Worker_Schedule));
		size_t buf_size = _session.engine().raw_buffer_size(DataType::MIDI) * NBUFS;
		_worker                     = new Worker(this, buf_size);
		schedule->handle            = this;
		schedule->schedule_work     = work_schedule;
		_work_schedule_feature.data = schedule;
		_features[n_features++]     = &_work_schedule_feature;
	}
	lilv_node_free(worker_schedule);

	_impl->instance = lilv_plugin_instantiate(plugin, rate, _features);
	_impl->name     = lilv_plugin_get_name(plugin);
	_impl->author   = lilv_plugin_get_author_name(plugin);

	if (_impl->instance == 0) {
		error << _("LV2: Failed to instantiate plugin ") << uri() << endmsg;
		throw failed_constructor();
	}

	_instance_access_feature.data              = (void*)_impl->instance->lv2_handle;
	_data_access_extension_data.extension_data = _impl->instance->lv2_descriptor->extension_data;
	_data_access_feature.data                  = &_data_access_extension_data;

	LilvNode* worker_iface_uri = lilv_new_uri(_world.world, LV2_WORKER__interface);
	if (lilv_plugin_has_extension_data(plugin, worker_iface_uri)) {
		_impl->work_iface = (const LV2_Worker_Interface*)extension_data(
			LV2_WORKER__interface);
	}
	lilv_node_free(worker_iface_uri);


	LilvNode* options_iface_uri = lilv_new_uri(_world.world, LV2_OPTIONS__interface);
	if (lilv_plugin_has_extension_data(plugin, options_iface_uri)) {
		_impl->opts_iface = (const LV2_Options_Interface*)extension_data(
			LV2_OPTIONS__interface);
	}
	lilv_node_free(options_iface_uri);

	if (lilv_plugin_has_feature(plugin, _world.lv2_inPlaceBroken)) {
		error << string_compose(
		    _("LV2: \"%1\" cannot be used, since it cannot do inplace processing."),
		    lilv_node_as_string(_impl->name)) << endmsg;
		lilv_node_free(_impl->name);
		lilv_node_free(_impl->author);
		throw failed_constructor();
	}

#ifdef HAVE_LV2_1_2_0
	LilvNodes *required_features = lilv_plugin_get_required_features (plugin);
	if (lilv_nodes_contains (required_features, _world.bufz_powerOf2BlockLength) ||
			lilv_nodes_contains (required_features, _world.bufz_fixedBlockLength)
	   ) {
		error << string_compose(
		    _("LV2: \"%1\" buffer-size requirements cannot be satisfied."),
		    lilv_node_as_string(_impl->name)) << endmsg;
		lilv_node_free(_impl->name);
		lilv_node_free(_impl->author);
		lilv_nodes_free(required_features);
		throw failed_constructor();
	}
	lilv_nodes_free(required_features);
#endif

#ifdef HAVE_LILV_0_16_0
	// Load default state
	LilvState* state = lilv_state_new_from_world(
		_world.world, _uri_map.urid_map(), lilv_plugin_get_uri(_impl->plugin));
	if (state && _has_state_interface) {
		lilv_state_restore(state, _impl->instance, NULL, NULL, 0, NULL);
	}
	lilv_state_free(state);
#endif

	_sample_rate = rate;

	const uint32_t num_ports = this->num_ports();
	for (uint32_t i = 0; i < num_ports; ++i) {
		const LilvPort* port  = lilv_plugin_get_port_by_index(_impl->plugin, i);
		PortFlags       flags = 0;
		size_t          minimumSize = 0;

		if (lilv_port_is_a(_impl->plugin, port, _world.lv2_OutputPort)) {
			flags |= PORT_OUTPUT;
		} else if (lilv_port_is_a(_impl->plugin, port, _world.lv2_InputPort)) {
			flags |= PORT_INPUT;
		} else {
			error << string_compose(
				"LV2: \"%1\" port %2 is neither input nor output",
				lilv_node_as_string(_impl->name), i) << endmsg;
			throw failed_constructor();
		}

		if (lilv_port_is_a(_impl->plugin, port, _world.lv2_ControlPort)) {
			flags |= PORT_CONTROL;
		} else if (lilv_port_is_a(_impl->plugin, port, _world.lv2_AudioPort)) {
			flags |= PORT_AUDIO;
		} else if (lilv_port_is_a(_impl->plugin, port, _world.ev_EventPort)) {
			flags |= PORT_EVENT;
			flags |= PORT_MIDI;  // We assume old event API ports are for MIDI
		} else if (lilv_port_is_a(_impl->plugin, port, _world.atom_AtomPort)) {
			LilvNodes* buffer_types = lilv_port_get_value(
				_impl->plugin, port, _world.atom_bufferType);
			LilvNodes* atom_supports = lilv_port_get_value(
				_impl->plugin, port, _world.atom_supports);

			if (lilv_nodes_contains(buffer_types, _world.atom_Sequence)) {
				flags |= PORT_SEQUENCE;
				if (lilv_nodes_contains(atom_supports, _world.midi_MidiEvent)) {
					flags |= PORT_MIDI;
				}
				if (lilv_nodes_contains(atom_supports, _world.time_Position)) {
					flags |= PORT_POSITION;
				}
				if (lilv_nodes_contains(atom_supports, _world.patch_Message)) {
					flags |= PORT_PATCHMSG;
					if (flags & PORT_INPUT) {
						_patch_port_in_index = i;
					} else {
						_patch_port_out_index = i;
					}
				}
			}
			LilvNodes* min_size_v = lilv_port_get_value(_impl->plugin, port, _world.rsz_minimumSize);
			LilvNode* min_size = min_size_v ? lilv_nodes_get_first(min_size_v) : NULL;
			if (min_size && lilv_node_is_int(min_size)) {
				minimumSize = lilv_node_as_int(min_size);
			}
			lilv_nodes_free(min_size_v);
			lilv_nodes_free(buffer_types);
			lilv_nodes_free(atom_supports);
		} else {
			error << string_compose(
				"LV2: \"%1\" port %2 has no known data type",
				lilv_node_as_string(_impl->name), i) << endmsg;
			throw failed_constructor();
		}

		_port_flags.push_back(flags);
		_port_minimumSize.push_back(minimumSize);
	}

	_control_data = new float[num_ports];
	_shadow_data  = new float[num_ports];
	_defaults     = new float[num_ports];
	_ev_buffers   = new LV2_Evbuf*[num_ports];
	memset(_ev_buffers, 0, sizeof(LV2_Evbuf*) * num_ports);

	const bool     latent        = lilv_plugin_has_latency(plugin);
	const uint32_t latency_index = (latent)
		? lilv_plugin_get_latency_port_index(plugin)
		: 0;

	// Build an array of pointers to special parameter buffers
	void*** params = new void**[num_ports];
	for (uint32_t i = 0; i < num_ports; ++i) {
		params[i] = NULL;
	}
	_impl->designated_input (LV2_TIME__beatsPerMinute, params, (void**)&_bpm_control_port);
	_impl->designated_input (LV2_CORE__freeWheeling, params, (void**)&_freewheel_control_port);

	for (uint32_t i = 0; i < num_ports; ++i) {
		const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
		const LilvNode* sym  = lilv_port_get_symbol(plugin, port);

		// Store index in map so we can look up index by symbol
		_port_indices.insert(std::make_pair(lilv_node_as_string(sym), i));

		// Get range and default value if applicable
		if (parameter_is_control(i)) {
			LilvNode* def;
			lilv_port_get_range(plugin, port, &def, NULL, NULL);
			_defaults[i] = def ? lilv_node_as_float(def) : 0.0f;
			if (lilv_port_has_property (plugin, port, _world.lv2_sampleRate)) {
				_defaults[i] *= _session.frame_rate ();
			}
			lilv_node_free(def);

			lilv_instance_connect_port(_impl->instance, i, &_control_data[i]);

			if (latent && i == latency_index) {
				_latency_control_port  = &_control_data[i];
				*_latency_control_port = 0;
			}

			if (parameter_is_input(i)) {
				_shadow_data[i] = default_value(i);
				if (params[i]) {
					*params[i] = (void*)&_shadow_data[i];
				}
			}
		} else {
			_defaults[i] = 0.0f;
		}
	}

	delete[] params;

	LilvUIs* uis = lilv_plugin_get_uis(plugin);
	if (lilv_uis_size(uis) > 0) {
#ifdef HAVE_SUIL
		// Look for embeddable UI
		LILV_FOREACH(uis, u, uis) {
			const LilvUI*   this_ui      = lilv_uis_get(uis, u);
			const LilvNode* this_ui_type = NULL;
			if (lilv_ui_is_supported(this_ui,
			                         suil_ui_supported,
			                         _world.ui_GtkUI,
			                         &this_ui_type)) {
				// TODO: Multiple UI support
				_impl->ui      = this_ui;
				_impl->ui_type = this_ui_type;
				break;
			}
		}
#else
		// Look for Gtk native UI
		LILV_FOREACH(uis, i, uis) {
			const LilvUI* ui = lilv_uis_get(uis, i);
			if (lilv_ui_is_a(ui, _world.ui_GtkUI)) {
				_impl->ui      = ui;
				_impl->ui_type = _world.ui_GtkUI;
				break;
			}
		}
#endif

		// If Gtk UI is not available, try to find external UI
		if (!_impl->ui) {
			LILV_FOREACH(uis, i, uis) {
				const LilvUI* ui = lilv_uis_get(uis, i);
				if (lilv_ui_is_a(ui, _world.ui_externalkx)) {
					_impl->ui      = ui;
					_impl->ui_type = _world.ui_external;
					break;
				}
				if (lilv_ui_is_a(ui, _world.ui_external)) {
					_impl->ui      = ui;
					_impl->ui_type = _world.ui_external;
				}
			}
		}
	}

	load_supported_properties(_property_descriptors);
	allocate_atom_event_buffers();
	latency_compute_run();
}

int
LV2Plugin::set_block_size (pframes_t nframes)
{
#ifdef HAVE_LV2_1_2_0
	if (_impl->opts_iface) {
		LV2_URID atom_Int = _uri_map.uri_to_id(LV2_ATOM__Int);
		_impl->block_length = nframes;
		LV2_Options_Option block_size_option = {
			LV2_OPTIONS_INSTANCE, 0, _uri_map.uri_to_id ("http://lv2plug.in/ns/ext/buf-size#nominalBlockLength"),
			sizeof(int32_t), atom_Int, (void*)&_impl->block_length
		};
		_impl->opts_iface->set (_impl->instance->lv2_handle, &block_size_option);
	}
#endif
	return 0;
}

LV2Plugin::~LV2Plugin ()
{
	DEBUG_TRACE(DEBUG::LV2, string_compose("%1 destroy\n", name()));

	deactivate();
	cleanup();

	lilv_instance_free(_impl->instance);
	lilv_state_free(_impl->state);
	lilv_node_free(_impl->name);
	lilv_node_free(_impl->author);
	free(_impl->options);

	free(_features);
	free(_make_path_feature.data);
	free(_work_schedule_feature.data);

	delete _to_ui;
	delete _from_ui;
	delete _worker;

	if (_atom_ev_buffers) {
		LV2_Evbuf**  b = _atom_ev_buffers;
		while (*b) {
			free(*b);
			b++;
		}
		free(_atom_ev_buffers);
	}

	delete [] _control_data;
	delete [] _shadow_data;
	delete [] _defaults;
	delete [] _ev_buffers;
}

bool
LV2Plugin::is_external_ui() const
{
	if (!_impl->ui) {
		return false;
	}
	return lilv_ui_is_a(_impl->ui, _world.ui_external) || lilv_ui_is_a(_impl->ui, _world.ui_externalkx);
}

bool
LV2Plugin::is_external_kx() const
{
	if (!_impl->ui) {
		return false;
	}
	return lilv_ui_is_a(_impl->ui, _world.ui_externalkx);
}

bool
LV2Plugin::ui_is_resizable () const
{
	const LilvNode* s   = lilv_ui_get_uri(_impl->ui);
	LilvNode*       p   = lilv_new_uri(_world.world, LV2_CORE__optionalFeature);
	LilvNode*       fs  = lilv_new_uri(_world.world, LV2_UI__fixedSize);
	LilvNode*       nrs = lilv_new_uri(_world.world, LV2_UI__noUserResize);

	LilvNodes* fs_matches = lilv_world_find_nodes(_world.world, s, p, fs);
	LilvNodes* nrs_matches = lilv_world_find_nodes(_world.world, s, p, nrs);

	lilv_nodes_free(nrs_matches);
	lilv_nodes_free(fs_matches);
	lilv_node_free(nrs);
	lilv_node_free(fs);
	lilv_node_free(p);

	return !fs_matches && !nrs_matches;
}

string
LV2Plugin::unique_id() const
{
	return lilv_node_as_uri(lilv_plugin_get_uri(_impl->plugin));
}

const char*
LV2Plugin::uri() const
{
	return lilv_node_as_uri(lilv_plugin_get_uri(_impl->plugin));
}

const char*
LV2Plugin::label() const
{
	return lilv_node_as_string(_impl->name);
}

const char*
LV2Plugin::name() const
{
	return lilv_node_as_string(_impl->name);
}

const char*
LV2Plugin::maker() const
{
	return _impl->author ? lilv_node_as_string (_impl->author) : "Unknown";
}

uint32_t
LV2Plugin::num_ports() const
{
	return lilv_plugin_get_num_ports(_impl->plugin);
}

uint32_t
LV2Plugin::parameter_count() const
{
	return lilv_plugin_get_num_ports(_impl->plugin);
}

float
LV2Plugin::default_value(uint32_t port)
{
	return _defaults[port];
}

const char*
LV2Plugin::port_symbol(uint32_t index) const
{
	const LilvPort* port = lilv_plugin_get_port_by_index(_impl->plugin, index);
	if (!port) {
		error << name() << ": Invalid port index " << index << endmsg;
	}

	const LilvNode* sym = lilv_port_get_symbol(_impl->plugin, port);
	return lilv_node_as_string(sym);
}

uint32_t
LV2Plugin::port_index (const char* symbol) const
{
	const map<string, uint32_t>::const_iterator i = _port_indices.find(symbol);
	if (i != _port_indices.end()) {
		return  i->second;
	} else {
		warning << string_compose(_("LV2: Unknown port %1"), symbol) << endmsg;
		return (uint32_t)-1;
	}
}

void
LV2Plugin::set_parameter(uint32_t which, float val)
{
	DEBUG_TRACE(DEBUG::LV2, string_compose(
		            "%1 set parameter %2 to %3\n", name(), which, val));

	if (which < lilv_plugin_get_num_ports(_impl->plugin)) {
		if (get_parameter (which) == val) {
			return;
		}

		_shadow_data[which] = val;
	} else {
		warning << string_compose(
		    _("Illegal parameter number used with plugin \"%1\". "
		      "This is a bug in either %2 or the LV2 plugin <%3>"),
		    name(), PROGRAM_NAME, unique_id()) << endmsg;
	}

	Plugin::set_parameter(which, val);
}

float
LV2Plugin::get_parameter(uint32_t which) const
{
	if (parameter_is_input(which)) {
		return (float)_shadow_data[which];
	} else {
		return (float)_control_data[which];
	}
	return 0.0f;
}

std::string
LV2Plugin::get_docs() const
{
	LilvNodes* comments = lilv_plugin_get_value(_impl->plugin, _world.rdfs_comment);
	if (comments) {
		const std::string docs(lilv_node_as_string(lilv_nodes_get_first(comments)));
		lilv_nodes_free(comments);
		return docs;
	}

	return "";
}

std::string
LV2Plugin::get_parameter_docs(uint32_t which) const
{
	LilvNodes* comments = lilv_port_get_value(
		_impl->plugin,
		lilv_plugin_get_port_by_index(_impl->plugin, which),
		_world.rdfs_comment);

	if (comments) {
		const std::string docs(lilv_node_as_string(lilv_nodes_get_first(comments)));
		lilv_nodes_free(comments);
		return docs;
	}

	return "";
}

uint32_t
LV2Plugin::nth_parameter(uint32_t n, bool& ok) const
{
	ok = false;
	for (uint32_t c = 0, x = 0; x < lilv_plugin_get_num_ports(_impl->plugin); ++x) {
		if (parameter_is_control(x)) {
			if (c++ == n) {
				ok = true;
				return x;
			}
		}
	}

	return 0;
}

const void*
LV2Plugin::extension_data(const char* uri) const
{
	return lilv_instance_get_extension_data(_impl->instance, uri);
}

const void*
LV2Plugin::c_plugin()
{
	return _impl->plugin;
}

const void*
LV2Plugin::c_ui()
{
	return (const void*)_impl->ui;
}

const void*
LV2Plugin::c_ui_type()
{
	return (const void*)_impl->ui_type;
}

/** Directory for all plugin state. */
const std::string
LV2Plugin::plugin_dir() const
{
	return Glib::build_filename(_session.plugins_dir(), _insert_id.to_s());
}

/** Directory for files created by the plugin (except during save). */
const std::string
LV2Plugin::scratch_dir() const
{
	return Glib::build_filename(plugin_dir(), "scratch");
}

/** Directory for snapshots of files in the scratch directory. */
const std::string
LV2Plugin::file_dir() const
{
	return Glib::build_filename(plugin_dir(), "files");
}

/** Directory to save state snapshot version @c num into. */
const std::string
LV2Plugin::state_dir(unsigned num) const
{
	return Glib::build_filename(plugin_dir(), string_compose("state%1", num));
}

/** Implementation of state:makePath for files created at instantiation time.
 * Note this is not used for files created at save time (Lilv deals with that).
 */
char*
LV2Plugin::lv2_state_make_path(LV2_State_Make_Path_Handle handle,
                               const char*                path)
{
	LV2Plugin* me = (LV2Plugin*)handle;
	if (me->_insert_id == PBD::ID("0")) {
		warning << string_compose(
			"File path \"%1\" requested but LV2 %2 has no insert ID",
			path, me->name()) << endmsg;
		return g_strdup(path);
	}

	const std::string abs_path = Glib::build_filename(me->scratch_dir(), path);
	const std::string dirname  = Glib::path_get_dirname(abs_path);
	g_mkdir_with_parents(dirname.c_str(), 0744);

	DEBUG_TRACE(DEBUG::LV2, string_compose("new file path %1 => %2\n",
	                                       path, abs_path));

	return g_strndup(abs_path.c_str(), abs_path.length());
}

void
LV2Plugin::add_state(XMLNode* root) const
{
	assert(_insert_id != PBD::ID("0"));

	XMLNode*    child;
	char        buf[16];
	LocaleGuard lg(X_("C"));

	for (uint32_t i = 0; i < parameter_count(); ++i) {
		if (parameter_is_input(i) && parameter_is_control(i)) {
			child = new XMLNode("Port");
			child->add_property("symbol", port_symbol(i));
			snprintf(buf, sizeof(buf), "%+f", _shadow_data[i]);
			child->add_property("value", string(buf));
			root->add_child_nocopy(*child);
		}
	}

	if (_has_state_interface) {
		// Provisionally increment state version and create directory
		const std::string new_dir = state_dir(++_state_version);
		g_mkdir_with_parents(new_dir.c_str(), 0744);

		LilvState* state = lilv_state_new_from_instance(
			_impl->plugin,
			_impl->instance,
			_uri_map.urid_map(),
			scratch_dir().c_str(),
			file_dir().c_str(),
			_session.externals_dir().c_str(),
			new_dir.c_str(),
			NULL,
			const_cast<LV2Plugin*>(this),
			0,
			NULL);

		if (!_impl->state || !lilv_state_equals(state, _impl->state)) {
			lilv_state_save(_world.world,
			                _uri_map.urid_map(),
			                _uri_map.urid_unmap(),
			                state,
			                NULL,
			                new_dir.c_str(),
			                "state.ttl");

			lilv_state_free(_impl->state);
			_impl->state = state;
		} else {
			// State is identical, decrement version and nuke directory
			lilv_state_free(state);
			PBD::remove_directory(new_dir);
			--_state_version;
		}

		root->add_property("state-dir", string_compose("state%1", _state_version));
	}
}

// TODO: Once we can rely on lilv 0.16.0, lilv_world_get can replace this
static LilvNode*
get_value(LilvWorld* world, const LilvNode* subject, const LilvNode* predicate)
{
	LilvNodes* vs = lilv_world_find_nodes(world, subject, predicate, NULL);
	if (vs) {
		LilvNode* node = lilv_node_duplicate(lilv_nodes_get_first(vs));
		lilv_nodes_free(vs);
		return node;
	}
	return NULL;
}

void
LV2Plugin::find_presets()
{
	LilvNode* lv2_appliesTo = lilv_new_uri(_world.world, LV2_CORE__appliesTo);
	LilvNode* pset_Preset   = lilv_new_uri(_world.world, LV2_PRESETS__Preset);
	LilvNode* rdfs_label    = lilv_new_uri(_world.world, LILV_NS_RDFS "label");

	LilvNodes* presets = lilv_plugin_get_related(_impl->plugin, pset_Preset);
	LILV_FOREACH(nodes, i, presets) {
		const LilvNode* preset = lilv_nodes_get(presets, i);
		lilv_world_load_resource(_world.world, preset);
		LilvNode* name = get_value(_world.world, preset, rdfs_label);
		if (name) {
			_presets.insert(std::make_pair(lilv_node_as_string(preset),
			                               Plugin::PresetRecord(
				                               lilv_node_as_string(preset),
				                               lilv_node_as_string(name))));
			lilv_node_free(name);
		} else {
			warning << string_compose(
			    _("Plugin \"%1\" preset \"%2\" is missing a label\n"),
			    lilv_node_as_string(lilv_plugin_get_uri(_impl->plugin)),
			    lilv_node_as_string(preset)) << endmsg;
		}
	}
	lilv_nodes_free(presets);

	lilv_node_free(rdfs_label);
	lilv_node_free(pset_Preset);
	lilv_node_free(lv2_appliesTo);
}

static void
set_port_value(const char* port_symbol,
               void*       user_data,
               const void* value,
               uint32_t    /*size*/,
               uint32_t    type)
{
	LV2Plugin* self = (LV2Plugin*)user_data;
	if (type != 0 && type != URIMap::instance().urids.atom_Float) {
		return;  // TODO: Support non-float ports
	}

	const uint32_t port_index = self->port_index(port_symbol);
	if (port_index != (uint32_t)-1) {
		self->set_parameter(port_index, *(const float*)value);
	}
}

bool
LV2Plugin::load_preset(PresetRecord r)
{
	LilvWorld* world = _world.world;
	LilvNode*  pset  = lilv_new_uri(world, r.uri.c_str());
	LilvState* state = lilv_state_new_from_world(world, _uri_map.urid_map(), pset);

	if (state) {
		lilv_state_restore(state, _impl->instance, set_port_value, this, 0, NULL);
		lilv_state_free(state);
		Plugin::load_preset(r);
	}

	lilv_node_free(pset);
	return state;
}

const void*
ARDOUR::lv2plugin_get_port_value(const char* port_symbol,
                                 void*       user_data,
                                 uint32_t*   size,
                                 uint32_t*   type)
{
	LV2Plugin *plugin = (LV2Plugin *) user_data;

	uint32_t index = plugin->port_index(port_symbol);
	if (index != (uint32_t) -1) {
		if (plugin->parameter_is_input(index) && plugin->parameter_is_control(index)) {
			float *value;
			*size = sizeof(float);
			*type = plugin->_uri_map.uri_to_id(LV2_ATOM__Float);
			value = &plugin->_shadow_data[index];

			return value;
		}
	}

	*size = *type = 0;
	return NULL;
}


std::string
LV2Plugin::do_save_preset(string name)
{
	LilvNode*    plug_name = lilv_plugin_get_name(_impl->plugin);
	const string prefix    = legalize_for_uri(lilv_node_as_string(plug_name));
	const string base_name = legalize_for_uri(name);
	const string file_name = base_name + ".ttl";
	const string bundle    = Glib::build_filename(
		Glib::get_home_dir(),
		Glib::build_filename(".lv2", prefix + "_" + base_name + ".lv2"));

#ifdef HAVE_LILV_0_21_3
	/* delete reference to old preset (if any) */
	const PresetRecord* r = preset_by_label(name);
	if (r) {
		LilvNode*  pset  = lilv_new_uri (_world.world, r->uri.c_str());
		if (pset) {
			lilv_world_unload_resource (_world.world, pset);
			lilv_node_free(pset);
		}
	}
#endif

	LilvState* state = lilv_state_new_from_instance(
		_impl->plugin,
		_impl->instance,
		_uri_map.urid_map(),
		scratch_dir().c_str(),                   // file_dir
		bundle.c_str(),                          // copy_dir
		bundle.c_str(),                          // link_dir
		bundle.c_str(),                          // save_dir
		lv2plugin_get_port_value,                // get_value
		(void*)this,                             // user_data
		LV2_STATE_IS_POD|LV2_STATE_IS_PORTABLE,  // flags
		_features                                // features
	);

	lilv_state_set_label(state, name.c_str());
	lilv_state_save(
		_world.world,           // world
		_uri_map.urid_map(),    // map
		_uri_map.urid_unmap(),  // unmap
		state,                  // state
		NULL,                   // uri (NULL = use file URI)
		bundle.c_str(),         // dir
		file_name.c_str()       // filename
	);

	lilv_state_free(state);

	std::string uri = Glib::filename_to_uri(Glib::build_filename(bundle, file_name));
	LilvNode *node_bundle = lilv_new_uri(_world.world, Glib::filename_to_uri(Glib::build_filename(bundle, "/")).c_str());
	LilvNode *node_preset = lilv_new_uri(_world.world, uri.c_str());
#ifdef HAVE_LILV_0_21_3
	lilv_world_unload_resource(_world.world, node_preset);
	lilv_world_unload_bundle(_world.world, node_bundle);
#endif
	lilv_world_load_bundle(_world.world, node_bundle);
	lilv_world_load_resource(_world.world, node_preset);
	lilv_node_free(node_bundle);
	lilv_node_free(node_preset);
	lilv_node_free(plug_name);
	return uri;
}

void
LV2Plugin::do_remove_preset(string name)
{
#ifdef HAVE_LILV_0_21_3
	/* Look up preset record by label (FIXME: ick, label as ID) */
	const PresetRecord* r = preset_by_label(name);
	if (!r) {
		return;
	}

	/* Load a LilvState for the preset. */
	LilvWorld* world = _world.world;
	LilvNode*  pset  = lilv_new_uri(world, r->uri.c_str());
	LilvState* state = lilv_state_new_from_world(world, _uri_map.urid_map(), pset);
	if (!state) {
		lilv_node_free(pset);
		return;
	}

	/* Unload preset from world. */
	lilv_world_unload_resource(world, pset);

	/* Delete it from the file system.  This will remove the preset file and the entry
	   from the manifest.  If this results in an empty manifest (i.e. the
	   preset is the only thing in the bundle), then the bundle is removed. */
	lilv_state_delete(world, state);

	lilv_state_free(state);
	lilv_node_free(pset);
#endif
	/* Without lilv_state_delete(), we could delete the preset file, but this
	   would leave a broken bundle/manifest around, so the preset would still
	   be visible, but broken.  Naively deleting a bundle is too dangerous, so
	   we simply do not support preset deletion with older Lilv */
}

bool
LV2Plugin::has_editor() const
{
	return _impl->ui != NULL;
}

bool
LV2Plugin::has_message_output() const
{
	for (uint32_t i = 0; i < num_ports(); ++i) {
		if ((_port_flags[i] & PORT_SEQUENCE) &&
		    (_port_flags[i] & PORT_OUTPUT)) {
			return true;
		}
	}
	return false;
}

bool
LV2Plugin::write_to(RingBuffer<uint8_t>* dest,
                    uint32_t             index,
                    uint32_t             protocol,
                    uint32_t             size,
                    const uint8_t*       body)
{
	const uint32_t  buf_size = sizeof(UIMessage) + size;
	vector<uint8_t> buf(buf_size);

	UIMessage* msg = (UIMessage*)&buf[0];
	msg->index    = index;
	msg->protocol = protocol;
	msg->size     = size;
	memcpy(msg + 1, body, size);

	return (dest->write(&buf[0], buf_size) == buf_size);
}

bool
LV2Plugin::write_from_ui(uint32_t       index,
                         uint32_t       protocol,
                         uint32_t       size,
                         const uint8_t* body)
{
	if (!_from_ui) {
		size_t rbs = _session.engine().raw_buffer_size(DataType::MIDI) * NBUFS;
		/* buffer data communication from plugin UI to plugin instance.
		 * this buffer needs to potentially hold
		 *   (port's minimumSize) * (audio-periods) / (UI-periods)
		 * bytes.
		 *
		 *  e.g 48kSPS / 128fpp -> audio-periods = 375 Hz
		 *  ui-periods = 25 Hz (SuperRapidScreenUpdate)
		 *  default minimumSize = 32K (see LV2Plugin::allocate_atom_event_buffers()
		 *
		 * it is NOT safe to overflow (msg.size will be misinterpreted)
		 */
		uint32_t bufsiz = 32768;
		if (_atom_ev_buffers && _atom_ev_buffers[0]) {
			bufsiz =  lv2_evbuf_get_capacity(_atom_ev_buffers[0]);
		}
		rbs = max((size_t) bufsiz * 8, rbs);
		_from_ui = new RingBuffer<uint8_t>(rbs);
	}

	if (!write_to(_from_ui, index, protocol, size, body)) {
		error << "Error writing from UI to plugin" << endmsg;
		return false;
	}
	return true;
}

bool
LV2Plugin::write_to_ui(uint32_t       index,
                       uint32_t       protocol,
                       uint32_t       size,
                       const uint8_t* body)
{
	if (!write_to(_to_ui, index, protocol, size, body)) {
		error << "Error writing from plugin to UI" << endmsg;
		return false;
	}
	return true;
}

static void
forge_variant(LV2_Atom_Forge* forge, const Variant& value)
{
	switch (value.type()) {
	case Variant::NOTHING:
		break;
	case Variant::BEATS:
		// No atom type for this, just forge a double
		lv2_atom_forge_double(forge, value.get_beats().to_double());
		break;
	case Variant::BOOL:
		lv2_atom_forge_bool(forge, value.get_bool());
		break;
	case Variant::DOUBLE:
		lv2_atom_forge_double(forge, value.get_double());
		break;
	case Variant::FLOAT:
		lv2_atom_forge_float(forge, value.get_float());
		break;
	case Variant::INT:
		lv2_atom_forge_int(forge, value.get_int());
		break;
	case Variant::LONG:
		lv2_atom_forge_long(forge, value.get_long());
		break;
	case Variant::PATH:
		lv2_atom_forge_path(
			forge, value.get_path().c_str(), value.get_path().size());
		break;
	case Variant::STRING:
		lv2_atom_forge_string(
			forge, value.get_string().c_str(), value.get_string().size());
		break;
	case Variant::URI:
		lv2_atom_forge_uri(
			forge, value.get_uri().c_str(), value.get_uri().size());
		break;
	}
}

/** Get a variant type from a URI, return false iff no match found. */
static bool
uri_to_variant_type(const std::string& uri, Variant::Type& type)
{
	if (uri == LV2_ATOM__Bool) {
		type = Variant::BOOL;
	} else if (uri == LV2_ATOM__Double) {
		type = Variant::DOUBLE;
	} else if (uri == LV2_ATOM__Float) {
		type = Variant::FLOAT;
	} else if (uri == LV2_ATOM__Int) {
		type = Variant::INT;
	} else if (uri == LV2_ATOM__Long) {
		type = Variant::LONG;
	} else if (uri == LV2_ATOM__Path) {
		type = Variant::PATH;
	} else if (uri == LV2_ATOM__String) {
		type = Variant::STRING;
	} else if (uri == LV2_ATOM__URI) {
		type = Variant::URI;
	} else {
		return false;
	}
	return true;
}

void
LV2Plugin::set_property(uint32_t key, const Variant& value)
{
	if (_patch_port_in_index == (uint32_t)-1) {
		error << "LV2: set_property called with unset patch_port_in_index" << endmsg;
		return;
	} else if (value.type() == Variant::NOTHING) {
		error << "LV2: set_property called with void value" << endmsg;
		return;
	}

	// Set up forge to write to temporary buffer on the stack
	LV2_Atom_Forge*      forge = &_impl->ui_forge;
	LV2_Atom_Forge_Frame frame;
	uint8_t              buf[PATH_MAX];  // Ought to be enough for anyone...

	lv2_atom_forge_set_buffer(forge, buf, sizeof(buf));

	// Serialize patch:Set message to set property
#ifdef HAVE_LV2_1_10_0
	lv2_atom_forge_object(forge, &frame, 1, _uri_map.urids.patch_Set);
	lv2_atom_forge_key(forge, _uri_map.urids.patch_property);
	lv2_atom_forge_urid(forge, key);
	lv2_atom_forge_key(forge, _uri_map.urids.patch_value);
#else
	lv2_atom_forge_blank(forge, &frame, 1, _uri_map.urids.patch_Set);
	lv2_atom_forge_property_head(forge, _uri_map.urids.patch_property, 0);
	lv2_atom_forge_urid(forge, key);
	lv2_atom_forge_property_head(forge, _uri_map.urids.patch_value, 0);
#endif

	forge_variant(forge, value);

	// Write message to UI=>Plugin ring
	const LV2_Atom* const atom = (const LV2_Atom*)buf;
	write_from_ui(_patch_port_in_index,
	              _uri_map.urids.atom_eventTransfer,
	              lv2_atom_total_size(atom),
	              (const uint8_t*)atom);
}

const ParameterDescriptor&
LV2Plugin::get_property_descriptor(uint32_t id) const
{
	PropertyDescriptors::const_iterator p = _property_descriptors.find(id);
	if (p != _property_descriptors.end()) {
		return p->second;
	}
	return Plugin::get_property_descriptor(id);
}

static void
load_parameter_descriptor_units(LilvWorld* lworld, ParameterDescriptor& desc, const LilvNodes* units)
{
	if (lilv_nodes_contains(units, _world.units_midiNote)) {
		desc.unit = ParameterDescriptor::MIDI_NOTE;
	} else if (lilv_nodes_contains(units, _world.units_db)) {
		desc.unit = ParameterDescriptor::DB;
	} else if (lilv_nodes_contains(units, _world.units_hz)) {
		desc.unit = ParameterDescriptor::HZ;
	}
	if (lilv_nodes_size(units) > 0) {
		const LilvNode* unit = lilv_nodes_get_first(units);
		LilvNode* render = get_value(lworld, unit, _world.units_render);
		if (render) {
			desc.print_fmt = lilv_node_as_string(render);
			lilv_node_free(render);
		}
	}
}

static void
load_parameter_descriptor(LV2World&            world,
                          ParameterDescriptor& desc,
                          Variant::Type        datatype,
                          const LilvNode*      subject)
{
	LilvWorld* lworld  = _world.world;
	LilvNode*  label   = get_value(lworld, subject, _world.rdfs_label);
	LilvNode*  def     = get_value(lworld, subject, _world.lv2_default);
	LilvNode*  minimum = get_value(lworld, subject, _world.lv2_minimum);
	LilvNode*  maximum = get_value(lworld, subject, _world.lv2_maximum);
	LilvNodes* units   = lilv_world_find_nodes(lworld, subject, _world.units_unit, NULL);
	if (label) {
		desc.label = lilv_node_as_string(label);
	}
	if (def && lilv_node_is_float(def)) {
		desc.normal = lilv_node_as_float(def);
	}
	if (minimum && lilv_node_is_float(minimum)) {
		desc.lower = lilv_node_as_float(minimum);
	}
	if (maximum && lilv_node_is_float(maximum)) {
		desc.upper = lilv_node_as_float(maximum);
	}
	load_parameter_descriptor_units(lworld, desc, units);
	desc.datatype      = datatype;
	desc.toggled      |= datatype == Variant::BOOL;
	desc.integer_step |= datatype == Variant::INT || datatype == Variant::LONG;
	desc.update_steps();

	lilv_nodes_free(units);
	lilv_node_free(label);
	lilv_node_free(def);
	lilv_node_free(minimum);
	lilv_node_free(maximum);
}

void
LV2Plugin::load_supported_properties(PropertyDescriptors& descs)
{
	LilvWorld*       lworld     = _world.world;
	const LilvNode*  subject    = lilv_plugin_get_uri(_impl->plugin);
	LilvNodes*       properties = lilv_world_find_nodes(
		lworld, subject, _world.patch_writable, NULL);
	LILV_FOREACH(nodes, p, properties) {
		// Get label and range
		const LilvNode* prop  = lilv_nodes_get(properties, p);
		LilvNode*       range = get_value(lworld, prop, _world.rdfs_range);
		if (!range) {
			warning << string_compose(_("LV2: property <%1> has no range datatype, ignoring"),
			                          lilv_node_as_uri(prop)) << endmsg;
			continue;
		}

		// Convert range to variant type (TODO: support for multiple range types)
		Variant::Type datatype;
		if (!uri_to_variant_type(lilv_node_as_uri(range), datatype)) {
			error << string_compose(_("LV2: property <%1> has unsupported datatype <%1>"),
			                        lilv_node_as_uri(prop), lilv_node_as_uri(range)) << endmsg;
			continue;
		}

		// Add description to result
		ParameterDescriptor desc;
		desc.key      = _uri_map.uri_to_id(lilv_node_as_uri(prop));
		desc.datatype = datatype;
		load_parameter_descriptor(_world, desc, datatype, prop);
		descs.insert(std::make_pair(desc.key, desc));

		lilv_node_free(range);
	}
	lilv_nodes_free(properties);
}

void
LV2Plugin::announce_property_values()
{
	if (_patch_port_in_index == (uint32_t)-1) {
		return;
	}

	// Set up forge to write to temporary buffer on the stack
	LV2_Atom_Forge*      forge = &_impl->ui_forge;
	LV2_Atom_Forge_Frame frame;
	uint8_t              buf[PATH_MAX];  // Ought to be enough for anyone...

	lv2_atom_forge_set_buffer(forge, buf, sizeof(buf));

	// Serialize patch:Get message with no subject (implicitly plugin instance)
#ifdef HAVE_LV2_1_10_0
	lv2_atom_forge_object(forge, &frame, 1, _uri_map.urids.patch_Get);
#else
	lv2_atom_forge_blank(forge, &frame, 1, _uri_map.urids.patch_Get);
#endif

	// Write message to UI=>Plugin ring
	const LV2_Atom* const atom = (const LV2_Atom*)buf;
	write_from_ui(_patch_port_in_index,
	              _uri_map.urids.atom_eventTransfer,
	              lv2_atom_total_size(atom),
	              (const uint8_t*)atom);
}

void
LV2Plugin::enable_ui_emission()
{
	if (!_to_ui) {
		/* see note in LV2Plugin::write_from_ui() */
		uint32_t bufsiz = 32768;
		if (_atom_ev_buffers && _atom_ev_buffers[0]) {
			bufsiz =  lv2_evbuf_get_capacity(_atom_ev_buffers[0]);
		}
		size_t rbs = _session.engine().raw_buffer_size(DataType::MIDI) * NBUFS;
		rbs = max((size_t) bufsiz * 8, rbs);
		_to_ui = new RingBuffer<uint8_t>(rbs);
	}
}

void
LV2Plugin::emit_to_ui(void* controller, UIMessageSink sink)
{
	if (!_to_ui) {
		return;
	}

	uint32_t read_space = _to_ui->read_space();
	while (read_space > sizeof(UIMessage)) {
		UIMessage msg;
		if (_to_ui->read((uint8_t*)&msg, sizeof(msg)) != sizeof(msg)) {
			error << "Error reading from Plugin=>UI RingBuffer" << endmsg;
			break;
		}
		vector<uint8_t> body(msg.size);
		if (_to_ui->read(&body[0], msg.size) != msg.size) {
			error << "Error reading from Plugin=>UI RingBuffer" << endmsg;
			break;
		}

		sink(controller, msg.index, msg.size, msg.protocol, &body[0]);

		read_space -= sizeof(msg) + msg.size;
	}
}

int
LV2Plugin::work(uint32_t size, const void* data)
{
	return _impl->work_iface->work(
		_impl->instance->lv2_handle, work_respond, this, size, data);
}

int
LV2Plugin::work_response(uint32_t size, const void* data)
{
	return _impl->work_iface->work_response(
		_impl->instance->lv2_handle, size, data);
}

void
LV2Plugin::set_insert_id(PBD::ID id)
{
	_insert_id = id;
}

int
LV2Plugin::set_state(const XMLNode& node, int version)
{
	XMLNodeList          nodes;
	const XMLProperty*   prop;
	XMLNodeConstIterator iter;
	XMLNode*             child;
	const char*          sym;
	const char*          value;
	uint32_t             port_id;
	LocaleGuard          lg(X_("C"));

	if (node.name() != state_node_name()) {
		error << _("Bad node sent to LV2Plugin::set_state") << endmsg;
		return -1;
	}

#ifndef NO_PLUGIN_STATE

	if (version < 3000) {
		nodes = node.children("port");
	} else {
		nodes = node.children("Port");
	}

	for (iter = nodes.begin(); iter != nodes.end(); ++iter) {

		child = *iter;

		if ((prop = child->property("symbol")) != 0) {
			sym = prop->value().c_str();
		} else {
			warning << _("LV2: port has no symbol, ignored") << endmsg;
			continue;
		}

		map<string, uint32_t>::iterator i = _port_indices.find(sym);

		if (i != _port_indices.end()) {
			port_id = i->second;
		} else {
			warning << _("LV2: port has unknown index, ignored") << endmsg;
			continue;
		}

		if ((prop = child->property("value")) != 0) {
			value = prop->value().c_str();
		} else {
			warning << _("LV2: port has no value, ignored") << endmsg;
			continue;
		}

		set_parameter(port_id, atof(value));
	}

	_state_version = 0;
	if ((prop = node.property("state-dir")) != 0) {
		if (sscanf(prop->value().c_str(), "state%u", &_state_version) != 1) {
			error << string_compose(
				"LV2: failed to parse state version from \"%1\"",
				prop->value()) << endmsg;
		}

		std::string state_file = Glib::build_filename(
			plugin_dir(),
			Glib::build_filename(prop->value(), "state.ttl"));

		LilvState* state = lilv_state_new_from_file(
			_world.world, _uri_map.urid_map(), NULL, state_file.c_str());

		lilv_state_restore(state, _impl->instance, NULL, NULL, 0, NULL);
		lilv_state_free(_impl->state);
		_impl->state = state;
	}

	latency_compute_run();
#endif

	return Plugin::set_state(node, version);
}

int
LV2Plugin::get_parameter_descriptor(uint32_t which, ParameterDescriptor& desc) const
{
	const LilvPort* port = lilv_plugin_get_port_by_index(_impl->plugin, which);
	if (!port) {
		error << string_compose("LV2: get descriptor of non-existent port %1", which)
		      << endmsg;
		return 1;
	}

	LilvNodes* portunits;
	LilvNode *def, *min, *max;
	lilv_port_get_range(_impl->plugin, port, &def, &min, &max);
	portunits = lilv_port_get_value(_impl->plugin, port, _world.units_unit);

	// TODO: Once we can rely on lilv 0.18.0 being present,
	// load_parameter_descriptor() can be used for ports as well
	desc.integer_step = lilv_port_has_property(_impl->plugin, port, _world.lv2_integer);
	desc.toggled      = lilv_port_has_property(_impl->plugin, port, _world.lv2_toggled);
	desc.logarithmic  = lilv_port_has_property(_impl->plugin, port, _world.ext_logarithmic);
	desc.sr_dependent = lilv_port_has_property(_impl->plugin, port, _world.lv2_sampleRate);
	desc.label        = lilv_node_as_string(lilv_port_get_name(_impl->plugin, port));
	desc.normal       = def ? lilv_node_as_float(def) : 0.0f;
	desc.lower        = min ? lilv_node_as_float(min) : 0.0f;
	desc.upper        = max ? lilv_node_as_float(max) : 1.0f;
	load_parameter_descriptor_units(_world.world, desc, portunits);

	if (desc.sr_dependent) {
		desc.lower *= _session.frame_rate ();
		desc.upper *= _session.frame_rate ();
	}

	desc.min_unbound  = false; // TODO: LV2 extension required
	desc.max_unbound  = false; // TODO: LV2 extension required

	desc.enumeration = lilv_port_has_property(_impl->plugin, port, _world.lv2_enumeration);
	desc.scale_points = get_scale_points(which);

	desc.update_steps();

	lilv_node_free(def);
	lilv_node_free(min);
	lilv_node_free(max);
	lilv_nodes_free(portunits);

	return 0;
}

string
LV2Plugin::describe_parameter(Evoral::Parameter which)
{
	if (( which.type() == PluginAutomation) && ( which.id() < parameter_count()) ) {

		if (lilv_port_has_property(_impl->plugin,
					lilv_plugin_get_port_by_index(_impl->plugin, which.id()), _world.ext_notOnGUI)) {
			return X_("hidden");
		}

		if (lilv_port_has_property(_impl->plugin,
					lilv_plugin_get_port_by_index(_impl->plugin, which.id()), _world.lv2_freewheeling)) {
			return X_("hidden");
		}

		if (lilv_port_has_property(_impl->plugin,
					lilv_plugin_get_port_by_index(_impl->plugin, which.id()), _world.lv2_reportsLatency)) {
			return X_("latency");
		}

		LilvNode* name = lilv_port_get_name(_impl->plugin,
		                                    lilv_plugin_get_port_by_index(_impl->plugin, which.id()));
		string ret(lilv_node_as_string(name));
		lilv_node_free(name);
		return ret;
	} else {
		return "??";
	}
}

framecnt_t
LV2Plugin::signal_latency() const
{
	if (_latency_control_port) {
		return (framecnt_t)floor(*_latency_control_port);
	} else {
		return 0;
	}
}

set<Evoral::Parameter>
LV2Plugin::automatable() const
{
	set<Evoral::Parameter> ret;

	for (uint32_t i = 0; i < parameter_count(); ++i) {
		if (parameter_is_input(i) && parameter_is_control(i)) {
			ret.insert(ret.end(), Evoral::Parameter(PluginAutomation, 0, i));
		}
	}

	for (PropertyDescriptors::const_iterator p = _property_descriptors.begin();
	     p != _property_descriptors.end();
	     ++p) {
		ret.insert(ret.end(), Evoral::Parameter(PluginPropertyAutomation, 0, p->first));
	}
	return ret;
}

void
LV2Plugin::activate()
{
	DEBUG_TRACE(DEBUG::LV2, string_compose("%1 activate\n", name()));

	if (!_was_activated) {
		lilv_instance_activate(_impl->instance);
		_was_activated = true;
	}
}

void
LV2Plugin::deactivate()
{
	DEBUG_TRACE(DEBUG::LV2, string_compose("%1 deactivate\n", name()));

	if (_was_activated) {
		lilv_instance_deactivate(_impl->instance);
		_was_activated = false;
	}
}

void
LV2Plugin::cleanup()
{
	DEBUG_TRACE(DEBUG::LV2, string_compose("%1 cleanup\n", name()));

	activate();
	deactivate();
	lilv_instance_free(_impl->instance);
	_impl->instance = NULL;
}

void
LV2Plugin::allocate_atom_event_buffers()
{
	/* reserve local scratch buffers for ATOM event-queues */
	const LilvPlugin* p = _impl->plugin;

	/* count non-MIDI atom event-ports
	 * TODO: nicely ask drobilla to make a lilv_ call for that
	 */
	int count_atom_out = 0;
	int count_atom_in = 0;
	int minimumSize = 32768; // TODO use a per-port minimum-size
	for (uint32_t i = 0; i < lilv_plugin_get_num_ports(p); ++i) {
		const LilvPort* port  = lilv_plugin_get_port_by_index(p, i);
		if (lilv_port_is_a(p, port, _world.atom_AtomPort)) {
			LilvNodes* buffer_types = lilv_port_get_value(
				p, port, _world.atom_bufferType);
			LilvNodes* atom_supports = lilv_port_get_value(
				p, port, _world.atom_supports);

			if (!lilv_nodes_contains(buffer_types, _world.atom_Sequence)
					|| !lilv_nodes_contains(atom_supports, _world.midi_MidiEvent)) {
				if (lilv_port_is_a(p, port, _world.lv2_InputPort)) {
					count_atom_in++;
				}
				if (lilv_port_is_a(p, port, _world.lv2_OutputPort)) {
					count_atom_out++;
				}
				LilvNodes* min_size_v = lilv_port_get_value(_impl->plugin, port, _world.rsz_minimumSize);
				LilvNode* min_size = min_size_v ? lilv_nodes_get_first(min_size_v) : NULL;
				if (min_size && lilv_node_is_int(min_size)) {
					minimumSize = std::max(minimumSize, lilv_node_as_int(min_size));
				}
				lilv_nodes_free(min_size_v);
			}
			lilv_nodes_free(buffer_types);
			lilv_nodes_free(atom_supports);
		}
	}

	DEBUG_TRACE(DEBUG::LV2, string_compose("%1 need buffers for %2 atom-in and %3 atom-out event-ports\n",
				name(), count_atom_in, count_atom_out));

	const int total_atom_buffers = (count_atom_in + count_atom_out);
	if (_atom_ev_buffers || total_atom_buffers == 0) {
		return;
	}

	DEBUG_TRACE(DEBUG::LV2, string_compose("allocate %1 atom_ev_buffers of %d bytes\n", total_atom_buffers, minimumSize));
	_atom_ev_buffers = (LV2_Evbuf**) malloc((total_atom_buffers + 1) * sizeof(LV2_Evbuf*));
	for (int i = 0; i < total_atom_buffers; ++i ) {
		_atom_ev_buffers[i] = lv2_evbuf_new(minimumSize, LV2_EVBUF_ATOM,
				_uri_map.urids.atom_Chunk, _uri_map.urids.atom_Sequence);
	}
	_atom_ev_buffers[total_atom_buffers] = 0;
	return;
}

/** Write an ardour position/time/tempo/meter as an LV2 event.
 * @return true on success.
 */
static bool
write_position(LV2_Atom_Forge*     forge,
               LV2_Evbuf*          buf,
               const TempoMetric&  t,
               Timecode::BBT_Time& bbt,
               double              speed,
               framepos_t          position,
               framecnt_t          offset)
{
	const URIMap::URIDs& urids = URIMap::instance().urids;

	uint8_t pos_buf[256];
	lv2_atom_forge_set_buffer(forge, pos_buf, sizeof(pos_buf));
	LV2_Atom_Forge_Frame frame;
#ifdef HAVE_LV2_1_10_0
	lv2_atom_forge_object(forge, &frame, 1, urids.time_Position);
	lv2_atom_forge_key(forge, urids.time_frame);
	lv2_atom_forge_long(forge, position);
	lv2_atom_forge_key(forge, urids.time_speed);
	lv2_atom_forge_float(forge, speed);
	lv2_atom_forge_key(forge, urids.time_barBeat);
	lv2_atom_forge_float(forge, bbt.beats - 1 +
	                     (bbt.ticks / Timecode::BBT_Time::ticks_per_beat));
	lv2_atom_forge_key(forge, urids.time_bar);
	lv2_atom_forge_long(forge, bbt.bars - 1);
	lv2_atom_forge_key(forge, urids.time_beatUnit);
	lv2_atom_forge_int(forge, t.meter().note_divisor());
	lv2_atom_forge_key(forge, urids.time_beatsPerBar);
	lv2_atom_forge_float(forge, t.meter().divisions_per_bar());
	lv2_atom_forge_key(forge, urids.time_beatsPerMinute);
	lv2_atom_forge_float(forge, t.tempo().beats_per_minute());
#else
	lv2_atom_forge_blank(forge, &frame, 1, urids.time_Position);
	lv2_atom_forge_property_head(forge, urids.time_frame, 0);
	lv2_atom_forge_long(forge, position);
	lv2_atom_forge_property_head(forge, urids.time_speed, 0);
	lv2_atom_forge_float(forge, speed);
	lv2_atom_forge_property_head(forge, urids.time_barBeat, 0);
	lv2_atom_forge_float(forge, bbt.beats - 1 +
	                     (bbt.ticks / Timecode::BBT_Time::ticks_per_beat));
	lv2_atom_forge_property_head(forge, urids.time_bar, 0);
	lv2_atom_forge_long(forge, bbt.bars - 1);
	lv2_atom_forge_property_head(forge, urids.time_beatUnit, 0);
	lv2_atom_forge_int(forge, t.meter().note_divisor());
	lv2_atom_forge_property_head(forge, urids.time_beatsPerBar, 0);
	lv2_atom_forge_float(forge, t.meter().divisions_per_bar());
	lv2_atom_forge_property_head(forge, urids.time_beatsPerMinute, 0);
	lv2_atom_forge_float(forge, t.tempo().beats_per_minute());
#endif

	LV2_Evbuf_Iterator    end  = lv2_evbuf_end(buf);
	const LV2_Atom* const atom = (const LV2_Atom*)pos_buf;
	return lv2_evbuf_write(&end, offset, 0, atom->type, atom->size,
	                       (const uint8_t*)(atom + 1));
}

int
LV2Plugin::connect_and_run(BufferSet& bufs,
	ChanMapping in_map, ChanMapping out_map,
	pframes_t nframes, framecnt_t offset)
{
	DEBUG_TRACE(DEBUG::LV2, string_compose("%1 run %2 offset %3\n", name(), nframes, offset));
	Plugin::connect_and_run(bufs, in_map, out_map, nframes, offset);

	cycles_t then = get_cycles();

	TempoMap&               tmap     = _session.tempo_map();
	Metrics::const_iterator metric_i = tmap.metrics_end();
	TempoMetric             tmetric  = tmap.metric_at(_session.transport_frame(), &metric_i);

	if (_freewheel_control_port) {
		*_freewheel_control_port = _session.engine().freewheeling() ? 1.f : 0.f;
	}

	if (_bpm_control_port) {
		*_bpm_control_port = tmetric.tempo().beats_per_minute();
	}

	ChanCount bufs_count;
	bufs_count.set(DataType::AUDIO, 1);
	bufs_count.set(DataType::MIDI, 1);
	BufferSet& silent_bufs  = _session.get_silent_buffers(bufs_count);
	BufferSet& scratch_bufs = _session.get_scratch_buffers(bufs_count);
	uint32_t const num_ports = parameter_count();
	uint32_t const nil_index = std::numeric_limits<uint32_t>::max();

	uint32_t audio_in_index  = 0;
	uint32_t audio_out_index = 0;
	uint32_t midi_in_index   = 0;
	uint32_t midi_out_index  = 0;
	uint32_t atom_port_index = 0;
	for (uint32_t port_index = 0; port_index < num_ports; ++port_index) {
		void*     buf   = NULL;
		uint32_t  index = nil_index;
		PortFlags flags = _port_flags[port_index];
		bool      valid = false;
		if (flags & PORT_AUDIO) {
			if (flags & PORT_INPUT) {
				index = in_map.get(DataType::AUDIO, audio_in_index++, &valid);
				buf = (valid)
					? bufs.get_audio(index).data(offset)
					: silent_bufs.get_audio(0).data(offset);
			} else {
				index = out_map.get(DataType::AUDIO, audio_out_index++, &valid);
				buf = (valid)
					? bufs.get_audio(index).data(offset)
					: scratch_bufs.get_audio(0).data(offset);
			}
		} else if (flags & (PORT_EVENT|PORT_SEQUENCE)) {
			/* FIXME: The checks here for bufs.count().n_midi() > index shouldn't
			   be necessary, but the mapping is illegal in some cases.  Ideally
			   that should be fixed, but this is easier...
			*/
			if (flags & PORT_MIDI) {
				if (flags & PORT_INPUT) {
					index = in_map.get(DataType::MIDI, midi_in_index++, &valid);
				} else {
					index = out_map.get(DataType::MIDI, midi_out_index++, &valid);
				}
				if (valid && bufs.count().n_midi() > index) {
					/* Note, ensure_lv2_bufsize() is not RT safe!
					 * However free()/alloc() is only called if a
					 * plugin requires a rsz:minimumSize buffersize
					 * and the existing buffer if smaller.
					 */
					bufs.ensure_lv2_bufsize((flags & PORT_INPUT), index, _port_minimumSize[port_index]);
					_ev_buffers[port_index] = bufs.get_lv2_midi(
						(flags & PORT_INPUT), index, (flags & PORT_EVENT));
				}
			} else if ((flags & PORT_POSITION) && (flags & PORT_INPUT)) {
				lv2_evbuf_reset(_atom_ev_buffers[atom_port_index], true);
				_ev_buffers[port_index] = _atom_ev_buffers[atom_port_index++];
				valid                   = true;
			}

			if (valid && (flags & PORT_INPUT)) {
				Timecode::BBT_Time bbt;
				if ((flags & PORT_POSITION)) {
					if (_session.transport_frame() != _next_cycle_start ||
					    _session.transport_speed() != _next_cycle_speed) {
						// Transport has changed, write position at cycle start
						tmap.bbt_time(_session.transport_frame(), bbt);
						write_position(&_impl->forge, _ev_buffers[port_index],
						               tmetric, bbt, _session.transport_speed(),
						               _session.transport_frame(), 0);
					}
				}

				// Get MIDI iterator range (empty range if no MIDI)
				MidiBuffer::iterator m = (index != nil_index)
					? bufs.get_midi(index).begin()
					: silent_bufs.get_midi(0).end();
				MidiBuffer::iterator m_end = (index != nil_index)
					? bufs.get_midi(index).end()
					: m;

				// Now merge MIDI and any transport events into the buffer
				const uint32_t     type = _uri_map.urids.midi_MidiEvent;
				const framepos_t   tend = _session.transport_frame() + nframes;
				++metric_i;
				while (m != m_end || (metric_i != tmap.metrics_end() &&
				                      (*metric_i)->frame() < tend)) {
					MetricSection* metric = (metric_i != tmap.metrics_end())
						? *metric_i : NULL;
					if (m != m_end && (!metric || metric->frame() > (*m).time())) {
						const Evoral::MIDIEvent<framepos_t> ev(*m, false);
						LV2_Evbuf_Iterator eend = lv2_evbuf_end(_ev_buffers[port_index]);
						lv2_evbuf_write(&eend, ev.time(), 0, type, ev.size(), ev.buffer());
						++m;
					} else {
						tmetric.set_metric(metric);
						bbt = metric->start();
						write_position(&_impl->forge, _ev_buffers[port_index],
						               tmetric, bbt, _session.transport_speed(),
						               metric->frame(),
						               metric->frame() - _session.transport_frame());
						++metric_i;
					}
				}
			} else if (!valid) {
				// Nothing we understand or care about, connect to scratch
				// see note for midi-buffer size above
				scratch_bufs.ensure_lv2_bufsize((flags & PORT_INPUT),
						0, _port_minimumSize[port_index]);
				_ev_buffers[port_index] = scratch_bufs.get_lv2_midi(
					(flags & PORT_INPUT), 0, (flags & PORT_EVENT));
			}

			buf = lv2_evbuf_get_buffer(_ev_buffers[port_index]);
		} else {
			continue;  // Control port, leave buffer alone
		}
		lilv_instance_connect_port(_impl->instance, port_index, buf);
	}

	// Read messages from UI and push into appropriate buffers
	if (_from_ui) {
		uint32_t read_space = _from_ui->read_space();
		while (read_space > sizeof(UIMessage)) {
			UIMessage msg;
			if (_from_ui->read((uint8_t*)&msg, sizeof(msg)) != sizeof(msg)) {
				error << "Error reading from UI=>Plugin RingBuffer" << endmsg;
				break;
			}
			vector<uint8_t> body(msg.size);
			if (_from_ui->read(&body[0], msg.size) != msg.size) {
				error << "Error reading from UI=>Plugin RingBuffer" << endmsg;
				break;
			}
			if (msg.protocol == URIMap::instance().urids.atom_eventTransfer) {
				LV2_Evbuf*            buf  = _ev_buffers[msg.index];
				LV2_Evbuf_Iterator    i    = lv2_evbuf_end(buf);
				const LV2_Atom* const atom = (const LV2_Atom*)&body[0];
				if (!lv2_evbuf_write(&i, nframes - 1, 0, atom->type, atom->size,
				                (const uint8_t*)(atom + 1))) {
					error << "Failed to write data to LV2 event buffer\n";
				}
			} else {
				error << "Received unknown message type from UI" << endmsg;
			}
			read_space -= sizeof(UIMessage) + msg.size;
		}
	}

	run(nframes);

	midi_out_index = 0;
	for (uint32_t port_index = 0; port_index < num_ports; ++port_index) {
		PortFlags flags = _port_flags[port_index];
		bool      valid = false;

		/* TODO ask drobilla about comment
		 * "Make Ardour event buffers generic so plugins can communicate"
		 * in libs/ardour/buffer_set.cc:310
		 *
		 * ideally the user could choose which of the following two modes
		 * to use (e.g. instrument/effect chains  MIDI OUT vs MIDI TRHU).
		 *
		 * This implementation follows the discussion on IRC Mar 16 2013 16:47 UTC
		 * 16:51 < drobilla> rgareus: [..] i.e always replace with MIDI output [of LV2 plugin] if it's there
		 * 16:52 < drobilla> rgareus: That would probably be good enough [..] to make users not complain
		 *                            for quite a while at least ;)
		 */
		// copy output of LV2 plugin's MIDI port to Ardour MIDI buffers -- MIDI OUT
		if ((flags & PORT_OUTPUT) && (flags & (PORT_EVENT|PORT_SEQUENCE|PORT_MIDI))) {
			const uint32_t buf_index = out_map.get(
				DataType::MIDI, midi_out_index++, &valid);
			if (valid) {
				bufs.forward_lv2_midi(_ev_buffers[port_index], buf_index);
			}
		}
		// Flush MIDI (write back to Ardour MIDI buffers) -- MIDI THRU
		else if ((flags & PORT_OUTPUT) && (flags & (PORT_EVENT|PORT_SEQUENCE))) {
			const uint32_t buf_index = out_map.get(
				DataType::MIDI, midi_out_index++, &valid);
			if (valid) {
				bufs.flush_lv2_midi(true, buf_index);
			}
		}


		// Write messages to UI
		if ((_to_ui || _patch_port_out_index != (uint32_t)-1) &&
		    (flags & PORT_OUTPUT) && (flags & (PORT_EVENT|PORT_SEQUENCE))) {
			LV2_Evbuf* buf = _ev_buffers[port_index];
			for (LV2_Evbuf_Iterator i = lv2_evbuf_begin(buf);
			     lv2_evbuf_is_valid(i);
			     i = lv2_evbuf_next(i)) {
				uint32_t frames, subframes, type, size;
				uint8_t* data;
				lv2_evbuf_get(i, &frames, &subframes, &type, &size, &data);

				// Intercept patch change messages to emit PropertyChanged signal
				if ((flags & PORT_PATCHMSG)) {
					LV2_Atom* atom = (LV2_Atom*)(data - sizeof(LV2_Atom));
					if (atom->type == _uri_map.urids.atom_Blank ||
					    atom->type == _uri_map.urids.atom_Object) {
						LV2_Atom_Object* obj = (LV2_Atom_Object*)atom;
						if (obj->body.otype == _uri_map.urids.patch_Set) {
							const LV2_Atom* property = NULL;
							const LV2_Atom* value    = NULL;
							lv2_atom_object_get(obj,
							                    _uri_map.urids.patch_property, &property,
							                    _uri_map.urids.patch_value,    &value,
							                    0);

							if (!property || !value ||
							    property->type != _uri_map.urids.atom_URID ||
							    value->type    != _uri_map.urids.atom_Path) {
								std::cerr << "warning: patch:Set for unknown property" << std::endl;
								continue;
							}

							const uint32_t prop_id = ((const LV2_Atom_URID*)property)->body;
							const char*    path    = (const char*)LV2_ATOM_BODY_CONST(value);

							// Emit PropertyChanged signal for UI
							// TODO: This should emit the control's Changed signal
							PropertyChanged(prop_id, Variant(Variant::PATH, path));
						}
					}
				}

				if (!_to_ui) continue;
				write_to_ui(port_index, URIMap::instance().urids.atom_eventTransfer,
				            size + sizeof(LV2_Atom),
				            data - sizeof(LV2_Atom));
			}
		}
	}

	cycles_t now = get_cycles();
	set_cycles((uint32_t)(now - then));

	// Update expected transport information for next cycle so we can detect changes
	_next_cycle_speed = _session.transport_speed();
	_next_cycle_start = _session.transport_frame() + (nframes * _next_cycle_speed);

	return 0;
}

bool
LV2Plugin::parameter_is_control(uint32_t param) const
{
	assert(param < _port_flags.size());
	return _port_flags[param] & PORT_CONTROL;
}

bool
LV2Plugin::parameter_is_audio(uint32_t param) const
{
	assert(param < _port_flags.size());
	return _port_flags[param] & PORT_AUDIO;
}

bool
LV2Plugin::parameter_is_event(uint32_t param) const
{
	assert(param < _port_flags.size());
	return _port_flags[param] & PORT_EVENT;
}

bool
LV2Plugin::parameter_is_output(uint32_t param) const
{
	assert(param < _port_flags.size());
	return _port_flags[param] & PORT_OUTPUT;
}

bool
LV2Plugin::parameter_is_input(uint32_t param) const
{
	assert(param < _port_flags.size());
	return _port_flags[param] & PORT_INPUT;
}

void
LV2Plugin::print_parameter(uint32_t param, char* buf, uint32_t len) const
{
	if (buf && len) {
		if (param < parameter_count()) {
			snprintf(buf, len, "%.3f", get_parameter(param));
		} else {
			strcat(buf, "0");
		}
	}
}

boost::shared_ptr<ScalePoints>
LV2Plugin::get_scale_points(uint32_t port_index) const
{
	const LilvPort*  port   = lilv_plugin_get_port_by_index(_impl->plugin, port_index);
	LilvScalePoints* points = lilv_port_get_scale_points(_impl->plugin, port);

	boost::shared_ptr<ScalePoints> ret;
	if (!points) {
		return ret;
	}

	ret = boost::shared_ptr<ScalePoints>(new ScalePoints());

	LILV_FOREACH(scale_points, i, points) {
		const LilvScalePoint* p     = lilv_scale_points_get(points, i);
		const LilvNode*       label = lilv_scale_point_get_label(p);
		const LilvNode*       value = lilv_scale_point_get_value(p);
		if (label && (lilv_node_is_float(value) || lilv_node_is_int(value))) {
			ret->insert(make_pair(lilv_node_as_string(label),
			                      lilv_node_as_float(value)));
		}
	}

	lilv_scale_points_free(points);
	return ret;
}

void
LV2Plugin::run(pframes_t nframes)
{
	uint32_t const N = parameter_count();
	for (uint32_t i = 0; i < N; ++i) {
		if (parameter_is_control(i) && parameter_is_input(i)) {
			_control_data[i] = _shadow_data[i];
		}
	}

	lilv_instance_run(_impl->instance, nframes);

	if (_impl->work_iface) {
		_worker->emit_responses();
		if (_impl->work_iface->end_run) {
			_impl->work_iface->end_run(_impl->instance->lv2_handle);
		}
	}
}

void
LV2Plugin::latency_compute_run()
{
	if (!_latency_control_port) {
		return;
	}

	// Run the plugin so that it can set its latency parameter

	bool was_activated = _was_activated;
	activate();

	uint32_t port_index = 0;
	uint32_t in_index   = 0;
	uint32_t out_index  = 0;

	// this is done in the main thread. non realtime.
	const framecnt_t bufsize = _engine.samples_per_cycle();
	float            *buffer = (float*) malloc(_engine.samples_per_cycle() * sizeof(float));

	memset(buffer, 0, sizeof(float) * bufsize);

	// FIXME: Ensure plugins can handle in-place processing

	port_index = 0;

	while (port_index < parameter_count()) {
		if (parameter_is_audio(port_index)) {
			if (parameter_is_input(port_index)) {
				lilv_instance_connect_port(_impl->instance, port_index, buffer);
				in_index++;
			} else if (parameter_is_output(port_index)) {
				lilv_instance_connect_port(_impl->instance, port_index, buffer);
				out_index++;
			}
		}
		port_index++;
	}

	run(bufsize);
	deactivate();
	if (was_activated) {
		activate();
	}
	free(buffer);
}

const LilvPort*
LV2Plugin::Impl::designated_input (const char* uri, void** bufptrs[], void** bufptr)
{
	const LilvPort* port = NULL;
	LilvNode* designation = lilv_new_uri(_world.world, uri);
	port = lilv_plugin_get_port_by_designation(
		plugin, _world.lv2_InputPort, designation);
	lilv_node_free(designation);
	if (port) {
		bufptrs[lilv_port_get_index(plugin, port)] = bufptr;
	}
	return port;
}

static bool lv2_filter (const string& str, void* /*arg*/)
{
	/* Not a dotfile, has a prefix before a period, suffix is "lv2" */

	return str[0] != '.' && (str.length() > 3 && str.find (".lv2") == (str.length() - 4));
}


LV2World::LV2World()
	: world(lilv_world_new())
	, _bundle_checked(false)
{
	atom_AtomPort      = lilv_new_uri(world, LV2_ATOM__AtomPort);
	atom_Chunk         = lilv_new_uri(world, LV2_ATOM__Chunk);
	atom_Sequence      = lilv_new_uri(world, LV2_ATOM__Sequence);
	atom_bufferType    = lilv_new_uri(world, LV2_ATOM__bufferType);
	atom_supports      = lilv_new_uri(world, LV2_ATOM__supports);
	atom_eventTransfer = lilv_new_uri(world, LV2_ATOM__eventTransfer);
	ev_EventPort       = lilv_new_uri(world, LILV_URI_EVENT_PORT);
	ext_logarithmic    = lilv_new_uri(world, LV2_PORT_PROPS__logarithmic);
	ext_notOnGUI       = lilv_new_uri(world, LV2_PORT_PROPS__notOnGUI);
	lv2_AudioPort      = lilv_new_uri(world, LILV_URI_AUDIO_PORT);
	lv2_ControlPort    = lilv_new_uri(world, LILV_URI_CONTROL_PORT);
	lv2_InputPort      = lilv_new_uri(world, LILV_URI_INPUT_PORT);
	lv2_OutputPort     = lilv_new_uri(world, LILV_URI_OUTPUT_PORT);
	lv2_inPlaceBroken  = lilv_new_uri(world, LV2_CORE__inPlaceBroken);
	lv2_integer        = lilv_new_uri(world, LV2_CORE__integer);
	lv2_default        = lilv_new_uri(world, LV2_CORE__default);
	lv2_minimum        = lilv_new_uri(world, LV2_CORE__minimum);
	lv2_maximum        = lilv_new_uri(world, LV2_CORE__maximum);
	lv2_reportsLatency = lilv_new_uri(world, LV2_CORE__reportsLatency);
	lv2_sampleRate     = lilv_new_uri(world, LV2_CORE__sampleRate);
	lv2_toggled        = lilv_new_uri(world, LV2_CORE__toggled);
	lv2_enumeration    = lilv_new_uri(world, LV2_CORE__enumeration);
	lv2_freewheeling   = lilv_new_uri(world, LV2_CORE__freeWheeling);
	midi_MidiEvent     = lilv_new_uri(world, LILV_URI_MIDI_EVENT);
	rdfs_comment       = lilv_new_uri(world, LILV_NS_RDFS "comment");
	rdfs_label         = lilv_new_uri(world, LILV_NS_RDFS "label");
	rdfs_range         = lilv_new_uri(world, LILV_NS_RDFS "range");
	rsz_minimumSize    = lilv_new_uri(world, LV2_RESIZE_PORT__minimumSize);
	time_Position      = lilv_new_uri(world, LV2_TIME__Position);
	ui_GtkUI           = lilv_new_uri(world, LV2_UI__GtkUI);
	ui_external        = lilv_new_uri(world, "http://lv2plug.in/ns/extensions/ui#external");
	ui_externalkx      = lilv_new_uri(world, "http://kxstudio.sf.net/ns/lv2ext/external-ui#Widget");
	units_unit         = lilv_new_uri(world, LV2_UNITS__unit);
	units_render       = lilv_new_uri(world, LV2_UNITS__render);
	units_hz           = lilv_new_uri(world, LV2_UNITS__hz);
	units_midiNote     = lilv_new_uri(world, LV2_UNITS__midiNote);
	units_db           = lilv_new_uri(world, LV2_UNITS__db);
	patch_writable     = lilv_new_uri(world, LV2_PATCH__writable);
	patch_Message      = lilv_new_uri(world, LV2_PATCH__Message);
#ifdef HAVE_LV2_1_2_0
	bufz_powerOf2BlockLength = lilv_new_uri(world, LV2_BUF_SIZE__powerOf2BlockLength);
	bufz_fixedBlockLength    = lilv_new_uri(world, LV2_BUF_SIZE__fixedBlockLength);
	bufz_nominalBlockLength  = lilv_new_uri(world, "http://lv2plug.in/ns/ext/buf-size#nominalBlockLength");
#endif

}

LV2World::~LV2World()
{
#ifdef HAVE_LV2_1_2_0
	lilv_node_free(bufz_nominalBlockLength);
	lilv_node_free(bufz_fixedBlockLength);
	lilv_node_free(bufz_powerOf2BlockLength);
#endif
	lilv_node_free(patch_Message);
	lilv_node_free(patch_writable);
	lilv_node_free(units_hz);
	lilv_node_free(units_midiNote);
	lilv_node_free(units_db);
	lilv_node_free(units_unit);
	lilv_node_free(units_render);
	lilv_node_free(ui_externalkx);
	lilv_node_free(ui_external);
	lilv_node_free(ui_GtkUI);
	lilv_node_free(time_Position);
	lilv_node_free(rsz_minimumSize);
	lilv_node_free(rdfs_comment);
	lilv_node_free(rdfs_label);
	lilv_node_free(rdfs_range);
	lilv_node_free(midi_MidiEvent);
	lilv_node_free(lv2_enumeration);
	lilv_node_free(lv2_freewheeling);
	lilv_node_free(lv2_toggled);
	lilv_node_free(lv2_sampleRate);
	lilv_node_free(lv2_reportsLatency);
	lilv_node_free(lv2_integer);
	lilv_node_free(lv2_inPlaceBroken);
	lilv_node_free(lv2_OutputPort);
	lilv_node_free(lv2_InputPort);
	lilv_node_free(lv2_ControlPort);
	lilv_node_free(lv2_AudioPort);
	lilv_node_free(ext_notOnGUI);
	lilv_node_free(ext_logarithmic);
	lilv_node_free(ev_EventPort);
	lilv_node_free(atom_supports);
	lilv_node_free(atom_eventTransfer);
	lilv_node_free(atom_bufferType);
	lilv_node_free(atom_Sequence);
	lilv_node_free(atom_Chunk);
	lilv_node_free(atom_AtomPort);
	lilv_world_free(world);
}

void
LV2World::load_bundled_plugins(bool verbose)
{
	if (!_bundle_checked) {
		if (verbose) {
			cout << "Scanning folders for bundled LV2s: " << ARDOUR::lv2_bundled_search_path().to_string() << endl;
		}

		vector<string> plugin_objects;
		find_paths_matching_filter (plugin_objects, ARDOUR::lv2_bundled_search_path(), lv2_filter, 0, true, true, true);
		for ( vector<string>::iterator x = plugin_objects.begin(); x != plugin_objects.end (); ++x) {
#ifdef PLATFORM_WINDOWS
			string uri = "file:///" + *x + "/";
#else
			string uri = "file://" + *x + "/";
#endif
			LilvNode *node = lilv_new_uri(world, uri.c_str());
			lilv_world_load_bundle(world, node);
			lilv_node_free(node);
		}

		lilv_world_load_all(world);
		_bundle_checked = true;
	}
}

LV2PluginInfo::LV2PluginInfo (const char* plugin_uri)
{
	type = ARDOUR::LV2;
	_plugin_uri = strdup(plugin_uri);
}

LV2PluginInfo::~LV2PluginInfo()
{
	free(_plugin_uri);
	_plugin_uri = NULL;
}

PluginPtr
LV2PluginInfo::load(Session& session)
{
	try {
		PluginPtr plugin;
		const LilvPlugins* plugins = lilv_world_get_all_plugins(_world.world);
		LilvNode* uri = lilv_new_uri(_world.world, _plugin_uri);
		if (!uri) { throw failed_constructor(); }
		const LilvPlugin* lp = lilv_plugins_get_by_uri(plugins, uri);
		if (!lp) { throw failed_constructor(); }
		plugin.reset(new LV2Plugin(session.engine(), session, lp, session.frame_rate()));
		lilv_node_free(uri);
		plugin->set_info(PluginInfoPtr(shared_from_this ()));
		return plugin;
	} catch (failed_constructor& err) {
		return PluginPtr((Plugin*)0);
	}

	return PluginPtr();
}

PluginInfoList*
LV2PluginInfo::discover()
{
	LV2World world;
	world.load_bundled_plugins();
	_world.load_bundled_plugins(true);

	PluginInfoList*    plugs   = new PluginInfoList;
	const LilvPlugins* plugins = lilv_world_get_all_plugins(world.world);

	LILV_FOREACH(plugins, i, plugins) {
		const LilvPlugin* p = lilv_plugins_get(plugins, i);
		const LilvNode* pun = lilv_plugin_get_uri(p);
		if (!pun) continue;
		LV2PluginInfoPtr info(new LV2PluginInfo(lilv_node_as_string(pun)));

		LilvNode* name = lilv_plugin_get_name(p);
		if (!name || !lilv_plugin_get_port_by_index(p, 0)) {
			warning << "Ignoring invalid LV2 plugin "
			        << lilv_node_as_string(lilv_plugin_get_uri(p))
			        << endmsg;
			continue;
		}

		if (lilv_plugin_has_feature(p, world.lv2_inPlaceBroken)) {
			warning << string_compose(
			    _("Ignoring LV2 plugin \"%1\" since it cannot do inplace processing."),
			    lilv_node_as_string(name)) << endmsg;
			lilv_node_free(name);
			continue;
		}

#ifdef HAVE_LV2_1_2_0
		LilvNodes *required_features = lilv_plugin_get_required_features (p);
		if (lilv_nodes_contains (required_features, world.bufz_powerOf2BlockLength) ||
				lilv_nodes_contains (required_features, world.bufz_fixedBlockLength)
		   ) {
			warning << string_compose(
			    _("Ignoring LV2 plugin \"%1\" because its buffer-size requirements cannot be satisfied."),
			    lilv_node_as_string(name)) << endmsg;
			lilv_nodes_free(required_features);
			lilv_node_free(name);
			continue;
		}
		lilv_nodes_free(required_features);
#endif

		info->type = LV2;

		info->name = string(lilv_node_as_string(name));
		lilv_node_free(name);
		ARDOUR::PluginScanMessage(_("LV2"), info->name, false);

		const LilvPluginClass* pclass = lilv_plugin_get_class(p);
		const LilvNode*        label  = lilv_plugin_class_get_label(pclass);
		info->category = lilv_node_as_string(label);

		LilvNode* author_name = lilv_plugin_get_author_name(p);
		info->creator = author_name ? string(lilv_node_as_string(author_name)) : "Unknown";
		lilv_node_free(author_name);

		info->path = "/NOPATH"; // Meaningless for LV2

		/* count atom-event-ports that feature
		 * atom:supports <http://lv2plug.in/ns/ext/midi#MidiEvent>
		 *
		 * TODO: nicely ask drobilla to make a lilv_ call for that
		 */
		int count_midi_out = 0;
		int count_midi_in = 0;
		for (uint32_t i = 0; i < lilv_plugin_get_num_ports(p); ++i) {
			const LilvPort* port  = lilv_plugin_get_port_by_index(p, i);
			if (lilv_port_is_a(p, port, world.atom_AtomPort)) {
				LilvNodes* buffer_types = lilv_port_get_value(
					p, port, world.atom_bufferType);
				LilvNodes* atom_supports = lilv_port_get_value(
					p, port, world.atom_supports);

				if (lilv_nodes_contains(buffer_types, world.atom_Sequence)
						&& lilv_nodes_contains(atom_supports, world.midi_MidiEvent)) {
					if (lilv_port_is_a(p, port, world.lv2_InputPort)) {
						count_midi_in++;
					}
					if (lilv_port_is_a(p, port, world.lv2_OutputPort)) {
						count_midi_out++;
					}
				}
				lilv_nodes_free(buffer_types);
				lilv_nodes_free(atom_supports);
			}
		}

		info->n_inputs.set_audio(
			lilv_plugin_get_num_ports_of_class(
				p, world.lv2_InputPort, world.lv2_AudioPort, NULL));
		info->n_inputs.set_midi(
			lilv_plugin_get_num_ports_of_class(
				p, world.lv2_InputPort, world.ev_EventPort, NULL)
			+ count_midi_in);

		info->n_outputs.set_audio(
			lilv_plugin_get_num_ports_of_class(
				p, world.lv2_OutputPort, world.lv2_AudioPort, NULL));
		info->n_outputs.set_midi(
			lilv_plugin_get_num_ports_of_class(
				p, world.lv2_OutputPort, world.ev_EventPort, NULL)
			+ count_midi_out);

		info->unique_id = lilv_node_as_uri(lilv_plugin_get_uri(p));
		info->index     = 0; // Meaningless for LV2

		plugs->push_back(info);
	}

	return plugs;
}
