#!/usr/bin/env bash
#
# 把 NewsBoard 注册到应用启动器（Walker / 各类应用菜单）。
# 目录被搬动后重跑一次即可修正路径。
#
set -euo pipefail
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$DEST"
sed -e "s|@ROOT@|$ROOT|g" "$ROOT/assets/newsboard.desktop.in" > "$DEST/newsboard.desktop"
chmod 644 "$DEST/newsboard.desktop"
echo "已安装启动项：$DEST/newsboard.desktop"
echo "  命令：$ROOT/run.sh"
echo "  Walker 里搜：newsboard / 资讯 / 行情"
