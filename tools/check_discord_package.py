#!/usr/bin/env python3
"""Reject a release missing the real Discord SDK, bundled fonts or notices."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import subprocess

RUNTIME = {'Linux': 'libdiscord_partner_sdk.so', 'Windows': 'discord_partner_sdk.dll',
           'macOS': 'Frameworks/discord_partner_sdk.framework/Versions/A/discord_partner_sdk'}

def required_files(platform: str) -> tuple[str, ...]:
    return ('bin/' + ('gekkoaot.exe' if platform == 'Windows' else 'gekkoaot'),
            'bin/' + RUNTIME[platform], 'share/licenses/GekkoAOT/discord/License-Notices.txt',
            'share/licenses/GekkoAOT/fonts/GoogleSansFlex-OFL.txt',
            'share/licenses/GekkoAOT/fonts/GoogleSansFlex-TRADEMARKS.md',
            'share/licenses/GekkoAOT/fonts/GoogleSansFlex-SOURCE.txt',
            'share/licenses/GekkoAOT/fonts/Doto-OFL.txt')

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--platform', choices=RUNTIME, required=True)
    parser.add_argument('--application-id', default='1560710093606228101')
    args = parser.parse_args()
    try:
        for relative in required_files(args.platform):
            path = args.package / relative
            if not path.is_file() or not path.stat().st_size:
                raise ValueError(f'Missing packaged dependency: {relative}')
        executable = (args.package / required_files(args.platform)[0]).resolve()
        result = subprocess.run([str(executable), '--check-discord-sdk'], capture_output=True, text=True, timeout=20, check=True)
        data = json.loads(result.stdout)
        if data.get('available') is not True or data.get('applicationId') != args.application_id:
            raise ValueError('Installed launcher has no real SDK or the wrong public Application ID')
        print('Packaged Discord SDK loads successfully; Application ID, fonts and notices verified')
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f'discord-package: {error}\n')

if __name__ == '__main__': raise SystemExit(main())
