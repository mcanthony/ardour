/*
	Copyright (C) 2006,2007 John Anderson
	Copyright (C) 2012 Paul Davis

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

#include <sstream>
#include <stdint.h>
#include "strip.h"

#include <sys/time.h>

#include <glibmm/convert.h>

#include "midi++/port.h"

#include "pbd/compose.h"
#include "pbd/convert.h"

#include "ardour/amp.h"
#include "ardour/bundle.h"
#include "ardour/debug.h"
#include "ardour/midi_ui.h"
#include "ardour/meter.h"
#include "ardour/pannable.h"
#include "ardour/panner.h"
#include "ardour/panner_shell.h"
#include "ardour/rc_configuration.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/send.h"
#include "ardour/track.h"
#include "ardour/user_bundle.h"

#include "mackie_control_protocol.h"
#include "surface_port.h"
#include "surface.h"
#include "button.h"
#include "led.h"
#include "pot.h"
#include "fader.h"
#include "jog.h"
#include "meter.h"

using namespace std;
using namespace ARDOUR;
using namespace PBD;
using namespace ArdourSurface;
using namespace Mackie;

#ifndef timeradd /// only avail with __USE_BSD
#define timeradd(a,b,result)                         \
  do {                                               \
    (result)->tv_sec = (a)->tv_sec + (b)->tv_sec;    \
    (result)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
    if ((result)->tv_usec >= 1000000)                \
    {                                                \
      ++(result)->tv_sec;                            \
      (result)->tv_usec -= 1000000;                  \
    }                                                \
  } while (0)
#endif

#define ui_context() MackieControlProtocol::instance() /* a UICallback-derived object that specifies the event loop for signal handling */

Strip::Strip (Surface& s, const std::string& name, int index, const map<Button::ID,StripButtonInfo>& strip_buttons)
	: Group (name)
	, _solo (0)
	, _recenable (0)
	, _mute (0)
	, _select (0)
	, _vselect (0)
	, _fader_touch (0)
	, _vpot (0)
	, _fader (0)
	, _meter (0)
	, _index (index)
	, _surface (&s)
	, _controls_locked (false)
	, _transport_is_rolling (false)
	, _metering_active (true)
	, _block_vpot_mode_redisplay_until (0)
	, _block_screen_redisplay_until (0)
	, _pan_mode (PanAzimuthAutomation)
	, _last_gain_position_written (-1.0)
	, _last_pan_azi_position_written (-1.0)
	, _last_pan_width_position_written (-1.0)
	, _last_trim_position_written (-1.0)
	, redisplay_requests (256)
{
	_fader = dynamic_cast<Fader*> (Fader::factory (*_surface, index, "fader", *this));
	_vpot = dynamic_cast<Pot*> (Pot::factory (*_surface, Pot::ID + index, "vpot", *this));

	if (s.mcp().device_info().has_meters()) {
		_meter = dynamic_cast<Meter*> (Meter::factory (*_surface, index, "meter", *this));
	}

	for (map<Button::ID,StripButtonInfo>::const_iterator b = strip_buttons.begin(); b != strip_buttons.end(); ++b) {
		Button* bb = dynamic_cast<Button*> (Button::factory (*_surface, b->first, b->second.base_id + index, b->second.name, *this));
		DEBUG_TRACE (DEBUG::MackieControl, string_compose ("surface %1 strip %2 new button BID %3 id %4 from base %5\n",
								   _surface->number(), index, Button::id_to_name (bb->bid()),
								   bb->id(), b->second.base_id));
	}
}

Strip::~Strip ()
{
	/* surface is responsible for deleting all controls */
}

void
Strip::add (Control & control)
{
	Button* button;

	Group::add (control);

	/* fader, vpot, meter were all set explicitly */

	if ((button = dynamic_cast<Button*>(&control)) != 0) {
		switch (button->bid()) {
		case Button::RecEnable:
			_recenable = button;
			break;
		case Button::Mute:
			_mute = button;
			break;
		case Button::Solo:
			_solo = button;
			break;
		case Button::Select:
			_select = button;
			break;
		case Button::VSelect:
			_vselect = button;
			break;
		case Button::FaderTouch:
			_fader_touch = button;
			break;
		default:
			break;
		}
	}
}

