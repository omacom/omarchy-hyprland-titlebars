"""Check client isolation when the ambient parent display differs from the verifier.

Run inside a disposable Omabox with the same arguments as verify.py, including
--tablet-fixture. The real tablet checks must map their client using the supplied
environment even while the ambient Wayland connection is unusable.
"""
import os
from pathlib import Path
import runpy
import sys
from unittest.mock import patch

import tablet_checks

if os.environ.get('OMABOX') != '1' or not os.environ.get('OMABOX_NAME'):
  raise SystemExit('Run this regression inside a disposable Omabox')
if '--tablet-fixture' not in sys.argv:
  raise SystemExit('This regression requires --tablet-fixture')

verify_tablet = tablet_checks.verify_tablet

def different_parent(*args, **kwargs):
  with patch.dict(os.environ, {
    'WAYLAND_DISPLAY': '/nonexistent/oht-parent-wayland',
    'XDG_RUNTIME_DIR': '/nonexistent/oht-parent-runtime',
    'HYPRLAND_INSTANCE_SIGNATURE': 'oht-parent-unreachable',
  }):
    return verify_tablet(*args, **kwargs)

sys.argv[0] = str(Path(__file__).with_name('verify.py'))
with patch.object(tablet_checks, 'verify_tablet', different_parent):
  runpy.run_path(sys.argv[0], run_name='__main__')
print('PASS: tablet clients use the supplied compositor environment with an unusable parent display', flush=True)
