"""Verify omarchy-hyprland-titlebars lifecycle and pointer gestures in an isolated nested Hyprland.

The candidate library is only ever loaded into the disposable nested compositor,
never into the parent session.
"""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import time

source = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('library', type=Path, help='built titlebars.so to test')
parser.add_argument('--artifacts', type=Path, default=Path('/tmp/omarchy-hyprland-titlebars-verification'))
args = parser.parse_args()
args.library = args.library.resolve()
if not args.library.is_file():
  parser.error(f'{args.library} does not exist')

# Probe the same interpreter used for the GTK fixture before compiling the
# pointer helper or starting a compositor. A missing binding/typelib otherwise
# looks like a client that simply failed to map.
gtk_probe = subprocess.run(
    [sys.executable, '-c', "import gi; gi.require_version('Gtk', '3.0'); from gi.repository import GLib, Gtk"],
    capture_output=True, text=True)
if gtk_probe.returncode:
  parser.error('The GTK fixture requires PyGObject and GTK 3 introspection data '
               f'for {sys.executable}:\n{gtk_probe.stderr.strip()}')
args.artifacts.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix='oht-') as scratch:
  scratch = Path(scratch)
  applications = scratch / 'data/applications'
  applications.mkdir(parents=True)
  (applications / 'omarchy-client-controls.desktop').write_text(
      '[Desktop Entry]\nName=Client controls fixture\nStartupWMClass=omarchy-client-controls\nX-Omarchy-ClientControls=true\n')
  for kind, target in [('client-header', 'pointer-protocol.h'), ('private-code', 'pointer-protocol.c')]:
    subprocess.run(['wayland-scanner', kind, str(source / 'wlr-virtual-pointer-unstable-v1.xml'), str(scratch / target)], check=True)
  subprocess.run(['cc', str(source / 'pointer.c'), str(scratch / 'pointer-protocol.c'), '-I', str(scratch), '-lwayland-client', '-o', str(scratch / 'pointer')], check=True)
  unconfigured = scratch / 'unconfigured'
  config = scratch / 'hyprland.lua'
  config.write_text('''
hl.monitor({ output = "", mode = "1280x800@60", position = "0x0", scale = 1 })
hl.config({ general = { gaps_in = 8, gaps_out = 8, border_size = 2, resize_on_border = true }, animations = { enabled = false }, input = { follow_mouse = 0 }, misc = { disable_hyprland_logo = true, disable_splash_rendering = true } })
hl.window_rule({ name = "test-float", match = { class = ".*" }, float = true, size = { 600, 400 }, center = true })
local unconfigured = io.open("UNCONFIGURED", "r")
if unconfigured then unconfigured:close() end
if hl.plugin and hl.plugin.hyprbars and not unconfigured then
  hl.config({ plugin = { hyprbars = {
    enabled = true, bar_height = 30, bar_color = "rgb(24283b)", bar_color_inactive = "rgb(1a1b26)", col = { text = "rgb(c0caf5)" },
    button_rounding = 4, bar_text_align = "left", bar_text_font = "Sans", bar_text_size = 12, bar_part_of_window = true,
    on_double_click = [[hyprctl dispatch 'hl.dsp.window.fullscreen({ mode = "maximized", action = "toggle", window = "address:%WINDOW%" })']],
  } } })
  hl.plugin.hyprbars.add_button({ bg_color = "rgb(f7768e)", fg_color = "rgb(1a1b26)", size = 18, icon = "×", action = [[hyprctl dispatch 'hl.dsp.window.close({ window = "address:%WINDOW%" })']] })
end
'''.replace('UNCONFIGURED', str(unconfigured)))
  parent_instances = json.loads(subprocess.check_output(['hyprctl', 'instances', '-j'], text=True))
  if len(parent_instances) != 1:
    raise RuntimeError('Verification expects exactly one parent compositor')
  parent_signature = parent_instances[0]['instance']
  parent_display = str(Path(os.environ['XDG_RUNTIME_DIR']) / parent_instances[0]['wl_socket'])
  env = dict(os.environ, XDG_RUNTIME_DIR=str(scratch), XDG_DATA_HOME=str(scratch / 'data'), AQ_BACKEND='wayland', WAYLAND_DISPLAY=parent_display, HYPRLAND_NO_SD_NOTIFY='1', HYPRLAND_NO_SD_VARS='1')
  env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
  log = (args.artifacts / 'compositor.log').open('w')
  compositor = subprocess.Popen(['Hyprland', '--config', str(config)], env=env, stdout=log, stderr=log)
  terminal = None
  secondary = None
  gtk_client = None
  csd_terminal = None

  def ctl(*argv):
    # Never let a plugin command reach the parent compositor.
    if argv[0] == 'plugin':
      signature = env.get('HYPRLAND_INSTANCE_SIGNATURE')
      if not signature or signature == parent_signature or env['XDG_RUNTIME_DIR'] != str(scratch):
        raise RuntimeError('Refusing to send a plugin command outside the nested compositor')
    result = subprocess.run(['hyprctl', *argv], env=env, capture_output=True, text=True, timeout=5, check=True)
    if result.stdout.startswith('Err') or "Couldn't" in result.stdout:
      raise RuntimeError(result.stdout)
    return result.stdout

  def client():
    return json.loads(ctl('clients', '-j'))[0]

  def pointer(commands):
    subprocess.run([str(scratch / 'pointer'), '1280', '800'], input='\n'.join(commands) + '\n', text=True, env=env, check=True, timeout=10)
    time.sleep(.15)

  def drag_to(x, y):
    window = client()
    start_x = window['at'][0] + min(window['size'][0] // 2, 220)
    start_y = window['at'][1] - 15
    commands = [f'move {start_x} {start_y}', 'wait 100', 'down', 'wait 120']
    for i in range(1, 21):
      commands += [f'move {round(start_x + (x - start_x) * i / 20)} {round(start_y + (y - start_y) * i / 20)}', 'wait 20']
    commands += ['wait 100', 'up']
    pointer(commands)
    return client()

  def capture(name):
    monitor = json.loads(ctl('monitors', '-j'))[0]['name']
    subprocess.run(['grim', '-o', monitor, str(args.artifacts / f'{name}.png')], env=env, check=True)

  try:
    for _ in range(80):
      if compositor.poll() is not None:
        raise RuntimeError('Isolated compositor exited; inspect compositor.log')
      try:
        instances = json.loads(ctl('instances', '-j'))
      except (ValueError, subprocess.SubprocessError):
        instances = []
      if instances:
        env['HYPRLAND_INSTANCE_SIGNATURE'] = instances[0]['instance']
        env['WAYLAND_DISPLAY'] = instances[0]['wl_socket']
        break
      time.sleep(.1)
    else:
      raise RuntimeError('Isolated compositor did not start')
    # The socket is ready before the nested Wayland output is necessarily
    # advertised. foot exits immediately if it connects during that interval.
    for _ in range(80):
      if json.loads(ctl('monitors', '-j')):
        break
      time.sleep(.1)
    else:
      raise RuntimeError('Isolated compositor did not advertise a monitor')
    terminal = subprocess.Popen(['foot', '--app-id=omarchy-titlebars-isolated-test', '--title=Titlebars — isolated verification', 'sleep', '90'], env=env, stdout=log, stderr=log)
    for _ in range(60):
      if json.loads(ctl('clients', '-j')):
        break
      time.sleep(.1)
    else:
      raise RuntimeError('Test client did not map')

    xp = Path('/usr/lib/omarchy-windows-xp/hyprbars.so')
    sequence = [args.library, args.library]
    if xp.exists():
      sequence = [xp, args.library, xp, args.library]
    for library in sequence:
      ctl('plugin', 'load', str(library))
      time.sleep(.15)
      for _ in range(3):
        ctl('reload')
        time.sleep(.1)
        if library == args.library:
          assert not ctl('configerrors').strip(), ctl('configerrors')
        assert compositor.poll() is None
        assert client()
      ctl('plugin', 'unload', str(library))
      time.sleep(.15)
      assert not json.loads(ctl('plugin', 'list', '-j'))
    print('PASS: repeated load/reload/unload' + (', interleaved with the XP fork' if xp.exists() else '; XP fork unavailable'), flush=True)

    # Hyprland keeps a plugin loaded after the config stops asking for it, and
    # puts its options back to their defaults, so an unconfigured bar must
    # reserve no space.
    unconfigured.touch()
    ctl('plugin', 'load', str(args.library))
    ctl('reload')
    ctl('dispatch', 'hl.dsp.window.fullscreen({ mode = "maximized", action = "set" })')
    time.sleep(.3)
    assert client()['at'][1] <= 12, client()
    capture('unconfigured-no-titlebar')
    ctl('dispatch', 'hl.dsp.window.fullscreen({ mode = "maximized", action = "unset" })')
    ctl('plugin', 'unload', str(args.library))
    unconfigured.unlink()
    time.sleep(.15)
    print('PASS: a loaded but unconfigured plugin draws no titlebars', flush=True)

    ctl('plugin', 'load', str(args.library))
    ctl('reload')
    time.sleep(.25)
    assert not ctl('configerrors').strip(), ctl('configerrors')
    capture('floating')

    # Omarchy draws the border around the titlebar too. Hyprland measures its
    # resize band from the window's content, which puts the top of that band
    # under the titlebar, so the titlebar has to start a resize from the band
    # above it itself.
    ctl('eval', 'hl.config({ general = { extend_border_grab_area = 8 }, plugin = { hyprbars = { bar_precedence_over_border = true } } })')
    ctl('dispatch', 'hl.dsp.window.move({ x = 340, y = 260 })')
    time.sleep(.3)
    before = client()
    x = before['at'][0] + before['size'][0] // 3
    y = before['at'][1] - 30 - 2 - 4
    commands = [f'move {x} {y}', 'wait 100', 'down', 'wait 120']
    for i in range(1, 11):
      commands += [f'move {x} {y - 6 * i}', 'wait 20']
    commands += ['wait 100', 'up']
    pointer(commands)
    after = client()
    assert after['size'][1] >= before['size'][1] + 40, (before, after)
    assert after['at'][1] + after['size'][1] == before['at'][1] + before['size'][1], (before, after)
    capture('top-edge-resized')
    print('PASS: dragging just above the titlebar resizes the window from its top edge', flush=True)
    ctl('eval', 'hl.config({ plugin = { hyprbars = { bar_precedence_over_border = false } } })')
    ctl('dispatch', f'hl.dsp.window.resize({{ x = {before["size"][0]}, y = {before["size"][1]} }})')
    ctl('dispatch', 'hl.dsp.window.center()')
    time.sleep(.3)

    original = client()

    edge = drag_to(1, 360)
    assert edge['size'] == original['size'] and edge['fullscreen'] == 0, edge
    top = drag_to(650, 1)
    assert top['size'] == original['size'] and top['fullscreen'] == 0, top
    # Dropped with its titlebar above the top edge, the window comes back down.
    assert top['at'][1] >= 32, top
    print('PASS: real pointer titlebar drags move the window to any edge without snapping and keep its titlebar on screen', flush=True)

    window = client()
    x, y = window['at'][0] + 120, window['at'][1] - 15
    pointer([f'move {x} {y}', 'wait 450', 'down', 'up', 'wait 100', 'down', 'up'])
    assert client()['fullscreen'] == 1, client()
    capture('maximized')
    print('PASS: titlebar double-click maximize', flush=True)
    restored = drag_to(650, 220)
    assert restored['fullscreen'] == 0 and restored['size'] == original['size'], restored
    print('PASS: dragging a maximized titlebar restores its size under the pointer', flush=True)

    # Make dispatch deliberately slow, then focus another window after clicking.
    # Both double-click and button actions must keep the decoration owner.
    config.write_text(config.read_text().replace('hyprctl dispatch', 'sleep 0.6; hyprctl dispatch'))
    ctl('reload')
    first_address = client()['address']
    ctl('dispatch', f'hl.dsp.window.fullscreen({{ mode = "maximized", action = "unset", window = "address:{first_address}" }})')
    ctl('dispatch', f'hl.dsp.window.resize({{ x = 500, y = 350, window = "address:{first_address}" }})')
    ctl('dispatch', f'hl.dsp.window.move({{ x = 60, y = 320, window = "address:{first_address}" }})')
    secondary = subprocess.Popen(['foot', '--app-id=omarchy-titlebars-focus-race', '--title=Second window must stay unchanged', 'sleep', '90'], env=env, stdout=log, stderr=log)
    for _ in range(60):
      windows = json.loads(ctl('clients', '-j'))
      if len(windows) == 2:
        break
      time.sleep(.1)
    assert len(windows) == 2, windows
    second_address = next(window['address'] for window in windows if window['address'] != first_address)
    ctl('dispatch', f'hl.dsp.window.move({{ x = 690, y = 320, window = "address:{second_address}" }})')
    pointer(['move 180 305', 'wait 450', 'down', 'up', 'wait 100', 'down', 'up'])
    ctl('dispatch', f'hl.dsp.focus({{ window = "address:{second_address}" }})')
    time.sleep(.7)
    windows = {window['address']: window for window in json.loads(ctl('clients', '-j'))}
    assert windows[first_address]['fullscreen'] == 1 and windows[second_address]['fullscreen'] == 0, windows
    ctl('dispatch', f'hl.dsp.window.fullscreen({{ mode = "maximized", action = "unset", window = "address:{first_address}" }})')
    ctl('dispatch', f'hl.dsp.window.move({{ x = 60, y = 320, window = "address:{first_address}" }})')
    pointer(['move 540 305', 'wait 450', 'down', 'up'])
    ctl('dispatch', f'hl.dsp.focus({{ window = "address:{second_address}" }})')
    time.sleep(.7)
    remaining = json.loads(ctl('clients', '-j'))
    assert len(remaining) == 1 and remaining[0]['address'] == second_address, remaining
    print('PASS: delayed double-click and close target the clicked window after focus changes', flush=True)

    # A bar whose window loses focus before the release, as one minimized from
    # its own button does, must not keep the release armed: the next click in
    # the window would lose its release and the app would see the button held.
    # The client logs its pointer events, so the release it gets is visible.
    pressed = args.artifacts / 'release-client.log'
    released = subprocess.Popen(['foot', '--app-id=omarchy-titlebars-release', '--title=Release must arrive', 'sleep', '90'], env=dict(env, WAYLAND_DEBUG='client'), stdout=log, stderr=pressed.open('w'))
    for _ in range(60):
      windows = json.loads(ctl('clients', '-j'))
      if len(windows) == 2:
        break
      time.sleep(.1)
    assert len(windows) == 2, windows
    third_address = next(window['address'] for window in windows if window['address'] != second_address)
    ctl('dispatch', f'hl.dsp.window.resize({{ x = 500, y = 350, window = "address:{third_address}" }})')
    ctl('dispatch', f'hl.dsp.window.move({{ x = 60, y = 320, window = "address:{third_address}" }})')
    ctl('dispatch', f'hl.dsp.focus({{ window = "address:{third_address}" }})')
    time.sleep(.3)
    held = subprocess.Popen([str(scratch / 'pointer'), '1280', '800'], stdin=subprocess.PIPE, text=True, env=env)
    held.stdin.write('move 180 305\nwait 100\ndown\nwait 500\nup\nwait 100\n')
    held.stdin.close()
    time.sleep(.35)
    ctl('dispatch', f'hl.dsp.focus({{ window = "address:{second_address}" }})')
    held.wait(timeout=10)
    pointer(['move 300 500', 'wait 100', 'down', 'wait 80', 'up', 'wait 400'])
    times = [int(t) for t in re.findall(r'wl_pointer#\d+\.button\(\d+, (\d+), 272, \d\)', pressed.read_text(errors='replace'))]
    assert len(times) >= 2 and times[-1] - times[-2] < 300, times
    print('PASS: a titlebar press released after focus moves away leaves the next click intact', flush=True)

    # The focused window's bar can sit under another window, and a press there
    # belongs to the window on top.
    ctl('dispatch', f'hl.dsp.window.move({{ x = 160, y = 220, window = "address:{second_address}" }})')
    ctl('dispatch', f'hl.dsp.focus({{ window = "address:{third_address}" }})')
    ctl('dispatch', f'hl.dsp.window.alter_zorder({{ mode = "top", window = "address:{second_address}" }})')
    time.sleep(.3)
    covered = {window['address']: window for window in json.loads(ctl('clients', '-j'))}[third_address]
    pointer(['move 300 305', 'wait 100', 'down', 'wait 80', 'move 340 345', 'wait 80', 'up'])
    windows = {window['address']: window for window in json.loads(ctl('clients', '-j'))}
    assert json.loads(ctl('activewindow', '-j'))['address'] == second_address, windows
    assert windows[third_address]['at'] == covered['at'], windows
    print('PASS: a press over a covered titlebar goes to the window on top', flush=True)
    released.terminate()
    released.wait(timeout=5)
    for _ in range(60):
      if len(json.loads(ctl('clients', '-j'))) == 1:
        break
      time.sleep(.1)

    # A workspace-scoped bar must release its reserved space as soon as a
    # window joins tiling, without waiting for a compositor config reload.
    ctl('eval', 'hl.config({ plugin = { hyprbars = { workspace_tag = "floating-test" } } })')
    ctl('dispatch', f'hl.dsp.window.tag({{ tag = "+floating-test", window = "address:{second_address}" }})')
    ctl('dispatch', f'hl.dsp.window.float({{ action = "disable", window = "address:{second_address}" }})')
    time.sleep(.3)
    tiled = client()
    assert not tiled['floating'] and tiled['at'][1] < 30, tiled
    capture('workspace-tiled-no-titlebar')
    ctl('dispatch', f'hl.dsp.window.float({{ action = "enable", window = "address:{second_address}" }})')
    ctl('dispatch', f'hl.dsp.window.fullscreen({{ mode = "maximized", action = "set", window = "address:{second_address}" }})')
    time.sleep(.3)
    assert client()['at'][1] >= 30, client()
    ctl('dispatch', f'hl.dsp.window.tag({{ tag = "-floating-test", window = "address:{second_address}" }})')
    time.sleep(.3)
    assert client()['at'][1] < 30, client()
    capture('workspace-untagged-no-titlebar')
    print('PASS: workspace-scoped titlebars release space immediately when tiled or untagged', flush=True)

    # A frameless app declares its own controls in the installed desktop entry.
    # Exercise real GTK requests under Omarchy's maximize suppression.
    command_file, state_file = scratch / 'client-command.json', scratch / 'client-state.json'
    ctl('eval', 'hl.window_rule({ name = "suppress-client-maximize", match = { class = "omarchy-client-controls" }, suppress_event = "maximize" })')
    gtk_client = subprocess.Popen([sys.executable, str(source / 'gtk-client.py'), str(command_file), str(state_file)],
                                  env=dict(env, GDK_BACKEND='wayland'), stdout=log, stderr=log)
    for _ in range(60):
      windows = json.loads(ctl('clients', '-j'))
      native = next((window for window in windows if window['pid'] == gtk_client.pid), None)
      if native and state_file.exists():
        break
      time.sleep(.1)
    assert native and state_file.exists(), 'GTK client did not map'
    native_address = native['address']
    ctl('dispatch', f'hl.dsp.window.tag({{ tag = "+floating-test", window = "address:{native_address}" }})')
    time.sleep(.4)
    capture('client-decorations-initial')
    assert not json.loads(state_file.read_text())['maximized'], state_file.read_text()

    sequence = [0]
    def request(action, settle=.35):
      sequence[0] += 1
      pending = command_file.with_suffix('.tmp')
      pending.write_text(json.dumps({'sequence': sequence[0], 'action': action}))
      pending.replace(command_file)
      time.sleep(settle)

    def native_window():
      return next(window for window in json.loads(ctl('clients', '-j')) if window['address'] == native_address)

    request('maximize')
    assert native_window()['fullscreen'] == 1, native_window()
    assert native_window()['at'][1] < 30, native_window()
    capture('client-decorations-single-header')
    request('maximize')
    assert native_window()['fullscreen'] == 1, native_window()
    request('restore')
    assert native_window()['fullscreen'] == 0, native_window()

    request('dialog')
    dialog = next(window for window in json.loads(ctl('clients', '-j'))
                  if window['pid'] == gtk_client.pid and window['address'] != native_address)
    ctl('dispatch', f'hl.dsp.window.tag({{ tag = "+floating-test", window = "address:{dialog["address"]}" }})')
    ctl('dispatch', f'hl.dsp.window.fullscreen({{ mode = "maximized", action = "set", window = "address:{dialog["address"]}" }})')
    time.sleep(.3)
    dialog = next(window for window in json.loads(ctl('clients', '-j')) if window['address'] == dialog['address'])
    assert dialog['at'][1] >= 30, dialog
    capture('transient-retains-caption')
    request('close-dialog')
    request('restore')
    assert native_window()['fullscreen'] == 0, native_window()

    # Untagged windows remain subject to the compositor's normal policy.
    ctl('dispatch', f'hl.dsp.window.tag({{ tag = "-floating-test", window = "address:{native_address}" }})')
    request('maximize')
    assert native_window()['fullscreen'] == 0, native_window()
    ctl('dispatch', f'hl.dsp.window.tag({{ tag = "+floating-test", window = "address:{native_address}" }})')
    minimize = '''sleep 0.25; hyprctl dispatch 'hl.dsp.window.move({ workspace = "special:client-minimized", follow = false, window = "address:%WINDOW%" })' '''
    ctl('eval', 'hl.config({ plugin = { hyprbars = { on_minimize = ' + json.dumps(minimize) + ' } } })')
    request('minimize', settle=.05)
    ctl('dispatch', f'hl.dsp.focus({{ window = "address:{second_address}" }})')
    time.sleep(.4)
    assert native_window()['workspace']['name'] == 'special:client-minimized', native_window()
    other = next(window for window in json.loads(ctl('clients', '-j')) if window['address'] == second_address)
    assert other['workspace']['name'] != 'special:client-minimized', other

    # A normal xdg-decoration client keeps the existing server-decoration policy.
    csd_log = args.artifacts / 'xdg-client.log'
    with csd_log.open('w') as stream:
      csd_terminal = subprocess.Popen(['foot', '-o', 'csd.preferred=client', '--app-id=omarchy-xdg-client',
                                       '--title=Negotiated client caption', 'sleep', '90'],
                                      env=dict(env, WAYLAND_DEBUG='client'), stdout=log, stderr=stream)
    for _ in range(60):
      csd = next((window for window in json.loads(ctl('clients', '-j')) if window['pid'] == csd_terminal.pid), None)
      if csd:
        break
      time.sleep(.1)
    assert csd, 'xdg-decoration client did not map'
    ctl('dispatch', f'hl.dsp.window.tag({{ tag = "+floating-test", window = "address:{csd["address"]}" }})')
    time.sleep(.4)
    assert re.search(r'zxdg_toplevel_decoration_v1.*configure\(2\)', csd_log.read_text()), 'Server decoration mode was not preserved'
    capture('xdg-client-server-policy')
    ctl('plugin', 'unload', str(args.library))
    request('restore')
    assert compositor.poll() is None
    print('PASS: declared frameless controls, truthful state, maximize/restore and targeted minimize; transient, untagged and toolkit policies preserved', flush=True)
  finally:
    if csd_terminal and csd_terminal.poll() is None:
      csd_terminal.terminate()
      csd_terminal.wait(timeout=5)
    if gtk_client and gtk_client.poll() is None:
      gtk_client.terminate()
      gtk_client.wait(timeout=5)
    if secondary and secondary.poll() is None:
      secondary.terminate()
      secondary.wait(timeout=5)
    if terminal and terminal.poll() is None:
      terminal.terminate()
      terminal.wait(timeout=5)
    compositor.terminate()
    try:
      compositor.wait(timeout=5)
    except subprocess.TimeoutExpired:
      compositor.kill()
      compositor.wait()
    log.close()
