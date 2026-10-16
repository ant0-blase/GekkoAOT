# Compatibility

This list records the furthest point each game reached in development testing
for GekkoAOT 0.0.8. Unless noted otherwise, results come from automated runs on
Linux (Intel Core Ultra 7 258V, Arc 140V), in 16:9, with NativeOS and NativeHW
enabled, driving the menus with scripted input for 100–240 seconds. Notes
marked "user report" come from players.

No game has been verified from start to finish. A state only describes the
scenes that were reached. When the latest run stopped earlier than a previous
test, the state keeps the furthest result and the note says so.

## States

| State | Meaning |
| --- | --- |
| Gameplay | An in-game scene is controllable. Later levels can still fail. |
| Cutscene | An in-engine opening or story scene runs; gameplay was not reached in the test. |
| Menu | Title screen, menus or memory-card prompts work. |
| Boots | The game runs and renders, but stops before its menus. |
| Compiles | The game compiles; it does not run correctly yet. |
| Untested | No result for this version. |

## Games

| Game | Disc ID | State | Notes |
| --- | --- | --- | --- |
| Animaniacs: The Great Edgar Hunt | `GANE7U` | Gameplay | First level, from a new game and from a save. Level loading fixed in 0.0.8. |
| Call of Duty: Finest Hour | `GCOE52` | Gameplay | Stalingrad, Volga crossing. |
| Disney/Pixar Cars | `GKJE78` | Gameplay | Race in Radiator Springs. |
| Disney/Pixar Finding Nemo | `GNEE78` | Menu | Underwater title screen. Runs at about 16 fps. |
| Disney's Hide & Sneak | `GHVE08` | Cutscene | Player select and opening cutscene; earlier builds reached the first level. |
| Disney's Magical Mirror Starring Mickey Mouse | `GDME01` | Cutscene | New game started; opening in Mickey's bedroom. |
| Driven | `GDVE6L` | Gameplay | Race at 60 fps with NativeOS (user report). |
| GoldenEye: Rogue Agent | `GOYE69` | Gameplay | First mission with NativeOS (user report). Still unstable. |
| Harry Potter and the Chamber of Secrets | `GHSE69` | Gameplay | The Burrow. |
| Harry Potter and the Prisoner of Azkaban | `GAZE69` | Gameplay | Hogwarts Express and the in-game pause menu. |
| Harry Potter and the Sorcerer's Stone | `GHLE69` | Gameplay | Diagon Alley with Hagrid and Quirrell. Thread-reuse crash fixed in 0.0.8. |
| Killer7 | `GK7E08` | Gameplay | Opening chapter (user report). Aiming fixed in 0.0.8; some textures still render incorrectly. |
| Lemony Snicket's A Series of Unfortunate Events | `GLCE52` | Menu | Main menu and save selection. |
| Luigi's Mansion | `GLME01` | Gameplay | Training room, with the Game Boy Horror HUD in place. |
| Mario Kart: Double Dash!! | `GM4E01` | Gameplay | Time trial on Luigi Circuit. |
| Medal of Honor: Frontline | `GMFE69` | Gameplay | D-Day trenches. Abort Mission freeze fixed in 0.0.8. |
| Medal of Honor: Rising Sun | `GR8E69` | Menu | Load-game menu. A crash when selecting a level was reported in 0.0.7. |
| Metroid Prime | `GM8E01` | Cutscene | In-engine opening above Tallon IV. |
| Nintendo Developer Demo | `DNDD01` | Gameplay | Demo scene and pause menu at full speed. |
| Pikmin | `GPIE01` | Cutscene | File select and the opening crash-landing scene. |
| Rayman 3: Hoodlum Havoc | `GRHE41` | Menu | Opening scene and profile creation. |
| Resident Evil | `GBIE08` | Gameplay | Rooms and inventory; pre-rendered rooms shown in 4:3 since 0.0.8. |
| Resident Evil 4 | `G4BE08` | Boots | Crashed in the opening in 0.0.7 (a REL module stored in an archive). The 0.0.8 extraction fix is not yet verified in game. |
| Shadow the Hedgehog | `GUPE8P` | Gameplay | First stage. Bloom seams in widescreen fixed in 0.0.8. |
| Spyro: A Hero's Tail | `G5SE7D` | Cutscene | Freezes during the first in-engine cutscene. |
| SSX Tricky | `GSTE69` | Untested | Compilation did not finish within the test time limit. |
| Star Fox Adventures | `GSAE01` | Gameplay | Save loaded at Krazoa Palace. |
| Super Mario Strikers | `G4QE01` | Gameplay | Match at 60 fps with NativeOS (user report). |
| Super Mario Sunshine | `GMSE01` | Gameplay | Opening cutscene in the latest run; gameplay in 0.0.7 testing. Water and EFB effects are incomplete. |
| Super Monkey Ball | `GMBE8P` | Gameplay | Stages played through Floor 2; 60 fps at 5 % CPU (user report). |
| Super Monkey Ball 2 | `GM2E8P` | Menu | Crashes after Start is pressed on the title screen. |
| Super Monkey Ball Adventure | `G3LE8P` | Gameplay | Jungle Island at 60 fps with NativeOS (user report). |
| Tales of Symphonia | `GQSEAF` | Gameplay | Save loaded in Iselia. Memory use down to 430 MB in 0.0.8. |
| Terminator 3: The Redemption | `GT6E70` | Gameplay | In-game corridor scene. |
| The Legend of Zelda: The Wind Waker | `GZLE01` | Menu | Title screen with NativeOS disabled (user report). With NativeOS enabled it halts a few seconds after boot. |
| The Legend of Zelda: Twilight Princess | `GZ2E01` | Boots | Compiles since 0.0.8 (757 REL modules). Crashes after the health warning and the Nintendo logo. |
| Tom Clancy's Ghost Recon 2 | `GGYE41` | Menu | Profile menus without an IPL ROM since 0.0.8; in-game reached by a user in 0.0.7. |
| Tony Hawk's Underground | `GTDE52` | Menu | Can freeze after the Neversoft logo while waiting for the GPU. |

## Reporting a problem

Please include:

- the game title, disc ID, region and revision;
- the GekkoAOT version or commit;
- your OS, CPU, GPU and driver version;
- whether NativeOS was enabled, and any options you changed;
- the save and scene needed to reach the problem;
- whether the game was compiled fresh or loaded from the cache.

For crashes, attach the log around the failure. For performance problems,
say whether CPU figures are per core or for the whole machine and include the
frame rate (the performance HUD shows both). For graphics problems, compare
widescreen with the 4:3 aspect ratio.

Do not attach or commit game images, extracted game files, saves, SDK files or
firmware dumps.
