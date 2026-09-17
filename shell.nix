# NewsBoard 构建环境（NixOS）
#
#   nix-shell
#   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --parallel 8
#   ./build/NewsBoard
#
# 说明：Qt6Gui 依赖 WrapOpenGL → find_package(OpenGL)，NixOS 上必须显式提供
# libglvnd（libGL.so）与 GL 头文件，否则 Qt6 会被判定为 NOT FOUND。
{ pkgs ? import <nixpkgs> { } }:

let
  support = with pkgs; [
    libglvnd
    libglvnd.dev
    libxkbcommon
    libxkbcommon.dev
  ];
in
pkgs.mkShell {
  packages = with pkgs; [
    qt6.qtbase
    qt6.qtwayland
    cmake
    ninja
    pkg-config
    gcc              # 必须用 nixpkgs 默认 GCC：Qt6 就是它构建的，用旧版会缺 CXXABI 符号
    openssl
    mesa
  ] ++ support;

  CMAKE_PREFIX_PATH = pkgs.lib.concatStringsSep ":" (map toString (with pkgs; [
    qt6.qtbase
    qt6.qtwayland
  ] ++ support));
}
