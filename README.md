# ch32x035-dfu-boot

A small **USB DFU 1.1 bootloader for the CH32X035** (QingKe V4C, RV32IMAC),
compatible with standard `dfu-util`. Flash your application over plain USB - no
programmer needed after the bootloader is installed once.

There is no off-the-shelf standard-DFU bootloader for this part; the WCH factory
ROM bootloader speaks a proprietary protocol (wchisp), not USB DFU.

## Flash layout (62 KB flash, 20 KB RAM)

```
0x00000000  +------------------+
            |   bootloader     |  8 KB   (BOOT_SIZE)   ld/boot.ld
0x00002000  +------------------+
            |   application    |  54 KB  (APP_SIZE)    ld/app.ld
0x0000F800  +------------------+
RAM top word 0x20004FF0 reserved as the boot/app hand-off flag.
```
All sizes/addresses live in [`config.h`](config.h) and must match the linker
scripts in [`ld/`](ld).

## Build

Needs the WCH RISC-V toolchain (`riscv-wch-elf-*`) on PATH, CMake + Ninja.

```
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-wch-riscv.cmake
cmake --build build
# -> build/bootloader.bin  and  build/app.bin (example app)
```

## Install (once, via WCH-Link)

```
./flash.sh                      # writes bootloader + app together
# or: OPENOCD=/path/to/openocd ./flash.sh  build/your_app.bin
```

## Update the application over USB (DFU)

1. Put the board in DFU mode (see *Entering DFU* below).
2. With `dfu-util`:
   ```
   dfu-util -l                                   # shows "CH32X035 DFU" (1a86:8035)
   dfu-util -a 0 -d 1a86:8035 -D build/app.bin -R # download + reboot to app
   dfu-util -a 0 -d 1a86:8035 -U dump.bin         # (optional) read back
   ```
The board reboots into the new application automatically after download.

## Entering DFU

Any of (configurable in `config.h`):
- **From the app**: call `request_dfu()` (writes the RAM magic flag + resets).
  See [`example-app/main.c`](example-app/main.c).
- **Button at reset**: set `DFU_BUTTON_ENABLE 1` and the pin; hold at reset.
- **Empty app** (`DFU_ENTER_IF_NO_APP`): bootloader stays in DFU if the app
  region is blank - an anti-brick fallback.

## Putting your own app on top

1. **Link at `APP_BASE` (0x2000)** with [`ld/app.ld`](ld/app.ld). That script
   also caps RAM at `0x4FF0`, leaving the hand-off flag word at
   `BOOT_FLAG_ADDR` (0x20004FF0) untouched - keep this if you write your own
   linker, or `request_dfu()` won't survive the reset.
2. **Use the stock WCH startup** (`sdk/Startup/startup_ch32x035.S`) - it sets
   `mtvec` to its own vector table via `la`, so running from 0x2000 just works
   (no manual vector relocation). The bootloader hands off in M-mode, so the
   app's normal `mret`-to-User startup behaves exactly as on a bare chip.
3. **Produce a raw binary** (`objcopy -O binary app.elf app.bin`). DFU block 0
   maps to `APP_BASE`, so the `.bin` is the app image with no offset/header.
   (`.hex` is not used by `dfu-util`.)
4. Flash it: `dfu-util -a 0 -d 1a86:8035 -D app.bin -R`.
5. Optionally call `request_dfu()` from your app to re-enter DFU on demand.

The `CMakeLists.txt` example-app target shows the full recipe (sources +
`ld/app.ld` + `objcopy`); copy it and swap in your sources.

## How it works / notes

- The bootloader runs at reset, decides DFU vs app, and **hands off in Machine
  mode**. (The stock WCH startup ends with `mret` to User mode; in a
  bootloader->app chain the app's `csrw` setup would then trap illegal, so the
  bootloader uses its own `startup_boot.S` that stays in M-mode - `jal main`.)
- DFU transfer size is 64 B (one EP0 packet); blocks are buffered into 256-byte
  flash pages and programmed with `FLASH_ROM_ERASE/WRITE`.
- After the host reads the post-manifest status, the bootloader busy-waits
  `DFU_MANIFEST_REBOOT_LOOPS` iterations (~1 s; USB IRQs keep being serviced so
  `dfu-util` finishes cleanly) and then resets into the new app. A busy loop is
  used on purpose - `Delay_Ms` can be mis-scaled in the bootloader, and a
  `dfu-util -R` USB reset is not relied upon (it may not reach the device as a
  bus reset, e.g. over usbip).

## Limitations / TODO

- No DFU file-suffix/CRC checking (raw `.bin` only; `dfu-util` warns - harmless).
- Flash erase/write runs in the USB ISR (blocking, a few ms); fine at 64-B
  blocks but not a true `dfuDNBUSY`+poll implementation.
- `USB_VID 0x1A86` is WCH's Vendor ID - OK for private use; use your own for a
  product (see `config.h`).
- Depends on the bundled WCH StdPeriph SDK (`sdk/`, WCH license, "for WCH
  microcontrollers").

## Layout

```
config.h            all knobs (addresses, VID/PID, triggers)
boot/               bootloader: main.c, dfu.c, startup_boot.S
ld/                 boot.ld, app.ld (linker scripts)
example-app/        minimal blink app linked at APP_BASE
sdk/                bundled WCH StdPeriph SDK (Core/Debug/Peripheral/Startup)
cmake/              toolchain file
openocd/            wch-riscv.cfg (WCH-Link)
flash.sh            flash bootloader + app via WCH-Link
```

## License

This project's own source - `boot/main.c`, `boot/dfu.c`, `config.h`,
`example-app/main.c`, `CMakeLists.txt`, `cmake/toolchain-wch-riscv.cmake`,
`flash.sh` - is **Copyright (c) 2026 Krzysztof Gawryś**, licensed under the
**Apache License 2.0** (see [LICENSE](LICENSE); files carry SPDX headers).

**Exception - WCH vendor code is NOT Apache-licensed:**

- `sdk/` is the WCH StdPeriph SDK; `boot/startup_boot.S` and the `ld/*.ld`
  linker scripts are derived from WCH files. These carry their original headers:
  *"Copyright (c) Nanjing Qinheng Microelectronics Co., Ltd. This software
  (modified or not) and binary are used for microcontroller manufactured by
  Nanjing Qinheng Microelectronics."*
- That is a **field-of-use** grant (use/modify/redistribute, but for use with
  WCH microcontrollers), **not** an OSI/Apache license, and it is **not**
  relicensed here. WCH publishes this SDK itself in
  [openwch/ch32x035](https://github.com/openwch/ch32x035) (which has no LICENSE
  file), so redistributing it for CH32X035 development is intended; bundling it
  here is for convenience.

Provenance of the bundled files (from `openwch/ch32x035`):
`sdk/{Core,Debug,Peripheral,Startup}` come from `EVT/EXAM/SRC/`, and
`sdk/{system_ch32x035.c,system_ch32x035.h,ch32x035_conf.h}` from an EVT example's
`User/` folder (e.g. `EVT/EXAM/GPIO/GPIO_Toggle/User/`); `boot/startup_boot.S`
is `EVT/EXAM/SRC/Startup/startup_ch32x035.S` with the reset tail changed to stay
in M-mode; `ld/*.ld` are `EVT/EXAM/SRC/Ld/Link.ld` with the MEMORY region
adjusted for the boot/app split. A git submodule is impractical (that repo also ships the datasheet
and every example, and the SDK sits deep under `EVT/EXAM/SRC/`); to refresh,
re-copy those paths.

Not legal advice - review the WCH terms yourself if redistribution matters.
