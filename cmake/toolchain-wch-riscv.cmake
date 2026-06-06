# Copyright (c) 2026 Krzysztof Gawryś
# SPDX-License-Identifier: Apache-2.0
#
# Toolchain file for the WCH RISC-V embedded GCC (riscv-wch-elf-).
# The compiler must be reachable through PATH, e.g.
#   /opt/wch/Toolchain/RISC-V_Embedded_GCC12/bin

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

set(TOOLCHAIN_PREFIX riscv-wch-elf-)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}objcopy CACHE INTERNAL "")
set(CMAKE_OBJDUMP      ${TOOLCHAIN_PREFIX}objdump CACHE INTERNAL "")
set(CMAKE_SIZE_UTIL    ${TOOLCHAIN_PREFIX}size    CACHE INTERNAL "")

# Do not try to link a full executable when probing the compiler; this is a
# bare-metal target with a custom linker script.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
