# Build OCTOPATH TRAVELER II from Cloudflare R2 using GitHub Actions

**Outputs are GitHub Actions artifacts only.** No build output, APK, or game bundle is written to R2.

## Requirements and limitations

- The fork supports the exact **OCTOPATH TRAVELER II v1.1.1** main NSO build ID described in `profiles/0100a3501946e000.json`. Older cartridge-only builds are not accepted.
- Your R2 source object must be an archive containing an **already-decrypted** `ExeFS/` and `RomFS/` directory. Optional: `Control/` and `AddOnContent/`.
- **An XCI labelled "cracked" is not necessarily decrypted**. This workflow intentionally rejects raw `.xci`. If only the XCI is on R2, archive and upload your previously extracted decrypted directory as a separate input object. This is not an APK converter for encrypted XCI files.
- The standard GitHub-hosted Ubuntu runner only has about 14 GiB available. The project recommends approximately 60 GiB, so this workflow uses your own Linux x64 self-hosted runner labelled `ot2-builder` with >= 60 GiB free (80-100 GiB recommended).
- For a **public GitHub repository**, the workflow uploads **APK and metadata only**. It does not publish `game.s2a`, which contains licensed game data. The APK alone cannot run the game. **Create a separate private repository (not a GitHub fork) if you want the complete APK + S2A artifact**, accessible to its authorized readers. GitHub does not allow independently changing the visibility of a public upstream fork. Private artifacts also remain subject to GitHub storage limits, retention, and permitted use.

## 1. Make a decrypted game archive and put it on R2

Assume your Linux machine has `/home/me/ot2-extracted/ExeFS/main` and `/home/me/ot2-extracted/RomFS/...`. Create a TAR archive with these folders **at the archive root**:

```bash
cd /home/me/ot2-extracted
tar -czf /home/me/ot2-decrypted.tar.gz ExeFS RomFS
# Or include the optional folders that actually exist:
# tar -czf /home/me/ot2-decrypted.tar.gz ExeFS RomFS Control AddOnContent
```

Upload this archive as a **second, private R2 input object** (e.g. `ot2/ot2-decrypted.tar.gz`). Do not commit XCI, decrypted files, games, or keys to GitHub.

## 2. Configure GitHub repository Secrets

Go to **Settings → Secrets and variables → Actions** and add four Repository Secrets:

| Secret | Value |
|---|---|
| `R2_ACCOUNT_ID` | Cloudflare R2 account ID |
| `R2_BUCKET` | Private R2 bucket containing the input archive |
| `R2_ACCESS_KEY_ID` | Read-only R2 API credential access key |
| `R2_SECRET_ACCESS_KEY` | Corresponding secret access key |

Create an R2 token with access limited to reading the relevant bucket. The workflow uses the S3-compatible endpoint `https://<ACCOUNT_ID>.r2.cloudflarestorage.com` and only the `aws s3 cp ... s3://... -> local` direction.

No R2 upload command is included.

## 3. Register and prepare your Linux runner

In GitHub, choose **Settings → Actions → Runners → New self-hosted runner**, follow the Linux x64 registration steps, and assign the custom label `ot2-builder`.

On this runner, install Git, Make, C/C++ build tools, CMake, Rust/Cargo, SPIRV-Tools (`spirv-val`), AWS CLI, and the Android SDK. Provide these SDK components:

- Platform `android-36`
- NDK `28.2.13676358`
- CMake `3.22.1`
- Platform tools and command-line tools

Set `ANDROID_HOME` in the **runner service environment**. Java 21, Python 3.13, and .NET SDK 10 are selected in the workflow. Ensure your runner user can read/write its build scratch directory.

Optional: set a repository Variable `S2A_SCRATCH` to an absolute path on a disk with at least 60 GiB free; by default the workflow uses `/mnt/ot2-actions`.

**Security:** Restrict who can modify and dispatch workflows on public forks with self-hosted runners. Do not enable an untrusted pull-request job on the game-build runner.

## 4. Run the workflow

1. Open **Actions → OT2 Android (R2 input -> GitHub artifact)**.
2. Select **Run workflow**.
3. Enter the R2 object key, for example: `ot2/ot2-decrypted.tar.gz`.
4. Confirm the runner is online. The workflow downloads the decrypted archive, validates the game's real build ID, builds the APK and S2A bundle, and verifies the resource file.
5. Open the completed run and download its item under **Artifacts**.

In an independent private repository, the complete private artifact includes `game.apk`, `game.s2a`, `package.json`, and `INSTALL.txt`. It expires after one day. Public repositories receive an APK-only artifact that is not by itself playable. For a complete playable package, use the separate private repository workflow or keep the game bundle from a local build. The public fork deliberately will not distribute the game bundle.

The workflow does not create a GitHub Release, commit binaries to the repository, or upload anything back to R2.

## Local build without Actions

The source tree also contains the following commands for already-decrypted files (no `prod.keys`):

```bash
python3 switch2android.py inspect-extracted /path/to/ot2-extracted \
  --title-id 0100a3501946e000 --version 131072

./scripts/bootstrap-tools.sh "$HOME/s2a-work"

.venv/bin/python switch2android.py build-extracted /path/to/ot2-extracted \
  --title-id 0100a3501946e000 --version 131072 \
  --work "$HOME/s2a-work" --output "$HOME/s2a-output" --embed never
```

See `INSTALL.txt` for the project's original build dependencies. Original NSP-based commands are unchanged.

## Safety and troubleshooting

- Never paste your R2 keys or prod.keys into an issue or workflow run log.
- `Unsupported main NSO build ID`: your decrypted game executable does not match the supported 1.1.1 update. Do not bypass this check.
- `Raw .xci cannot be built`: your R2 object is a cartridge image rather than the required extracted archive.
- `No supported Unreal pak` or shader errors: verify that RomFS is fully extracted and that the archive is not just a game-content metadata dump.
- `Missing tool`: install the prerequisite on your **self-hosted runner**, not just your desktop.
- If your private `.s2a` exceeds your GitHub Actions artifact storage allowance, you must use a larger plan, an alternative private manual delivery method, or build locally. This workflow deliberately does **not** fall back to uploading it to R2.
