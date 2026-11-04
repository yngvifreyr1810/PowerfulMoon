"""MIT. Build a host-only update, never copy an installed game's directory."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

def main():
    parser=argparse.ArgumentParser()
    for name in ('exe','libcxx','libcxx-license','notices','output'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();root=Path(__file__).resolve().parents[2]
    files={}
    for name in ('PowerMoon-Switch2Pro-Setup.ps1','PowerMoon-Switch2Pro-Install.cmd','PowerMoon-Switch2Pro-Play.cmd','PowerMoon-Switch2Pro-Restore.cmd'):files[name]=(root/'tools/windows'/name).read_bytes()
    for name in ('README.md','SECURITY.md','CREDITS.md','CONTROLLER-NOTES.md','LICENSE'):files[name]=(root/name).read_bytes()
    files['payload/PowerMoonRecomp.exe']=args.exe.read_bytes();files['payload/libc++.dll']=args.libcxx.read_bytes()
    files['licenses/LLVM-libcxx.txt']=args.libcxx_license.read_bytes()
    # Only explicit legal notices, never neighboring DLLs, keys or other resources.
    for p in args.notices.glob('*.txt'):files['licenses/'+p.name]=p.read_bytes()
    manifest=dict(version='0.1.6-s2pc.2',supportedOriginalHashes=['3699dff7fb3f139a9accfdc7291a1fb362acfd904702460016321db7d8278054'],
        files=[dict(name=name,sha256=hashlib.sha256(files['payload/'+name]).hexdigest()) for name in ('PowerMoonRecomp.exe','libc++.dll')])
    files['runtime-manifest.json']=json.dumps(manifest,indent=2).encode()
    commit=subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()
    files['SOURCE.txt']=f'PowerMoon-Switch2Pro 0.1.6-s2pc.2\nhttps://github.com/yngvifreyr1810/PowerMoon-Switch2Pro\nSource commit: {commit}\nUpstream 0.1.6: 2957896536727cdff26598bdd007733e65c6af89\nNo game data or generated game library is included.\n'.encode()
    files['CHECKSUMS-SHA256.txt']=''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n' for name,data in sorted(files.items())).encode()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(args.output,'w',zipfile.ZIP_DEFLATED) as archive:
        for name,data in sorted(files.items()):archive.writestr('PowerMoon-Switch2Pro/'+name,data)
    print(args.output.name,len(files),'allowlisted files')
if __name__=='__main__':main()
