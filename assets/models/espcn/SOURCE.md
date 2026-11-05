# Native ESPCN weights

The small 2× and 4× pretrained ESPCN networks are from
[fannymonori/TF-ESPCN](https://github.com/fannymonori/TF-ESPCN/tree/5c628eca82028161a53e1265cc3a5b571ab8625f),
revision `5c628eca82028161a53e1265cc3a5b571ab8625f`, under the accompanying Apache-2.0 license.
The upstream project describes training on DIV2K. These are general image models,
not models trained to reconstruct a particular game's missing artwork.

`manifest.json` records the upstream frozen-graph hashes and parameter counts.
`tools/import_espcn_weights.py` extracts only the six float32 constant tensors
into `runtime/gx/espcn_weights.h`. It validates pinned checksums, shapes and
finite coefficients. The importer uses Python's standard library only.
GekkoAOT inference uses its own native C++ convolutions, ReLU, pixel shuffle and
tanh; TensorFlow, PyTorch, ONNX and Python are not runtime dependencies.

Architecture: 5×5 convolution (1→64), ReLU; 3×3 convolution (64→32), ReLU;
3×3 convolution (32→4 or 16); pixel shuffle (2× or 4×); tanh. Input and output
are normalized luma; original chroma is spatially reconstructed separately.
The original game textures and their authored mip levels are retained.

Algorithm reference: Shi et al., [Efficient Sub-Pixel Convolutional Neural
Network](https://arxiv.org/abs/1609.05158), CVPR 2016.

The current integration uses 128-texel tiles with four source texels of context,
checks tile-edge equivalence against whole-image inference, and limits luma
residuals to the common RGB gamut. Cache identity includes the model fingerprint
and the native reconstruction algorithm revision (`espcn-native-v2`).
