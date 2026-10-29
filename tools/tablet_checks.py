"""Tablet handler integration tests using synthetic Aquamarine events.

Runs the actual CTablet -> PointerManager -> CInputManager -> plugin callbacks.
This is not libinput, uinput, driver, or physical-tablet coverage.
"""
import json
import re
import subprocess
import time


def verify_tablet(ctl, pointer, config, artifacts, library, fixture, first, second, env):
  def windows():
    return {w['address']: w for w in json.loads(ctl('clients', '-j'))}

  def event(op, x=0, y=0, down=0, expected=-1, tool=0):
    monitors = json.loads(ctl('monitors', '-j'))
    left = min(m['x'] for m in monitors)
    top = min(m['y'] for m in monitors)
    right = max(m['x'] + m['width'] / m['scale'] for m in monitors)
    bottom = max(m['y'] + m['height'] / m['scale'] for m in monitors)
    reply = ctl('eval', 'hl.plugin.oht_tablet.event(' + json.dumps(f'{op} {tool} {(x-left)/(right-left)} {(y-top)/(bottom-top)} {down} {expected}') + ')')
    assert reply.strip() == 'ok', reply
    time.sleep(.08)

  def place():
    ctl('dispatch', f'hl.dsp.window.fullscreen({{ mode = "maximized", action = "unset", window = "address:{first}" }})')
    ctl('dispatch', f'hl.dsp.window.move({{ x = 60, y = 320, window = "address:{first}" }})')
    time.sleep(.2)
    return windows()[first]

  # Count activations instead of closing our fixture window.
  saved = config.read_text()
  marker = artifacts / 'tablet-buttons.log'
  marker.write_text('')
  text = re.sub(r'action = \[\[.*?\]\]', lambda _: "action = [[printf '%s\\n' %WINDOW% >> " + str(marker) + "]]", saved)
  config.write_text(text)
  ctl('reload')
  if not any(p['name'] == 'oht-tablet-fixture' for p in json.loads(ctl('plugin', 'list', '-j'))):
    ctl('plugin', 'load', str(fixture.resolve()))
    ctl('reload')
  try:
    event('proximity', 100, 100, 1)
    before = place()
    x = before['at'][0] + before['size'][0] - 20
    y = before['at'][1] - 15
    # Deliberately stale cursor: a tip must use the event, not hover coordinates.
    pointer(['move 1100 700'])
    event('tip', x, y, 1, 1)
    event('tip', x, y, 0, 1)
    time.sleep(.2)
    assert marker.read_text().splitlines() == [first], marker.read_text()
    print('PASS: tablet button activates once at event coordinates; paired release consumed', flush=True)
    pointer([f'move {x} {y}'])
    event('touch-down', x, y, 1, 1)
    event('touch-up', x, y, expected=0)
    event('tip', x, y, 1, 1)
    event('tip', x, y, 0, 1)
    time.sleep(.2)
    assert marker.read_text().splitlines() == [first, first, first]
    print('PASS: a completed touch button action cannot block subsequent pen input', flush=True)

    # A consumed mouse button down is still owned when a pen subsequently
    # captures a bar. Its release must be consumed even after focus changes.
    for pen_window in [first, second]:
      place()
      ctl('dispatch', f'hl.dsp.focus({{ window = "address:{first}" }})')
      event('mouse-down', x, y, 1, 1)
      pen_bar = windows()[pen_window]
      px, py = pen_bar['at'][0] + 120, pen_bar['at'][1] - 15
      time.sleep(.45)
      event('tip', px, py, 1, 1)
      event('mouse-up', 100, 400, expected=1)
      event('axis', px + 20, py + 10, expected=1)
      event('tip', px + 20, py + 10, 0, 1)
      ctl('dispatch', f'hl.dsp.focus({{ window = "address:{first}" }})')
      # A following application click must keep its unconsumed release.
      event('mouse-down', 100, 400, 1, 0)
      event('mouse-up', 100, 400, expected=0)
    place()
    print('PASS: earlier mouse-button releases stay paired during same/other-window pen capture', flush=True)

    # Touch must not replace the owner of a mouse resize, pending move or
    # consumed button press. Use a non-closing button to expose stale state.
    for gesture in ['resize', 'pending-move', 'button']:
      before = place()
      ctl('dispatch', f'hl.dsp.focus({{ window = "address:{first}" }})')
      count = len(marker.read_text().splitlines())
      mx = before['at'][0] + (before['size'][0] - 20 if gesture == 'button' else 120)
      my = before['at'][1] - (34 if gesture == 'resize' else 15)
      time.sleep(.45)
      event('mouse-down', mx, my, 1, 1)
      button_x, button_y = before['at'][0] + before['size'][0] - 20, before['at'][1] - 15
      event('touch-down', button_x, button_y, 1, 0)
      event('touch-up', button_x, button_y, expected=0)
      event('mouse-up', 100, 400, expected=1)
      assert len(marker.read_text().splitlines()) == count + (gesture == 'button'), marker.read_text()
      ctl('dispatch', f'hl.dsp.window.resize({{ x = {before["size"][0]}, y = {before["size"][1]}, window = "address:{first}" }})')
      place()
      # The released mouse gesture cannot strand either core drag state or
      # decoration state: a new pen must capture and move this same window.
      time.sleep(.45)
      event('tip', 180, 305, 1, 1)
      event('axis', 200, 315, expected=1)
      event('tip', 1100, 700, 0, 1)
      assert windows()[first]['at'] == [80, 330], windows()[first]
    place()
    print('PASS: touch cannot steal mouse resize, pending-move or button ownership; pen capture resumes', flush=True)

    event('axis', 100, 400, expected=0)
    event('tip', 100, 400, 1, 0)
    event('tip', 100, 400, 0, 0)
    assert windows()[first]['at'] == before['at']
    event('axis', 400, 100, expected=0)
    assert windows()[first]['at'] == before['at']
    print('PASS: application tip events pass through; hover cannot move a window', flush=True)

    time.sleep(.45)
    event('tip', 180, 305, 1, 1)
    event('axis', 260, 350, expected=1)
    after = windows()[first]
    assert after['at'] == [140, 365], (before, after)
    event('axis', 900, 600, expected=0, tool=1)
    event('tip', 900, 600, 0, 0, tool=1)
    pointer(['move 900 600', 'up'])
    event('touch-down', 180, 305, 1, 0)
    event('touch-motion', 900, 600, expected=0)
    event('touch-up', expected=0)
    assert windows()[first]['at'] == after['at']
    event('axis-x', 280, 0, expected=1)
    assert windows()[first]['at'] == [160, 365], windows()
    event('axis', 180, 305, expected=1)
    assert windows()[first]['at'] == [60, 320], windows()
    event('axis', 280, 370, expected=1)
    assert windows()[first]['at'] == [160, 385], windows()
    event('tip', 1100, 700, 0, 1)
    stopped = windows()[first]['at']
    event('axis', 700, 600, expected=0)
    assert windows()[first]['at'] == stopped
    print('PASS: tablet drag preserves grab offset, ignores another tool/mouse/touch, ends outside bar', flush=True)

    before = place()
    time.sleep(.45)
    event('tip', 180, 305, 1, 1)
    event('tip', 180, 305, 0, 1)
    event('tip', 180, 305, 1, 1)
    event('tip', 180, 305, 0, 1)
    ctl('dispatch', f'hl.dsp.focus({{ window = "address:{second}" }})')
    time.sleep(.75)
    assert windows()[first]['fullscreen'] == 1 and windows()[second]['fullscreen'] == 0, windows()
    print('PASS: delayed tablet double-tap targets its titlebar owner after focus changes', flush=True)
    maximized = windows()[first]
    tx, ty = maximized['at'][0] + 120, maximized['at'][1] - 15
    time.sleep(.45)
    event('tip', tx, ty, 1, 1)
    event('axis', tx + 80, ty + 120, expected=1)
    event('tip', tx + 80, ty + 120, 0, 1)
    restored = windows()[first]
    assert restored['fullscreen'] == 0 and restored['size'] == before['size'], restored
    print('PASS: tablet dragging restores a maximized window', flush=True)

    place()
    time.sleep(.45)
    event('tip', 180, 305, 1, 1)
    event('proximity', 180, 305, 0)
    event('axis', 500, 500, expected=0)
    assert windows()[first]['at'] == [60, 320]
    event('proximity', 180, 305, 1, tool=1)
    event('tip', 180, 305, 1, 1, tool=1)
    event('tip', 500, 500, 0, 1)
    event('axis', 220, 325, expected=1, tool=1)
    assert windows()[first]['at'] == [100, 340]
    event('tip', 220, 325, 0, 1, tool=1)
    place()
    event('proximity', 180, 305, 1)
    time.sleep(.45)
    event('tip', 180, 305, 1, 1)
    event('destroy')
    event('axis', 500, 500, expected=0)
    assert windows()[first]['at'] == [60, 320]
    print('PASS: proximity loss and tool destruction cancel movement safely', flush=True)

    event('proximity', 180, 305, 1)
    time.sleep(.45)
    event('tip', 180, 305, 1, 1)
    config.write_text(text.replace('enabled = true', 'enabled = false'))
    ctl('reload')
    event('axis', 500, 500, expected=0)
    assert windows()[first]['at'] == [60, 320]
    event('tip', 500, 500, 0, 1)
    config.write_text(text)
    ctl('reload')
    place()
    time.sleep(.45)
    event('tip', 180, 305, 1, 1)
    ctl('plugin', 'unload', str(library))
    event('tip', 180, 305, 0, 0)
    ctl('plugin', 'load', str(library))
    ctl('reload')
    time.sleep(.25)
    # The candidate has reconnected once; a single tap still runs one action.
    before_count = len(marker.read_text().splitlines())
    event('tip', x, y, 1, 1)
    event('tip', x, y, 0, 1)
    time.sleep(.2)
    assert len(marker.read_text().splitlines()) == before_count + 1
    print('PASS: disabling and unloading mid-contact cancel movement; reload has no stale listeners', flush=True)

    place()
    time.sleep(.45)
    event('tip', 180, 305, 1, 1)
    event('destroy-tablet')
    event('proximity', 100, 100, 1)
    event('axis', 500, 500, expected=0)
    assert windows()[first]['at'] == [60, 320]
    print('PASS: tablet device removal cancels capture', flush=True)

    monitor = json.loads(ctl('monitors', '-j'))[0]['name']
    ctl('eval', 'hl.monitor(' + '{ output = ' + json.dumps(monitor) + ', mode = "1600x1000@60", position = "-800x-100", scale = 1.25 })')
    time.sleep(.3)
    mapped = json.loads(ctl('monitors', '-j'))[0]
    assert (mapped['width'], mapped['height'], mapped['x'], mapped['y'], mapped['scale']) == (1600, 1000, -800, -100, 1.25), mapped
    ctl('dispatch', f'hl.dsp.window.move({{ x = -600, y = 150, window = "address:{first}" }})')
    time.sleep(.2)
    event('tip', -480, 135, 1, 1)
    event('axis', -400, 175, expected=1)
    event('tip', -100, 500, 0, 1)
    assert windows()[first]['at'] == [-520, 190], windows()[first]
    print('PASS: scaled output with negative layout origin preserves logical grab offset', flush=True)
    ctl('eval', 'hl.monitor(' + '{ output = ' + json.dumps(monitor) + ', mode = "1280x800@60", position = "0x0", scale = 1 })')
    ctl('reload')
    place()

    ctl('output', 'create', 'headless', 'OHT-SECOND')
    ctl('eval', 'hl.monitor({ output = "OHT-SECOND", mode = "1200x900@60", position = "-800x0", scale = 1.5 })')
    time.sleep(.3)
    monitors = json.loads(ctl('monitors', '-j'))
    other = next(m for m in monitors if m['name'] == 'OHT-SECOND')
    assert other['width'] == 1200 and other['scale'] == 1.5 and other['x'] == -800, other
    time.sleep(.45)
    event('tip', 180, 305, 1, 1)
    event('axis', -400, 325, expected=1)
    moved = windows()[first]
    assert moved['at'] == [-520, 340] and moved['monitor'] == other['id'], moved
    event('tip', -400, 325, 0, 1)
    time.sleep(.45)
    event('tip', -400, 325, 1, 1)
    event('axis', 180, 305, expected=1)
    event('tip', 180, 305, 0, 1)
    assert windows()[first]['at'] == [60, 320] and windows()[first]['monitor'] != other['id'], windows()[first]
    ctl('output', 'remove', 'OHT-SECOND')
    ctl('reload')
    place()
    print('PASS: tablet drag crosses monitors with mixed scales and transfers workspace ownership', flush=True)

    third = subprocess.Popen(['foot', '--app-id=oht-tablet-close', 'sleep', '90'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
      for _ in range(50):
        found = [w for w in windows().values() if w['class'] == 'oht-tablet-close']
        if found:
          break
        time.sleep(.05)
      assert found, 'closure test window did not map'
      address = found[0]['address']
      ctl('dispatch', f'hl.dsp.window.move({{ x = 700, y = 180, window = "address:{address}" }})')
      time.sleep(.2)
      event('tip', 800, 165, 1, 1)
      event('axis', 820, 185, expected=1)
      ctl('dispatch', f'hl.dsp.window.close({{ window = "address:{address}" }})')
      for _ in range(50):
        if address not in windows():
          break
        time.sleep(.05)
      assert address not in windows()
      event('tip', 1100, 700, 0, 1)
      event('axis', 100, 100, expected=0)
      print('PASS: window closure during a tablet drag retains the paired release safely', flush=True)
    finally:
      if third.poll() is None:
        third.terminate()
      third.wait(timeout=5)
    place()

  finally:
    if any(m['name'] == 'OHT-SECOND' for m in json.loads(ctl('monitors', '-j'))):
      ctl('output', 'remove', 'OHT-SECOND')
    config.write_text(saved)
    ctl('reload')
