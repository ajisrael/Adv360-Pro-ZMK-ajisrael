#!/bin/bash
# Simulates the keyboard and two hosts in BabbleSim (ZMK's BLE test setup,
# see zmk/app/run-ble-test.sh) and compares what happens with snapshot.log.
#
# Run inside the ZMK build container, from a west workspace whose manifest is
# this repo's config/ and that has the babblesim group enabled:
#   west config manifest.group-filter -- +babblesim && west update
#   (cd tools/bsim && make everything)
#   tests/host-select/run.sh <workspace> [ZMK_TESTS_AUTO_ACCEPT=1 to update snapshots]
set -euo pipefail

ws=$(realpath "${1:?usage: run.sh <west workspace>}")
here=$(realpath "$(dirname "$0")")
export ZEPHYR_BASE=$ws/zephyr
# Same as `west zephyr-export`, without writing to ~/.cmake.
export CMAKE_PREFIX_PATH=$ZEPHYR_BASE/share/zephyr-package/cmake
export BSIM_OUT_PATH=$ws/tools/bsim BSIM_COMPONENTS_PATH=$ws/tools/bsim/components
build=$ws/build/host-select-tests
mkdir -p "$build"

west build -p -s "$here/central" -d "$build/central" -b nrf52_bsim > "$build/central.log" 2>&1 ||
    { tail -30 "$build/central.log"; exit 1; }
cp "$build/central/zephyr/zephyr.exe" "$BSIM_OUT_PATH/bin/ble_test_host_select_central.exe"

status=0
for testcase in "$here"/*/; do
    testcase=${testcase%/}
    [ -f "$testcase/nrf52_bsim.keymap" ] || continue
    name=host_select_$(basename "$testcase" | tr - _)
    out=$build/$name
    west build -p -s "$ws/zmk/app" -d "$out" -b nrf52_bsim -- -DZMK_CONFIG="$testcase" > "$out.build.log" 2>&1 ||
        { tail -30 "$out.build.log"; echo "FAILED: $name did not build"; status=1; continue; }
    cp "$out/zephyr/zmk.exe" "$BSIM_OUT_PATH/bin/$name"
    (
        cd "$BSIM_OUT_PATH/bin"
        ./"$name" -d=0 -s="$name" > "$out.log" 2>&1 &
        ./bs_device_handbrake -s="$name" -d=1 -r=10 > /dev/null &
        centrals=0
        while IFS= read -r line; do
            $line -s="$name" > "$out.central$centrals.log" 2>&1 &
            centrals=$((centrals + 1))
        done < "$testcase/centrals.txt"
        ./bs_2G4_phy_v1 -s="$name" -D=$((2 + centrals)) -sim_length=20e6 > /dev/null 2>&1
        # Every device exits when the simulation ends; a failure means a crash.
        for job in $(jobs -p); do
            wait "$job" || echo "a simulated device exited with status $?" >> "$out.log"
        done
    )
    # Interleave the devices' logs by simulated time (the 15 characters after
    # the @), keeping each device's own order for equal times.
    cat "$out.log" "$out".central*.log | sort -s -t@ -k2.1,2.15 |sed -E -n -f "$testcase/events.patterns" > "$out.filtered.log"
    if diff -u "$testcase/snapshot.log" "$out.filtered.log"; then
        echo "PASS: $name"
    elif [ -n "${ZMK_TESTS_AUTO_ACCEPT:-}" ]; then
        cp "$out.filtered.log" "$testcase/snapshot.log"
        echo "ACCEPTED: $name"
    else
        echo "FAILED: $name"
        status=1
    fi
done
exit $status
