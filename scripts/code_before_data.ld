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
    *(.sdata)                /* small-data section (ARM EABI small data area) */
    *(.sdata*)
. = ALIGN(4);
		KEEP(*(.init))
		__preinit_array_start = .;
		KEEP (*(.preinit_array))
		__preinit_array_end = .;
		__init_array_start = .;
		KEEP (*(.init_array))
		__init_array_end = .;
		/* One word after the last array entry: an end symbol that also
		   equals .data's end resolves through the *next* section's VA span,
		   and the loader aligns each main section's base. Where .bss needs
		   more than 4-byte alignment (alignas(32) DMA buffers), its runtime
		   base lands past the array's real end, so __init_array_end would
		   point into the alignment padding and cpp_init_fini would call the
		   zero words there. Keeping the symbol strictly inside .data pins it
		   to the data section's span, which is exact.
		   Only when the array is non-empty: a module with no constructors
		   must keep its zero-byte .data (an unconditional word would make
		   every such module claim RAM it never uses). */
		. = . + 4 * (__init_array_end != __init_array_start);
		. = ALIGN(4);
  } > all

  .bss :
  {
    . = ALIGN(4);
    *(.bss)
    *(.bss*)
    *(.sbss)                 /* small-bss section (ARM EABI small data area) */
    *(.sbss*)
    *(COMMON)
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

