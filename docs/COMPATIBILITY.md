# Compatibility

GekkoAOT is pre-alpha. This table records the **furthest state observed** during current development testing; it is not a promise that every machine, driver or dump reaches the same point.

Snapshot date: **2026-09-26**.

## Status definitions

| State | Meaning |
| --- | --- |
| Untested | No current result recorded. |
| Compiles | AOT/module build succeeds, but runtime boot has not been established. |
| Boot | Runtime starts the title and reaches recognizable title execution. |
| Menu | Title reaches a usable/recognizable menu. |
| Gameplay / early 3D | Title reaches an in-game 3D scene, but correctness may be poor. |
| Working | The currently tested sample/path completes without a known blocking issue. This does not imply exhaustive compatibility. |
| Playable | Reserved for a title that can be played meaningfully with no major known blocker. No retail title is claimed as Playable in v0.0.2. |

## v0.0.2 matrix

| Title | Region / ID | State | Observed result | Main blocker |
| --- | --- | --- | --- | --- |
| Harry Potter and the Sorcerer's Stone | USA / `GHLE69` | Gameplay / early 3D | Reaches gameplay and renders 3D. | Visible graphics/rendering glitches remain. |
| Harry Potter and the Chamber of Secrets | USA / `GHSE69` | Gameplay / early 3D | Reaches gameplay / early 3D. | Visible graphics/rendering glitches remain. |
| Nintendo Developer Demo | developer sample | Working | Current tested developer-demo path runs correctly. | No known blocker in the tested path. |
| Super Mario Sunshine | USA / `GMSE01` | Gameplay / early 3D | Reaches gameplay / early 3D. | Graphics/runtime correctness is still incomplete. |
| Mario Kart: Double Dash!! | USA / `GM4E01` | Gameplay / early 3D | Reaches gameplay / early 3D. | Some visible graphics/runtime glitches remain. |
| Medal of Honor: Frontline | USA / `GMFE69` | Boot / early execution | Boots and continues into early execution. | Multiple rendering/runtime compatibility glitches. |

## How to report a result

A useful compatibility report should include:

- exact six-character disc ID and region;
- GekkoAOT version/commit;
- GPU and driver;
- compiler/LLVM version;
- whether the run was cold-build or cache-hit;
- the last `GEKKOAOT_*` status lines before a hang/crash;
- a GDB backtrace for SIGSEGV/SIGABRT where possible;
- whether the same title reaches a different state with `GEKKOAOT_GUI_MATCH_GDB=1`.

Do not submit copyrighted game files, SDK files, firmware dumps or other proprietary assets with reports.
