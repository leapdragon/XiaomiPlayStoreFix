#!/bin/bash
# Build the XiaomiPlayStoreFix Zygisk module zip.
set -euo pipefail
cd "$(dirname "$0")"

SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
export JAVA_HOME=${JAVA_HOME:-$HOME/Android/jdk-17}
NDK=$(ls -d "$SDK"/ndk/* | sort -V | tail -1)
BT=$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)
ANDROID_JAR=$SDK/platforms/android-35/android.jar
CXX=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang++
OUT=build

rm -rf "$OUT" && mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/zip/zygisk"

"$JAVA_HOME/bin/javac" --release 11 -cp "$ANDROID_JAR" -d "$OUT/classes" java/xiaomiplaystorefix/*.java
"$BT/d8" --release --min-api 31 --lib "$ANDROID_JAR" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')

"$CXX" -std=c++20 -O2 -fPIC -shared -fvisibility=hidden -fno-exceptions \
    -static-libstdc++ -Wl,--gc-sections -Wl,-z,max-page-size=16384 -s \
    -DLSPLANT_SO_PATH="\"$PWD/native/third_party/liblsplant.so\"" \
    -DCLASSES_DEX_PATH="\"$PWD/$OUT/dex/classes.dex\"" \
    native/main.cpp native/third_party/And64InlineHook.cpp \
    -llog -o "$OUT/zip/zygisk/arm64-v8a.so"

cp module/module.prop module/customize.sh LICENSE THIRD_PARTY_NOTICES.md "$OUT/zip/"
mkdir -p "$OUT/zip/licenses"
cp native/third_party/LICENSE.* "$OUT/zip/licenses/"
mkdir -p "$OUT/zip/META-INF/com/google/android"
curl -fsSL -o "$OUT/zip/META-INF/com/google/android/update-binary" \
    https://raw.githubusercontent.com/topjohnwu/Magisk/master/scripts/module_installer.sh
echo "#MAGISK" > "$OUT/zip/META-INF/com/google/android/updater-script"

VER=$(sed -n 's/^version=//p' module/module.prop)
(cd "$OUT/zip" && rm -f "../XiaomiPlayStoreFix-$VER.zip" && zip -qr9 "../XiaomiPlayStoreFix-$VER.zip" .)
echo "built $OUT/XiaomiPlayStoreFix-$VER.zip"
