#!/bin/sh
# Builds core and then compiles and runs every phasing test suite against it.
#
#   ./core/test/run_tests.sh [build-dir]
#
# Always rebuild through this script rather than reusing an old test binary. The suites
# link against libsdrpp_core, and a binary built before core was rebuilt will happily run
# against a library whose class layouts have moved underneath it -- which shows up as a
# segfault or a "mutex lock failed" in code that is actually fine.

set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-"$REPO/build"}
CORE_LIB="$BUILD/core"
OUT=$(mktemp -d)

if [ ! -d "$CORE_LIB" ]; then
    echo "No core library directory at $CORE_LIB -- pass the build dir as the first argument."
    exit 1
fi

# Homebrew prefix for volk/fftw headers; adjust if yours differs.
BREW=${BREW_PREFIX:-/opt/homebrew}

echo "Building core..."
cmake --build "$BUILD" --target sdrpp_core -j8 > "$OUT/build.log" 2>&1 || {
    echo "core build FAILED:"; tail -30 "$OUT/build.log"; exit 1;
}

# Suites that need to link against core.
LINKED="$REPO/core/test/test_phaser.cpp
$REPO/core/test/test_phasing.cpp
$REPO/core/test/test_tube_warmth.cpp
$REPO/core/test/test_wav_meta.cpp
$REPO/core/test/test_wav_roundtrip.cpp
$REPO/core/test/test_decorrelation.cpp
$REPO/core/test/test_wideband_decorrelation.cpp
$REPO/source_modules/phasing_test_source/test/test_worker.cpp"

# Suites with no dependency beyond the standard library (and BSD sockets/pthreads, which
# count as "standard library" on every platform this actually runs on locally).
STANDALONE="$REPO/source_modules/phasing_test_source/test/test_signal_model.cpp
$REPO/source_modules/rsr200_source/test/test_protocol.cpp
$REPO/source_modules/rsr200_source/test/test_device.cpp
$REPO/source_modules/rsr200_source/test/test_lan_transport.cpp
$REPO/core/test/test_linrad_raw.cpp
$REPO/core/test/test_crowded_band.cpp
$REPO/misc_modules/recorder/test/test_recording_queue.cpp"

fail=0

run_one() {
    name=$(basename "$1" .cpp)
    # The suites spin up worker threads; if one wedges, do not hang the whole run.
    "$OUT/$name" > "$OUT/$name.out" 2>&1 &
    pid=$!
    i=0
    while kill -0 $pid 2>/dev/null; do
        i=$((i + 1))
        if [ $i -gt 60 ]; then
            kill $pid 2>/dev/null
            echo "  $name: TIMED OUT"
            fail=1
            return
        fi
        sleep 1
    done
    wait $pid 2>/dev/null || true
    result=$(grep -aE '^(PASSED|FAILED)' "$OUT/$name.out" || echo "NO RESULT")
    case "$result" in
        PASSED*) echo "  $name: $result" ;;
        *)       echo "  $name: $result"; fail=1; sed 's/^/      /' "$OUT/$name.out" | tail -25 ;;
    esac
}

echo "Compiling and running suites..."
for src in $STANDALONE; do
    name=$(basename "$src" .cpp)
    c++ -std=c++17 -O2 -pthread -I"$REPO/core/src" -o "$OUT/$name" "$src" || { echo "  $name: COMPILE FAILED"; fail=1; continue; }
    run_one "$src"
done

for src in $LINKED; do
    name=$(basename "$src" .cpp)
    c++ -std=c++17 -O2 -I"$REPO/core/src" -I"$BREW/include" -o "$OUT/$name" "$src" \
        -L"$CORE_LIB" -lsdrpp_core -L"$BREW/lib" -lvolk -Wl,-rpath,"$CORE_LIB" \
        || { echo "  $name: COMPILE FAILED"; fail=1; continue; }
    run_one "$src"
done

rm -rf "$OUT"
if [ $fail -ne 0 ]; then
    echo "\nSome suites failed."
    exit 1
fi
echo "\nAll suites passed."
