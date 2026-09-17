#!/usr/bin/env bash
#
# NewsBoard 启动脚本 —— 直接运行，无需安装任何东西。
#
#   ./run.sh              启动看板
#   ./run.sh --selftest   行情自检（报价/迷你走势/历史K线），打印结果后退出
#
# 运行需要的一切都在这个目录里，整个目录可以整体拷走：
#   build/NewsBoard                       程序本体
#   lib/                                  运行库（Qt6 等）+ 自带动态加载器 + Qt 插件
#   runtime/python/current/bin/python3    自带 Python（已内置 yfinance）
#   scripts/yf_fetch.py                   行情桥脚本
#   config/  data/                        配置与缓存（首次运行自动创建）
#
# 用 lib/ 里的加载器 + --library-path 启动，所以不依赖系统里的 Qt，也不依赖
# 任何 nix store 路径；目录搬走照样能跑。
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"
ROOT="$PWD"
BIN="$ROOT/build/NewsBoard"
LOADER="$ROOT/lib/ld-linux-x86-64.so.2"
PY="$ROOT/runtime/python/current/bin/python3"

if [[ ! -x "$BIN" ]]; then
    cat >&2 <<MSG
找不到可执行文件：$BIN

如果是从源码重新编译：
  nix-shell --run 'cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 8'
MSG
    exit 1
fi

if [[ "${1:-}" == "--selftest" ]]; then
    export NB_SELFTEST=1
    shift
fi

# Qt 插件（平台/图片格式/TLS 等）
export QT_PLUGIN_PATH="$ROOT/lib/qt6/plugins"

if [[ -x "$LOADER" && -d "$ROOT/lib" ]]; then
    # 自带运行库：完全自包含
    exec "$LOADER" --library-path "$ROOT/lib" "$BIN" "$@"
else
    # 兜底：库目录缺失时仍按系统库启动（本机 nix store 里已有 Qt）
    echo "提示：lib/ 目录不完整，改用系统运行库启动" >&2
    exec "$BIN" "$@"
fi
