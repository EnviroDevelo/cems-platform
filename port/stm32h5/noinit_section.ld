  /* CEMS platform: RAM kept across warm resets (plat_diag). NOLOAD, so the
   * startup code neither copies nor zeroes it. Insert just before ._user_heap_stack. */
  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.noinit)
    *(.noinit*)
    . = ALIGN(4);
  } >RAM

