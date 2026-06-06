# Copyright (c) 2026 Krzysztof Gawryś
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(ch32x035_dfu_boot C ASM)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

option(BUILD_EXAMPLE_APP "Build the example application image" ON)

set(MACHINE_OPTIONS -march=rv32imac -mabi=ilp32)

add_compile_options(${MACHINE_OPTIONS}
	-fdata-sections -ffunction-sections -fsigned-char -nostartfiles
	-Wall -Wextra -Os -g)
add_compile_definitions(CH32X035)
include_directories(
	${CMAKE_CURRENT_SOURCE_DIR}          # config.h
	sdk sdk/Core sdk/Debug sdk/Peripheral/inc)
add_link_options(${MACHINE_OPTIONS}
	-nostartfiles -specs=nano.specs -specs=nosys.specs -lc -lm
	-Wl,--gc-sections -Wl,--print-memory-usage)

# common SDK pieces both images need
set(SDK_COMMON
	sdk/Core/core_riscv.c
	sdk/Debug/debug.c
	sdk/system_ch32x035.c
	sdk/Peripheral/src/ch32x035_gpio.c
	sdk/Peripheral/src/ch32x035_rcc.c
	sdk/Peripheral/src/ch32x035_misc.c)

function(wch_finalize tgt ldscript)
	target_link_options(${tgt} PRIVATE -T${ldscript} -Wl,-Map=${tgt}.map,--cref)
	set_target_properties(${tgt} PROPERTIES LINK_DEPENDS ${ldscript})
	add_custom_command(TARGET ${tgt} POST_BUILD
		COMMAND ${CMAKE_SIZE_UTIL} ${tgt}.elf
		COMMAND ${CMAKE_OBJCOPY} -O ihex   ${tgt}.elf ${tgt}.hex
		COMMAND ${CMAKE_OBJCOPY} -O binary ${tgt}.elf ${tgt}.bin
		BYPRODUCTS ${tgt}.hex ${tgt}.bin VERBATIM)
endfunction()

# ---- Bootloader (runs at BOOT_BASE) ----
add_executable(bootloader
	boot/startup_boot.S
	boot/main.c
	boot/dfu.c
	sdk/Peripheral/src/ch32x035_flash.c
	${SDK_COMMON})
wch_finalize(bootloader ${CMAKE_CURRENT_SOURCE_DIR}/ld/boot.ld)

# ---- Example application (runs at APP_BASE) ----
if(BUILD_EXAMPLE_APP)
	add_executable(app
		example-app/main.c
		sdk/Startup/startup_ch32x035.S
		${SDK_COMMON})
	wch_finalize(app ${CMAKE_CURRENT_SOURCE_DIR}/ld/app.ld)
endif()
