#ifndef __libgtkmm2ext_bindings_h__
#define __libgtkmm2ext_bindings_h__

#include <map>
#include <stdint.h>
#include <gdk/gdkkeysyms.h>
#include <gtkmm/action.h>
#include <gtkmm/accelkey.h>
#include <gtkmm/radioaction.h>
#include <gtkmm/toggleaction.h>

class XMLNode;

namespace Gtk {
  class Widget;
}

namespace Gtkmm2ext {

class KeyboardKey
{
  public:
        KeyboardKey () {
                _val = GDK_VoidSymbol;
        }
        
        KeyboardKey (uint32_t state, uint32_t keycode);
        
        uint32_t state() const { return _val >> 32; }
        uint32_t key() const { return _val & 0xffff; }
        
        bool operator<(const KeyboardKey& other) const {
                return _val < other._val;
        }

        bool operator==(const KeyboardKey& other) const {
                return _val == other._val;
        }

        std::string name() const;
        static bool make_key (const std::string&, KeyboardKey&);

  private:
        uint64_t _val;
};

class MouseButton {
  public:
        MouseButton () {
                _val = ~0ULL;
        }

        MouseButton (uint32_t state, uint32_t button_number);
        uint32_t state() const { return _val >> 32; }
        uint32_t button() const { return _val & 0xffff; }

        bool operator<(const MouseButton& other) const {
                return _val < other._val;
        }

        bool operator==(const MouseButton& other) const {
                return _val == other._val;
        }

        std::string name() const;
        static bool make_button (const std::string&, MouseButton&);
        static void set_ignored_state (int mask) {
                _ignored_state = mask;
        }

  private:
        uint64_t _val;
        static uint32_t _ignored_state;
};

class ActionMap {
  public:
        ActionMap() {}
        ~ActionMap() {}

	Glib::RefPtr<Gtk::Action> register_action (const char* path,
						   const char* name, const char* label, sigc::slot<void> sl);
	Glib::RefPtr<Gtk::Action> register_radio_action (const char* path, Gtk::RadioAction::Group&,
							 const char* name, const char* label, 
                                                         sigc::slot<void,GtkAction*> sl,
                                                         int value);
	Glib::RefPtr<Gtk::Action> register_toggle_action (const char*path,
							  const char* name, const char* label,
							  sigc::slot<void> sl);

        Glib::RefPtr<Gtk::Action> find_action (const std::string& name);

	/* this uses the GtkUIManager (and in the future, GtkBuilder) to get a
	   widget (typically a MenuItem) that was constructed automatically
	   when building menus dynamically from lists of actions.
	*/
	Gtk::Widget* get_widget (const std::string& name);

	void do_action (const std::string& name);
	void check_toggleaction (const std::string& name);
	void uncheck_toggleaction (const std::string& name);

	static void set_sensitive (std::vector<Glib::RefPtr<Gtk::Action> >& actions, bool);

  private:
        typedef std::map<std::string, Glib::RefPtr<Gtk::Action> > _ActionMap;
        _ActionMap actions;

	void set_toggleaction_state (const std::string&, bool);
};        

class Bindings {
  public:
        enum Operation { 
                Press,
                Release
        };
        
        Bindings();
        ~Bindings ();

	bool bind (const std::string&, KeyboardKey new_binding, Operation op = Press);
        bool activate (KeyboardKey, Operation);
        bool activate (MouseButton, Operation);

        bool load (const std::string& path);
        void load (const XMLNode& node);
        bool save (const std::string& path);
        void save (XMLNode& root);
        
        std::string get_key_representation (const std::string& action, Gtk::AccelKey& key);

        void set_action_map (ActionMap&);

        static void set_ignored_state (int mask) {
                _ignored_state = mask;
        }

        static uint32_t ignored_state() { return _ignored_state; }

        static std::string unbound_string() { return _unbound_string; }

  private:
        typedef std::map<KeyboardKey,Glib::RefPtr<Gtk::Action> > KeybindingMap;

        KeybindingMap press_bindings;
        KeybindingMap release_bindings;

        void add (KeyboardKey, Operation, Glib::RefPtr<Gtk::Action>);
        void remove (KeyboardKey, Operation);

        typedef std::map<MouseButton,Glib::RefPtr<Gtk::Action> > MouseButtonBindingMap;
        MouseButtonBindingMap button_press_bindings;
        MouseButtonBindingMap button_release_bindings;

        void add (MouseButton, Operation, Glib::RefPtr<Gtk::Action>);
        void remove (MouseButton, Operation);

        ActionMap* action_map;
        static uint32_t _ignored_state;
        static std::string _unbound_string;
};

} // namespace

#endif /* __libgtkmm2ext_bindings_h__ */
