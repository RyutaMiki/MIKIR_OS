/*
 * MIKIR-OS kernel (C)
 * Called from loader in 32-bit protected mode.
 * VGA text buffer at 0xb8000: each character = 2 bytes (attr, char).
 */

#define VGA_BASE  0xb8000
#define VGA_ATTR  0x0f   /* white on black */

static volatile unsigned short *vga = (volatile unsigned short *)VGA_BASE;

static void putchar(int x, int y, char c)
{
	vga[y * 80 + x] = (VGA_ATTR << 8) | (unsigned char)c;
}

static void putstr(int x, int y, const char *s)
{
	while (*s)
		putchar(x++, y, *s++);
}

void kernel_main(void)
{
	const char *msg = "MIKIR-OS (C kernel)";
	putstr(0, 0, msg);
	putstr(0, 1, "Press key to continue...");

	/* Simple idle (no interrupts yet) */
	while (1)
		__asm__ volatile ("hlt");
}
