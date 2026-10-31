# First-stage bootloader (CIP-04.1, 04.2, 20.7)

Gary's decision (29 Oct): our own small bootloader rather than ST's OEMiRoT or MCUboot. It
reuses the update format the application already checks (`fw_header_t`, `tools/mkfwimage.py`),
so there is one image format, one signing key and one set of tests.

Code: `platform/include/boot.h` (layout and constants), `platform/core/boot_core.c` (the whole
decision, no HAL, host-tested in `tests/test_boot.c`), `boot/boot_main.c` (the STM32H573 shell),
`boot/boot.mk` (`make boot`). Size today: about 6 KB of the 32 KB it is given.

## 1. Flash layout (each 1 MB bank, 8 KB sectors)

| Offset in the bank | What | Written by |
|---|---|---|
| 0x00000 - 0x07FFF (`PLAT_BOOT_SIZE`, 4 sectors) | the bootloader, the same copy in both banks, write protected | the factory only |
| 0x08000 - 0xFDFFF | the application image, linked for 0x08008000 | an update (`fw_update.c`) or the factory |
| 0xFE000 (last sector) | the image's header | an update, or `mkfwimage.py --factory` |

SWAP_BANK maps one bank at 0x08000000 and the other at 0x08100000. Because both banks start
with the bootloader, whichever one is mapped starts it.

## 2. What it does at every reset

1. Reads the trial register (TAMP backup register 31, `BOOT_TRIAL_REG`).
2. Checks the image in the mapped bank: header magic, size and CRC, product, image size within
   the image area, SHA-256 of the image, and the ECDSA P-256 signature (the PKA, with the key in
   `fw_pubkey.c`) when the build has a key. It also checks the vector table is plausible (stack
   pointer in SRAM, reset handler inside the image).
3. Good: starts it (VTOR, MSP, jump). Bad: if the other bank holds a good image it swaps to it
   (option byte SWAP_BANK, reset), else it stops. A swap made by the bootloader itself is marked
   (`BOOT_SWAP_MAGIC`) so it never swaps straight back: two bad banks stop instead of looping.
4. Stopped: nothing runs, the outputs stay as reset leaves them. Recovery is SWD, or the ST ROM
   bootloader with BOOT0 high (CIP-04.3/04.4).

Development builds (no key in `fw_pubkey.c`) also start an image without a header, as the
debugger programs it, so Debug/Run from CubeIDE keeps working. A build with the product key
does not: every image must be a signed update or a signed factory image.

## 3. Trial counter: the case `fw_update` could not cover

`fw_update.c` already tries a new image and rolls back if it does not confirm within the trial
(PLATFORM.md 5b). That needs the new image to reach `fw_boot()`. An image that hangs or
crashes earlier was the open limit. Now:

- before swapping to a new image the application writes `BOOT_TRIAL_MAGIC` to the trial
  register (`fw_flash_t.trial_mark`);
- the bootloader counts each start while it is set; after `BOOT_TRIAL_MAX` (3) starts without
  a confirmation it swaps back to the previous image;
- the application clears it when the image confirms itself, before its own rollback swap, and
  at every ordinary start.

A power cut clears the register (no VBAT on the DK). The FRAM boot record then decides, as before.

## 4. Building and programming

The bootloader: `make boot` (arm-none-eabi-gcc from `../ext`) gives `build/boot/cems_boot.bin`.

The application, once, in its CubeIDE project (the CEMS one, never the Smart Filter ones):
1. Linker script (the project's `*_FLASH.ld`): `FLASH (rx) : ORIGIN = 0x08008000, LENGTH = 984K`
   (1 MB bank - 32 KB bootloader - 8 KB header sector).
2. `Core/Src/system_stm32h5xx.c`: `#define VECT_TAB_OFFSET 0x00008000U`. (`bsp_init()` also sets
   VTOR from the link address, but SystemInit runs first.)
3. Update files are made as before; the binary now starts at 0x08008000, which is what the
   update writes at offset `PLAT_BOOT_SIZE`. Check with `mkfwimage.py ... --boot-size 0x8000`.

Factory programming (STM32CubeProgrammer):

    python3 tools/mkfwimage.py Debug/cems.bin bank.bin --version 0.9.2 --factory build/boot/cems_boot.bin [--key ...]
    STM32_Programmer_CLI -c port=SWD -w bank.bin 0x08000000 -w build/boot/cems_boot.bin 0x08100000 -v

then write protect the bootloader in both banks (below). The bank image carries the header, so
the bootloader can check the factory image from the first start.

The application shows what the bootloader found at this start in `/api/fw` as `"bootloader"`
(`{"version","image","swapped","rollback","other"}`, or null when no bootloader ran), from
backup register 30.

## 5. Write protection

The H573's write protection works on groups of 4 sectors (32 KB): `WRPSGn` bit 0 covers sectors
0-3, exactly the bootloader. In CubeProgrammer, Option bytes → Write protection: clear bit 0 of
`WRPSG1` and `WRPSG2` (0 = protected). An update then cannot erase the bootloader even by
mistake (`fw_update.c` never tries: it only erases from `image_off`).

While the product state is OPEN this is reversible in the same screen. Making it permanent
(product state CLOSED/LOCKED, which also locks out the debugger) is the production lock, to be
chosen with the CORE PCB work and only with Gary's go. The DK stays OPEN.

## 6. Status

- Host tests: `tests/test_boot.c` (decision, signatures, header checks, never swapping back and
  forth, debugger images, trial counter) and `tests/test_fw.c` `t_boot_offset` (update behind the
  bootloader, trial mark). `make boot` links and fits.
- Start-up time: the bootloader runs on the reset clock (32 MHz) and hashes the whole image in
  software, so expect roughly 0.5-1 s per start for a 600 KB image. Measure it on the DK. If it is
  too slow, raise the clock to 64 MHz (HSIDIV) or use the H5's HASH unit.
- To do on the DK: program bootloader + factory bank, start; update over the network and back;
  a damaged image swaps; a hanging image rolls back after 3 starts; flash ECC NMI on a cut-off
  write; WRP refuses an erase of sector 0.