void
Strip::set_route (boost::shared_ptr<Route> r, bool /*with_messages*/)
{
	if (_controls_locked) {
		return;
	}

	route_connections.drop_connections ();

	_solo->set_control (boost::shared_ptr<AutomationControl>());
	_mute->set_control (boost::shared_ptr<AutomationControl>());
	_select->set_control (boost::shared_ptr<AutomationControl>());
	_recenable->set_control (boost::shared_ptr<AutomationControl>());
	_fader->set_control (boost::shared_ptr<AutomationControl>());
	_vpot->set_control (boost::shared_ptr<AutomationControl>());

	_route = r;

	control_by_parameter.clear ();

	control_by_parameter[PanAzimuthAutomation] = (Control*) 0;
	control_by_parameter[PanWidthAutomation] = (Control*) 0;
	control_by_parameter[PanElevationAutomation] = (Control*) 0;
	control_by_parameter[PanFrontBackAutomation] = (Control*) 0;
	control_by_parameter[PanLFEAutomation] = (Control*) 0;
	control_by_parameter[GainAutomation] = (Control*) 0;
	control_by_parameter[TrimAutomation] = (Control*) 0;

	reset_saved_values ();

	if (!r) {
		zero ();
		return;
	}

	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("Surface %1 strip %2 now mapping route %3\n",
							   _surface->number(), _index, _route->name()));

	_solo->set_control (_route->solo_control());
	_mute->set_control (_route->mute_control());

	_pan_mode = PanAzimuthAutomation;
	potmode_changed (true);

	_route->solo_changed.connect (route_connections, MISSING_INVALIDATOR, boost::bind (&Strip::notify_solo_changed, this), ui_context());
	_route->listen_changed.connect (route_connections, MISSING_INVALIDATOR, boost::bind (&Strip::notify_solo_changed, this), ui_context());

	_route->mute_control()->Changed.connect(route_connections, MISSING_INVALIDATOR, boost::bind (&Strip::notify_mute_changed, this), ui_context());

	if (_route->trim()) {
		_route->trim_control()->Changed.connect(route_connections, MISSING_INVALIDATOR, boost::bind (&Strip::notify_trim_changed, this, false), ui_context());
	}

	boost::shared_ptr<Pannable> pannable = _route->pannable();

	if (pannable && _route->panner()) {
		pannable->pan_azimuth_control->Changed.connect(route_connections, MISSING_INVALIDATOR, boost::bind (&Strip::notify_panner_azi_changed, this, false), ui_context());
		pannable->pan_width_control->Changed.connect(route_connections, MISSING_INVALIDATOR, boost::bind (&Strip::notify_panner_width_changed, this, false), ui_context());
	}
	_route->gain_control()->Changed.connect(route_connections, MISSING_INVALIDATOR, boost::bind (&Strip::notify_gain_changed, this, false), ui_context());
	_route->PropertyChanged.connect (route_connections, MISSING_INVALIDATOR, boost::bind (&Strip::notify_property_changed, this, _1), ui_context());

	boost::shared_ptr<Track> trk = boost::dynamic_pointer_cast<ARDOUR::Track>(_route);

	if (trk) {
		_recenable->set_control (trk->rec_enable_control());
		trk->rec_enable_control()->Changed .connect(route_connections, MISSING_INVALIDATOR, boost::bind (&Strip::notify_record_enable_changed, this), ui_context());


	}

	// TODO this works when a currently-banked route is made inactive, but not
	// when a route is activated which should be currently banked.

	_route->active_changed.connect (route_connections, MISSING_INVALIDATOR, boost::bind (&Strip::notify_active_changed, this), ui_context());
	_route->DropReferences.connect (route_connections, MISSING_INVALIDATOR, boost::bind (&Strip::notify_route_deleted, this), ui_context());

	/* Update */

	notify_all ();

	/* setup legal VPot modes for this route */

	possible_pot_parameters.clear();

	if (pannable) {
		boost::shared_ptr<Panner> panner = _route->panner();
		if (panner) {
			set<Evoral::Parameter> automatable = panner->what_can_be_automated ();
			set<Evoral::Parameter>::iterator a;

			if ((a = automatable.find (PanAzimuthAutomation)) != automatable.end()) {
				possible_pot_parameters.push_back (PanAzimuthAutomation);
			}

			if ((a = automatable.find (PanWidthAutomation)) != automatable.end()) {
				possible_pot_parameters.push_back (PanWidthAutomation);
			}
		}
	}

	if (_route->trim()) {
		possible_pot_parameters.push_back (TrimAutomation);
	}
}

void
Strip::notify_all()
{
	if (!_route) {
		zero ();
		return;
	}

	notify_solo_changed ();
	notify_mute_changed ();
	notify_gain_changed ();
	notify_property_changed (PBD::PropertyChange (ARDOUR::Properties::name));
	notify_panner_azi_changed ();
	notify_panner_width_changed ();
	notify_record_enable_changed ();
	notify_trim_changed ();
}

