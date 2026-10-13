# Compatibility

GekkoAOT 0.0.7 has been exercised on substantially more titles than the original public compatibility table. The entries below record the furthest documented development state from local captures, test logs and user observations. They do not claim that every level or the final packaged binaries have been exhaustively validated.

## Titles observed running

“Gameplay observed” includes direct user reports/captures. “Menu” and “opening video” are intentionally separate from gameplay. A matching cached AOT module alone is not counted as a launch.

| Title | Disc ID | Furthest observed state | Observation / remaining work |
| --- | --- | --- | --- |
| Harry Potter and the Sorcerer's Stone | GHLE69 | Gameplay / early 3D | Captured gameplay; rendering and complete progression remain under test. |
| Harry Potter and the Chamber of Secrets | GHSE69 | Gameplay / early 3D | Burrow courtyard/tutorial captured with player HUD; scene-dependent performance and progression remain under test. |
| Harry Potter and the Prisoner of Azkaban | GAZE69 | Gameplay / early 3D | Hogwarts Express corridor captured with player HUD; resident-VM and memory-fault paths were exercised. |
| Mario Kart: Double Dash!! | GM4E01 | Gameplay / early 3D | Gameplay captured; audio and race/level transitions need longer validation. |
| Super Mario Sunshine | GMSE01 | Gameplay / early 3D | Gameplay captured; water/EFB effects and rendering remain active correctness targets. |
| Luigi’s Mansion | GLME01 | Gameplay / early 3D | User gameplay captures and earlier UI-scissor tests; flashlight effects, the central rectangle and JAudio still need follow-up. |
| Tales of Symphonia | GQSEAF | Gameplay / save loaded | Launcher-card gameplay and widescreen/map result confirmed by the user; combat/world transitions require continued coverage. |
| Call of Duty: Finest Hour | GCOE52 | Gameplay / early 3D | Stalingrad boat/pier gameplay reviewed; movies, later missions and performance remain under test. |
| GoldenEye: Rogue Agent | GOYE69 | Gameplay / early 3D | Fort Knox helicopter gameplay and camera interaction reviewed; later progression remains under test. |
| Tom Clancy’s Ghost Recon 2 | GGYE41 | Gameplay observed | User in-game capture; Ubisoft/H4M movies and widescreen side visibility remain unresolved targets. |
| Star Fox Adventures | GSAE01 | Gameplay observed | User reports from in-game scenes; effect-dependent slowdowns and widescreen shadows remain targets. |
| Terminator 3: The Redemption | — | Gameplay observed | User in-game reports; movie, lighting/particle and shadow correctness require further validation. |
| Resident Evil | — | Gameplay observed | User reported an in-game crash; scene/level transitions are not certified crash-free. |
| Medal of Honor: Frontline | GMFE69 | Early level execution | Earlier level/movie progress observed; rendering and level/exit transitions remain under test. |
| Disney’s Hide & Sneak | GHVE08 | Gameplay in an earlier LLVM test | Earlier first-level Mickey/HUD run documented; nested REL discovery restores later boot tests. Subsequent code coverage remains under test. |
| Disney’s Magical Mirror Starring Mickey Mouse | GDME01 | Boot / rendering activity | Nested-directory/REL discovery restores runtime/frame activity; complete gameplay is not certified. |
| Disney/Pixar Cars | GKJE78 | Gameplay / racing scene | Captured race with Lightning McQueen, lap counter and HUD. |
| Disney/Pixar Finding Nemo | GNEE78 | Menu | Captured New Game / Load Game menu; low frame rate remains an active issue. |
| Animaniacs: The Great Edgar Hunt | GANE7U | Menu / memory-card prompt | Captured overwrite-save prompt and animated menu scene; gameplay has not been established by that capture. |
| Driven | GDVE6L | Menu / memory-card prompt | Captured load-data/card prompt; clock-polling optimization tested, complete racing gameplay not established by that capture. |
| Tony Hawk’s Underground | GTDE52 | Opening video / card navigation | Earlier opening VID video was documented as visible; later menu/level transition black screens remain under test. |
| Medal of Honor: Rising Sun | — | Level-selection menu observed | User reported a crash when selecting a level; successful full level execution needs renewed proof. |
| Nintendo Developer Demo | DNDD01 | Working tested sample | Current demo scene captured and exercised; wider sample/pacing coverage remains ongoing. |

## Additional corpus targets

The local corpus and compiler/service recognition tests also cover or prepare additional targets, including The Wind Waker, Twilight Princess, Pikmin, Spyro: A Hero’s Tail, Killer7, Metroid Prime, Rayman 3, Resident Evil 4, SSX Tricky, Shadow the Hedgehog, Super Mario Strikers and the Super Monkey Ball titles. Their presence in the corpus or a successful AOT build is not a verified boot/gameplay result. Add a stronger status only when a reproducible run establishes it.

## Status definitions

| State | Meaning |
| --- | --- |
| Untested | No reproducible runtime result for the stated configuration |
| Compiles | Module generation succeeds; boot is unconfirmed |
| Boot / rendering activity | The executable runs and runtime/rendering activity is observed; the final scene can be unclassified |
| Menu / video | A recognizable menu, prompt or opening movie is reached |
| Gameplay / early 3D | An in-game scene is reached; correctness and progression can remain incomplete |
| Working sample | The stated sample/path runs without a known blocker |
| Playable | Meaningful gameplay and progression validated without a major blocker |

No retail title is certified completely playable by this release preparation. Known graphics, audio, movie, widescreen and transition issues remain documented alongside the successful starts.

## Reporting a problem

Include the game ID, region/revision, GekkoAOT commit/version, host CPU/GPU/driver, LLVM version and backend. State the native options, graphics settings, cache-hit/cold-build status and save/scene needed to reproduce the first failure.

For crashes, include a backtrace and concise log excerpts around the failure. For performance problems, distinguish whole-machine CPU use from one-core utilization and record the game's frame rate. For graphics problems, compare 4:3 and widescreen where relevant.

Keep raw profiling captures and full benchmark reports in local state or issue attachments. Do not add game images, commercial executables, saves, SDK files or firmware dumps to the repository.
