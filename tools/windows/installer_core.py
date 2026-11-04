"""MIT. Transactional full-package install; user game files are never packaged or removed."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tempfile
import time
import zipfile

PREFIX = 'PowerfulMoon/'
FORBIDDEN_PARTS = {'extracted','exefs','romfs','save','cache','screenshots'}
FORBIDDEN_SUFFIXES = {'.nsp','.xci','.nca','.nso','.nsz','.xcz','.keys','.npdm'}

def safe_path(root, name):
    rel=PurePosixPath(name)
    if rel.is_absolute() or not rel.parts or any(p in ('..','.') or ':' in p or chr(92) in p for p in rel.parts):
        raise ValueError('Unsafe package path: '+name)
    dest=root.joinpath(*rel.parts)
    if not dest.resolve().is_relative_to(root.resolve()): raise ValueError('Path leaves install folder: '+name)
    return dest

def check_name(name):
    rel=PurePosixPath(name)
    if any(p.lower() in FORBIDDEN_PARTS or p.lower().startswith('generated') for p in rel.parts):
        raise ValueError('Game/private content in package: '+name)
    if rel.suffix.lower() in FORBIDDEN_SUFFIXES or rel.name.lower().startswith('odyssey_game.'):
        raise ValueError('Game/private content in package: '+name)
    if rel.name.lower() in ('powermoon.ini','powermoon.ini.controller.ini','config.json','status.json','desktop.json') or rel.suffix.lower() in ('.log','.pyc'):
        raise ValueError('Local configuration in package: '+name)

def install(archive, target, progress=lambda text: None):
    target=Path(target).expanduser().resolve()
    target.mkdir(parents=True,exist_ok=True)
    progress('Checking package integrity...')
    with zipfile.ZipFile(archive) as z:
        manifest=json.loads(z.read(PREFIX+'PACKAGE.json'))
        files=manifest['files']
        if 'PowerfulMoon.exe' not in files or 'PowerfulMoon.dll.a' not in files: raise ValueError('Incomplete PowerfulMoon package')
        names=z.namelist()
        if len(names)!=len(set(names)): raise ValueError('Duplicate package paths')
        if set(names)!={PREFIX+n for n in files}|{PREFIX+'PACKAGE.json'}: raise ValueError('Package contains unexpected files')
        for name in files:
            check_name(name);safe_path(target,name)
        with tempfile.TemporaryDirectory(prefix='PowerfulMoon-stage-',dir=target.parent) as directory:
            stage=Path(directory)
            for index,(name,expected) in enumerate(files.items()):
                dest=safe_path(stage,name);dest.parent.mkdir(parents=True,exist_ok=True)
                with z.open(PREFIX+name) as src,dest.open('wb') as out:
                    digest=hashlib.sha256()
                    while chunk:=src.read(1024*1024):digest.update(chunk);out.write(chunk)
                if digest.hexdigest()!=expected: raise ValueError('Package checksum mismatch: '+name)
                if index%30==0:progress(f'Checking files: {index+1}/{len(files)}')
            # The installer preserves active user settings; copy snapshots for recovery.
            backup=target/'powerfulmoon-backups'/(time.strftime('%Y%m%d-%H%M%S')+'-'+str(time.time_ns())[-6:])
            backup.mkdir(parents=True,exist_ok=False)
            for name in ('powermoon.ini','powermoon.ini.controller.ini'):
                src=target/name
                if src.is_file():shutil.copy2(src,backup/name)
            records=[]
            try:
                for index,(name,digest) in enumerate(files.items()):
                    dest=safe_path(target,name)
                    existed=dest.is_file()
                    if existed and hashlib.sha256(dest.read_bytes()).hexdigest()==digest:continue
                    if existed:
                        saved=safe_path(backup,name);saved.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(dest,saved)
                    records.append((name,existed))
                    dest.parent.mkdir(parents=True,exist_ok=True)
                    shutil.copy2(stage/name,dest)
                    if index%25==0:progress(f'Installing files: {index+1}/{len(files)}')
                # Retire only exact legacy program/launcher bytes known to this release.
                # User-modified or unknown files remain untouched.
                for name,hashes in manifest.get('legacy_programs',{}).items():
                    if name not in ('PowerMoonRecomp.exe','PowerMoon-Switch2Pro-Play.cmd'):raise ValueError('Unexpected legacy program')
                    dest=safe_path(target,name)
                    if dest.is_file() and hashlib.sha256(dest.read_bytes()).hexdigest() in hashes:
                        shutil.copy2(dest,backup/name);records.append((name,True));dest.unlink()
                (backup/'program-files.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
                (target/'PACKAGE.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
            except BaseException:
                for name,existed in reversed(records):
                    dest=safe_path(target,name)
                    if existed:shutil.copy2(safe_path(backup,name),dest)
                    else:dest.unlink(missing_ok=True)
                raise
    progress('Installation complete. Your settings, saves and game files were retained.')
    return target/'PowerfulMoon.exe'