void
Strip::notify_solo_changed ()
{
	if (_route && _solo) {
		_surface->write (_solo->set_state ((_route->soloed() || _route->listening_via_monitor()) ? on : off));
	}
}

void
Strip::notify_mute_changed ()
{
	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("Strip %1 mute changed\n", _index));
	if (_route && _mute) {
		DEBUG_TRACE (DEBUG::MackieControl, string_compose ("\troute muted ? %1\n", _route->muted()));
		DEBUG_TRACE (DEBUG::MackieControl, string_compose ("mute message: %1\n", _mute->set_state (_route->muted() ? on : off)));

		_surface->write (_mute->set_state (_route->muted() ? on : off));
	}
}

void
Strip::notify_record_enable_changed ()
{
	if (_route && _recenable)  {
		_surface->write (_recenable->set_state (_route->record_enabled() ? on : off));
	}
}

void
Strip::notify_active_changed ()
{
	_surface->mcp().refresh_current_bank();
}

void
Strip::notify_route_deleted ()
{
	_surface->mcp().refresh_current_bank();
}

void
Strip::notify_gain_changed (bool force_update)
{
	if (_route) {

		Control* control;

		if (_surface->mcp().flip_mode() != MackieControlProtocol::Normal) {
			control = _vpot;
		} else {
			control = _fader;
		}

		boost::shared_ptr<AutomationControl> ac = _route->gain_control();

		float gain_coefficient = ac->get_value();
		float normalized_position = ac->internal_to_interface (gain_coefficient);


		if (force_update || normalized_position != _last_gain_position_written) {

			if (_surface->mcp().flip_mode() != MackieControlProtocol::Normal) {
				if (!control->in_use()) {
					_surface->write (_vpot->set (normalized_position, true, Pot::wrap));
				}
				queue_parameter_display (GainAutomation, gain_coefficient);
			} else {
				if (!control->in_use()) {
					_surface->write (_fader->set_position (normalized_position));
				}
				queue_parameter_display (GainAutomation, gain_coefficient);
			}

			_last_gain_position_written = normalized_position;
		}
	}
}

void
Strip::notify_trim_changed (bool force_update)
{
	if (_route) {

		if (!_route->trim()) {
			_surface->write (_vpot->zero());
			return;
		}
		Control* control = 0;
		ControlParameterMap::iterator i = control_by_parameter.find (TrimAutomation);

		if (i == control_by_parameter.end()) {
			return;
		}

		control = i->second;

		boost::shared_ptr<AutomationControl> ac = _route->trim_control();

		float gain_coefficient = ac->get_value();
		float normalized_position = ac->internal_to_interface (gain_coefficient);

		if (force_update || normalized_position != _last_trim_position_written) {
			if (control == _fader) {
				if (!_fader->in_use()) {
					_surface->write (_fader->set_position (normalized_position));
					queue_parameter_display (TrimAutomation, gain_coefficient);
				}
			} else if (control == _vpot) {
				_surface->write (_vpot->set (normalized_position, true, Pot::dot));
				queue_parameter_display (TrimAutomation, gain_coefficient);
			}
			_last_trim_position_written = normalized_position;
		}
	}
}

void
Strip::notify_property_changed (const PropertyChange& what_changed)
{
	if (!what_changed.contains (ARDOUR::Properties::name)) {
		return;
	}

	show_route_name ();
}

void
Strip::show_route_name ()
{
	if (!_route) {
		return;
	}
	string line1;
	string fullname = _route->name();

	if (fullname.length() <= 6) {
		line1 = fullname;
	} else {
		line1 = PBD::short_version (fullname, 6);
	}

	_surface->write (display (0, line1));
}

void
Strip::notify_panner_azi_changed (bool force_update)
{
	if (_route) {

		DEBUG_TRACE (DEBUG::MackieControl, string_compose ("pan change for strip %1\n", _index));

		boost::shared_ptr<Pannable> pannable = _route->pannable();

		if (!pannable || !_route->panner()) {
			_surface->write (_vpot->zero());
			return;
		}

		Control* control = 0;
		ControlParameterMap::iterator i = control_by_parameter.find (PanAzimuthAutomation);

		if (i == control_by_parameter.end()) {
			return;
		}

		control = i->second;

		double pos = pannable->pan_azimuth_control->internal_to_interface (pannable->pan_azimuth_control->get_value());

		if (force_update || pos != _last_pan_azi_position_written) {

			if (control == _fader) {
				if (!_fader->in_use()) {
					_surface->write (_fader->set_position (pos));
					queue_parameter_display (PanAzimuthAutomation, pos);
				}
			} else if (control == _vpot) {
				_surface->write (_vpot->set (pos, true, Pot::dot));
				queue_parameter_display (PanAzimuthAutomation, pos);
			}

			_last_pan_azi_position_written = pos;
		}
	}
}

