# Native VFS and DVD services

GekkoAOT reads discs without an emulator frontend. `gekkoaot-native-disc` uses
[encounter/nod](https://github.com/encounter/nod) to open ISO, GCM, RVZ, WIA,
WBFS and GCZ images. The controller caches system files, executables and
metadata under `<state>/cache/discs/<game>/`.

## Extraction

- The FST directory tree is preserved, so REL modules in nested directories
  are found.
- REL modules stored inside archive files (for example Resident Evil 4's
  `.drs` archives) are detected and staged for compilation.
- Caches written by older versions are refreshed once, using an
  extraction-policy key.

## File access

NativeVFS normalizes paths and resolves them through the FST, against host
files or the mounted nod partition. Recognized DVD and FST SDK functions use
native lookup and read services; unknown SDK variants and custom DI protocols
keep their compiled or register-level paths.

DI reads keep the console's ordering: DMA visibility, completion interrupts,
cancellation and chained transfers happen at the same points as on hardware.
A read that finishes early on the host is not shown to the game before that
point. Optional `GEKKOAOT_VFS_GC_TIMING=1` adds GameCube disc timing.

## Storage

Mutable caches live outside the installed resources. `GEKKOAOT_STATE_DIR`
selects the state root; memory cards and input configuration live in the
launcher's user directories. Extracted game files and saves are never
distributed with the project.
