#!/bin/bash
# Builds SpectraBlade as a universal (Apple silicon and Intel) app, signs it, runs the tests with
# the signed app as the plug-in scan worker, and packs it into a disk image:
#   dist/SpectraBlade-<version>.dmg (the app and the licence) and its .sha256
#
# Usage: Scripts/package.sh [--juce <JUCE checkout>] [--jobs <n>] [--skip-tests]
#
# Without a Developer ID the app is signed ad hoc: it runs on any Mac, but Gatekeeper asks the
# first time it is opened (see README, "Installing").
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build-release"
dist="$root/dist"
juce=""
jobs=4
tests=1

while [ $# -gt 0 ]; do
    case "$1" in
        --juce) juce="$2"; shift 2 ;;
        --jobs) jobs="$2"; shift 2 ;;
        --skip-tests) tests=0; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

version="$(sed -n 's/^project(SpectraBlade VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
[ -n "$version" ] || { echo "no version in CMakeLists.txt" >&2; exit 1; }

echo "== Building SpectraBlade $version (arm64 + x86_64)"
cmake -S "$root" -B "$build" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" ${juce:+-DSPECTRABLADE_JUCE_DIR="$juce"}
cmake --build "$build" --config Release -j "$jobs"

app="$build/SpectraBlade_artefacts/Release/SpectraBlade.app"
tester="$build/SpectraBladeTests_artefacts/Release/SpectraBladeTests"

echo "== Signing (ad hoc)"
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
lipo -archs "$app/Contents/MacOS/SpectraBlade"

if [ "$tests" = 1 ]; then
    echo "== Tests, with the signed app as the scan worker"
    if ! "$tester" --app "$app/Contents/MacOS/SpectraBlade" > "$build/tests.log" 2>&1; then
        grep FAIL "$build/tests.log" >&2 || true
        echo "tests failed; the full output is in $build/tests.log" >&2
        exit 1
    fi
    tail -n 1 "$build/tests.log"
fi

echo "== Disk image"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
ditto "$app" "$stage/SpectraBlade.app"
ln -s /Applications "$stage/Applications"
cp "$root/LICENSE" "$stage/LICENSE.txt"
cp "$build/_deps/lame-src/COPYING" "$stage/LAME-LICENSE.txt"
mkdir -p "$dist"
dmg="$dist/SpectraBlade-$version.dmg"
rm -f "$dmg"
hdiutil create -quiet -volname "SpectraBlade $version" -srcfolder "$stage" -format UDZO "$dmg"
(cd "$dist" && shasum -a 256 "$(basename "$dmg")" > "$(basename "$dmg").sha256")

echo "== Done"
ls -l "$dmg"
cat "$dmg.sha256"