void
Strip::notify_panner_width_changed (bool force_update)
{
	if (_route) {

		DEBUG_TRACE (DEBUG::MackieControl, string_compose ("pan width change for strip %1\n", _index));

		boost::shared_ptr<Pannable> pannable = _route->pannable();

		if (!pannable || !_route->panner()) {
			_surface->write (_vpot->zero());
			return;
		}

		Control* control = 0;
		ControlParameterMap::iterator i = control_by_parameter.find (PanWidthAutomation);

		if (i == control_by_parameter.end()) {
			return;
		}

		control = i->second;

		double pos = pannable->pan_width_control->internal_to_interface (pannable->pan_width_control->get_value());

		if (force_update || pos != _last_pan_azi_position_written) {

			if (_surface->mcp().flip_mode() != MackieControlProtocol::Normal) {

				if (control == _fader) {
					if (!control->in_use()) {
						_surface->write (_fader->set_position (pos));
						queue_parameter_display (PanWidthAutomation, pos);
					}
				}

			} else if (control == _vpot) {
				_surface->write (_vpot->set (pos, true, Pot::spread));
				queue_parameter_display (PanWidthAutomation, pos);
			}

			_last_pan_azi_position_written = pos;
		}
	}
}

void
Strip::select_event (Button&, ButtonState bs)
{
	DEBUG_TRACE (DEBUG::MackieControl, "select button\n");

	if (bs == press) {

		int ms = _surface->mcp().main_modifier_state();

		if (ms & MackieControlProtocol::MODIFIER_CMDALT) {
			_controls_locked = !_controls_locked;
			_surface->write (display (1,_controls_locked ?  "Locked" : "Unlock"));
			block_vpot_mode_display_for (1000);
			return;
		}

		if (ms & MackieControlProtocol::MODIFIER_SHIFT) {
			/* reset to default */
			boost::shared_ptr<AutomationControl> ac = _fader->control ();
			if (ac) {
				ac->set_value (ac->normal());
			}
			return;
		}

		DEBUG_TRACE (DEBUG::MackieControl, "add select button on press\n");
		_surface->mcp().add_down_select_button (_surface->number(), _index);
		_surface->mcp().select_range ();

	} else {
		DEBUG_TRACE (DEBUG::MackieControl, "remove select button on release\n");
		_surface->mcp().remove_down_select_button (_surface->number(), _index);
	}
}

void
Strip::vselect_event (Button&, ButtonState bs)
{
	if (bs == press) {

		int ms = _surface->mcp().main_modifier_state();

		if (ms & MackieControlProtocol::MODIFIER_SHIFT) {

			boost::shared_ptr<AutomationControl> ac = _vpot->control ();

			if (ac) {

				/* reset to default/normal value */
				ac->set_value (ac->normal());
			}

		}  else {

			DEBUG_TRACE (DEBUG::MackieControl, "switching to next pot mode\n");
			next_pot_mode ();
		}

	}
}

void
Strip::fader_touch_event (Button&, ButtonState bs)
{
	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("fader touch, press ? %1\n", (bs == press)));

	if (bs == press) {

		boost::shared_ptr<AutomationControl> ac = _fader->control ();

		if (_surface->mcp().main_modifier_state() & MackieControlProtocol::MODIFIER_SHIFT) {
			if (ac) {
				ac->set_value (ac->normal());
			}
		} else {

			_fader->set_in_use (true);
			_fader->start_touch (_surface->mcp().transport_frame());

			if (ac) {
				queue_parameter_display ((AutomationType) ac->parameter().type(), ac->get_value());
			}
		}

	} else {

		_fader->set_in_use (false);
		_fader->stop_touch (_surface->mcp().transport_frame(), true);

	}
}


