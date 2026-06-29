/*
 * hk.ld — HeteroKern GCN linker script
 *
 * Puts .text.entry first (the entry point), then the rest of .text,
 * .rodata, and .bss into a single flat section.
 */

SECTIONS
{
    . = 0;

    .text :
    {
        *(.text.entry)
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.bss .bss.* COMMON)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.gnu.hash)
        *(.relro_padding)
        *(.AMDGPU.gpr_maximums)
        *(.kd)
    }
}
