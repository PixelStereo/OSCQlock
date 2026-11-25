#!/usr/bin/env bash
# OSCQlock-linux-<x64|arm64>-<tag>.AppImage and .tar.gz (arm64: the Raspberry Pi 4 and 5, 64-bit Raspberry Pi OS) from a Release build.
# usage: package-linux.sh <build dir> <output dir> <tag>   (QMAKE must point at the Qt to bundle)
set -euo pipefail
build=$(cd "$1" && pwd); out=$(mkdir -p "$2" && cd "$2" && pwd); tag=$3
root=$(cd "$(dirname "$0")/.." && pwd)
name=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["name"])' "$build/application.json")
case "$(uname -m)" in
  x86_64) host=x86_64; arch=x64 ;;
  aarch64|arm64) host=aarch64; arch=arm64 ;;
  *) echo "unsupported machine $(uname -m)" >&2; exit 1 ;;
esac
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
app="$work/AppDir"
mkdir -p "$app/usr/bin" "$app/usr/share/applications" "$app/usr/share/icons/hicolor/256x256/apps"
cp "$build/$name" "$app/usr/bin/oscqlock"
cp "$root/packaging/oscqlock.desktop" "$app/usr/share/applications/oscqlock.desktop"
cp "$root/packaging/oscqlock.png" "$app/usr/share/icons/hicolor/256x256/apps/oscqlock.png"

cd "$work"
base=https://github.com/linuxdeploy
for tool in linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$host.AppImage \
            linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$host.AppImage; do
  curl -fsSL --retry 4 -o "$(basename "$tool")" "https://github.com/$tool"
  chmod +x "$(basename "$tool")"
done
export APPIMAGE_EXTRACT_AND_RUN=1 PATH="$work:$PATH" LINUXDEPLOY_OUTPUT_VERSION="$tag" QMAKE=${QMAKE:?QMAKE is not set}
export EXTRA_QT_PLUGINS="platforms;platformthemes;xcbglintegration"
./linuxdeploy-$host.AppImage --appdir "$app" --plugin qt --output appimage \
  -d "$app/usr/share/applications/oscqlock.desktop" -i "$app/usr/share/icons/hicolor/256x256/apps/oscqlock.png"
ai=$(ls OSCQlock*.AppImage | head -1)
cp "$ai" "$out/OSCQlock-linux-$arch-$tag.AppImage"

# The tar.gz: the same bundle, as a folder
mkdir -p "pkg/OSCQlock-$tag"
cp -a "$app"/. "pkg/OSCQlock-$tag/"
ln -sf usr/bin/oscqlock "pkg/OSCQlock-$tag/OSCQlock"
tar -C pkg -czf "$out/OSCQlock-linux-$arch-$tag.tar.gz" "OSCQlock-$tag"
