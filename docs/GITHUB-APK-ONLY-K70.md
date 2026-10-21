# Build the K70 handheld/docked and 30/60 FPS APK with GitHub Actions

Workflow: [OT2 K70 APK-only (GitHub hosted)](../.github/workflows/build-k70-apk-only.yml).

This workflow runs on **GitHub-hosted Ubuntu 24.04**, not the Tencent Cloud server. It checks out the `feature/handheld-docked-fps-settings` branch and builds **only a signed Android APK**. It does not use or download game files, shader caches, XCI, `game.s2a`, Switch prod.keys or R2 credentials. The only outputs are `game.apk` and its checksum in GitHub Actions Artifacts.

## One-time setup required for an APK that updates an existing installation

The already-installed app must be signed using the **same Android signing key**. The original `switch2android.py build-extracted` Release APK used the **local Android debug keystore** belonging to the Ubuntu account that ran Gradle (typically `~/.android/debug.keystore`). A new GitHub-hosted runner generates a *different* signing key unless you provide the original. Without the same key Android will reject an in-place update, and uninstalling the game destroys app-private saves.

From the Ubuntu account that **originally built the installed APK**, first verify that the original keystore exists:

```bash
test -s "$HOME/.android/debug.keystore" && echo "Found original signing key"
```

Back up this file privately and never commit it or paste its contents into a chat, issue, workflow log or public file. Review whether allowing your GitHub Actions workflow to access a copy of this private signing key matches your security requirements. In this public repository, avoid running untrusted code with secrets; restrict repository write access.

To upload the original key **directly to GitHub's encrypted Actions Secrets** without printing its contents, on a machine with GitHub CLI installed and authenticated:

```bash
base64 -w0 "$HOME/.android/debug.keystore" |
  gh secret set S2A_DEBUG_KEYSTORE_BASE64 \
    --repo FanFront/NS-Octopath-Traveler-II-translate
```

Alternatively set repository secret `S2A_DEBUG_KEYSTORE_BASE64` in **Settings → Secrets and variables → Actions → New repository secret** using the base64-encoded file contents. Do not put the key into an Actions Variable or a workflow dispatch input. The workflow checks that the keystore has the default Android debug keystore alias and password and refuses to build an upgrade APK if the secret is missing.

## Supply the old bundle metadata (two small values, no assets)

On your Ubuntu server read the existing build's metadata:

```bash
python3 - <<'PY'
from pathlib import Path
import json
p = json.loads(Path('/ossdata/ot2-output/0100a3501946e000-v131072/package.json').read_text())
assert p['title_id'] == '0100a3501946e000'
assert p['embedded'] is False
print('bundle_id:', p['bundle_id'])
print('total_bytes:', p['total_bytes'])
PY
```

**Do not use the ZIP size of `game.s2a` for `total_bytes`**. The workflow requires the numbers from the previous `package.json`. These values identify the resource package already imported on your phone and let the APK find it without re-downloading it.

Open **Actions → OT2 K70 APK-only (GitHub hosted) → Run workflow** and enter the two values. Click **Run workflow**.

When successful, download artifact `ot2-k70-settings-apk-only-<run number>`, extract `game.apk`, and copy **only the APK** to your phone. The APK uses the previous bundle ID and the same signing keystore. Install as an **update**, without uninstalling the old game; existing `game.s2a` contents and saves remain in app-private storage.

## Important caveats

- This is a new APK with 30/60 FPS and handheld/docked mode UI, **not a guarantee of 1080p or 60 actual frames per second**. Docked mode may require a full game restart to have any effect.
- The feature branch is experimental. Review/validate the generated APK on your Redmi K70 before relying on it.
- If the original keystore is missing, the safe next step is to recover that key. **Do not create a replacement key and uninstall the old application to force installation.**
- Because the repository is public, **do not upload game data or keys**. The GitHub Actions artifact contains only APK and checksum.
