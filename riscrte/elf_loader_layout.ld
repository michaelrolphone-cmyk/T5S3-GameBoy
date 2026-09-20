/* RiscRTE's ESP32-S3 ELF loader maps precisely .text, .rodata, .data,
 * .data.rel.ro and .bss. The standalone board build puts IRAM_ATTR code
 * in .iram1.*, and GNU Xtensa also creates separately named constructor,
 * GOT and unwind sections. Keep those bytes in the same canonical loadable
 * sections without changing the original source or its hardware behavior.
 *
 * INSERT supplies explicit input patterns to the toolchain's normal shared
 * object linker script, retaining dynamic symbol and relocation metadata
 * and standard ET_DYN machinery rather than replacing the complete script.
 */
SECTIONS
{
  .text :
  {
    *(.text .text.* .gnu.linkonce.t.*)
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
} INSERT AFTER .text;
