/*
 * chocola kernel (C)
 * 32-bit protected mode, flat memory model.
 * VGA text mode 80x25 at 0xb8000.
 */

#define VGA_COLS      80
#define VGA_ROWS      25
#define VGA_ATTR      0x0f   /* white on black */
#define PROMPT_ATTR   0x0a   /* green on black */
#define CMD_BUF_SIZE  64

static volatile unsigned short *const vga = (volatile unsigned short *)0xb8000;

/* ---- I/O port helpers ---- */

static inline unsigned char inb(unsigned short port)
{
	unsigned char val;
	__asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

static inline void outb(unsigned short port, unsigned char val)
{
	__asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* ---- Scan code set 1 → ASCII (US layout, lowercase) ---- */

static const char sc_to_ascii[128] = {
	 0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',  /* 00-0E */
	'\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',      /* 0F-1C */
	 0,  'a','s','d','f','g','h','j','k','l',';','\'','`',          /* 1D-29 */
	 0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,           /* 2A-36 */
	'*', 0, ' '                                                      /* 37-39 */
};

/* ---- VGA text mode ---- */

static int cur_x, cur_y;

static void vga_update_cursor(void)
{
	unsigned short pos = (unsigned short)(cur_y * VGA_COLS + cur_x);
	outb(0x3D4, 14);
	outb(0x3D5, (unsigned char)(pos >> 8));
	outb(0x3D4, 15);
	outb(0x3D5, (unsigned char)(pos & 0xFF));
}

static void vga_scroll(void)
{
	int i;
	for (i = 0; i < VGA_COLS * (VGA_ROWS - 1); i++)
		vga[i] = vga[i + VGA_COLS];
	for (i = VGA_COLS * (VGA_ROWS - 1); i < VGA_COLS * VGA_ROWS; i++)
		vga[i] = (VGA_ATTR << 8) | ' ';
	cur_y = VGA_ROWS - 1;
}

static void vga_putchar(char c)
{
	if (c == '\n') {
		cur_x = 0;
		cur_y++;
	} else if (c == '\b') {
		if (cur_x > 0) {
			cur_x--;
			vga[cur_y * VGA_COLS + cur_x] = (VGA_ATTR << 8) | ' ';
		}
	} else if (c == '\t') {
		cur_x = (cur_x + 4) & ~3;
		if (cur_x >= VGA_COLS) { cur_x = 0; cur_y++; }
	} else {
		vga[cur_y * VGA_COLS + cur_x] = (VGA_ATTR << 8) | (unsigned char)c;
		cur_x++;
		if (cur_x >= VGA_COLS) { cur_x = 0; cur_y++; }
	}
	if (cur_y >= VGA_ROWS)
		vga_scroll();
	vga_update_cursor();
}

static void vga_puts(const char *s)
{
	while (*s)
		vga_putchar(*s++);
}

/* Put a character with a specific attribute (for coloured prompt) */
static void vga_putchar_attr(char c, unsigned char attr)
{
	vga[cur_y * VGA_COLS + cur_x] = ((unsigned short)attr << 8) | (unsigned char)c;
	cur_x++;
	if (cur_x >= VGA_COLS) { cur_x = 0; cur_y++; }
	if (cur_y >= VGA_ROWS)
		vga_scroll();
	vga_update_cursor();
}

static void vga_puts_attr(const char *s, unsigned char attr)
{
	while (*s)
		vga_putchar_attr(*s++, attr);
}

static void vga_clear(void)
{
	int i;
	for (i = 0; i < VGA_COLS * VGA_ROWS; i++)
		vga[i] = (VGA_ATTR << 8) | ' ';
	cur_x = cur_y = 0;
	vga_update_cursor();
}

/* ---- Keyboard (polling) ---- */

static char kbd_getchar(void)
{
	unsigned char sc;
	for (;;) {
		while (!(inb(0x64) & 1))
			;
		sc = inb(0x60);
		if (sc & 0x80)          /* key release → ignore */
			continue;
		if (sc < sizeof(sc_to_ascii) && sc_to_ascii[sc])
			return sc_to_ascii[sc];
	}
}

/* ---- String helpers ---- */

static int my_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (unsigned char)*a - (unsigned char)*b;
}

static int starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++)
			return 0;
	}
	return 1;
}

/* ---- Shell ---- */

static void print_prompt(void)
{
	vga_puts_attr("C:\\>", VGA_ATTR);
}

static void shell_exec(char *cmd)
{
	/* skip leading spaces */
	while (*cmd == ' ') cmd++;

	if (cmd[0] == '\0') {
		/* empty line */
	} else if (my_strcmp(cmd, "help") == 0) {
		vga_puts("Commands:\n");
		vga_puts("  help    - Show this help\n");
		vga_puts("  ver     - Show version\n");
		vga_puts("  clear   - Clear screen\n");
		vga_puts("  echo .. - Echo text\n");
	} else if (my_strcmp(cmd, "ver") == 0) {
		vga_puts("Chocola Ver0.1\n");
	} else if (my_strcmp(cmd, "clear") == 0) {
		vga_clear();
	} else if (starts_with(cmd, "echo ")) {
		vga_puts(cmd + 5);
		vga_putchar('\n');
	} else if (my_strcmp(cmd, "echo") == 0) {
		vga_putchar('\n');
	} else {
		vga_puts("Unknown command: ");
		vga_puts(cmd);
		vga_putchar('\n');
	}
}

static void shell_run(void)
{
	char buf[CMD_BUF_SIZE];
	int pos;
	char c;

	for (;;) {
		print_prompt();
		pos = 0;

		for (;;) {
			c = kbd_getchar();

			if (c == '\n') {
				vga_putchar('\n');
				buf[pos] = '\0';
				break;
			} else if (c == '\b') {
				if (pos > 0) {
					pos--;
					vga_putchar('\b');
				}
			} else {
				if (pos < CMD_BUF_SIZE - 1) {
					buf[pos++] = c;
					vga_putchar(c);
				}
			}
		}

		shell_exec(buf);
	}
}

/* ---- Kernel entry ---- */

void kernel_main(void)
{
	vga_clear();
	vga_puts("Chocola Ver0.1\n");
	vga_puts("Type 'help' for available commands.\n\n");
	shell_run();
}
