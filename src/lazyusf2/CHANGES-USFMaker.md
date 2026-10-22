# Changes made to lazyusf2 for USF Maker

This directory is lazyusf2 (by kode54 and others, derived from mupen64plus) as
bundled with Cog (https://github.com/losnoco/Cog, `Frameworks/lazyusf2/lazyusf2`,
revision fb2ed48 of 2026-10-07), modified for USF Maker in October 2026 by
Nekokabu.  All changes are marked with `USFX` in the sources.

lazyusf2 is a *player* core: it starts from a Project64 save state and has no
controllers.  USF Maker has to boot a cartridge from power-on, press buttons
and write the running machine back out as a save state.  The additions are
opt-in: a state that never calls the `usfx_*` API behaves like the original,
which is what lets USF Maker check a generated file "the way a player would".

## New files

| File | Purpose |
| --- | --- |
| `usf/usfx.h`, `usf/usfx.c` | The extension API: cold boot, controller input, run-until-event, save state export (Project64 1.x layout), cartridge read coverage, statistics, blank cartridge save memory (EEPROM / SRAM / FlashRAM), the Aleck64 arcade board (SDRAM, DIP switches and inputs at physical 0xC0000000+, layout as in MAME). |
| `usf/adler32.h` | Small Adler-32 so that the core does not need zlib. |
| `sources.mk` | List of the C files that make up the library. |
| `lazyusf2.vcxproj`, `.filters` | Visual Studio project. |

## Removed

The x86 / x86-64 dynamic recompiler sources and the unused "new dynarec"
(the cached interpreter is used on every platform; the headers are kept),
`rsp_lle/bench.c`, and the Xcode project files.

## Modified files

| File | Change |
| --- | --- |
| `usf/usf.c`, `usf/usf.h`, `usf/usf_internal.h` | Extension block appended to the state; audio callback / stop hook when buffers are pushed; controllers wired up in `usf_clear()`; `usf_startup()` exported; shared (caller owned) ROM image; clean-up of extension data. |
| `main/main.c` | Cold boot path: enter IPL3 at 0xA4000040 instead of loading a save state; MAME's IPL3 patch for Aleck64 cartridges when the board is enabled. |
| `main/rom.c` | Does not free a ROM image owned by the caller. |
| `si/game_controller.c`, `.h` | A controller that can be plugged in and whose buttons can be set; poll statistics. |
| `si/pif.c` | Cartridge channel commands answered by the optional EEPROM. |
| `si/si_controller.c` | SI DMA address masked to 24 bits and bounds checked. |
| `pi/pi_controller.c`, `.h` | Cartridge read coverage and DMA callback; `PI_DRAM_ADDR` masked to 24 bits; SRAM / FlashRAM transfers when enabled. |
| `pi/cart_rom.c` | Coverage for direct (non DMA) cartridge reads. |
| `memory/memory.c` | Coverage for `fast_mem_access`; FlashRAM status / command registers when enabled; TLB-mapped accesses to the Aleck64 board. |
| `r4300/tlb.c` | Keeps TLB entries for physical 0xC0000000+ when the Aleck64 board is enabled (stock lazyusf2 drops them); counts TLB misses. |
| `si/cic.c`, `si/cic.h` | CIC-NUS-5101 (Aleck64, seed 0xAC), recognised by the sum of 0x40-0xBFF or 0x40-0xFFF. |
| `ai/ai_controller.c` | AI DMA address masked and bounds checked. |
| `rsp/rsp_core.c` | SP DMA wraps inside DMEM/IMEM and is bounds checked against RDRAM; task statistics. |
| `rsp_hle/memory.h`, `rsp_hle/plugin.c` | RDRAM mask for HLE accesses; counts audio tasks that fell back to the interpreter. |
| `vi/vi_controller.c` | Frame / time statistics; optional stop at each vertical interrupt. |
| `r4300/cached_interp.c` | Optional invalidation of cached code that is rewritten through the other KSEG0/KSEG1 mirror. |
| `r4300/r4300.c` | NULL check. |
| `r4300/fpu.h`, `osal/preproc.h`, `rsp_lle/config.h` | Build fixes for current MSVC (`<fenv.h>`, no `isnan` macro, no duplicate CRT defines). |
| `r4300/interpreter_tlb.def` | Uses `usf/adler32.h` instead of zlib. |

## Licence

lazyusf2 / mupen64plus: GNU GPL version 2 or later.  `rsp_lle/`: CC0 (Iconoclast).
`rsp_hle/`: GPL version 2 or later, see `rsp_hle/LICENSES`.  The USF Maker
additions are GPL version 2 or later.
