#!/usr/bin/env bash
# Build the Allwinner A733 EDK2 firmware (A733.fd) on Linux / WSL.
#
# Usage:  build/build_a733.sh [output.fd]
#
# Environment (all optional):
#   EDK2_ROOT      edk2 checkout (default: $HOME/edk2). The platform package must be reachable as
#                  $EDK2_ROOT/Platform/Allwinner/A733Pkg  (symlink it to src/edk2/Platform/Allwinner/A733Pkg).
#   EDK2_PLATFORMS extra PACKAGES_PATH entry (default: $HOME/edk2-platforms)
#   CROSS_PREFIX   AArch64 GCC prefix (default: aarch64-linux-gnu-)
#   MEM_SIZE       RAM size in bytes, hex  (default: 0x200000000 = 8 GiB; use 0x80000000 for 2 GiB)
#   SMHC_UHS_LEVEL SD host UHS level PCD   (default: 3 = SDR50)
#   CPU_LN, CPU_VOLTAGE_CODE  CPU PLL / voltage PCDs (defaults 54 / 45)
#   BUILD_LOG      log file (default: /tmp/a733_build.log)
set -euo pipefail

EDK2_ROOT="${EDK2_ROOT:-$HOME/edk2}"
EDK2_PLATFORMS="${EDK2_PLATFORMS:-$HOME/edk2-platforms}"
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
MEM_SIZE="${MEM_SIZE:-0x200000000}"
SMHC_UHS_LEVEL="${SMHC_UHS_LEVEL:-3}"
CPU_LN="${CPU_LN:-54}"
CPU_VOLTAGE_CODE="${CPU_VOLTAGE_CODE:-45}"
BUILD_LOG="${BUILD_LOG:-/tmp/a733_build.log}"
OUT="${1:-}"

export WORKSPACE="$EDK2_ROOT"
export EDK_TOOLS_PATH="$EDK2_ROOT/BaseTools"
export GCC_AARCH64_PREFIX="$CROSS_PREFIX"
export PACKAGES_PATH="$EDK2_ROOT:$EDK2_PLATFORMS"
export PYTHON_COMMAND=python3
export PYTHONPATH="$EDK2_ROOT/BaseTools/Source/Python"
export PATH="$EDK2_ROOT/BaseTools/BinWrappers/PosixLike:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

cd "$EDK2_ROOT"

# Regenerate every module so PCD changes always take effect.
rm -rf "$EDK2_ROOT/Build/A733"
# Make __TIME__/__DATE__ in the SEC banner reflect this build.
touch "$EDK2_ROOT/ArmPlatformPkg/PeilessSec/PeilessSec.c"

python3 "$EDK2_ROOT/BaseTools/Source/Python/build/build.py" \
  -a AARCH64 -t GCC \
  -p Platform/Allwinner/A733Pkg/A733Pkg.dsc \
  -b DEBUG \
  --pcd gA733TokenSpaceGuid.PcdA733HdmiTestPatternEnable=TRUE \
  --pcd gA733TokenSpaceGuid.PcdA733PhysicalGopEnable=TRUE \
  --pcd gArmTokenSpaceGuid.PcdSystemMemorySize="$MEM_SIZE" \
  --pcd gA733TokenSpaceGuid.PcdSmhcUhsLevel="$SMHC_UHS_LEVEL" \
  --pcd gA733TokenSpaceGuid.PcdA733CpuLN="$CPU_LN" \
  --pcd gA733TokenSpaceGuid.PcdA733CpuVoltageCode="$CPU_VOLTAGE_CODE" \
  > "$BUILD_LOG" 2>&1 || { echo "build failed, see $BUILD_LOG"; tail -20 "$BUILD_LOG"; exit 1; }

FD="$EDK2_ROOT/Build/A733/DEBUG_GCC/FV/A733.fd"
tail -3 "$BUILD_LOG"
ls -l "$FD"
sha256sum "$FD"
if [ -n "$OUT" ]; then cp "$FD" "$OUT"; echo "copied to $OUT"; fi
