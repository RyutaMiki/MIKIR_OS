/* MIKIR-OS linker script
 * .text.boot (16-bit): VMA 0  (offsets for real-mode CS=0x8000)
 * .text      (32-bit): VMA 0x80000+N  (linear addresses for flat pmode)
 * Pad .text.boot to match .text alignment so LMA = SIZEOF(.text.boot) is exact.
 */
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)

SECTIONS
{
	. = 0;
	.text.boot : {
		*(.text.boot)
		. = ALIGN(32);   /* match .text section alignment (2**5) */
	}

	. += 0x80000;
	.text : AT(SIZEOF(.text.boot)) {
		*(.text .text.* .rodata .rodata.* .data .data.*)
	}
	.bss : { *(.bss .bss.*) }
	/DISCARD/ : { *(.note*) *(.comment) }
}
