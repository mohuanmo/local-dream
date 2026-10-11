#!/usr/bin/env bash
# Copies the prebuilt native libraries and QNN assets out of a Local Dream APK
# (or an apktool dump of one) into this source tree, so Gradle packages them
# as-is.
#
# Those libraries are normally built by app/src/main/cpp/build.sh and
# app/src/main/cpp/dit/build.sh, which need the proprietary Qualcomm QNN and
# Hexagon SDKs. Reusing them from an already-built APK needs neither.
#
# Usage:
#   tools/import_native.sh path/to/LocalDream.apk
#   tools/import_native.sh path/to/apktool_dump_dir
set -euo pipefail

INPUT="${1:?usage: import_native.sh <apk-or-apktool-dir>}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEST_JNI="$REPO/app/src/main/jniLibs/arm64-v8a"
DEST_ASSETS="$REPO/app/src/main/assets"

WORK=""
cleanup() { if [ -n "$WORK" ]; then rm -rf "$WORK"; fi; }
trap cleanup EXIT

if [ -f "$INPUT" ]; then
    echo "Unpacking $INPUT ..."
    WORK="$(mktemp -d)"
    unzip -q "$INPUT" -d "$WORK"
    # An APK is flat; an apktool dump keeps the same layout under its root.
    BASE="$WORK"
    if [ ! -d "$WORK/lib" ]; then
        BASE="$(find "$WORK" -maxdepth 2 -type d -name lib -print -quit | xargs -r dirname)"
    fi
else
    BASE="$INPUT"
fi

if [ ! -d "$BASE/lib/arm64-v8a" ]; then
    echo "error: $BASE/lib/arm64-v8a not found" >&2
    exit 1
fi

mkdir -p "$DEST_JNI"
for lib in libstable_diffusion_core.so libdit_engine.so; do
    src="$BASE/lib/arm64-v8a/$lib"
    if [ ! -f "$src" ]; then
        echo "error: missing $src" >&2
        exit 1
    fi
    cp -f "$src" "$DEST_JNI/"
    echo "  -> jniLibs/arm64-v8a/$lib"
done

for dir in qnnlibs ditlibs licenses; do
    src="$BASE/assets/$dir"
    if [ -d "$src" ]; then
        rm -rf "${DEST_ASSETS:?}/$dir"
        cp -r "$src" "$DEST_ASSETS/"
        echo "  -> assets/$dir ($(find "$DEST_ASSETS/$dir" -type f | wc -l) files)"
    fi
done

echo "Done. Gradle can now package the app without the QNN/Hexagon SDKs."
