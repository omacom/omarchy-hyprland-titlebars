"""Exercise verifier failure cleanup inside a disposable Omabox.

Each child executes the real verify.py and injects a subprocess failure, then
checks that the box configuration was restored byte for byte.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('library', type=Path)
parser.add_argument('--artifacts', type=Path, required=True)
args = parser.parse_args()
if os.environ.get('OMABOX') != '1' or not os.environ.get('OMABOX_NAME'):
  parser.error('run this verifier inside a disposable Omabox')
args.library = args.library.resolve()
args.artifacts.mkdir(parents=True, exist_ok=True)
verifier = Path(__file__).with_name('verify.py')
config = Path.home() / '.config/hypr/looknfeel.lua'
original = config.read_bytes()

# Patch only the IPC subprocess boundary. The actual setup, overwrite, plugin
# loading, finally block and ExitStack callbacks execute from verify.py.
child = r'''
import runpy
import subprocess
import sys
from unittest.mock import patch
mode, verifier, library, artifacts = sys.argv[1:]
real_output, real_run = subprocess.check_output, subprocess.run

def output(argv, *args, **kwargs):
  if argv[:2] == ['hyprctl', 'instances']:
    if mode == 'instances-error':
      raise subprocess.CalledProcessError(9, argv, 'injected instances failure')
    if mode == 'instance-count':
      return '[]'
  return real_output(argv, *args, **kwargs)

def run(argv, *args, **kwargs):
  if mode == 'unload-error' and argv[:3] == ['hyprctl', 'plugin', 'unload']:
    return subprocess.CompletedProcess(argv, 9, 'injected unload failure', '')
  return real_run(argv, *args, **kwargs)

sys.argv = [verifier, library, '--in-omabox', '--no-screenshots', '--artifacts', artifacts]
with patch.object(subprocess, 'check_output', output), patch.object(subprocess, 'run', run):
  runpy.run_path(verifier, run_name='__main__')
'''

for mode, reason in [('instances-error', 'CalledProcessError'),
                     ('instance-count', 'exactly one parent compositor'),
                     ('unload-error', 'injected unload failure')]:
  try:
    result = subprocess.run([sys.executable, '-c', child, mode, str(verifier), str(args.library), str(args.artifacts / mode)],
                            text=True, capture_output=True, timeout=30)
    (args.artifacts / f'{mode}.log').write_text(result.stdout + result.stderr)
    assert result.returncode != 0 and reason in result.stderr, (mode, result)
    assert config.read_bytes() == original, f'{mode}: test configuration was not restored'
    print(f'PASS: {mode} restores the original box configuration', flush=True)
  finally:
    # A deliberately failed unload leaves that plugin loaded. Remove it through
    # real IPC after recording the restoration result, for the next case.
    plugins = subprocess.check_output(['hyprctl', 'plugin', 'list', '-j'], text=True)
    if any(p['name'] == 'hyprbars' for p in json.loads(plugins)):
      subprocess.run(['hyprctl', 'plugin', 'unload', str(args.library)], check=True, capture_output=True, text=True)
    subprocess.run(['hyprctl', 'reload'], check=True, capture_output=True, text=True)
