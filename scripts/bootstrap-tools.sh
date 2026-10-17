#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $# != 1 ]]; then echo 'Usage: scripts/bootstrap-tools.sh /absolute/path/outside/repo/work' >&2; exit 2; fi
work="$(python3 -c 'import sys; from pathlib import Path; from s2a.cli import outside; print(outside(Path(sys.argv[1])))' "$1")"
mkdir -p "$work/tools"
checkout() {
  local destination="$1" url="$2" revision="$3"
  if [[ ! -d "$destination/.git" ]]; then git clone "$url" "$destination"; fi
  if [[ "$(git -C "$destination" rev-parse HEAD)" != "$revision" ]]; then
    if [[ -n "$(git -C "$destination" status --porcelain)" ]]; then echo 'Existing tool differs; refusing overwrite' >&2; exit 1; fi
    git -C "$destination" checkout --detach "$revision"
  fi
}
checkout "$work/tools/hactool" https://github.com/SciresM/hactool.git 1d64a83450e025622f3468c28fc4164dad2c5ef6
patch_file="$PWD/tools/hactool-titlekeys.patch"
if git -C "$work/tools/hactool" apply --check "$patch_file" 2>/dev/null; then
  git -C "$work/tools/hactool" apply "$patch_file"
elif ! git -C "$work/tools/hactool" apply --reverse --check "$patch_file" 2>/dev/null; then
  echo 'hactool key-pipe patch differs; refusing overwrite' >&2; exit 1
fi
[[ -f "$work/tools/hactool/config.mk" ]] || cp "$work/tools/hactool/config.mk.template" "$work/tools/hactool/config.mk"
make -C "$work/tools/hactool" -j4
checkout "$work/tools/repak" https://github.com/trumank/repak.git 355b5f62f51959c7cc6dd5a51708646ef483065d
cargo build --manifest-path "$work/tools/repak/Cargo.toml" --release --locked -p repak_cli
[[ -x .venv/bin/python ]] || python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
printf 'Tools ready. Next: .venv/bin/python switch2android.py doctor\n'
