#!/usr/bin/env bash
# 通用游戏构建脚本（基于 switch2android + 本包的稳定性补丁）
# 用法:
#   build-game.sh <title_id> <version> <input_dir> <prod.keys> <work_dir> <out_dir> [repo_dir]
#
# 例:
#   build-game.sh 0100a3501946e000 131072 ~/ot2-input ~/prod.keys ~/s2a-work ~/s2a-out
#   build-game.sh 0100cc80140f8000 262144 ~/ts-input ~/prod.keys ~/s2a-work ~/s2a-out
set -euo pipefail

if [ $# -lt 6 ]; then
  echo "用法: $0 <title_id> <version> <input_dir> <prod.keys> <work_dir> <out_dir> [repo_dir]" >&2
  exit 2
fi

TITLE="$1"; VERSION="$2"; INPUT="$3"; KEYS="$4"; WORK="$5"; OUT="$6"
REPO="${7:-$(cd "$(dirname "$0")/../../repo/switch2android" 2>/dev/null && pwd)}"

[ -n "${REPO:-}" ] && [ -f "$REPO/switch2android.py" ] || { echo "找不到 switch2android 仓库，请传第 7 个参数" >&2; exit 2; }
[ -d "$INPUT" ] || { echo "输入目录不存在: $INPUT" >&2; exit 2; }
[ -f "$KEYS" ]  || { echo "密钥文件不存在: $KEYS" >&2; exit 2; }

# 环境变量（若未设置则尝试从同目录 env.sh 载入）
if ! command -v cmake >/dev/null 2>&1; then
  [ -f "$(dirname "$0")/env.sh" ] && source "$(dirname "$0")/env.sh"
fi

mkdir -p "$WORK" "$OUT"
cd "$REPO"

echo "== doctor =="
.venv/bin/python switch2android.py doctor

echo "== inspect =="
.venv/bin/python switch2android.py inspect "$INPUT" --keys "$KEYS" --hactool "$WORK/tools/hactool/hactool"

echo "== build: $TITLE v$VERSION =="
.venv/bin/python switch2android.py build "$INPUT" \
  --keys "$KEYS" --work "$WORK" --output "$OUT" \
  --title-id "$TITLE" --version "$VERSION"

echo "== 完成，产物在 $OUT/$TITLE-v$VERSION =="
ls -la "$OUT/$TITLE-v$VERSION" 2>/dev/null || true
