/* RiscRTE's ESP32-S3 ELF loader maps precisely .text, .rodata, .data,
 * .data.rel.ro and .bss. The standalone board build puts IRAM_ATTR code
 * in .iram1.*, and GNU Xtensa also creates separately named constructor,
 * GOT and unwind sections. Keep those bytes in the same canonical loadable
 * sections without changing the original source or its hardware behavior.
 *
 * INSERT supplies explicit input patterns to the toolchain's normal shared
 * object linker script, retaining dynamic symbol and relocation metadata
 * and standard ET_DYN machinery rather than replacing the complete script.
 * Use .dynamic as the insertion anchor: it is supplied by the shared-object
 * default script and is not redefined here. Anchoring to our own .text
 * definition makes Xtensa ld fail with ".text not found for insert".
 */
ENTRY(app_main)
SECTIONS
{
  .text : ALIGN(4)
  {
    /* Xtensa PLT uses backward L32R loads into its own literal table.
     * Preserve the toolchain's paired input order; .got.plt is code-local,
     * unlike the ordinary .got collected in .data below. */
    *(.got.plt* .plt*)
    *(.literal .text .stub .literal.* .text.*
      .gnu.linkonce.literal.* .gnu.linkonce.t.*.literal .gnu.linkonce.t.*)
    *(.iram1 .iram1.*)
  }
  .rodata : ALIGN(16)
  {
    __app_rodata_start = .;
    *(.rodata .rodata.* .gnu.linkonce.r.*)
    *(.got.loc)
    *(.eh_frame .eh_frame.*)
    *(.gcc_except_table .gcc_except_table.*)
    /* Loader packs DATA, RODATA, DRLRO, BSS without inter-section padding. */
    /* The firmware packs sections by size. Absolute ALIGN can leave the
     * section size 4 mod 8 when the linker chooses a shifted VMA. */
    . = __app_rodata_start + ALIGN(. - __app_rodata_start, 16);
  }
  .data : ALIGN(16)
  {
    __app_init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __app_init_array_end = .;
    __app_ctors_start = .;
    KEEP(*(.ctors .ctors.*))
    __app_ctors_end = .;
    __app_fini_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
    KEEP(*(.fini_array))
    __app_fini_array_end = .;
    __app_dtors_start = .;
    KEEP(*(.dtors .dtors.*))
    __app_dtors_end = .;
    *(.got .got.*)
    *(.data .data.* .gnu.linkonce.d.*)
    . = ALIGN(16);
  }
  .data.rel.ro : ALIGN(16)
  {
    *(.data.rel.ro.local* .gnu.linkonce.d.rel.ro.local.*)
    *(.data.rel.ro .data.rel.ro.* .gnu.linkonce.d.rel.ro.*)
    . = ALIGN(16);
  }
} INSERT BEFORE .dynamic;