void
Strip::handle_button (Button& button, ButtonState bs)
{
	boost::shared_ptr<AutomationControl> control;

	if (bs == press) {
		button.set_in_use (true);
	} else {
		button.set_in_use (false);
	}

	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("strip %1 handling button %2 press ? %3\n", _index, button.bid(), (bs == press)));

	switch (button.bid()) {
	case Button::Select:
		select_event (button, bs);
		break;

	case Button::VSelect:
		vselect_event (button, bs);
		break;

	case Button::FaderTouch:
		fader_touch_event (button, bs);
		break;

	default:
		if ((control = button.control ())) {
			if (bs == press) {
				DEBUG_TRACE (DEBUG::MackieControl, "add button on press\n");
				_surface->mcp().add_down_button ((AutomationType) control->parameter().type(), _surface->number(), _index);

				float new_value;
				int ms = _surface->mcp().main_modifier_state();

				if (ms & MackieControlProtocol::MODIFIER_SHIFT) {
					/* reset to default/normal value */
					new_value = control->normal();
				} else {
					new_value = control->get_value() ? 0.0 : 1.0;
				}

				/* get all controls that either have their
				 * button down or are within a range of
				 * several down buttons
				 */

				MackieControlProtocol::ControlList controls = _surface->mcp().down_controls ((AutomationType) control->parameter().type());


				DEBUG_TRACE (DEBUG::MackieControl, string_compose ("there are %1 buttons down for control type %2, new value = %3\n",
									    controls.size(), control->parameter().type(), new_value));

				/* apply change */

				for (MackieControlProtocol::ControlList::iterator c = controls.begin(); c != controls.end(); ++c) {
					(*c)->set_value (new_value);
				}

			} else {
				DEBUG_TRACE (DEBUG::MackieControl, "remove button on release\n");
				_surface->mcp().remove_down_button ((AutomationType) control->parameter().type(), _surface->number(), _index);
			}
		}
		break;
	}
}

void
Strip::queue_parameter_display (AutomationType type, float val)
{
	RedisplayRequest req;

	req.type = type;
	req.val = val;

	redisplay_requests.write (&req, 1);
}

void
Strip::do_parameter_display (AutomationType type, float val)
{
	bool screen_hold = false;

	switch (type) {
	case GainAutomation:
		if (val == 0.0) {
			_surface->write (display (1, " -inf "));
		} else {
			char buf[16];
			float dB = accurate_coefficient_to_dB (val);
			snprintf (buf, sizeof (buf), "%6.1f", dB);
			_surface->write (display (1, buf));
			screen_hold = true;
		}
		break;

	case PanAzimuthAutomation:
		if (_route) {
			boost::shared_ptr<Pannable> p = _route->pannable();
			if (p && _route->panner()) {
				string str =_route->panner()->value_as_string (p->pan_azimuth_control);
				_surface->write (display (1, str));
				screen_hold = true;
			}
		}
		break;

	case PanWidthAutomation:
		if (_route) {
			char buf[16];
			snprintf (buf, sizeof (buf), "%5ld%%", lrintf ((val * 200.0)-100));
			_surface->write (display (1, buf));
			screen_hold = true;
		}
		break;

	case TrimAutomation:
		if (_route) {
			char buf[16];
			float dB = accurate_coefficient_to_dB (val);
			snprintf (buf, sizeof (buf), "%6.1f", dB);
			_surface->write (display (1, buf));
			screen_hold = true;
		}
		break;

	default:
		break;
	}

	if (screen_hold) {
		block_vpot_mode_display_for (1000);
	}
}

void
Strip::handle_fader_touch (Fader& fader, bool touch_on)
{
	if (touch_on) {
		fader.start_touch (_surface->mcp().transport_frame());
	} else {
		fader.stop_touch (_surface->mcp().transport_frame(), false);
	}
}

void
Strip::handle_fader (Fader& fader, float position)
{
	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("fader to %1\n", position));

	fader.set_value (position);

	/* From the Mackie Control MIDI implementation docs:

	   In order to ensure absolute synchronization with the host software,
	   Mackie Control uses a closed-loop servo system for the faders,
	   meaning the faders will always move to their last received position.
	   When a host receives a Fader Position Message, it must then
	   re-transmit that message to the Mackie Control or else the faders
	   will return to their last position.
	*/

	_surface->write (fader.set_position (position));
}

void
Strip::handle_pot (Pot& pot, float delta)
{
	/* Pots only emit events when they move, not when they
	   stop moving. So to get a stop event, we need to use a timeout.
	*/

	boost::shared_ptr<AutomationControl> ac = pot.control();
	double p = pot.get_value ();
	p += delta;
	p = max (ac->lower(), p);
	p = min (ac->upper(), p);
	pot.set_value (p);
}

