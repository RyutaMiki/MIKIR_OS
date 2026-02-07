/* MIKIR-OS: loaded at 0x8000:0 (linear 0x80000). GDT base set to 0x80000. */
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)

SECTIONS
{
	. = 0;
	.text.boot : { *(.text.boot) }
	.text      : { *(.text) }
	.data      : { *(.data) }
	.bss       : { *(.bss) }
	/DISCARD/  : { *(.note*)(.*) }
}
