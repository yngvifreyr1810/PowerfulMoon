"""The game code generator for the Windows release: Python's own interpreter (python.exe, python312.dll, ...: built and
signed by the Python Software Foundation, as in python.org's embeddable package) with its standard library zipped, the
generator's packages (capstone, unicorn, lz4) and tools/gen.py with the modules it imports - instead of a PyInstaller
bundle, which Windows Defender's machine-learning check took for a trojan (Trojan:Win32/Bearfoos.B!ml, 2026-11-02).
python312._pth fixes the module path (no environment variables, no user site). runtime/gamebuild.c starts
odysseygen/python.exe -B odysseygen/tools/gen.py <args>, with Control Flow Guard off (unicorn).

usage: python tools/pyembed.py <out dir>   (run with build/gen-venv's python: its base Python and site-packages)
"""
import ast
import os
import shutil
import sys
import sysconfig
import zipfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
PACKAGES = ['capstone', 'unicorn', 'lz4']
# the extension modules of the standard library the generator can reach (ctypes: capstone, unicorn), with their DLLs
PYDS = ['_ctypes', '_bz2', '_lzma', '_decimal', '_queue', '_overlapped', '_asyncio', '_multiprocessing', '_socket',
        'select', 'unicodedata', 'pyexpat', '_elementtree', '_uuid', '_zoneinfo']
DLLS = ['libffi-8.dll']
# not zipped: what the generator never uses
SKIP = {'test', 'tests', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'venv', 'lib2to3', 'pydoc_data', 'site-packages',
        '__pycache__'}


def local_modules(script):
    """tools/*.py that script imports, transitively"""
    todo, seen = [script], set()
    while todo:
        path = todo.pop()
        if path in seen:
            continue
        seen.add(path)
        tree = ast.parse(open(path, encoding='utf-8').read(), path)
        for node in ast.walk(tree):
            names = [a.name for a in node.names] if isinstance(node, ast.Import) else \
                    [node.module] if isinstance(node, ast.ImportFrom) and node.module and not node.level else []
            for name in names:
                cand = os.path.join(TOOLS, name.split('.')[0] + '.py')
                if os.path.exists(cand):
                    todo.append(cand)
    return sorted(seen)


def main():
    out = sys.argv[1]
    home = sys.base_prefix
    site = sysconfig.get_paths()['purelib']
    tag = 'python%d%d' % sys.version_info[:2]
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, 'tools'))
    for f in ['python.exe', 'python3.dll', tag + '.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'LICENSE.txt']:
        shutil.copy2(os.path.join(home, f), out)
    for m in PYDS:
        p = os.path.join(home, 'DLLs', m + '.pyd')
        if os.path.exists(p):
            shutil.copy2(p, out)
    for d in DLLS:
        shutil.copy2(os.path.join(home, 'DLLs', d), out)
    lib = os.path.join(home, 'Lib')
    n = 0
    with zipfile.ZipFile(os.path.join(out, tag + '.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(lib):
            dirs[:] = sorted(d for d in dirs if d not in SKIP)
            for f in sorted(files):
                if f.endswith('.py'):
                    p = os.path.join(root, f)
                    z.write(p, os.path.relpath(p, lib).replace(os.sep, '/'))
                    n += 1
    for pkg in PACKAGES:
        shutil.copytree(os.path.join(site, pkg), os.path.join(out, 'site-packages', pkg),
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '*.a', '*.lib', 'include'))
    scripts = local_modules(os.path.join(TOOLS, 'gen.py'))
    for s in scripts:
        shutil.copy2(s, os.path.join(out, 'tools'))
    with open(os.path.join(out, tag + '._pth'), 'w', newline='\n') as f:
        f.write(tag + '.zip\n.\nsite-packages\ntools\n')
    print('%s: Python %s (%s), %d standard modules, %s, %s' % (out, sys.version.split()[0], home, n, ', '.join(PACKAGES),
          ', '.join(os.path.basename(s) for s in scripts)))


main()