void
Strip::periodic (ARDOUR::microseconds_t now)
{
	bool reshow_vpot_mode = false;
	bool reshow_name = false;

	if (!_route) {
		return;
	}

	if (_block_screen_redisplay_until >= now) {

		/* no drawing here, for now */
		return;

	} else if (_block_screen_redisplay_until) {

		/* timeout reached, reset */

		_block_screen_redisplay_until = 0;
		reshow_vpot_mode = true;
		reshow_name = true;
	}

	if (_block_vpot_mode_redisplay_until >= now) {
		return;
	} else if (_block_vpot_mode_redisplay_until) {

		/* timeout reached, reset */

		_block_vpot_mode_redisplay_until = 0;
		reshow_vpot_mode = true;
	}

	if (reshow_name) {
		show_route_name ();
	}

	if (reshow_vpot_mode) {
		return_to_vpot_mode_display ();
	} else {
		/* no point doing this if we just switched back to vpot mode
		   display */
		update_automation ();
	}

	update_meter ();
}

void
Strip::redisplay (ARDOUR::microseconds_t now)
{
	RedisplayRequest req;
	bool have_request = false;

	while (redisplay_requests.read (&req, 1) == 1) {
		/* read them all */
		have_request = true;
	}

	if (_block_screen_redisplay_until >= now) {
		return;
	}

	if (have_request) {
		do_parameter_display (req.type, req.val);
	}
}

void
Strip::update_automation ()
{
	ARDOUR::AutoState gain_state = _route->gain_control()->automation_state();

	if (gain_state == Touch || gain_state == Play) {
		notify_gain_changed (false);
	}

	if (_route->panner()) {
		ARDOUR::AutoState panner_state = _route->panner()->automation_state();
		if (panner_state == Touch || panner_state == Play) {
			notify_panner_azi_changed (false);
			notify_panner_width_changed (false);
		}
	}
	if (_route->trim()) {
		ARDOUR::AutoState trim_state = _route->trim_control()->automation_state();
		if (trim_state == Touch || trim_state == Play) {
			notify_trim_changed (false);
		}
	}
}

void
Strip::update_meter ()
{
	if (_meter && _transport_is_rolling && _metering_active) {
		float dB = const_cast<PeakMeter&> (_route->peak_meter()).meter_level (0, MeterMCP);
		_meter->send_update (*_surface, dB);
	}
}

void
Strip::zero ()
{
	for (Group::Controls::const_iterator it = _controls.begin(); it != _controls.end(); ++it) {
		_surface->write ((*it)->zero ());
	}

	_surface->write (blank_display (0));
	_surface->write (blank_display (1));
}

MidiByteArray
Strip::blank_display (uint32_t line_number)
{
	return display (line_number, string());
}

MidiByteArray
Strip::display (uint32_t line_number, const std::string& line)
{
	assert (line_number <= 1);

	MidiByteArray retval;

	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("strip_display index: %1, line %2 = %3\n", _index, line_number, line));

	// sysex header
	retval << _surface->sysex_hdr();

	// code for display
	retval << 0x12;
	// offset (0 to 0x37 first line, 0x38 to 0x6f for second line)
	retval << (_index * 7 + (line_number * 0x38));

	// ascii data to display. @param line is UTF-8
	string ascii = Glib::convert_with_fallback (line, "UTF-8", "ISO-8859-1", "_");
	string::size_type len = ascii.length();
	if (len > 6) {
		ascii = ascii.substr (0, 6);
		len = 6;
	}
	retval << ascii;
	// pad with " " out to 6 chars
	for (int i = len; i < 6; ++i) {
		retval << ' ';
	}

	// column spacer, unless it's the right-hand column
	if (_index < 7) {
		retval << ' ';
	}

	// sysex trailer
	retval << MIDI::eox;

	return retval;
}

void
Strip::lock_controls ()
{
	_controls_locked = true;
}

void
Strip::unlock_controls ()
{
	_controls_locked = false;
}

void
Strip::gui_selection_changed (const ARDOUR::StrongRouteNotificationList& rl)
{
	for (ARDOUR::StrongRouteNotificationList::const_iterator i = rl.begin(); i != rl.end(); ++i) {
		if ((*i) == _route) {
			_surface->write (_select->set_state (on));
			return;
		}
	}

	_surface->write (_select->set_state (off));
}

string
Strip::vpot_mode_string () const
{
	boost::shared_ptr<AutomationControl> ac = _vpot->control();

	if (!ac) {
		return string();
	}

	switch (ac->parameter().type()) {
	case GainAutomation:
		return "Fader";
	case TrimAutomation:
		return "Trim";
	case PanAzimuthAutomation:
		return "Pan";
	case PanWidthAutomation:
		return "Width";
	case PanElevationAutomation:
		return "Elev";
	case PanFrontBackAutomation:
		return "F/Rear";
	case PanLFEAutomation:
		return "LFE";
	}

	return "???";
}

 void
