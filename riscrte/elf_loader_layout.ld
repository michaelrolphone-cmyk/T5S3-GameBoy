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
    *(.literal .literal.* .gnu.linkonce.literal.*)
    *(.text .text.* .gnu.linkonce.t.*)
    *(.plt .plt.*)
    *(.iram1 .iram1.*)
  }
  .rodata :
  {
    *(.rodata .rodata.* .gnu.linkonce.r.*)
    *(.got.loc)
    *(.eh_frame .eh_frame.*)
    *(.gcc_except_table .gcc_except_table.*)
  }
  .data :
  {
    KEEP(*(.ctors .ctors.*))
    KEEP(*(.dtors .dtors.*))
    *(.got .got.*)
    *(.data .data.* .gnu.linkonce.d.*)
  }
} INSERT BEFORE .dynamic;
