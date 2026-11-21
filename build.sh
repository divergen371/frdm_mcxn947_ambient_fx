#!/usr/bin/env zsh
set -euo pipefail

export PATH="/Users/atsushi/.mcuxpressotools/.mcux-venv-3.12/bin:$PATH"
export ARMGCC_DIR="/Users/atsushi/.mcuxpressotools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi"

SDK_WS="/Users/atsushi/MCXN947/frdm_mcxn947"
APP_DIR="/Users/atsushi/frdm_mcxn947_ambient_fx"
BUILD_DIR="${APP_DIR}/build"

cd "${SDK_WS}"
west build -b frdmmcxn947 "${APP_DIR}" -Dcore_id=cm33_core0 --config release -d "${BUILD_DIR}" "$@"
echo "Build artifacts:"
ls -lh "${BUILD_DIR}"/frdm_mcxn947_ambient_fx_cm33_core0.{elf,bin,hex} 2>/dev/null || true
