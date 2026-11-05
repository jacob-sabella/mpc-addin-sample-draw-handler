#!/usr/bin/env bash
# The addin for the MPC Sample (aarch64): build/aarch64/mpc_sample_draw_handler.so, with addin.manifest and the default
# settings beside it. Uses the aarch64-linux-gnu cross compiler when present, else Docker with QEMU for arm64.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/aarch64
VER=$(cat VERSION)
F="-std=gnu11 -O2 -Wall -Wextra -Werror -mcpu=cortex-a55 -DADDIN_VERSION=\"$VER\""
O="-fPIC -shared -fvisibility=hidden -Wl,-z,defs -Wl,--as-needed -o build/aarch64/mpc_sample_draw_handler.so src/sdraw.c src/ctl.c src/input.c -ldl -lpthread"
if command -v aarch64-linux-gnu-gcc >/dev/null && [ -z "${DOCKER:-}" ]; then
  # shellcheck disable=SC2086
  aarch64-linux-gnu-gcc $F $O && aarch64-linux-gnu-strip --strip-unneeded build/aarch64/mpc_sample_draw_handler.so
  OBJDUMP=aarch64-linux-gnu-objdump; NM=aarch64-linux-gnu-nm
else
  docker run --rm --platform linux/arm64 -u "$(id -u):$(id -g)" -v "$PWD":/b -w /b -e F="$F" -e O="$O" arm64v8/gcc:12-bookworm \
    sh -c 'gcc $F $O && strip --strip-unneeded build/aarch64/mpc_sample_draw_handler.so'
  OBJDUMP=objdump; NM=nm
fi
max=$($OBJDUMP -T build/aarch64/mpc_sample_draw_handler.so | grep -o "GLIBC_[0-9.]*" | sort -t. -k2 -n -u | tail -n 1)
echo "highest glibc symbol version: $max"
case "$max" in GLIBC_2.[0-9]|GLIBC_2.[123][0-9]) ;; *) echo "too new for the MPC Sample (glibc 2.39): $max" >&2; exit 1 ;; esac
echo "exports:"; $NM -D --defined-only build/aarch64/mpc_sample_draw_handler.so | awk '$2 == "T" {print "  " $3}'
cp -f mpc_sample_draw_handler.conf addin.manifest build/aarch64/
ls -l build/aarch64/mpc_sample_draw_handler.so
