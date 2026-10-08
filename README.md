# PocketStation

Emulation of the Sony PocketStation (SCPH-4000): an ARM7TDMI-based memory-card
peripheral with a 32x32 mono LCD, RTC, IR, and a serial link to the PSX over the
memory-card slot. PCSX-Redux uses it as a submodule under `src/pocketstation`.

## Origin and credit

The ARM7TDMI core, memory map, I/O registers and LCD are based on
**PocketBoyAdvance** by wheremyfoodat (https://github.com/wheremyfoodat), who
donated it to PCSX-Redux under GPL-2.0-or-later. Huge thanks for writing a clean ARM7TDMI core and
PocketStation I/O map and for making it available.

What changed since the donation:

- Dropped the standalone Qt front-end; the host draws the LCD.
- Restructured into a flat module that drops into the normal Redux build.
- Replaced the boot scaffolding (a patched kernel byte, hardcoded interrupt
  cadences) with timer, RTC, clock and COM modeling, so the retail kernel runs
  unpatched. The register and SWI reference is `docs/pocketstation.md` in
  psx-spx.

## Layout

- `pocketstation.{h,cc}` - the device: kernel/flash injection, cycle catch-up,
  card link.
- `cpu.{h,cc}` + `cpu/` - ARM7TDMI core and the ARM/Thumb instruction tables.
- `bus.{h,cc}` - memory map, FLASH banking, I/O registers, interrupts.
- `lcd.{h,cc}` - 32x32 1bpp framebuffer.
- `io.h`, `timer.h`, `types.h` - register map, timers, helpers.
- `openpsk/` - OpenPSK, a minimal open-source kernel that can stand in for the
  retail `kernel.bin`, the way OpenBIOS stands in for the PS1 BIOS. Build it
  with `make -C openpsk` and an `arm-none-eabi` toolchain.

## License

GPL-2.0-or-later, see `LICENSE`. `termcolor.h` is BSD-3-Clause, see
`LICENSE.termcolor`.
