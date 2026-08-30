# Cross-compiles Windows arm64 from any host llvm-mingw runs on.
#
# Prereqs:
#   llvm-mingw release tarball (github.com/mstorsjo/llvm-mingw) on PATH, for
#     aarch64-w64-mingw32-clang/-windres
#   AARCH64_MINGW_SYSROOT pointing at an aarch64 Windows include/ + lib/ tree
#     (openssl, curl, zlib, libuv, libffi headers and .dll.a import libs).
#     MSYS2's clangarm64 packages provide these - install them on a Windows
#     x64 box via pacman (mingw-w64-clang-aarch64-{openssl,curl,zlib,libuv,
#     libffi}), the compiler itself in that package set can't run on an x64
#     host, but the library files are just data.
#   The matching runtime .dll files (same MSYS2 clangarm64/bin) alongside the
#     built sxn.exe to actually run it - link-time .dll.a stubs aren't enough.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-w64-mingw32-clang)
set(CMAKE_RC_COMPILER aarch64-w64-mingw32-windres)

# MSYS2's clangarm64 packages are genuine aarch64 Windows binaries but can't
# run as the compiler itself on an x64 host - only usable as link-time data
# (headers/.a/.dll.a), pulled down separately into AARCH64_MINGW_SYSROOT.
if(NOT DEFINED AARCH64_MINGW_SYSROOT OR AARCH64_MINGW_SYSROOT STREQUAL "")
  set(AARCH64_MINGW_SYSROOT "$ENV{AARCH64_MINGW_SYSROOT}")
endif()
if(NOT EXISTS "${AARCH64_MINGW_SYSROOT}/include" OR NOT EXISTS "${AARCH64_MINGW_SYSROOT}/lib")
  message(FATAL_ERROR "AARCH64_MINGW_SYSROOT='${AARCH64_MINGW_SYSROOT}' missing include/ or lib/")
endif()
set(CMAKE_FIND_ROOT_PATH "${AARCH64_MINGW_SYSROOT}")
include_directories(SYSTEM "${AARCH64_MINGW_SYSROOT}/include")
link_directories("${AARCH64_MINGW_SYSROOT}/lib")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
