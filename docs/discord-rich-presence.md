# Discord Rich Presence

GekkoAOT is a portable compatibility layer. Its launcher uses one activity model
and the official Discord **Social SDK 1.10+** backend on Windows, Linux, macOS and
Android. It does not use a Linux-only socket implementation or a user token.

The public application ID is `1560710093606228101`. Packaged builds embed it by
default. Sharing and an ID override are available in **Settings → Discord Rich
Presence**, in Desktop and TV Mode.

The presence shows the running game's title and session start time, with a
paused state; otherwise it shows compilation or library browsing. A background
compilation never replaces a running game's activity. Fullscreen/presentation
changes preserve the game timer. File paths, ISO filenames, command arguments,
logs and user directories are not included. Updates are coalesced; failures use
backoff. Periodic refresh recovers after Discord restarts. Disabling/closing
clears activity.

## Build and deploy

Download the standalone C++ SDK for the **target OS and architecture** from your
application's **Discord Social SDK → Downloads** in the
[Developer Portal](https://discord.com/developers/applications). Discord supplies
it under its own SDK terms. The repository does not vendor or implicitly
download it.

```sh
cmake -S . -B build \
  -DGEKKOAOT_DISCORD_SDK=ON \
  -DGEKKOAOT_DISCORD_SDK_ROOT=/absolute/path/to/discord_social_sdk
cmake --build build --target gekkoaot
```

`GEKKOAOT_DISCORD_SDK=AUTO` (default) enables the backend when a SDK is supplied;
`ON` fails configuration if it is missing; `OFF` omits it. SDK-free builds remain
functional and explicitly report the feature unavailable. For nonstandard SDK
layouts, set `GEKKOAOT_DISCORD_INCLUDE_DIR`, `GEKKOAOT_DISCORD_LIBRARY` and, on
Windows, `GEKKOAOT_DISCORD_RUNTIME`. Use separate build directories per target.

Desktop deployment copies the runtime beside the executable, or the macOS
framework into `Frameworks`, with relative search paths. Preserve Discord's SDK
notices when distributing it. Sign/notarize macOS applications and frameworks
as instructed by Discord. The CMake cache variable
`GEKKOAOT_DISCORD_APPLICATION_ID` overrides the packaged ID; the environment
variable of the same name overrides the runtime default. A saved user ID takes
priority over either default. None of these values is a secret.

## Android

The Android backend uses the official **1.10+ AAR/Prefab** target
`discord_partner_sdk::discord_partner_sdk`. It initializes
`DiscordSocialSdkInit.setEngineActivity` with Qt's current Android Activity.
An Android host package must include `discord_partner_sdk.aar` in its Gradle
dependencies, enable Prefab and expose the CMake package. Copying its native
`.so` alone is insufficient: include the SDK's Java classes/manifest too.
This presence-only integration does not require microphone access.

The repository has no complete Android application/runtime package yet. This
backend prepares that port; it is **not a tested Android release**. See
[launcher platform validation](launcher-platforms.md).

Desktop Discord must be running and signed in. On Android the Discord app must
be installed and signed in. No OAuth login or bot token is used. Systems
without direct-presence support remain unavailable; iOS would need a separate
account-linking flow.

## Validation

```sh
cmake --build build --target gekkoaot-test-discord-presence
ctest --test-dir build -R '^launcher.discord-presence$' --output-on-failure
```

Tests inject a transport and monotonic clock to check activity priority,
pause/session continuity, Unicode limits, IDs, throttling, retries/recovery and
late callbacks after disabling/reconfiguration. Tests do not publish activity.
Actual publication also requires building with the real SDK and a signed-in
Discord client.

Local validation on 16 October 2026: compiled against the supplied official
1.10.19337 SDK on Linux; the manual `--live` smoke test received a successful
presence update from the signed-in Discord desktop client and cleared it on
shutdown. Windows/macOS/Android execution remains unvalidated in this session.
For release builds, require `GEKKOAOT_DISCORD_SDK=ON` so missing SDK dependencies
cannot silently produce a package without Rich Presence. End users install the
finished package and do not need a developer application or SDK download.

Official references: [direct presence](https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence#rich-presence-without-authentication),
[platform support](https://docs.discord.com/developers/discord-social-sdk/core-concepts/platform-compatibility),
[Android setup](https://docs.discord.com/developers/discord-social-sdk/development-guides/account-linking-on-mobile),
[release notes](https://discord.com/developers/docs/social-sdk/release_notes.html).

## GitHub CI and releases

Trusted CI runs and all releases use the private `ant0-blase/GekkoAOT-build-deps`
repository through a repository-specific **read-only deploy key**, stored as the
`GEKKOAOT_DISCORD_SDK_DEPLOY_KEY` Actions secret on GekkoAOT. The composite action
checks out the exact commit in `cmake/discord-sdk-pin.json` and verifies the SDK
archive's SHA-256 before extracting only the target platform's files. The
original upstream archive checksum is also recorded in the pin. Discord's
authenticated download URL expires; that URL is not a CI input.

The private SDK is not placed in a public Actions cache or uploaded as a raw
SDK artifact. Contributors' fork PRs and Dependabot run without the SDK and
without access to its key. Their activity model, UI and other regression tests
still run. Trusted builds require the real SDK; a missing key/dependency fails
the build instead of silently disabling presence.

Both workflows verify the installed runtime, embedded Application ID and font/
SDK notices with `tools/check_discord_package.py`. Its launcher probe
`--check-discord-sdk` loads the actual library without creating a Discord client
or publishing activity. AppImage packaging also includes the SDK dependency and
runs this probe from the generated AppImage. Current full application build and
release jobs remain Linux x86_64 and Windows x86_64; this does not create a
complete macOS/Android application pipeline.
