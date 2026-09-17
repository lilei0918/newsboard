#!/usr/bin/env bash
#
# 重新生成 lib/ —— 把程序依赖的共享库、动态加载器、Qt 插件都收进项目内，
# 使整个目录自包含、可整体搬移、不依赖 nix store 路径。
#
# 什么时候需要跑：重新编译之后，或者换了 Qt 版本之后。
# 用法（需要 Qt 环境）：
#     nix-shell --run 'tools/bundle-runtime.sh'
#
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."

BIN="build/NewsBoard"
[[ -x "$BIN" ]] || { echo "先编译出 $BIN" >&2; exit 1; }

echo "① 收集共享库依赖…"
mkdir -p lib/qt6/plugins
echo "   清理旧 lib/"
rm -rf lib && mkdir -p lib/qt6/plugins

ldd "$BIN" | awk '/=> \/nix\/store/ {print $3}' | sort -u |
    while read -r f; do cp -Lf "$f" lib/; done

echo "② 复制动态加载器…"
LOADER=$(readelf -l "$BIN" | awk '/interpreter/ {print $NF}' | tr -d ']')
[[ -f "$LOADER" ]] && cp -Lf "$LOADER" lib/ || echo "   警告：未找到加载器 $LOADER"

echo "③ 复制 Qt 插件…"
CORE=$(ldd "$BIN" | awk '/libQt6Core/ {print $3; exit}')
QPLUG="$(dirname "$(dirname "$CORE")")/qt-6/plugins"
if [[ -d "$QPLUG" ]]; then
    for c in platforms wayland-shell-integration wayland-decoration-client \
             wayland-graphics-integration-client imageformats platformthemes tls \
             networkinformation generic egldeviceintegrations xcbglintegrations \
             platforminputcontexts; do
        [[ -d "$QPLUG/$c" ]] && cp -rL "$QPLUG/$c" lib/qt6/plugins/ || true
    done
else
    echo "   警告：找不到 Qt 插件目录（$QPLUG）"
fi

echo "④ 补齐插件依赖的库（两轮迭代）…"
for _ in 1 2; do
    { for so in lib/qt6/plugins/*/*.so lib/*.so*; do
          ldd "$so" 2>/dev/null | awk '/=> \/nix\/store/ {print $3}'
      done; } | sort -u | while read -r f; do
        b=$(basename "$f")
        [[ -e "lib/$b" ]] || cp -Lf "$f" lib/ 2>/dev/null || true
    done
done

echo
echo "完成：$(ls lib/*.so* 2>/dev/null | wc -l) 个共享库，$(find lib/qt6/plugins -name '*.so' 2>/dev/null | wc -l) 个插件，共 $(du -shL lib | cut -f1)"
