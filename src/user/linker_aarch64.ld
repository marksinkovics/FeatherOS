OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

/* Position-independent: linked at 0, the kernel picks the load base and applies the
   RELATIVE relocations listed in .rela.dyn. */
PHDRS
{
    text PT_LOAD FLAGS(0x05);
    rodata PT_LOAD FLAGS(0x04);
    data PT_LOAD FLAGS(0x06);
    dynamic PT_DYNAMIC FLAGS(0x06);
}

SECTIONS
{
    . = 0;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .rela.dyn : {
        *(.rela.dyn .rela.*)
    } :rodata

    .dynsym : {
        *(.dynsym)
    } :rodata

    .dynstr : {
        *(.dynstr)
    } :rodata

    .hash : {
        *(.hash)
    } :rodata

    .gnu.hash : {
        *(.gnu.hash)
    } :rodata

    . = ALIGN(0x1000);

    .data : {
        *(.data .data.* .data.rel.ro .data.rel.ro.*)
    } :data

    .dynamic : {
        *(.dynamic)
    } :data :dynamic

    .got : {
        *(.got .got.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
        *(.interp)
        *(.ARM.attributes)
    }
}
