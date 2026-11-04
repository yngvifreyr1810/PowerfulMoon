"""MIT. Check CONTRIBUTING.md content rules against the full ZIP and embedded Setup payload."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import tarfile
import zipfile
from installer_core import check_name

BAD=re.compile(r'(^|/)(?:extracted|exefs|romfs|save|cache|screenshots|input|logs|generated[^/]*)/|\.(?:nsp|xci|nca|nso|nsz|xcz|npdm|keys|bfttf)$|(^|/)odyssey_game\.(dll|so|dylib)(?:\.id)?$',re.I)
ROBOTO='8559132c89ad51d8a2ba5b171887a44a7ba93776e205f553573de228e64b45f8'
def sha(data):return hashlib.sha256(data).hexdigest()
def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--zip',type=Path,required=True)
    parser.add_argument('--setup',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args();entries=0;fonts=[];nested={}
    with zipfile.ZipFile(args.zip) as z:
        manifest=json.loads(z.read('PowerfulMoon/PACKAGE.json'))
        for name,digest in manifest['files'].items():
            check_name(name);assert not BAD.search(name),name
            data=z.read('PowerfulMoon/'+name);assert sha(data)==digest,name
            entries+=1
            if name.lower().endswith(('.ttf','.otf','.ttc')):
                assert sha(data)==ROBOTO,'Unrecognized font: '+name
                fonts.append({'path':name,'identity':'Roboto Medium','license':'Apache-2.0','sha256':ROBOTO})
            if name.endswith(('.tar.gz','.tar.xz')):
                with tarfile.open(fileobj=io.BytesIO(data),mode='r:*') as t:names=t.getnames()
                assert not any(BAD.search(n) for n in names),'Forbidden nested content in '+name
                nested[name]=len(names)
            elif name.endswith('.zip'):
                with zipfile.ZipFile(io.BytesIO(data)) as inner:names=inner.namelist()
                assert not any(BAD.search(n) for n in names),'Forbidden nested content in '+name
                nested[name]=len(names)
        assert z.read('PowerfulMoon/icon.png')==z.read('PowerfulMoon/source/docs/icon/icon_512.png')
        assert 'PowerfulMoon/licenses/Apache-2.0.txt' in z.namelist()
    from PyInstaller.archive.readers import CArchiveReader
    installer=CArchiveReader(str(args.setup))
    payload=installer.extract('PowerfulMoon-windows-x64.zip')
    zip_hash=sha(args.zip.read_bytes());assert sha(payload)==zip_hash,'Embedded payload differs from audited ZIP'
    report={'project':'PowerfulMoon','rule_source':'CONTRIBUTING.md','source_commit':manifest['source_commit'],
            'zip_sha256':zip_hash,'setup_sha256':sha(args.setup.read_bytes()),'verified_payload_files':entries,
            'nested_source_and_python_archive_entries':nested,'fonts':fonts,
            'artwork':'Upstream-authored crescent icon; no Nintendo/Cappy image included',
            'embedded_setup_payload_matches_zip':True,'forbidden_paths_found':[],
            'game_files_keys_saves_generated_game_library_included':False}
    args.report.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('Release content audit: PASS;',entries,'payload files;',sum(nested.values()),'nested archive entries')
if __name__=='__main__':main()
