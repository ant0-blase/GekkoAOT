# Native VFS and DVD services

GekkoAOT owns its disc and filesystem path. `gekkoaot-native-disc` uses encounter/nod to read supported GameCube images and containers; the controller caches system metadata, executable images and extracted files below `<state>/cache/discs/`.

Extraction preserves the FST directory hierarchy. This matters for games with executable RELs nested inside directories or resource containers. Older incompatible extraction caches are migrated using an explicit extraction-policy key.

NativeVFS normalizes paths and performs FST lookup against host files or the mounted nod partition. Recognized DVD/FST SDK leaves can use direct native lookup/read services. Unknown SDK variants and custom DI protocols keep their existing compiled or register-facing paths.

DI reads retain asynchronous DMA visibility, completion interrupts, cancellation and transfer ordering. Host I/O completion alone is not permission to publish a guest transfer before its required boundary.

The controller keeps mutable caches outside installed resources. Use `GEKKOAOT_STATE_DIR` to select the state root; use launcher user directories for memory cards and input configuration. Extracted commercial assets and saves are not distributed with the project.
