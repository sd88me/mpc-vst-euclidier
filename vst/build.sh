#!/usr/bin/env bash
# Build Euclidier as a VST2 plugin for the MPC OS plugin host (armhf), plus an
# x86 build of the standalone engine + a host_test binary for offline testing.
#   vst/build/euclidier.so          -> /sdcard/vst/ on the device
#   vst/build/pluginlist-entry.xml  the <PLUGIN> line for MPC.settings' pluginList-arm
#   vst/build/skin/                 -> /sdcard/Synths/ on the device
#   vst/build/host_test             offline ASan test (see docs/PORTING.md "Offline test first")
# Not a Schwung DSP quick-start port (MIDI generator with host-side glue, docs/PORTING.md
# classification 0/1): euclidier.cpp is a monolithic standalone app, so euclidier_vst.cpp
# spawns the existing binary and drives it over its control socket + ALSA seq, instead of
# linking an engine library. vst.json/module.json only feed tools/gen_vst.py for params.h
# + the skin. The engine sources under ../src are vendored from force-euclidier (see
# ../src/VENDORED.md) so this port builds standalone, without mounting that repo.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="${MPC_VST:-../../mpc-vst}"
U="$(id -u):$(id -g)"
mkdir -p build

# 1. skin inputs (params.json, layout.conf, images/) from make_skin.py, then params.h + the skin (browser renderer:
#    vst.json "art": "html"; the mpc-vst-html-art image has Pillow + headless Chromium)
docker build -q -t mpc-vst-html-art "$MPC_VST/tools/html_art" >/dev/null
docker run --rm -u "$U" -e HOME=/tmp -v "$PWD":/w -w /w mpc-vst-html-art python3 make_skin.py
docker run --rm -u "$U" -e HOME=/tmp -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w mpc-vst-html-art \
  python3 /mv/tools/gen_vst.py vst.json

cp "$MPC_VST/wrapper/popup.h" build/   # popup open-flag handling shared with mpc-vst's own wrapper

# 3. (the host test uses fake_engine.py, which speaks the engine's control-socket protocol: the real engine needs an
#    ALSA sequencer, which the test container does not have)

# 4. wrapper + host_test, x86, ASan (see PORTING.md "Offline test first")
docker run --rm -v "$PWD/..":/b -w /b/vst gcc:12 bash -euxc '
  apt-get update -qq && apt-get install -y -qq libasound2-dev >/dev/null
  g++ -O0 -g -fsanitize=address,undefined -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      -Ibuild -shared -fPIC -o build/euclidier-x86.so euclidier_vst.cpp -lasound -lpthread
  gcc -O0 -g -fsanitize=address,undefined -Ibuild -o build/host_test host_test.c \
      build/euclidier-x86.so -ldl
  chown -R '"$U"' build
'
echo "-- run host_test (spawns build/euclidier-x86) --"
docker run --rm -v "$PWD/..":/b -w /b/vst -e EUCLIDIER_BIN=/b/vst/fake_engine.py \
  -e LD_LIBRARY_PATH=/b/vst/build -e ASAN_OPTIONS=detect_leaks=0 gcc:12 bash -euxc '
  apt-get update -qq && apt-get install -y -qq libasound2 >/dev/null
  ./build/host_test
'

# 5. the plugin (armhf, glibc 2.36 so it loads on the device's 2.39) -- wrapper only,
#    no engine sources: it spawns the already-deployed on-device binary.
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/b -w /b/vst arm32v7/gcc:12 bash -euxc '
  set -x
  apt-get update -qq && apt-get install -y -qq libasound2-dev >/dev/null
  mkdir -p build/obj
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      -Ibuild -c euclidier_vst.cpp -o build/obj/vst.o
  g++ -shared -o build/euclidier.so build/obj/vst.o \
      -static-libstdc++ -static-libgcc -lasound -lpthread -lm
  strip build/euclidier.so
  echo "-- exported --"; readelf --dyn-syms -W build/euclidier.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/euclidier.so | grep NEEDED
  echo "-- highest glibc (device has 2.39) --"; readelf -V build/euclidier.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/euclidier.so
