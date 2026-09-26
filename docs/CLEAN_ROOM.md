# Clean-room and repository content policy

GekkoAOT is intended to remain distributable without bundling copyrighted game or proprietary SDK assets.

## Do not commit

- GameCube ISO/GCM/RVZ/WIA/WBFS/GCZ images;
- extracted game assets or executable binaries from commercial titles;
- Nintendo SDK source code, object files or libraries;
- Nintendo PC emulator/SDK binaries;
- DSP ROM/coef dumps or other firmware unless redistribution rights are explicit;
- proprietary symbol/map files whose redistribution is not permitted;
- near-verbatim generated source reconstructed from proprietary source material.

## Allowed project material

- independently written compatibility/runtime code;
- public interface/behavior documentation;
- patches authored for open-source upstream projects under their licenses;
- test descriptions and compatibility results;
- structural metadata derived from executable analysis when it is non-reconstructive and contains no original binary payload;
- hashes, normalized instruction classes, control-flow/data-flow features, call relationships and similar recognition metadata.

## SDK recognition corpus

`runtime/sdk/sdk_groundtruth_v1.h` is intended to contain **normalized structural fingerprints**, not source binaries or raw executable payloads.
Future generators/reviews should preserve that property: absolute addresses, copyrighted assets and reconstructive binary blobs should not be committed merely to improve matching quality.

If a new recognition technique requires retaining large exact instruction sequences from a proprietary binary, redesign the representation before publishing it.

## User-provided material

Users are responsible for supplying their own legally obtained game dumps and any optional firmware/data required by a runtime path. GekkoAOT should reference user files from local state; it should not upload, redistribute or silently copy them into the repository.
