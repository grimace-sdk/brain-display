#!/bin/sh
set -eu

usage="Usage: EMMC_DUMP=... BSTART=... MODULES=... DRIVER=... PWSH2_BOOT=... PW_SH2=... OUTPUT=... tools/bundle-pwsh2.sh"
: "${EMMC_DUMP:?$usage}" "${BSTART:?$usage}" "${MODULES:?$usage}" "${DRIVER:?$usage}" "${PWSH2_BOOT:?$usage}" "${PW_SH2:?$usage}" "${OUTPUT:?$usage}"

mkdir -p "$OUTPUT"

set --
if [ -n "${DEBUG_LOG:-}" ]; then
    set -- --debug-log "$DEBUG_LOG" --debug-log-address "${DEBUG_LOG_ADDRESS:-401e0000}"
fi

node "$PWSH2_BOOT/tools/make-boot-nkp.mjs" --image "$EMMC_DUMP" --slot B --bstart "$BSTART" --skip \
    --replace-module "board.dll=$MODULES/board.dll" \
    --replace-module "ddraw_mx28.dll=$DRIVER" \
    --replace-module "touchraw.dll=$MODULES/touchraw.dll" \
    --registry "$PWSH2_BOOT/ce/drivers/board/board.reg" \
    --registry "$PW_SH2/card/patches/crash-handler.reg" \
    --patch-words "$PW_SH2/card/patches/ymu818-volume.json" \
    --output "$OUTPUT/boot.nkp" "$@"
