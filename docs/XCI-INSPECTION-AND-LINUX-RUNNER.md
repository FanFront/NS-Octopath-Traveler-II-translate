# Use your existing Cloudflare R2 XCI (without downloading the whole game)

The existing R2 object can be checked **without keys**, without downloading the whole XCI, and without registering a self-hosted runner.

## First: inspect the XCI that is already on R2

1. Open [Actions: Inspect XCI from Cloudflare R2 (no keys)](https://github.com/FanFront/NS-Octopath-Traveler-II-translate/actions/workflows/inspect-xci.yml).
2. Click **Run workflow**, and enter the exact R2 object key, e.g. `games/octopath2.xci`. Enter an **object key**, not a public URL or the bucket name.
3. Open the finished run → **Probe XCI headers and secure partition** and read its output.

The job uses the R2 secrets already configured: `R2_ACCOUNT_ID`, `R2_BUCKET`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`. It makes only read-only R2 HeadObject/GetObject byte-range requests. It uploads nothing to Cloudflare R2 or GitHub artifacts.

The result shows the XCI `HEAD` header, HFS0 partitions, number of NCA files, and whether a few NCA headers contain plaintext `NCA3`-family magic at offset 0x200.

**Important:** neither "cracked XCI" nor a plaintext NCA header proves that its contents are decrypted. This inspection cannot extract RomFS/ExeFS, identify the applied game patch/update, or establish that the title works with the supported 1.1.1 profile. A conventional Switch game card image normally still needs legitimate decryption material before NCA contents can be extracted.

## Checking a local XCI

You can run the same probe with just Python 3.11+ on your own Linux PC, no keys and no dependencies:

```bash
python3 scripts/probe_xci.py --local "/path/to/your/game.xci"
```

For more detailed format inspection, build or install [hactool](https://github.com/SciresM/hactool). These are the two distinct operations:

```bash
# Read the XCI's outer format.
hactool -t xci -i "/path/to/your/game.xci"

# Extract the secure partition's NCA containers to a LOCAL directory.
mkdir -p "$HOME/ot2-secure"
hactool -t xci --securedir="$HOME/ot2-secure" "/path/to/your/game.xci"
```

The second command only extracts NCA containers; it does not decrypt them.

If you have legally obtained keys, you can inspect individual program NCAs with:

```bash
hactool -t nca -i -k "$HOME/keys/prod.keys" "$HOME/ot2-secure/REPLACE_PROGRAM.nca"
```

Identify the program NCA from its content type/metadata, not merely by choosing the largest. Extracting an updated game may require the corresponding base NCA and update NCA via hactool's `--basenca` option. Use the game's matching v1.1.1 update; the base cartridge alone may not be compatible.

When the program has been legitimately decrypted, extract **ExeFS** and **RomFS** into a tree containing `ExeFS/main` and the original `RomFS` resources. For a compatible standalone decrypted program NCA, hactool supports:

```bash
mkdir -p "$HOME/ot2-decrypted/ExeFS" "$HOME/ot2-decrypted/RomFS"
hactool -t nca -k "$HOME/keys/prod.keys" \
  --exefsdir="$HOME/ot2-decrypted/ExeFS" \
  --romfsdir="$HOME/ot2-decrypted/RomFS" \
  "$HOME/ot2-secure/REPLACE_PROGRAM.nca"
```

This does not guarantee success for encrypted/program-patched NCAs or for updates requiring a base NCA. Do not use arbitrary internet keys or publish game files or secrets.

## Turn this PC into a runner *only after* input validation

The current APK workflow uses `runs-on: [self-hosted, linux, x64, ot2-builder]`. If your PC is x86-64 Linux, it can run this job after registering a runner with the custom `ot2-builder` label.

1. Run `uname -m` and `df -h`: you need `x86_64`, with at least 60 GiB free on the scratch volume (80–100 GiB recommended).
2. On your GitHub repository go to **Settings → Actions → Runners → New self-hosted runner**. Choose Linux x64. Copy its current download, verification, registration, and start commands; don't paste its registration token into a public issue.
3. During `./config.sh`, enter `ot2-builder` as the custom label (or add it through the runner settings). Start with `./run.sh`, or install its service according to GitHub's instructions.
4. Install Git, Make, C/C++ compiler, Rust/Cargo, .NET SDK 10, CMake, SPIRV-Tools, AWS CLI and Android SDK platform 36/NDK 28.2.13676358/CMake 3.22.1. The workflow also sets up Java 21, Python 3.13 and .NET 10.
5. Make sure `ANDROID_HOME` is visible to the user/service running the runner, and `S2A_SCRATCH` (a repository variable, optional) is an absolute path on the large disk. Otherwise the workflow defaults to `/mnt/ot2-actions`.

**Security:** GitHub warns against using a persistent personal self-hosted runner with a public repository. Prefer a dedicated Linux VM or isolated machine with no private personal data, and preferably an independent private source repository. Keep untrusted pull-request workflows off the self-hosted runner.

## Only once ExeFS/RomFS and the matching version are available

Archive `ExeFS/` and `RomFS/` at the archive root, put the archive on private R2, then run [OT2 Android (R2 input -> GitHub artifact)](https://github.com/FanFront/NS-Octopath-Traveler-II-translate/actions/workflows/build-ot2.yml).

The APK-build workflow remains deliberately strict: **it does not accept raw XCI as build input**. It validates a matching executable build ID and creates the output only after successful decryption/extraction of compatible game assets. Build results are uploaded **only as GitHub Actions artifacts**, never back to R2. The current public fork does not distribute the large `game.s2a` resource bundle; a complete private artifact requires an independently created private repository.
