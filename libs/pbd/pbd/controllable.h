/*
    Copyright (C) 2000-2007 Paul Davis

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

#ifndef __pbd_controllable_h__
#define __pbd_controllable_h__

#include <string>
#include <set>
#include <map>

#include "pbd/libpbd_visibility.h"
#include "pbd/signals.h"
#include <glibmm/threads.h>

#include "pbd/statefuldestructible.h"

using std::min;
using std::max;

class XMLNode;

namespace PBD {

class LIBPBD_API Controllable : public PBD::StatefulDestructible {
  public:
	enum Flag {
		Toggle = 0x1,
		GainLike = 0x2,
	};

	Controllable (const std::string& name, Flag f = Flag (0));
	virtual ~Controllable() { Destroyed (this); }

	/* We express Controllable values in one of three ways:
	 * 1. `user' --- as presented to the user (e.g. dB, Hz, etc.)
	 * 2. `interface' --- as used in some cases for the UI representation
	 * (in order to make controls behave logarithmically).
	 * 3. `internal' --- as passed to a processor, track, plugin, or whatever.
	 *
	 * Note that in some cases user and processor may be the same
	 * (and interface different) e.g. frequency, which is presented
	 * to the user and passed to the processor in linear terms, but
	 * which needs log scaling in the interface.
	 *
	 * In other cases, user and interface may be the same (and processor different)
	 * e.g. gain, which is presented to the user in log terms (dB)
	 * but passed to the processor as a linear quantity.
	 */

	/** Get and Set `internal' value */
	virtual void set_value (double) = 0;
	virtual double get_value (void) const = 0;

	/** Conversions between `internal', 'interface', and 'user' values */
	virtual double internal_to_interface (double i) const {return  (i-lower())/(upper() - lower());}  //by default, the interface range is just a linear interpolation between lower and upper values
	virtual double interface_to_internal (double i) const {return lower() + i*(upper() - lower());}
	virtual double internal_to_user (double i) const {return i;}  //by default the internal value is the same as the user value
	virtual double user_to_internal (double i) const {return i;}  //by default the internal value is the same as the user value

	/** Get and Set `interface' value  (typically, percent of knob travel) */
	virtual float get_interface() const { return (internal_to_interface(get_value())); }
	virtual void set_interface (float percent) { percent = min( max(0.0f, percent), 1.0f);  set_value(interface_to_internal(percent)); }

	/** Get and Set `user' value  ( dB or milliseconds, etc.  This MIGHT be the same as the internal value, but in a few cases it is not ) */
	virtual float get_user() const { return (internal_to_user(get_value())); }
	virtual void set_user (float user_v) { set_value(user_to_internal(user_v)); }
	virtual std::string get_user_string() const { return std::string(); }

	PBD::Signal0<void> LearningFinished;
	static PBD::Signal3<void,PBD::Controllable*,int,int> CreateBinding;
	static PBD::Signal1<void,PBD::Controllable*> DeleteBinding;

	static PBD::Signal1<bool,PBD::Controllable*> StartLearning;
	static PBD::Signal1<void,PBD::Controllable*> StopLearning;

	static PBD::Signal1<void,Controllable*> Destroyed;

	PBD::Signal0<void> Changed;

	int set_state (const XMLNode&, int version);
	XMLNode& get_state ();

	std::string name()      const { return _name; }

	bool touching () const { return _touching; }
	void set_touching (bool yn) { _touching = yn; }

	bool is_toggle() const { return _flags & Toggle; }
	bool is_gain_like() const { return _flags & GainLike; }

        virtual double lower() const { return 0.0; }
        virtual double upper() const { return 1.0; }
        virtual double normal() const { return 0.0; }  //the default value

	Flag flags() const { return _flags; }
	void set_flags (Flag f);

	static Controllable* by_id (const PBD::ID&);
	static Controllable* by_name (const std::string&);
        static const std::string xml_node_name;
  private:
	std::string _name;

	std::string _units;

	Flag        _flags;
	bool        _touching;

	static void add (Controllable&);
	static void remove (Controllable*);

	typedef std::set<PBD::Controllable*> Controllables;
        static Glib::Threads::RWLock registry_lock;
	static Controllables registry;
};

/* a utility class for the occasions when you need but do not have
   a Controllable
*/

class LIBPBD_API IgnorableControllable : public Controllable
{
  public:
	IgnorableControllable () : PBD::Controllable ("ignoreMe") {}
	~IgnorableControllable () {}

	void set_value (double /*v*/) {}
	double get_value () const { return 0.0; }
};

}

#endif /* __pbd_controllable_h__ */
