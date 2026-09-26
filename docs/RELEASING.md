# Releasing

GekkoAOT releases are **manual-only**.

Normal development commits pushed to `main` — including `fix:`, `feat:`, `perf:`, `docs:`, `refactor:`, `build:`, `ci:` and `chore:` commits — do **not** create a Git tag or GitHub Release.

This is intentional: merging a bug fix must never silently publish a new version.

## Release policy

The release pipeline has one trigger:

```text
Actions -> Release -> Run workflow
```

There is no `push:` trigger on `.github/workflows/release.yml`.

The version stored in `version.txt` is the version that will be published.

For example, if:

```text
version.txt = 0.0.3
```

a manual Release workflow run will target:

```text
v0.0.3
```

provided that release does not already exist.

## Publishing a new version

1. Update `version.txt`.
2. Update `CHANGELOG.md` / release notes for that version.
3. Commit those release-preparation changes to `main`.
4. Open **GitHub Actions -> Release**.
5. Choose **Run workflow** on `main`.
6. Leave `force_publish=false` for a new version.

The workflow then:

1. validates the version and release notes;
2. checks whether `vX.Y.Z` already exists;
3. builds Linux x86_64 and Windows x86_64;
4. stages the portable package trees;
5. creates archives/checksums;
6. creates the Git tag and GitHub Release;
7. uploads the release assets.

`0.x` releases are marked as GitHub prereleases.

## What normal commits do

These are ordinary development commits:

```text
fix(windows): shorten native module cache paths
feat: add another native SDK service
perf: reduce FIFO boundary overhead
docs: update compatibility notes
```

They may be merged directly to `main`.

They do **not**:

- bump `version.txt`;
- create a release commit;
- create a Git tag;
- create a GitHub Release;
- rebuild release assets.

A release happens only after an explicit manual workflow dispatch.

## Rebuilding an existing release

To rebuild the binary assets for the version currently stored in `version.txt`, manually run the **Release** workflow with:

```text
force_publish=true
```

If the matching GitHub Release already exists, the workflow keeps the existing tag/release and replaces its uploaded assets with the newly built packages.

This is useful for rebuilding `v0.0.2` after a packaging-only correction without inventing `v0.0.3`.

## Asset names

Release assets follow:

```text
GekkoAOT-vX.Y.Z-Linux-x86_64.tar.gz
GekkoAOT-vX.Y.Z-Linux-x86_64.tar.gz.sha256
GekkoAOT-vX.Y.Z-Linux-x86_64.AppImage
GekkoAOT-vX.Y.Z-Linux-x86_64.AppImage.sha256
GekkoAOT-vX.Y.Z-Windows-x86_64.zip
GekkoAOT-vX.Y.Z-Windows-x86_64.zip.sha256
```

## Version source of truth

`version.txt` is the canonical version file.

Top-level CMake reads it during configure, so source builds, binaries and the release workflow use the same version.

Do not manually create a new GitHub Release while leaving `version.txt` at another version.

## Release Please configuration

`release-please-config.json` and `.release-please-manifest.json` remain in the repository as versioning/changelog metadata, but the publication workflow no longer invokes or auto-merges Release Please.

They therefore cannot create a tag or release from an ordinary commit.
