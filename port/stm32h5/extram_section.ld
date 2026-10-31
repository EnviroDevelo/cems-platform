  /* CEMS platform: buffers in the external PSRAM (PLAT_EXTRAM, PLAT_PSRAM=1 builds only; empty
   * otherwise). NOLOAD: bsp_psram_init zeroes it once the chip is mapped and checked.
   * Insert just before ._user_heap_stack, after .noinit. Needs the PSRAM line in MEMORY. */
  .extram (NOLOAD) :
  {
    . = ALIGN(8);
    _sextram = .;
    *(.extram)
    *(.extram*)
    . = ALIGN(8);
    _eextram = .;
  } >PSRAM
