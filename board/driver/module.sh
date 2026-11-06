#!/bin/sh
# 构建 sg3000_rx.ko。迁移自 legacy build/module.sh，只改了源文件路径。
# Run in a Linux build environment with the pinned, prepared kernel tree.
# This does not install or load the resulting module.
set -eu
: "${KDIR:?Set KDIR to the prepared linux-xlnx b0c1be301e78c320df8c4d93b18393bfd7fd4e9d tree}"
: "${KERNEL_ANALYSIS:?Set KERNEL_ANALYSIS to the directory containing verified Image and symbols.json}"
CROSS_COMPILE=${CROSS_COMPILE:-aarch64-none-elf-}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$here/output/module"
cp "$here/sg3000_rx.c" "$here/../../include/sg3000_rx_uapi.h" "$here/output/module/"
cp "$here/Kbuild" "$here/output/module/Makefile"
make -C "$KDIR" M="$here/output/module" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" LOCALVERSION= KBUILD_MODPOST_WARN=1 modules
# No Module.symvers workaround is accepted without checking the actual target exports.
python3 "$here/verify_module.py" --kernel-dir "$KERNEL_ANALYSIS" --module "$here/output/module/sg3000_rx.ko" --output "$here/output/module-abi-check.json"
printf 'Compile and target ABI checks passed. Hardware validation is still required.\n'
