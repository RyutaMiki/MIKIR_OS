/*
 * chocola kernel (C)
 * 32-bit protected mode, flat memory model.
 * VGA text mode 80x25 at 0xb8000.
 */

#define VGA_COLS      80
#define VGA_ROWS      25
#define VGA_ATTR      0x0f   /* white on black */
#define CMD_BUF_SIZE  64
#define KBD_BUF_SIZE  32
#define TIMER_HZ      100

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

static inline void io_wait(void)
{
	outb(0x80, 0);   /* port 0x80 gives a short delay */
}

/* ---- Scan code set 1 -> ASCII (US layout, lowercase) ---- */

static const char sc_to_ascii[128] = {
	 0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',  /* 00-0E */
	'\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',      /* 0F-1C */
	 0,  'a','s','d','f','g','h','j','k','l',';','\'','`',          /* 1D-29 */
	 0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,           /* 2A-36 */
	'*', 0, ' '                                                      /* 37-39 */
};

/* ---- Keyboard ring buffer (producer: IRQ handler, consumer: main loop) ---- */

static volatile char    kbd_buf[KBD_BUF_SIZE];
static volatile int     kbd_head, kbd_tail;

/* ---- Timer tick counter ---- */

static volatile unsigned int ticks;

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

static void vga_putint(unsigned n)
{
	char buf[12];
	int i = 0;
	if (n == 0) { vga_putchar('0'); return; }
	while (n > 0) {
		buf[i++] = '0' + (n % 10);
		n /= 10;
	}
	while (--i >= 0)
		vga_putchar(buf[i]);
}

static void vga_clear(void)
{
	int i;
	for (i = 0; i < VGA_COLS * VGA_ROWS; i++)
		vga[i] = (VGA_ATTR << 8) | ' ';
	cur_x = cur_y = 0;
	vga_update_cursor();
}

/* ---- PIC (8259) initialization ---- */

static void pic_init(void)
{
	/* ICW1: start init, expect ICW4 */
	outb(0x20, 0x11);  io_wait();
	outb(0xA0, 0x11);  io_wait();

	/* ICW2: vector offset */
	outb(0x21, 0x20);  io_wait();   /* master: IRQ 0-7 -> INT 0x20-0x27 */
	outb(0xA1, 0x28);  io_wait();   /* slave:  IRQ 8-15 -> INT 0x28-0x2F */

	/* ICW3: master/slave wiring */
	outb(0x21, 0x04);  io_wait();   /* master: slave on IRQ2 */
	outb(0xA1, 0x02);  io_wait();   /* slave:  cascade identity */

	/* ICW4: 8086 mode */
	outb(0x21, 0x01);  io_wait();
	outb(0xA1, 0x01);  io_wait();

	/* Mask: unmask IRQ0 (timer) and IRQ1 (keyboard) only */
	outb(0x21, 0xFC);   /* master: 11111100 */
	outb(0xA1, 0xFF);   /* slave:  all masked */
}

/* ---- PIT (8253/8254) timer ---- */

static void pit_init(unsigned hz)
{
	unsigned short div = (unsigned short)(1193182 / hz);
	outb(0x43, 0x34);               /* channel 0, lo/hi, rate generator */
	outb(0x40, div & 0xFF);
	outb(0x40, (div >> 8) & 0xFF);
}

/* ---- IDT gate setter (overwrite entry in IDT at 0x81000) ---- */

struct idt_entry {
	unsigned short offset_low;
	unsigned short selector;
	unsigned char  zero;
	unsigned char  type_attr;
	unsigned short offset_high;
} __attribute__((packed));

static void idt_set_gate(int n, unsigned handler)
{
	volatile struct idt_entry *idt = (volatile struct idt_entry *)0x81000;
	idt[n].offset_low  = handler & 0xFFFF;
	idt[n].selector    = 0x08;
	idt[n].zero        = 0;
	idt[n].type_attr   = 0x8E;   /* present, ring 0, 32-bit interrupt gate */
	idt[n].offset_high = (handler >> 16) & 0xFFFF;
}

/* ---- Interrupt handlers (called from asm ISR stubs in loader.nas) ---- */

extern void isr_timer(void);
extern void isr_keyboard(void);

void timer_handler(void)
{
	ticks++;
}

void keyboard_handler(void)
{
	unsigned char sc = inb(0x60);
	if (sc & 0x80)           /* key release */
		return;
	if (sc < sizeof(sc_to_ascii) && sc_to_ascii[sc]) {
		int next = (kbd_head + 1) % KBD_BUF_SIZE;
		if (next != kbd_tail) {          /* buffer not full */
			kbd_buf[kbd_head] = sc_to_ascii[sc];
			kbd_head = next;
		}
	}
}

/* ---- Keyboard (interrupt-driven) ---- */

static char kbd_getchar(void)
{
	while (kbd_head == kbd_tail)
		__asm__ volatile ("hlt");        /* sleep until interrupt */
	char c = kbd_buf[kbd_tail];
	kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
	return c;
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
	vga_puts("C:\\>");
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
		vga_puts("  uptime  - Show uptime\n");
	} else if (my_strcmp(cmd, "ver") == 0) {
		vga_puts("Chocola Ver0.1\n");
	} else if (my_strcmp(cmd, "clear") == 0) {
		vga_clear();
	} else if (starts_with(cmd, "echo ")) {
		vga_puts(cmd + 5);
		vga_putchar('\n');
	} else if (my_strcmp(cmd, "echo") == 0) {
		vga_putchar('\n');
	} else if (my_strcmp(cmd, "uptime") == 0) {
		unsigned t = ticks;
		unsigned sec = t / TIMER_HZ;
		unsigned min = sec / 60;
		sec %= 60;
		vga_putint(min);
		vga_puts("m ");
		vga_putint(sec);
		vga_puts("s (");
		vga_putint(t);
		vga_puts(" ticks)\n");
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

	/* Set up interrupts */
	pic_init();
	pit_init(TIMER_HZ);
	idt_set_gate(0x20, (unsigned)isr_timer);
	idt_set_gate(0x21, (unsigned)isr_keyboard);

	/* Enable interrupts */
	__asm__ volatile ("sti");

	vga_puts("Chocola Ver0.1\n");
	vga_puts("Type 'help' for available commands.\n\n");
	shell_run();
}
