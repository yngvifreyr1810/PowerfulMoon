"""MIT. Fresh transaction + native dependency load; no game, GPU or controller setup."""
import argparse
import ctypes
from pathlib import Path
import subprocess
import tempfile
from installer_core import install

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--zip', type=Path, required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    previous = ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    try:
        with tempfile.TemporaryDirectory(prefix='PowerfulMoon-clean-test-') as temp:
            exe = install(args.zip, Path(temp) / 'PowerfulMoon')
            result = subprocess.run([str(exe), '--check-runtime'], cwd=exe.parent,
                                    capture_output=True, text=True, timeout=30, creationflags=0x08000000)
            if result.returncode != 0 or ('PowerfulMoon ' + args.version) not in result.stdout:
                raise RuntimeError('Packaged native host load failed: ' + str(result.returncode) + ' ' + result.stderr)
            print('Fresh package install and native dependency load: PASS')
    finally:
        ctypes.windll.kernel32.SetErrorMode(previous)

if __name__ == '__main__': main()
