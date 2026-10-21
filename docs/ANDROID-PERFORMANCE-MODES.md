# Handheld / docked and 30 / 60 FPS settings (experimental)

This feature applies to the **Android APK**, not the Nintendo Switch executable or the `game.s2a` archive. Game files and encryption keys remain private.

## Using the controls

After installing an updated APK, open Switch2Android. When the resource bundle is already imported, the launcher now shows **运行设置：掌机／底座 · 30／60 FPS** and **开始游戏**. Choose settings *before* starting the game if you want the game to read docked mode during initialization.

During gameplay, tap **⚙** at the top-right to change the same settings:

- **掌机模式** (default): Switch `nn::oe::GetOperationMode() == 0`.
- **底座模式** (experimental): `nn::oe::GetOperationMode() == 1`; `GetPerformanceMode()` likewise reports 1. The runtime also delivers opt-in operation/performance change notifications (30 and 31) after in-game switching.
- **30 FPS** (default) or **60 FPS** (experimental): the existing, validated OCTOPATH TRAVELER II v1.1.1 frame-rate policy is applied at runtime. A 60 FPS request does not imply a 60 FPS result. Android battery saver/game battery mode and the compatibility `force-30-fps` flag retain their 30 FPS override.
- **游戏中持续显示 FPS**: toggles the existing GPU presentation HUD, which reports *observed* frame rate rather than simply the selected target.

Selections persist in Android app-private preferences and do not touch game assets or saved games. A display/engine mode change may only affect output after **completely closing and reopening the game**, since some game code queries Switch operation mode only during initialization. A 1080p internal rendering resolution is **not implemented** by this toggle, nor guaranteed by docked mode. The Android window already scales frames to the device surface; this is independent of the game's internal render resolution.

## Redmi K70 test plan

Start with handheld / 30 FPS, then handheld / 60 FPS. Watch the on-screen HUD for measured FPS and frame time in the same scene for several minutes. Next try docked / 30 FPS, *fully quit and reopen the app*, and compare output clarity and temperature. Only then try docked / 60 FPS. Fall back to handheld / 30 FPS if the device overheats, audio desynchronizes, or the game misbehaves.

Android logcat exposes `HOST_PRESENTATION source=... output=...` and `PRESENT_RATE ... fps=...`; `source` is the presented game texture size and `output` is the Android swapchain size. Neither one alone proves the internal Unreal rendering resolution.

## Rebuild only the APK changes

On your Ubuntu builder, pull this feature branch (or merge its PR first). Keep the **same signing key** and the `/ossdata/ot2-work` directory:

```bash
cd /ossdata/NS-Octopath-Traveler-II-translate
git fetch origin feature/handheld-docked-fps-settings
git switch feature/handheld-docked-fps-settings
.venv/bin/python switch2android.py build-extracted \
  /ossdata/ot2-decrypted \
  --title-id 0100a3501946e000 --version 131072 \
  --work /ossdata/ot2-work \
  --output /ossdata/ot2-output-k70-settings \
  --embed never
```

Check that the newly generated `package.json` reports the **same `bundle_id`** as the previously installed build before transferring **only** the new `game.apk`. The app uses content-addressed, already-imported `game.s2a` data when the bundle identity has not changed. Install the new APK **over** the existing app with the **same signing key**; do not uninstall, because that deletes app-private settings and save data. The build still creates a local `game.s2a` file, but transferring it again should not be necessary if the existing resource identity matches.

If the bundle identity changes, this is no longer an APK-only update: do not assume old resources will work.


## Faster: rebuild only `game.apk` with your existing build cache

If you **already successfully built the same game** and still have `/ossdata/ot2-work/generated-assets/launch.json`, you can avoid recreating the multi-gigabyte `game.s2a`. This directly invokes the project's Gradle APK step with the previous build's content manifest.

**First validate the cached assets against the installed package:**

```bash
cd /ossdata/NS-Octopath-Traveler-II-translate
python3 - <<'PY'
import json
from pathlib import Path
old = json.loads(Path('/ossdata/ot2-output/0100a3501946e000-v131072/package.json').read_text())
cached = json.loads(Path('/ossdata/ot2-work/generated-assets/launch.json').read_text())
assert old['bundle_id'] == cached['bundle_id'], 'Different content bundle: STOP'
assert old['title_id'] == cached['title_id'] == '0100a3501946e000', 'Wrong game: STOP'
assert old['embedded'] is False and cached['embedded'] is False, 'Expected external game.s2a: STOP'
assert old['application_id'] == 'org.switch2android.g0100a3501946e000', 'Wrong application ID: STOP'
print('Cached resources match the original installed game; APK-only rebuild is safe to attempt.')
PY
```

**Then compile only the Android APK** (ensure `ANDROID_HOME` points to your installed SDK and Java 21 is available):

```bash
./gradlew --project-cache-dir /ossdata/ot2-work/gradle-cache \
  :app:assembleRelease :app:lintRelease \
  -Ps2aAssets=/ossdata/ot2-work/generated-assets \
  -Ps2aBuildDir=/ossdata/ot2-work/android-build \
  -Ps2aAppId=org.switch2android.g0100a3501946e000 \
  '-Ps2aName=OCTOPATH TRAVELER II' \
  -Ps2aVersionCode=131073

mkdir -p /ossdata/ot2-output-k70-apk-only
cp /ossdata/ot2-work/android-build/app/outputs/apk/release/app-release.apk \
  /ossdata/ot2-output-k70-apk-only/game.apk
```

Only transfer the new `game.apk` to your phone, then **install it over the existing app without uninstalling it**. The already-imported `game.s2a` is addressed by the unchanged bundle ID; the new APK embeds the same `launch.json`. This manual APK-only directory does not contain a complete `package.json` for the Python `install` helper, so install the APK directly (or via `adb install -r`). Do not use the above cached-assets shortcut after changing game content, shader output or build ID.

Back up your original signing key; if you use a different signing key Android will refuse the update and you must not uninstall just to force it.
