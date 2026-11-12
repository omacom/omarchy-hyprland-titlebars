"""Verify omarchy-hyprland-titlebars lifecycle and pointer gestures in an isolated nested Hyprland.

The candidate library is only ever loaded into the disposable nested compositor,
never into the parent session.
"""
import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

source = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('library', type=Path, help='built titlebars.so to test')
parser.add_argument('--in-omabox', action='store_true', help='test the current isolated Omabox compositor instead of starting another nested instance')
parser.add_argument('--no-screenshots', action='store_true', help='run assertions without grim (e.g. software rendering VMs)')
parser.add_argument('--tablet-fixture', type=Path, help='synthetic Aquamarine fixture library; handler coverage, not hardware input')
parser.add_argument('--artifacts', type=Path, default=Path('/tmp/omarchy-hyprland-titlebars-verification'))
args = parser.parse_args()
args.library = args.library.resolve()
if not args.library.is_file():
  parser.error(f'{args.library} does not exist')
args.artifacts.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix='oht-') as scratch, ExitStack() as cleanup:
  scratch = Path(scratch)
  for kind, target in [('client-header', 'pointer-protocol.h'), ('private-code', 'pointer-protocol.c')]:
    subprocess.run(['wayland-scanner', kind, str(source / 'wlr-virtual-pointer-unstable-v1.xml'), str(scratch / target)], check=True)
  subprocess.run(['cc', str(source / 'pointer.c'), str(scratch / 'pointer-protocol.c'), '-I', str(scratch), '-lwayland-client', '-o', str(scratch / 'pointer')], check=True)
  unconfigured = scratch / 'unconfigured'
  if args.in_omabox and (os.environ.get('OMABOX') != '1' or not os.environ.get('OMABOX_NAME')):
    raise RuntimeError('--in-omabox requires omabox run inside a disposable box')
  config = Path.home() / '.config/hypr/looknfeel.lua' if args.in_omabox else scratch / 'hyprland.lua'
  saved_config = config.read_bytes() if args.in_omabox else None
  if args.in_omabox:
    def restore_box_config():
      # Register before the first overwrite. This runs on setup failures and
      # after any failing process/plugin cleanup; the file is restored before IPC.
      config.write_bytes(saved_config)
      subprocess.run(['hyprctl', 'reload'], capture_output=True, text=True, timeout=5, check=True)
    cleanup.callback(restore_box_config)
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
  box_signature = os.environ.get('HYPRLAND_INSTANCE_SIGNATURE')
  parent_display = str(Path(os.environ['XDG_RUNTIME_DIR']) / parent_instances[0]['wl_socket'])
  env = dict(os.environ, XDG_RUNTIME_DIR=str(scratch), AQ_BACKEND='wayland', WAYLAND_DISPLAY=parent_display, HYPRLAND_NO_SD_NOTIFY='1', HYPRLAND_NO_SD_VARS='1')
  if args.in_omabox:
    env = dict(os.environ)
  else:
    env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
  if args.tablet_fixture:
    env['OHT_TABLET_FIXTURES'] = '1'

  log = (args.artifacts / 'compositor.log').open('w')
  cleanup.callback(log.close)
  class BoxCompositor:
    def poll(self):
      return None if Path('/proc', str(parent_instances[0]['pid'])).exists() else 1
  compositor = BoxCompositor() if args.in_omabox else subprocess.Popen(['Hyprland', '--config', str(config)], env=env, stdout=log, stderr=log)
  terminal = None
  secondary = None

  def ctl(*argv):
    # Never let a plugin command reach the parent compositor.
    if argv[0] == 'plugin':
      signature = env.get('HYPRLAND_INSTANCE_SIGNATURE')
      if not signature or (args.in_omabox and signature != box_signature) or (not args.in_omabox and (signature == parent_signature or env['XDG_RUNTIME_DIR'] != str(scratch))):
        raise RuntimeError('Refusing to send a plugin command outside the nested compositor')
    result = subprocess.run(['hyprctl', *argv], env=env, capture_output=True, text=True, timeout=5)
    if result.returncode:
      raise RuntimeError(f'hyprctl {argv}: {result.stdout} {result.stderr}')
    if result.stdout.startswith('Err') or "Couldn't" in result.stdout:
      raise RuntimeError(result.stdout)
    if argv[0] == "plugin" and argv[1] in ("load", "unload") and result.stdout.strip() != "ok":
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
    if args.no_screenshots:
      return
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
    if args.in_omabox:
      ctl('reload')
    time.sleep(.2)
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
      assert all(p['name'] == 'oht-tablet-fixture' for p in json.loads(ctl('plugin', 'list', '-j')))
    print('PASS: repeated load/reload/unload, interleaved with the XP fork', flush=True)

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
    (args.artifacts / 'monitors.json').write_text(ctl('monitors', '-j'))
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
    print('PASS: real pointer titlebar drags move the window to any edge without snapping', flush=True)

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
    if args.tablet_fixture:
      from tablet_checks import verify_tablet
      verify_tablet(ctl, pointer, config, args.artifacts, args.library, args.tablet_fixture, first_address, second_address, env)

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
  finally:
    if secondary and secondary.poll() is None:
      secondary.terminate()
      secondary.wait(timeout=5)
    if terminal and terminal.poll() is None:
      terminal.terminate()
      terminal.wait(timeout=5)
    if args.in_omabox:
      for name, library in [('oht-tablet-fixture', args.tablet_fixture), ('hyprbars', args.library)]:
        if library and any(p['name'] == name for p in json.loads(ctl('plugin', 'list', '-j'))):
          ctl('plugin', 'unload', str(library.resolve()))
    else:
      compositor.terminate()
      try:
        compositor.wait(timeout=5)
      except subprocess.TimeoutExpired:
        compositor.kill()
        compositor.wait()