Strip::potmode_changed (bool notify)
{
	if (!_route) {
		return;
	}

	if (_surface->mcp().flip_mode() != MackieControlProtocol::Normal) {
		/* do not change vpot mode while in flipped mode */
		DEBUG_TRACE (DEBUG::MackieControl, "not stepping pot mode - in flip mode\n");
		_surface->write (display (1, "Flip"));
		block_vpot_mode_display_for (1000);
		return;
	}
	// WIP
	int pm = _surface->mcp().pot_mode();
	switch (pm) {
	case MackieControlProtocol::Pan:
		// This needs to set current pan mode (azimuth or width... or whatever)
		set_vpot_parameter (_pan_mode);
		DEBUG_TRACE (DEBUG::MackieControl, "Assign pot to Pan mode.\n");
		break;
	case MackieControlProtocol::Tracks: // should change the Tracks to Trim
		DEBUG_TRACE (DEBUG::MackieControl, "Assign pot to Trim mode.\n");
			set_vpot_parameter (TrimAutomation);
			break;
	case MackieControlProtocol::Send:
		DEBUG_TRACE (DEBUG::MackieControl, "Assign pot to Send mode.\n");
		// set to current send
		break;
	default:
		cerr << "Pot mode " << pm << " not yet handled\n";
		break;
	}

	if (notify) {
		notify_all ();
	}
}

void
Strip::flip_mode_changed (bool notify)
{
	if (!_route) {
		return;
	}

	reset_saved_values ();

	boost::shared_ptr<AutomationControl> fader_controllable = _fader->control ();
	boost::shared_ptr<AutomationControl> vpot_controllable = _vpot->control ();

	_fader->set_control (vpot_controllable);
	_vpot->set_control (fader_controllable);

	control_by_parameter[fader_controllable->parameter()] = _vpot;
	control_by_parameter[vpot_controllable->parameter()] = _fader;

	_surface->write (display (1, vpot_mode_string ()));

	if (notify) {
		notify_all ();
	}
}

void
Strip::block_screen_display_for (uint32_t msecs)
{
	_block_screen_redisplay_until = ARDOUR::get_microseconds() + (msecs * 1000);
}

void
Strip::block_vpot_mode_display_for (uint32_t msecs)
{
	_block_vpot_mode_redisplay_until = ARDOUR::get_microseconds() + (msecs * 1000);
}

void
Strip::return_to_vpot_mode_display ()
{
	/* returns the second line of the two-line per-strip display
	   back the mode where it shows what the VPot controls.
	*/

	if (_route) {
		_surface->write (display (1, vpot_mode_string()));
	} else {
		_surface->write (blank_display (1));
	}
}

void
Strip::next_pot_mode ()
{
	vector<Evoral::Parameter>::iterator i;

	if (_surface->mcp().flip_mode() != MackieControlProtocol::Normal) {
		/* do not change vpot mode while in flipped mode */
		DEBUG_TRACE (DEBUG::MackieControl, "not stepping pot mode - in flip mode\n");
		_surface->write (display (1, "Flip"));
		block_vpot_mode_display_for (1000);
		return;
	}


	boost::shared_ptr<AutomationControl> ac = _vpot->control();

	if (!ac) {
		return;
	}
	if (_surface->mcp().pot_mode() == MackieControlProtocol::Pan) {
		if (possible_pot_parameters.empty() || (possible_pot_parameters.size() == 1 && possible_pot_parameters.front() == ac->parameter())) {
			return;
		}

		for (i = possible_pot_parameters.begin(); i != possible_pot_parameters.end(); ++i) {
			if ((*i) == ac->parameter()) {
				break;
			}
		}

		/* move to the next mode in the list, or back to the start (which will
		also happen if the current mode is not in the current pot mode list)
		*/

		if (i != possible_pot_parameters.end()) {
			++i;
		}

		if (i == possible_pot_parameters.end()) {
			i = possible_pot_parameters.begin();
		}
		set_vpot_parameter (*i);
	}
}

