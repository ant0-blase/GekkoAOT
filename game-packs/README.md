# GekkoAOT game packs

Game packs are declarative JSON metadata keyed by the six-character GameCube disc ID.
They are not compatibility claims by themselves.

The GUI loads `game-packs/v1/<DISC_ID>.json` after a game is selected.

A pack may contain:

- `compatibility`: informational state for the current snapshot;
- `environment`: runtime variables only when a generic runtime feature is actually implemented;
- `runtime_features`: features currently wired into GekkoAOT;
- `planned_runtime_features`: ideas/imported feature parity that is **not yet active** in the generic runtime;
- `ui.options`: controls only for runtime variables that have a real consumer.

Do not expose a GUI toggle merely because an older standalone recomp project had an equivalent patch. A public option should have a current GekkoAOT runtime consumer and a tested behavior.

Upstream source patching is not part of the game-pack format. GekkoAOT's DolRecomp/Aurora integration patches live under `patches/` and are applied by the native controller during the build stage.
