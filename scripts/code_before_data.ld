ENTRY(main)

/* Specify the memory areas */
MEMORY
{
  all                   (RWX) : ORIGIN = 0x00000000, LENGTH = 0xFFFFFFFF
}

/* Define output sections */
SECTIONS
{
  /* The startup code goes first into internal flash */
  .text :
  {
    . = ALIGN(4);
    KEEP(*(.text_nogc))
    *(.text)                 /* .text sections (code) */
    *(.text*)                /* .text* sections (code) */
    *(.rodata)               /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)              /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } > all

  .data :
  {
    . = ALIGN(4);
    *(.data)                 /* .data sections */
    *(.data*)                /* .data* sections */
. = ALIGN(4);
		KEEP(*(.init))
		__preinit_array_start = .;
		KEEP (*(.preinit_array))
		__preinit_array_end = .;
		__init_array_start = .;
		KEEP (*(.init_array))
		__init_array_end = .;
		. = ALIGN(4);
  } > all

  .bss :
  {
    . = ALIGN(4);
    *(.bss)
    *(.bss*)
    /* Thunk-export stub/gateway slots (UDYNLINK_THUNK_EXPORT).
     * Unreferenced, so KEEP is required to survive --gc-sections. */
    KEEP(*(.bss.udynlink_thunk_pool))
    . = ALIGN(4);
  } > all

  .got :
  {
    *(.got*)
  }
}

