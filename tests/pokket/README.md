# Pokket bus-level tests

`bu_test.c`, `mock_ps1.{c,h}` and `psemu/psemu.h` are copied unmodified from
[mentalfoundry/Pokket](https://github.com/mentalfoundry/Pokket) (commit in `UPSTREAM_COMMIT`),
MIT, Copyright (c) 2026 Darien Liu; see `LICENSE` here. They drive the memory card protocol
through the retail kernel, the way a PS1 does, and check the bytes it answers.

`psemu_shim.cc` implements their `psemu_*` API on `PCSX::PocketStation`, `bu_driver.c` runs
their cases one by one, and `override/assert.h` turns a failed assert into a failed case
instead of an abort. Entry points that exist only for their engine (savestates, idle-loop skip,
sub-instruction acknowledge timing) report the case as not portable.

    make -C tests/pokket check PSEMU_TEST_BIOS=/path/to/kernel.bin

The kernel dump is not part of this repository; without it the suite skips (exit 77).
