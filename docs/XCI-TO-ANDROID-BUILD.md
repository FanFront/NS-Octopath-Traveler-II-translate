# From your XCI + local key file to OCTOPATH TRAVELER II Android

**Current known image metadata (from the user's own local inspection):**

- Base CNMT: `0100a3501946e000`, version `0`.
- Update CNMT: `0100a3501946e800`, version `131072` (1.1.1).
- DLC CNMT: `0100a3501946f001`, version `0`.
- Base Program NCA: `ae7a781a1023ee50e732408a23ce8326.nca`.
- Update Program NCA: `59ba403fa5f2a2a5d66d57e47d61b572.nca`.
- Update Control NCA: `478995a3a829297eae0c7bb541769c42.nca`.

The XCI has the correct *update metadata*, but extraction and the patched main's build ID still need validation. Never assume container inspection proves full decryption.

## A. Test extraction **on your Ubuntu machine first**

Use the key file you already validated locally. Do not upload or paste it anywhere. If you are entitled to use the game and keys, run:

```bash
cd /ossdata
git clone https://github.com/FanFront/NS-Octopath-Traveler-II-translate.git
cd NS-Octopath-Traveler-II-translate
```

If the repository is already cloned:

```bash
git pull --ff-only
```

Ensure hactool is installed. Check there is sufficient free disk space:

```bash
df -h /ossdata
command -v hactool
```

Then test XCI extraction. This **does not build Android yet**:

```bash
umask 077
python3 -m scripts.extract_xci_local /ossdata/prodkeys/game.xci \
  --keys /ossdata/prodkeys/hactool.keys \
  --hactool "$(command -v hactool)" \
  --output /ossdata/ot2-decrypted \
  --title-id 0100a3501946e000 \
  --version 131072
```

This step:

1. Extracts the XCI's secure NCA containers into a disposable private workspace.
2. Reads all CNMT files and **selects by actual title ID and version**, not filename.
3. Checks NCA sizes and SHA-256 against CNMT metadata.
4. Uses the base Program NCA with the update Program NCA through hactool `--basenca`.
5. Extracts the selected Control and DLC resources.
6. Validates that `ExeFS/main` contains the project's required full 32-byte build ID and the RomFS contains Unreal `.pak` files.
7. Atomically publishes the decrypted game into `/ossdata/ot2-decrypted` only after success.

If the extraction fails, the temporary partial files are removed. Errors from hactool are **not dumped** because they may contain key material. `--no-dlc` is available to omit optional DLC on an initial trial.

When successful:

```bash
ls -lh /ossdata/ot2-decrypted/ExeFS/main
find /ossdata/ot2-decrypted/RomFS -name '*.pak' -printf '%p\n' | head
python3 switch2android.py inspect-extracted /ossdata/ot2-decrypted \
  --title-id 0100a3501946e000 --version 131072
```

If you hit `Unsupported main build ID`, the update data does not match the executable tested by this Android port. Do **not** bypass the build-ID check. If `hactool failed`, check your local keys, executable version, available disk space, and whether the game NCA contents are intact. Never post raw hactool logs or private keys.

## B. Build via R2 and GitHub Actions

The [Android build workflow](https://github.com/FanFront/NS-Octopath-Traveler-II-translate/actions/workflows/build-ot2.yml) now supports `*.xci` in addition to decrypted directory archives.

**R2 is read-only, and output goes to GitHub Actions Artifacts only.**

1. Configure your own Linux x64 machine as a GitHub **self-hosted** runner with label `ot2-builder`. Make sure it has **at least 60 GiB available** (80–100 GiB recommended), Git, Make, CMake, Cargo, .NET, SPIRV-Tools, AWS CLI, and the Android SDK with platform 36, NDK 28.2.13676358 and CMake 3.22.1.
2. Set `ANDROID_HOME` in the runner's service environment and optionally repository variable `S2A_SCRATCH` to an **absolute writable path** on the disk with free space, e.g. `/ossdata/ot2-actions`.
3. Add a **GitHub Actions repository variable** `S2A_LOCAL_KEYS_PATH` with the **path only**, e.g. `/ossdata/prodkeys/hactool.keys`. This is not a secret value; the actual key file must already exist locally on the runner with restrictive permissions (mode 600).
4. Keep the existing R2 repository Secrets: `R2_ACCOUNT_ID`, `R2_BUCKET`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`.
5. Open **Actions > OT2 Android (R2 input -> GitHub artifact) > Run workflow** and enter the object key of your existing `.xci`, e.g. `games/game.xci`.

The workflow downloads the XCI onto the runner, bootstraps its pinned hactool, performs the same CNMT-verified extraction and build-ID validation, generates APK + S2A, then uploads **only to GitHub Actions Artifacts**. It does **not** push any output back to Cloudflare R2.

### Public-fork caveat

Your existing GitHub fork is **public**. To avoid distributing licensed game assets, the job deliberately uploads only the APK and metadata in this repo. This is **not a fully playable artifact** without `game.s2a`. For a complete private artifact, create an **independent private repository** (not a public upstream fork) with appropriately restricted access and artifact storage.

### Security of self-hosted runners

A persistent self-hosted runner in a public repository is risky: code modifications can execute commands on your PC. Run it in an isolated VM/container without access to personal files or other secrets, and restrict write access to the repo. Protect the local keys and never post them in workflow logs or issue comments.
