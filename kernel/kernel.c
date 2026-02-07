/*
 * MIKIR-OS kernel (C)
 * Called from loader in 32-bit protected mode.
 * VGA text buffer at 0xb8000: each character = 2 bytes (attr, char).
 */

#define VGA_BASE  0xb8000
#define VGA_ATTR  0x0f   /* white on black */

static volatile unsigned short *vga = (volatile unsigned short *)VGA_BASE;

/* ---- I/O port helpers ---- */
static inline unsigned char inb(unsigned short port)
{
	unsigned char val;
	__asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

/* Wait until keyboard controller has data, then read scan code */
static unsigned char kbd_wait_scancode(void)
{
	while (!(inb(0x64) & 1))
		;  /* poll status port */
	return inb(0x60);
}

/* ---- VGA helpers ---- */
static void putchar_at(int x, int y, char c)
{
	vga[y * 80 + x] = (VGA_ATTR << 8) | (unsigned char)c;
}

static void putstr(int x, int y, const char *s)
{
	while (*s)
		putchar_at(x++, y, *s++);
}

/* ---- Kernel entry ---- */
/* load_base: 0x80000 = linear address where this image is loaded (passed from loader) */
void kernel_main(unsigned load_base)
{
	const char *msg;

	msg = (const char *)((unsigned)"MIKIR-OS (C kernel)" + load_base);
	putstr(0, 0, msg);

	msg = (const char *)((unsigned)"Press any key..." + load_base);
	putstr(0, 1, msg);

	/* Wait for a key press (poll keyboard controller port) */
	kbd_wait_scancode();

	msg = (const char *)((unsigned)"Key received! Welcome to MIKIR-OS." + load_base);
	putstr(0, 3, msg);

	while (1)
		__asm__ volatile ("hlt");
}
