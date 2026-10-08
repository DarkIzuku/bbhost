#!/bin/bash
# Cross-compiles the host-side unit tests for the Windows target (clang +
# lld against the mingw sysroot, static) and runs them under wine: the thunk
# trampolines, the TlsAlloc slot read through GS, setjmp/longjmp, and the
# guest va_list walker with its asm shims, all on Windows ABI semantics.
#   tools/win_tests.sh [OUTDIR]     (default build/win)
set -uo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/win}
mkdir -p "$out"
export WINEPREFIX=${WINEPREFIX:-$PWD/$out/wine} WINEDEBUG=${WINEDEBUG:--all}
# Use the installed sysroot, like win_deps.sh. A fixed GCC 16.1 path
# stopped finding the standard headers after Arch updated mingw-w64.
mingw_gcc_dir=$(ls -d /usr/lib/gcc/x86_64-w64-mingw32/*/ | sort -V | tail -1)
mingw_gcc_dir=${mingw_gcc_dir%/}
mingw_gcc_version=${mingw_gcc_dir##*/}
cxx() {
  local exe=$1; shift
  clang++ --target=x86_64-w64-mingw32 --sysroot=/usr/x86_64-w64-mingw32 \
    -isystem "/usr/x86_64-w64-mingw32/include/c++/$mingw_gcc_version" \
    -isystem "/usr/x86_64-w64-mingw32/include/c++/$mingw_gcc_version/x86_64-w64-mingw32" \
    -std=c++20 -Wno-invalid-constexpr -w -O1 -g -Isrc -fuse-ld=lld \
    -L/usr/x86_64-w64-mingw32/lib -L"$mingw_gcc_dir" -static "$@" -lwinpthread -lwinmm -o "$exe"
}
fail=0
run() {
  local name=$1; shift
  if ! cxx "$out/$name.exe" "$@"; then echo "$name: build failed"; fail=1; return; fi
  timeout 120 wine "$out/$name.exe" > "$out/$name.log" 2>&1
  local status=$?
  grep -v "pci id\|libEGL\|^$" "$out/$name.log" || true
  if [ "$status" = 0 ]; then
    echo "$name: ok under wine"
  else
    echo "$name: FAILED under wine"; fail=1
  fi
}
run sysv_va_test tests/sysv_va_test.cpp src/hle/sysv_va.cpp
run game_installation_test tests/game_installation_test.cpp src/core/game_installation.cpp src/core/game_folders.cpp src/core/sfo.cpp src/core/sha256.cpp
run upscale_policy_test tests/upscale_policy_test.cpp src/host/upscale_policy.cpp
run tess_lds_test tests/tess_lds_test.cpp src/host/tess_lds.cpp
run debug_font_test -Iplugins/debug_menu -DBB_FONT14_PATH=\"plugins/debug_menu/font14.bin\" tests/debug_font_test.cpp
run thunk_args tests/thunk_args.cpp src/core/thunk.cpp src/core/tls_rewrite.cpp src/core/portable.cpp src/hle/sysv_va.cpp src/host/main_wait.cpp
wineserver -k 2>/dev/null
exit $fail
