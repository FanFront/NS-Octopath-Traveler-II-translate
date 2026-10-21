# GitHub Actions: K70 handheld/docked and 30/60 FPS APK (new signing key)

The workflow [OT2 K70 APK-only (GitHub hosted)](../.github/workflows/build-k70-apk-only.yml) builds on GitHub-hosted Ubuntu and uploads only `game.apk`, `game.apk.sha256`, and `INSTALL-NOTES.txt` to GitHub Actions Artifacts. No Switch game assets, XCI, prod.keys, or `game.s2a` are uploaded or downloaded by GitHub.

**An arbitrary string is not an Android signing key.** This workflow now creates a valid fresh Android signing keystore with `keytool` on each GitHub-hosted runner. It does not require the original Ubuntu keystore or any GitHub signing secret. Each run gets a different certificate, so APKs from successive runs cannot automatically update one another.

## Select installation behavior

- `side_by_side` **(default)**: the new package name is `org.switch2android.ot2k70`. It can be installed alongside the original `org.switch2android.g0100a3501946e000` app, without deleting original saves or data. The two applications have **separate storage**, so the new one must separately import the matching `game.s2a`. Keep the original local ZIP file available on the phone or transfer it privately again. This can temporarily use considerable additional storage.
- `replace_after_uninstall`: use the old package name but sign with a new key. You **must uninstall the existing APK first**, which deletes its private saves and imported game resources. Do not select this unless you have backed up important saves and are prepared to reimport `game.s2a`.

## Provide the two bundle metadata fields

The APK must refer to the real game resources you own; values cannot be invented or random. On the Ubuntu server where the original APK and `game.s2a` were built, run:

```bash
python3 - <<'PY'
import json
from pathlib import Path
p = json.loads(Path('/ossdata/ot2-output/0100a3501946e000-v131072/package.json').read_text())
assert p['title_id'] == '0100a3501946e000' and not p['embedded']
print('bundle_id:', p['bundle_id'])
print('total_bytes:', p['total_bytes'])
PY
```

`total_bytes` means the uncompressed total resource bytes in `package.json`, **not** the ZIP length on disk. `bundle_id` is a 64-character hash, **not** a game key or signing key.

## Run the job

1. Open [GitHub Actions / OT2 K70 APK-only](../.github/workflows/build-k70-apk-only.yml) in the repository.
2. Choose **Run workflow**, select the `main` branch, and enter the original `bundle_id` and `total_bytes`.
3. Leave `installation_mode=side_by_side` to keep your original installation and its saves.
4. On success, open the job run, download the `ot2-k70-settings-apk-only-...` Artifact, and install its `game.apk` on Android.
5. In the new app, select the matching private `game.s2a` file when prompted. Choose handheld/docked and 30/60 FPS settings before starting the game.

The workflow checks out a pinned, previously checked feature-branch commit containing the mode/FPS controls. The Android APK build and device runtime remain experimental. Docked mode only reports Switch operation/performance mode to the guest—it does not guarantee native 1080p rendering. Actual 60 FPS must be measured on the device.

Never publish a game resource archive or your Nintendo Switch keys. An independent new Android signing key can be used for initial installation, but securely preserve a copy if you later require in-place upgrades. This simple APK-only workflow intentionally does not preserve its generated key, so future builds must be installed as separate packages or after uninstall.