void
Strip::set_vpot_parameter (Evoral::Parameter p)
{
	boost::shared_ptr<Pannable> pannable;

	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("switch to vpot mode %1\n", p));

	reset_saved_values ();

	/* unset any mapping between the vpot and any existing parameters */

	for (ControlParameterMap::iterator i = control_by_parameter.begin(); i != control_by_parameter.end(); ++i) {

		if (i != control_by_parameter.end() && i->second == _vpot) {
			i->second = 0;
		}
	}

	switch (p.type()) {
	case PanAzimuthAutomation:
		_pan_mode = PanAzimuthAutomation;
		pannable = _route->pannable ();
		if (pannable) {
			if (_surface->mcp().flip_mode() != MackieControlProtocol::Normal) {
				/* gain to vpot, pan azi to fader */
				_vpot->set_control (_route->gain_control());
				control_by_parameter[GainAutomation] = _vpot;
				if (pannable) {
					_fader->set_control (pannable->pan_azimuth_control);
					control_by_parameter[PanAzimuthAutomation] = _fader;
				} else {
					_fader->set_control (boost::shared_ptr<AutomationControl>());
					control_by_parameter[PanAzimuthAutomation] = 0;
				}
			} else {
				/* gain to fader, pan azi to vpot */
				_fader->set_control (_route->gain_control());
				control_by_parameter[GainAutomation] = _fader;
				if (pannable) {
					_vpot->set_control (pannable->pan_azimuth_control);
					control_by_parameter[PanAzimuthAutomation] = _vpot;
				} else {
					_vpot->set_control (boost::shared_ptr<AutomationControl>());
					control_by_parameter[PanAzimuthAutomation] = 0;
				}
			}
		}
		break;
	case PanWidthAutomation:
		_pan_mode = PanWidthAutomation;
		pannable = _route->pannable ();
		if (pannable) {
			if (_surface->mcp().flip_mode() != MackieControlProtocol::Normal) {
				/* gain to vpot, pan width to fader */
				_vpot->set_control (_route->gain_control());
				control_by_parameter[GainAutomation] = _vpot;
				if (pannable) {
					_fader->set_control (pannable->pan_width_control);
					control_by_parameter[PanWidthAutomation] = _fader;
				} else {
					_fader->set_control (boost::shared_ptr<AutomationControl>());
					control_by_parameter[PanWidthAutomation] = 0;
				}
			} else {
				/* gain to fader, pan width to vpot */
				_fader->set_control (_route->gain_control());
				control_by_parameter[GainAutomation] = _fader;
				if (pannable) {
					_vpot->set_control (pannable->pan_width_control);
					control_by_parameter[PanWidthAutomation] = _vpot;
				} else {
					_vpot->set_control (boost::shared_ptr<AutomationControl>());
					control_by_parameter[PanWidthAutomation] = 0;
				}
			}
		}
		break;
	case PanElevationAutomation:
		break;
	case PanFrontBackAutomation:
		break;
	case PanLFEAutomation:
		break;
	case TrimAutomation:
		if (_route->trim()) {
			if (_surface->mcp().flip_mode() != MackieControlProtocol::Normal) {
				/* gain to vpot, trim to fader */
				_vpot->set_control (_route->gain_control());
				control_by_parameter[GainAutomation] = _vpot;
				_fader->set_control (_route->trim_control());
				control_by_parameter[TrimAutomation] = _fader;
			} else {
				/* gain to fader, trim to vpot */
				_fader->set_control (_route->gain_control());
				control_by_parameter[GainAutomation] = _fader;
				_vpot->set_control (_route->trim_control());
				control_by_parameter[TrimAutomation] = _vpot;
			}
		} else {
			_vpot->set_control (boost::shared_ptr<AutomationControl>());
			control_by_parameter[TrimAutomation] = 0;
		}
		break;
	default:
		DEBUG_TRACE (DEBUG::MackieControl, string_compose ("vpot mode %1 not known.\n", p));
		break;

	}

	_surface->write (display (1, vpot_mode_string()));
}

void
Strip::reset_saved_values ()
{
	_last_pan_azi_position_written = -1.0;
	_last_pan_width_position_written = -1.0;
	_last_gain_position_written = -1.0;
	_last_trim_position_written = -1.0;

}

void
Strip::notify_metering_state_changed()
{
	if (!_route || !_meter) {
		return;
	}

	bool transport_is_rolling = (_surface->mcp().get_transport_speed () != 0.0f);
	bool metering_active = _surface->mcp().metering_active ();

	if ((_transport_is_rolling == transport_is_rolling) && (_metering_active == metering_active)) {
		return;
	}

	_meter->notify_metering_state_changed (*_surface, transport_is_rolling, metering_active);

	if (!transport_is_rolling || !metering_active) {
		notify_property_changed (PBD::PropertyChange (ARDOUR::Properties::name));
		notify_panner_azi_changed (true);
	}

	_transport_is_rolling = transport_is_rolling;
	_metering_active = metering_active;
}
