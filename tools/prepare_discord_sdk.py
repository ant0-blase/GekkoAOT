#!/usr/bin/env python3
"""Verify and stage a pinned, privately supplied Discord SDK for one target OS."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import tempfile
import zipfile

PLATFORM_FILES = {
    'Linux': ('lib/release/libdiscord_partner_sdk.so',),
    'Windows': ('lib/release/discord_partner_sdk.lib', 'bin/release/discord_partner_sdk.dll'),
    'macOS': ('lib/release/discord_partner_sdk.framework/Versions/A/discord_partner_sdk',),
    'Android': ('lib/release/discord_partner_sdk.aar',),
}
COMMON_FILES = ('include/discordpp.h', 'include/cdiscord.h', 'License-Notices.txt')

def stage(archive: Path, destination: Path, pin: dict, platform: str) -> None:
    with archive.open('rb') as handle:
        digest = hashlib.file_digest(handle, 'sha256').hexdigest()
    if digest != pin['sha256']:
        raise ValueError('Discord SDK checksum mismatch; nothing was extracted')
    if destination.exists():
        raise ValueError('SDK destination already exists; use a fresh staging directory')
    destination.parent.mkdir(parents=True, exist_ok=True)
    required = COMMON_FILES + PLATFORM_FILES[platform]
    with tempfile.TemporaryDirectory(dir=destination.parent, prefix='.discord-sdk-') as temporary:
        root = Path(temporary).resolve() / 'sdk'; root.mkdir()
        links = []
        with zipfile.ZipFile(archive) as source:
            for item in source.infolist():
                path = PurePosixPath(item.filename)
                if (path.is_absolute() or '..' in path.parts or '\\' in item.filename or
                        ':' in item.filename or not path.parts or path.parts[0] != 'discord_social_sdk'):
                    raise ValueError('Unsafe SDK archive entry')
                relative = PurePosixPath(*path.parts[1:])
                name = str(relative)
                selected = name in required or name.startswith('include/') or (
                    platform == 'macOS' and name.startswith('lib/release/discord_partner_sdk.framework/'))
                if not selected: continue
                target = root.joinpath(*relative.parts)
                mode = item.external_attr >> 16
                if item.is_dir(): target.mkdir(parents=True, exist_ok=True); continue
                data = source.read(item)
                target.parent.mkdir(parents=True, exist_ok=True)
                if stat.S_ISLNK(mode):
                    link = data.decode('utf-8')
                    if Path(link).is_absolute() or not (target.parent / link).resolve().is_relative_to(root):
                        raise ValueError('Unsafe SDK symlink')
                    links.append((target, link))
                else:
                    target.write_bytes(data)
                    target.chmod(mode & 0o777 or 0o644)
        for target, link in links: target.symlink_to(link)
        for name in required:
            if not (root / name).is_file(): raise ValueError(f'Missing SDK input: {name}')
        (root / 'gekkoaot-sdk-pin.json').write_text(json.dumps(pin, indent=2) + '\n', encoding='utf-8')
        os.replace(root, destination)

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--destination', required=True, type=Path)
    parser.add_argument('--pin', type=Path, default=Path(__file__).resolve().parents[1] / 'cmake/discord-sdk-pin.json')
    parser.add_argument('--platform', required=True, choices=PLATFORM_FILES)
    args = parser.parse_args()
    try:
        pin = json.loads(args.pin.read_text(encoding='utf-8'))
        stage(args.archive, args.destination, pin, args.platform)
        print(f'Discord Social SDK {pin["version"]}: checksum verified, {args.platform} files staged')
        return 0
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f'discord-sdk: {error}\n')

if __name__ == '__main__': raise SystemExit(main())
