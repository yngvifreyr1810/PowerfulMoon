import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/windows'))
import installer_core as installer

class FullInstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.target=self.root/'game';self.target.mkdir()
        self.payload={'PowerfulMoon.exe':b'new host','PowerfulMoon.dll.a':b'new import library','SDL3.dll':b'redistributable'}
    def package(self,corrupt=False,extra=None,legacy=None):
        data=dict(self.payload)
        if extra:data.update(extra)
        manifest={'files':{n:hashlib.sha256(b).hexdigest() for n,b in data.items()},'legacy_programs':legacy or {}}
        path=self.root/'package.zip'
        with zipfile.ZipFile(path,'w') as z:
            z.writestr('PowerfulMoon/PACKAGE.json',json.dumps(manifest))
            for n,b in data.items():z.writestr('PowerfulMoon/'+n,b+b'corrupt' if corrupt and n=='PowerfulMoon.exe' else b)
        return path
    def test_upgrade_keeps_user_data_and_retires_only_known_legacy_host(self):
        for name in ('powermoon.ini','powermoon.ini.controller.ini','save/slot.bin','extracted/user-owned.bin'):
            p=self.target/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'user data '+name.encode())
        before={p.relative_to(self.target):p.read_bytes() for p in self.target.rglob('*') if p.is_file()}
        (self.target/'PowerMoonRecomp.exe').write_bytes(b'known legacy host')
        legacy={'PowerMoonRecomp.exe':[hashlib.sha256(b'known legacy host').hexdigest()]}
        archive=self.package(legacy=legacy)
        installer.install(archive,self.target)
        self.assertFalse((self.target/'PowerMoonRecomp.exe').exists())
        for name,data in before.items():self.assertEqual((self.target/name).read_bytes(),data)
        installer.install(archive,self.target)
        for name,data in before.items():self.assertEqual((self.target/name).read_bytes(),data)
        self.assertEqual((self.target/'PowerfulMoon.exe').read_bytes(),b'new host')
    def test_corruption_is_rejected_before_modifying_program(self):
        (self.target/'PowerfulMoon.exe').write_bytes(b'old host')
        with self.assertRaisesRegex(ValueError,'checksum'):installer.install(self.package(corrupt=True),self.target)
        self.assertEqual((self.target/'PowerfulMoon.exe').read_bytes(),b'old host')
    def test_path_escape_and_user_payload_are_rejected(self):
        for name in ('../escape.exe','save/slot.bin','odyssey_game.dll','config.json','fonts/nintendo_udsg.bfttf','input/main.nso'):
            with self.subTest(name=name),self.assertRaises(ValueError):installer.install(self.package(extra={name:b'bad'}),self.target)
        self.assertFalse((self.root/'escape.exe').exists())
    def test_copy_failure_rolls_back_replaced_files(self):
        (self.target/'PowerfulMoon.exe').write_bytes(b'old host')
        original=installer.shutil.copy2
        def fail(source,dest,*args,**kwargs):
            if Path(source).name=='SDL3.dll' and Path(source).parent.name.startswith('PowerfulMoon-stage-'):raise OSError('disk failure')
            return original(source,dest,*args,**kwargs)
        with patch.object(installer.shutil,'copy2',side_effect=fail),self.assertRaisesRegex(OSError,'disk failure'):
            installer.install(self.package(),self.target)
        self.assertEqual((self.target/'PowerfulMoon.exe').read_bytes(),b'old host')
        self.assertFalse((self.target/'PowerfulMoon.dll.a').exists())
if __name__=='__main__':unittest.main()
