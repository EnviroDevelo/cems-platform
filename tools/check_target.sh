#!/bin/sh
# Compile and link the target sources in each hardware configuration (quiet).
#   tools/check_target.sh [EXT=/path/to/ext]
set -e
for cfg in "" "-DCHECK_WITH_I2C_FRAM" "-DCHECK_WITH_FRAM" "-DCHECK_WITH_I2C_FRAM -DCHECK_WITH_NOR" "-DCHECK_WITH_FRAM -DCHECK_WITH_NOR" "-DCHECK_WITH_I2C_FRAM -DCHECK_WITH_NOR -DCHECK_WITH_EVE"; do
    rm -rf build/target
    if ! make -s -f port/stm32h5/check.mk link -j8 "$@" DEV="-DSTM32H573xx -DUSE_HAL_DRIVER $cfg" > build/check.log 2>&1; then
        grep -E "error|undefined" build/check.log | head -20; echo "FAILED: [$cfg]"; exit 1
    fi
    printf '%-45s %s\n' "[${cfg:-no FRAM, no NOR}]" "$(tail -1 build/check.log | awk '{print "text+data", $1+$2, "bss+data", $2+$3}')"
done
# with the real application, when its directory is in this tree (request R5)
APPMAP=""
for app in app_vmmux; do
    if [ -f "$app/app_vars.mk" ]; then
        rm -rf build/target_app
        if ! make -s -f port/stm32h5/check.mk link -j8 "$@" APP=$app OUT=build/target_app \
             DEV="-DSTM32H573xx -DUSE_HAL_DRIVER -DCHECK_WITH_I2C_FRAM -DCHECK_WITH_NOR -DCHECK_WITH_EVE" > build/check_app.log 2>&1; then
            grep -E "error|undefined" build/check_app.log | head -20; echo "FAILED: [$app]"; exit 1
        fi
        printf '%-45s %s\n' "[$app, fullest]" "$(tail -1 build/check_app.log | awk '{print "text+data", $1+$2, "bss+data", $2+$3}')"
        APPMAP="build/target_app/cems_check.map"
    fi
done
if [ -n "$APPMAP" ]; then
    python3 tools/mem_report.py build/target/cems_check.map docs/MEMORY.md "$APPMAP" "VM/MUX application (app_vmmux)"
else
    python3 tools/mem_report.py build/target/cems_check.map docs/MEMORY.md
fi
