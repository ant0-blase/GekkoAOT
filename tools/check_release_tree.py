#!/usr/bin/env python3
"""Validate publishable sources or a staged GekkoAOT package without modifying Git."""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

LOCAL_DIRECTORIES = {
    '.gekkoaot', '.git', '.pytest_cache', '__pycache__', 'debug', 'build',
    'bin', 'stage', 'package', 'dist', '.deps', 'diagnostics', 'profiling',
    'profiles', 'bench-results',
}
BACKUP_RE = re.compile(r'(?:\.(?:bak|orig|rej)$|\.v\d+[-_]backup|~$)', re.I)
PROFILE_RE = re.compile(
    r'(?:\.prof(?:raw|data)$|(?:^perf|\.perf)\.(?:data|txt)(?:\.|$)|'
    r'^gekkoaot-.*(?:hotspots\.txt|threads\.(?:txt|data)(?:\.|$)))', re.I
)
VERSIONED_DOC_RE = re.compile(r'(?:^|[_-])v\d+', re.I)
LINK_RE = re.compile(r'!?\[[^\]\n]*\]\(([^\s)]+)(?:\s+"[^"\n]*")?\)')


def forbidden_reason(path: Path, *, package: bool = False) -> str | None:
    """A package may contain binaries/toolchains, but never local measurements."""
    parts = path.parts
    if not parts:
        return None
    if not package and (parts[0] in LOCAL_DIRECTORIES or
                        (len(parts) > 1 and parts[0].startswith(('build-', 'cmake-build-')))):
        return 'local state/build directory'
    if any(p in {'.gekkoaot', '.git', '.pytest_cache', '__pycache__',
                 'diagnostics', 'profiling', 'bench-results'} for p in parts):
        return 'local diagnostics/cache directory'
    name = path.name
    if BACKUP_RE.search(name):
        return 'code backup or patch reject'
    if PROFILE_RE.search(name):
        return 'profiling capture/report'
    if path.suffix.lower() in {'.log', '.pyc', '.iso', '.gcm', '.rvz', '.wia',
                               '.wbfs', '.gcz', '.raw', '.gci'}:
        return 'generated log, cache, game image or memory card'
    if 'docs' in parts and path.suffix.lower() in {'.md', '.csv'}:
        if VERSIONED_DOC_RE.search(name) or path.suffix.lower() == '.csv':
            return 'obsolete versioned audit/report'
    return None


def source_files(root: Path) -> list[Path]:
    result = subprocess.run(
        ['git', '-C', str(root), 'ls-files', '--cached', '--others',
         '--exclude-standard', '-z'], check=True, capture_output=True,
    )
    # Deleted tracked files are intentional working-tree deletions, not content
    # that would be published after committing the release preparation.
    return sorted({Path(s) for s in result.stdout.decode('utf-8').split('\0')
                   if s and (root / s).is_file()})


def document_errors(root: Path, paths: list[Path]) -> list[str]:
    errors: list[str] = []
    for relative in paths:
        if relative.suffix.lower() != '.md':
            continue
        document = root / relative
        for target in LINK_RE.findall(document.read_text(encoding='utf-8')):
            target = target.strip('<>')
            parsed = urlsplit(target)
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            destination = document.parent / unquote(parsed.path)
            if not destination.exists():
                errors.append(f'{relative}: missing local link {target}')
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--package', type=Path, help='validate a staged package instead of source')
    args = parser.parse_args()
    root = (args.package or args.root).resolve()
    if not root.is_dir():
        parser.error(f'directory does not exist: {root}')
    try:
        paths = ([p.relative_to(root) for p in root.rglob('*') if p.is_file()]
                 if args.package else source_files(root))
        errors = [f'{p}: {reason}' for p in paths
                  if (reason := forbidden_reason(p, package=bool(args.package)))]
        if not args.package:
            errors.extend(document_errors(root, paths))
            version = (root / 'version.txt').read_text(encoding='utf-8').strip()
            if not re.fullmatch(r'\d+\.\d+\.\d+(?:[-.][0-9A-Za-z.-]+)?', version):
                errors.append('version.txt: invalid release version')
        if errors:
            print('\n'.join(errors), file=sys.stderr)
            return 1
        print(f'release-tree: checked {len(paths)} files; no local artifacts or broken document links')
        return 0
    except (OSError, UnicodeError, subprocess.CalledProcessError) as error:
        print(f'release-tree: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
