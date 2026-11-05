# Native Real-ESRGAN compact weights

Official [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) model
[`realesr-general-x4v3.pth`, v0.2.5.0](https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-x4v3.pth).
Architecture: [SRVGGNetCompact](https://github.com/xinntao/Real-ESRGAN/blob/v0.3.0/realesrgan/archs/srvgg_arch.py),
34 convolutions, 64 hidden channels, channel-wise PReLU, RGB 4x pixel shuffle
and nearest-neighbor residual; 1,213,296 learned parameters.

`tools/import_srvgg_weights.py` verifies the pinned source SHA-256, restricts
pickle globals and tensor layouts, and writes native little-endian HWIO
weights. It uses Python's standard library only as an **offline importer**.
The runtime uses native C++/WGSL and has no Python/PyTorch/ONNX dependency.

`manifest.json` records original/converted SHA-256 checksums and the exact
converted length. Runtime additionally checks the native FNV-1a fingerprint,
layer dimensions and finite coefficients. Redistribution follows the included
BSD-3-Clause `LICENSE`, copyright Xintao Wang.

The general photographic model can smooth authored noise and invent details.
It does not recover a lost high-resolution original. Limited-palette graphics
use the separately labeled non-AI edge-rule reconstruction.
