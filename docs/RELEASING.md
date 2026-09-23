# Releasing

GekkoAOT uses two complementary pieces of automation:

1. **Release Please** prepares future version/changelog pull requests from Conventional Commits.
2. **Release workflow** builds and publishes the version currently stored in `version.txt` whenever the matching `vX.Y.Z` tag does not yet exist.

This makes the initial `v0.0.1` publish automatic while keeping later version bumps reviewable.

## Initial v0.0.1

After this release-preparation change reaches `main`, `.github/workflows/release.yml` sees `version.txt = 0.0.1`.
If `v0.0.1` does not exist, it:

1. validates `CHANGELOG.md`;
2. builds Linux x86_64 and Windows x86_64;
3. stages the CMake install tree;
4. creates OS/architecture-named archives and SHA-256 files;
5. creates GitHub tag/release `v0.0.1` at that commit;
6. attaches both platform archives/checksums.

`0.x` releases are marked as GitHub prereleases.

## Future versions

Use Conventional Commits on normal development work, for example:

```text
feat: add REL loader coverage for another relocation class
fix: preserve VI timing across native scheduler wakeups
perf: reduce redundant native FIFO boundary work
docs: document GHLE69 compatibility state
```

Release Please groups these changes and opens/updates a release PR. The manifest is configured with `include-component-in-tag: false`, so the expected release boundary is exactly `vX.Y.Z` rather than `GekkoAOT-vX.Y.Z`. For the pre-1.0 policy in `release-please-config.json`:

- `fix`, `feat`, `perf`, etc. normally advance the patch version;
- a breaking change advances the minor version while the project is below `1.0.0`.

When the release PR is merged, its updated `version.txt` has no corresponding tag yet. The release workflow therefore builds and publishes that exact version. After publication, Release Please sees the new release boundary and starts collecting changes for the next one.

## GitHub repository setting

Release Please needs permission to create release pull requests. In the repository's Actions settings, allow GitHub Actions to create pull requests, or provide an appropriately scoped token and configure the workflow to use it.

## Asset names

Release assets follow:

```text
GekkoAOT-vX.Y.Z-Linux-x86_64.tar.gz
GekkoAOT-vX.Y.Z-Linux-x86_64.tar.gz.sha256
GekkoAOT-vX.Y.Z-Windows-x86_64.zip
GekkoAOT-vX.Y.Z-Windows-x86_64.zip.sha256
```

## Version source of truth

`version.txt` is the canonical version file. Top-level CMake reads it during configure, so source builds, binaries and release automation cannot silently drift to different version numbers.

Do not manually create a new GitHub release while leaving `version.txt` unchanged. Either merge the Release Please PR or intentionally update `version.txt` + `CHANGELOG.md` together.

## Rebuilding an existing release

The workflow can be launched manually with `force_publish=true` to rebuild and replace the binary/checksum assets for the current `version.txt`. Manual release runs are intentionally restricted to the `main` branch. The existing tag and release notes are preserved.
