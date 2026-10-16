#!/usr/bin/env python3
"""Regression checks for private SDK staging and release deployment gates."""
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / filename)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module
sdk = load('prepare_discord_sdk', 'prepare_discord_sdk.py')
package = load('check_discord_package', 'check_discord_package.py')

class DiscordPackagingTests(unittest.TestCase):
    def archive(self, root, extra=None):
        archive = root / 'sdk.zip'
        with zipfile.ZipFile(archive, 'w') as output:
            for name in sdk.COMMON_FILES + sdk.PLATFORM_FILES['Linux'] + sdk.PLATFORM_FILES['Windows']:
                output.writestr('discord_social_sdk/' + name, b'sdk fixture')
            if extra: output.writestr(*extra)
        pin = {'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(), 'version': 'test'}
        return archive,pin

    def test_verified_platform_subset_and_existing_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); archive,pin=self.archive(root); output=root/'sdk'
            sdk.stage(archive,output,pin,'Linux')
            self.assertTrue((output/sdk.PLATFORM_FILES['Linux'][0]).is_file())
            self.assertFalse((output/sdk.PLATFORM_FILES['Windows'][0]).exists())
            with self.assertRaisesRegex(ValueError,'already exists'): sdk.stage(archive,output,pin,'Linux')

    def test_bad_hash_cannot_extract(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); archive,pin=self.archive(root); pin['sha256']='0'*64
            with self.assertRaisesRegex(ValueError,'checksum'): sdk.stage(archive,root/'sdk',pin,'Linux')
            self.assertFalse((root/'sdk').exists())

    def test_unsafe_path_and_symlink_are_rejected(self):
        for entry in [('../outside',b'bad'), ('discord_social_sdk/include/../../outside',b'bad')]:
            with tempfile.TemporaryDirectory() as directory:
                root=Path(directory); archive,pin=self.archive(root,entry)
                with self.assertRaisesRegex(ValueError,'Unsafe'): sdk.stage(archive,root/'sdk',pin,'Linux')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); archive,pin=self.archive(root)
            with zipfile.ZipFile(archive,'a') as output:
                info=zipfile.ZipInfo('discord_social_sdk/include/escape');info.external_attr=(stat.S_IFLNK|0o777)<<16
                output.writestr(info,'../../../outside')
            pin['sha256']=hashlib.sha256(archive.read_bytes()).hexdigest()
            with self.assertRaisesRegex(ValueError,'symlink'): sdk.stage(archive,root/'sdk',pin,'Linux')

    def test_missing_runtime_and_package_gates(self):
        self.assertIn('bin/discord_partner_sdk.dll', package.required_files('Windows'))
        self.assertIn('bin/libdiscord_partner_sdk.so', package.required_files('Linux'))
        self.assertIn('share/licenses/GekkoAOT/fonts/GoogleSansFlex-OFL.txt', package.required_files('Linux'))
        pin=json.loads((ROOT/'cmake/discord-sdk-pin.json').read_text())
        self.assertEqual(len(pin['revision']),40); self.assertEqual(len(pin['sha256']),64)
        for name in ['ci.yml','release.yml']:
            text=(ROOT/'.github/workflows'/name).read_text()
            self.assertIn('uses: ./.github/actions/discord-sdk',text)
            self.assertIn('tools/check_discord_package.py',text)
        self.assertIn('-DGEKKOAOT_DISCORD_SDK=ON',(ROOT/'.github/workflows/release.yml').read_text())

if __name__ == '__main__': unittest.main()
