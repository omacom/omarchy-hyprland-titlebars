"""Disposable native GTK client for the nested-compositor control checks."""
import json
from pathlib import Path
import sys

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import GLib, Gtk

GLib.set_prgname('omarchy-client-controls')
command = Path(sys.argv[1])
state = Path(sys.argv[2])
window = Gtk.Window(title='Client-side controls')
window.set_default_size(600, 400)
window.set_decorated(False)
content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
header = Gtk.Box()
header.pack_start(Gtk.Label(label='Client-side controls'), True, True, 8)
for label, callback in [('_', window.iconify), ('□', lambda: window.unmaximize() if window.is_maximized() else window.maximize()), ('×', window.destroy)]:
    button = Gtk.Button(label=label)
    button.connect('clicked', lambda _, action=callback: action())
    header.pack_start(button, False, False, 0)
content.pack_start(header, False, False, 0)
content.pack_start(Gtk.Label(label='App-owned frameless caption and native control requests'), True, True, 0)
window.add(content)
window.connect('destroy', Gtk.main_quit)
window.show_all()
sequence = -1
dialog = None


def tick():
    global sequence, dialog
    if command.exists():
        request = json.loads(command.read_text())
        if request['sequence'] != sequence:
            sequence = request['sequence']
            action = request['action']
            if action == 'maximize':
                window.maximize()
            elif action == 'restore':
                window.unmaximize()
            elif action == 'minimize':
                window.iconify()
            elif action == 'dialog':
                dialog = Gtk.Window(title='Native transient dialog', transient_for=window, modal=True)
                dialog.set_default_size(240, 120)
                dialog.add(Gtk.Label(label='Toolkit-owned transient caption'))
                dialog.show_all()
            elif action == 'close-dialog' and dialog is not None:
                dialog.destroy()
                dialog = None
    pending = state.with_suffix('.tmp')
    pending.write_text(json.dumps({'maximized': window.is_maximized(), 'sequence': sequence}))
    pending.replace(state)
    return True


GLib.timeout_add(100, tick)
Gtk.main()
