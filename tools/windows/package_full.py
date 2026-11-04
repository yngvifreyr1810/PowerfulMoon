"""MIT. Build a complete Windows distribution from pinned redistributables and tracked source."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile
from installer_core import check_name

VERSION='0.2.0-powerfulmoon.1'
def sha(data):return hashlib.sha256(data).hexdigest()
def generator_modules(root):
    todo=[root/'tools/gen.py'];seen=set()
    while todo:
        p=todo.pop()
        if p in seen:continue
        seen.add(p)
        for node in ast.walk(ast.parse(p.read_text(encoding='utf-8'))):
            names=[a.name for a in node.names] if isinstance(node,ast.Import) else [node.module] if isinstance(node,ast.ImportFrom) and node.module and not node.level else []
            for name in names:
                candidate=root/'tools'/(name.split('.')[0]+'.py')
                if candidate.is_file():todo.append(candidate)
    return sorted(seen)
def main():
    parser=argparse.ArgumentParser()
    for key in ('runtime-root','bridge-zip','libcxx','dependency-sources','output'):parser.add_argument('--'+key,type=Path,required=True)
    args=parser.parse_args();root=Path(__file__).resolve().parents[2];files={}
    def add(name,data):
        check_name(name)
        if name in files:raise ValueError('Duplicate payload path: '+name)
        files[name]=data
    vendor=json.loads((root/'tools/windows/redistributables-0.2.0.json').read_text())
    for item in vendor['files']:
        name=item['path'];data=(args.runtime_root/name).read_bytes()
        if sha(data)!=item['sha256']:raise ValueError('Redistributable changed: '+name)
        add(name,data)
    for item in json.loads((root/'tools/windows/dependency-sources.json').read_text()):
        data=(args.dependency_sources/item['name']).read_bytes()
        if sha(data)!=item['sha256']:raise ValueError('Dependency source archive changed: '+item['name'])
        add('licenses/source/'+item['name'],data)
    add('PowerfulMoon.exe',(root/'build/PowerfulMoon.exe').read_bytes())
    add('PowerfulMoon.dll.a',(root/'build/PowerfulMoon.dll.a').read_bytes())
    add('libc++.dll',args.libcxx.read_bytes())
    for name,source in {'cpu.h':'runtime/cpu.h','simd.h':'runtime/simd.h','simd_fp.h':'runtime/simd_fp.h','gamelib.h':'runtime/gamelib.h','gamelib_entry.c':'tools/gamelib_entry.c','hle_symbols.txt':'runtime/hle/hle_symbols.txt'}.items():
        add('gamelib-src/'+name,(root/source).read_bytes())
    for p in generator_modules(root):add('odysseygen/tools/'+p.name,p.read_bytes())
    add('known_programs.txt',(root/'runtime/nvn/vulkan/known_programs.txt').read_bytes())
    add('icon.png',(root/'docs/icon/icon_512.png').read_bytes())
    for name in ('README.md','SECURITY.md','CREDITS.md','CONTROLLER-NOTES.md','LICENSE'):add(name,(root/name).read_bytes())
    # Bridge has a separate GPL license. This is composition, not linking.
    if sha(args.bridge_zip.read_bytes())!='6cb9e17d45e2338689009a8fe57344c0160df124f0474bce91c7c1c487a145e6':raise ValueError('Unexpected bridge release')
    with zipfile.ZipFile(args.bridge_zip) as bridge:
        for name in bridge.namelist():
            if not name.startswith('ProCon2Bridge/'):raise ValueError('Unexpected bridge archive path')
            add('controller-bridge/'+name[len('ProCon2Bridge/'):],bridge.read(name))
    tracked=subprocess.check_output(['git','-C',str(root),'ls-files','-z']).decode().split('\0')[:-1]
    for name in tracked:
        p=root/name
        if p.is_file():add('source/'+name,p.read_bytes())
    commit=subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()
    add('SOURCE.txt',f'PowerfulMoon {VERSION}\nhttps://github.com/yngvifreyr1810/PowerfulMoon\nSource commit: {commit}\nFork/customization of https://gitlab.com/zzzarathustra-dev/power-moon-recomp at v0.2.0 (1fdc703def082ae44277e62d869f3de06ce5f699).\nMIT host source: source/. Separate GPL bridge source/licenses: controller-bridge/.\nUpstream-authored moon icon retained; no Nintendo logo/artwork is included.\nNo game data, keys, saves or generated game-code library are distributed.\n'.encode())
    manifest={'name':'PowerfulMoon','version':VERSION,'source_commit':commit,'legacy_programs':json.loads((root/'tools/windows/legacy-programs.json').read_text()),'files':{name:sha(data) for name,data in sorted(files.items())}}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(args.output,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for name,data in sorted(files.items()):z.writestr('PowerfulMoon/'+name,data)
        z.writestr('PowerfulMoon/PACKAGE.json',json.dumps(manifest,indent=2).encode())
    args.output.with_suffix(args.output.suffix+'.sha256').write_text(sha(args.output.read_bytes())+'  '+args.output.name+'\n',encoding='ascii')
    print(args.output.name,len(files),'audited files;',args.output.stat().st_size,'bytes')
if __name__=='__main__':main()
