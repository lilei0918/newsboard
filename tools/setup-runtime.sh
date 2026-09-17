#!/usr/bin/env bash
#
# 生成 runtime/ —— 项目自带的 Python 运行环境（行情桥脚本用它跑 yfinance）。
#
# 仓库里不包含 runtime/（自带解释器 + 依赖约 256MB），克隆后用这个脚本恢复：
#     tools/setup-runtime.sh
#
# 两种做法：
#   ① 有 python3.10+ ：建一个 venv 到 runtime/python/current（最省事，跨发行版可用）
#   ② 想要完全自包含：用 python-build-standalone 的可移植 CPython（官方发行版那套做法），
#      见脚本末尾注释里的步骤。
#
# 生成的目录结构必须满足：
#     runtime/python/current/bin/python3     ← 程序按这个路径找解释器
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")/.."
ROOT="$PWD"
TARGET="$ROOT/runtime/python/current"
PY="$TARGET/bin/python3"

if [[ -x "$PY" ]]; then
    echo "已经存在：$PY"
    "$PY" -V
    echo "如需重建：rm -rf runtime && tools/setup-runtime.sh"
    exit 0
fi

command -v python3 >/dev/null || { echo "找不到 python3（需要 3.10 或更新）" >&2; exit 1; }
SYS_PY="$(command -v python3)"
SYS_VER="$("$SYS_PY" -c 'import sys;print("%d.%d" % sys.version_info[:2])')"
echo "① 用系统 Python $SYS_VER（$SYS_PY）创建 venv"
mkdir -p "$ROOT/runtime/python"
"$SYS_PY" -m venv "$TARGET"

echo "② 安装行情依赖（yfinance / curl_cffi）"
# curl_cffi 负责伪装浏览器指纹去取 Yahoo 的 cookie+crumb；yfinance 本体也要它
"$PY" -m pip install --quiet --upgrade pip
"$PY" -m pip install --quiet --upgrade yfinance curl_cffi

echo "③ 自检：真去取一次报价"
"$PY" "$ROOT/scripts/yf_fetch.py" quotes AAPL | head -c 200
echo
echo "完成。现在可以 ./run.sh 了。" >&2
echo
cat >&2 <<'EOF'
────────────────────────────────────────────────────────────────────────────
想要完全自包含（不依赖系统 Python）？用 python-build-standalone 的可移植构建：

  # 1) 找一个 CPython 3.11 的 install_only 包（x86_64 Linux）
  #    https://github.com/astral-sh/python-build-standalone/releases
  #    资产名形如 cpython-3.11.16+<日期>-x86_64-unknown-linux-gnu-install_only.tar.gz
  curl -L -o /tmp/py.tar.gz <该资产 URL>
  tar -xzf /tmp/py.tar.gz -C runtime/python          # 解出 python/
  mv runtime/python/python runtime/python/cpython-3.11.16-linux-x86_64-gnu
  ln -sfn cpython-3.11.16-linux-x86_64-gnu runtime/python/current

  # 2) 往这个解释器里装依赖（它是独立构建，pip 需要 --break-system-packages）
  runtime/python/current/bin/python3 -m pip install --break-system-packages \
      --upgrade yfinance curl_cffi

本项目交付目录里的 runtime/ 就是这么来的（CPython 3.11.16 + yfinance 1.7.0 +
curl_cffi 0.16.3 + numpy 2.4.6 + pandas 3.0.5），好处是整个目录可以整体拷走、
目标机器不需要装 Python。
────────────────────────────────────────────────────────────────────────────
EOF
