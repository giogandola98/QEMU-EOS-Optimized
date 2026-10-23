/* versatilepb: RAM at 0; -kernel loads the image at 0x10000. */
ENTRY(_start)
SECTIONS
{
    . = 0x10000;
    .text : { *(.text) }
    .data : { *(.data) }
    .bss  : { *(.bss) }
}
