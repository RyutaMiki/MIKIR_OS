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

#define FS_DIR_SECTOR 100    /* sector holding the file directory */
#define FS_MAX_FILES  16     /* max entries in one sector (512/32) */
#define FILE_BUF_SIZE 2048   /* max file size for write command */

static volatile unsigned short *const vga = (volatile unsigned short *)0xb8000;

/* ---- I/O port helpers ---- */

static inline unsigned char inb(unsigned short port)
{
	unsigned char val;
	__asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

static inline unsigned short inw(unsigned short port)
{
	unsigned short val;
	__asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

static inline void outb(unsigned short port, unsigned char val)
{
	__asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(unsigned short port, unsigned short val)
{
	__asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void io_wait(void)
{
	outb(0x80, 0);
}

/* ---- Scan code set 1 -> ASCII (US layout, lowercase) ---- */

static const char sc_to_ascii[128] = {
	 0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',  /* 00-0E */
	'\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',      /* 0F-1C */
	 0,  'a','s','d','f','g','h','j','k','l',';','\'','`',          /* 1D-29 */
	 0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,           /* 2A-36 */
	'*', 0, ' '                                                      /* 37-39 */
};

/* ---- Keyboard ring buffer ---- */

static volatile char    kbd_buf[KBD_BUF_SIZE];
static volatile int     kbd_head, kbd_tail;

/* ---- Timer tick counter ---- */

static volatile unsigned int ticks;

/* ---- Disk I/O buffer (shared, single-threaded) ---- */

static unsigned char disk_buf[512];
static unsigned char file_buf[FILE_BUF_SIZE];

/* ---- Simple filesystem directory entry (32 bytes) ---- */

struct fs_entry {
	char         name[20];    /* null-terminated filename */
	unsigned int start;       /* start sector on disk */
	unsigned int size;        /* file size in bytes */
	unsigned int flags;       /* reserved */
} __attribute__((packed));

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
	outb(0x20, 0x11);  io_wait();
	outb(0xA0, 0x11);  io_wait();
	outb(0x21, 0x20);  io_wait();
	outb(0xA1, 0x28);  io_wait();
	outb(0x21, 0x04);  io_wait();
	outb(0xA1, 0x02);  io_wait();
	outb(0x21, 0x01);  io_wait();
	outb(0xA1, 0x01);  io_wait();
	outb(0x21, 0xFC);
	outb(0xA1, 0xFF);
}

/* ---- PIT timer ---- */

static void pit_init(unsigned hz)
{
	unsigned short div = (unsigned short)(1193182 / hz);
	outb(0x43, 0x34);
	outb(0x40, div & 0xFF);
	outb(0x40, (div >> 8) & 0xFF);
}

/* ---- IDT gate setter ---- */

struct idt_entry {
	unsigned short offset_low;
	unsigned short selector;
	unsigned char  zero;
	unsigned char  type_attr;
	unsigned short offset_high;
} __attribute__((packed));

static void idt_set_gate(int n, unsigned handler)
{
	volatile struct idt_entry *idt = (volatile struct idt_entry *)0x70000;
	idt[n].offset_low  = handler & 0xFFFF;
	idt[n].selector    = 0x08;
	idt[n].zero        = 0;
	idt[n].type_attr   = 0x8E;
	idt[n].offset_high = (handler >> 16) & 0xFFFF;
}

/* ---- Interrupt handlers ---- */

extern void isr_timer(void);
extern void isr_keyboard(void);

void timer_handler(void)
{
	ticks++;
}

void keyboard_handler(void)
{
	unsigned char sc = inb(0x60);
	int next;
	if (sc & 0x80)
		return;
	if (sc < sizeof(sc_to_ascii) && sc_to_ascii[sc]) {
		next = (kbd_head + 1) % KBD_BUF_SIZE;
		if (next != kbd_tail) {
			kbd_buf[kbd_head] = sc_to_ascii[sc];
			kbd_head = next;
		}
	}
}

/* ---- Keyboard (interrupt-driven) ---- */

static char kbd_getchar(void)
{
	char c;
	while (kbd_head == kbd_tail)
		__asm__ volatile ("hlt");
	c = kbd_buf[kbd_tail];
	kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
	return c;
}

/* ---- ATA PIO disk read ---- */

static void ata_read_sector(unsigned lba, void *buf)
{
	int i;
	unsigned short *p = (unsigned short *)buf;

	/* Wait for BSY clear */
	while (inb(0x1F7) & 0x80)
		;

	outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));   /* drive 0, LBA mode */
	outb(0x1F2, 1);                                /* 1 sector */
	outb(0x1F3, lba & 0xFF);                       /* LBA low */
	outb(0x1F4, (lba >> 8) & 0xFF);                /* LBA mid */
	outb(0x1F5, (lba >> 16) & 0xFF);               /* LBA high */
	outb(0x1F7, 0x20);                             /* READ SECTORS */

	/* Wait for DRQ */
	while (!(inb(0x1F7) & 0x08))
		;

	/* Read 256 words = 512 bytes */
	for (i = 0; i < 256; i++)
		p[i] = inw(0x1F0);
}

/* ---- ATA PIO disk write ---- */

static void ata_write_sector(unsigned lba, const void *buf)
{
	int i;
	const unsigned short *p = (const unsigned short *)buf;

	while (inb(0x1F7) & 0x80)
		;

	outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
	outb(0x1F2, 1);
	outb(0x1F3, lba & 0xFF);
	outb(0x1F4, (lba >> 8) & 0xFF);
	outb(0x1F5, (lba >> 16) & 0xFF);
	outb(0x1F7, 0x30);                             /* WRITE SECTORS */

	while (!(inb(0x1F7) & 0x08))
		;

	for (i = 0; i < 256; i++)
		outw(0x1F0, p[i]);

	/* Cache flush */
	outb(0x1F7, 0xE7);
	while (inb(0x1F7) & 0x80)
		;
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

/* ---- Filesystem commands ---- */

static void cmd_dir(void)
{
	struct fs_entry *entries;
	int i, count;

	ata_read_sector(FS_DIR_SECTOR, disk_buf);
	entries = (struct fs_entry *)disk_buf;

	count = 0;
	for (i = 0; i < FS_MAX_FILES; i++) {
		if (entries[i].name[0] == '\0')
			break;
		vga_puts("  ");
		vga_puts(entries[i].name);
		/* pad to column 24 */
		{
			int len = 0;
			const char *p = entries[i].name;
			while (*p++) len++;
			while (len++ < 20) vga_putchar(' ');
		}
		vga_putint(entries[i].size);
		vga_puts(" bytes\n");
		count++;
	}
	if (count == 0)
		vga_puts("  (no files)\n");
	vga_putint(count);
	vga_puts(" file(s)\n");
}

static void cmd_type(const char *filename)
{
	struct fs_entry *entries;
	int i;
	unsigned remaining, sector, to_print, j;

	ata_read_sector(FS_DIR_SECTOR, disk_buf);
	entries = (struct fs_entry *)disk_buf;

	for (i = 0; i < FS_MAX_FILES; i++) {
		if (entries[i].name[0] == '\0')
			break;
		if (my_strcmp(entries[i].name, filename) == 0) {
			remaining = entries[i].size;
			sector = entries[i].start;
			while (remaining > 0) {
				ata_read_sector(sector, disk_buf);
				to_print = remaining > 512 ? 512 : remaining;
				for (j = 0; j < to_print; j++)
					vga_putchar((char)disk_buf[j]);
				remaining -= to_print;
				sector++;
			}
			return;
		}
	}
	vga_puts("File not found: ");
	vga_puts(filename);
	vga_putchar('\n');
}

/* ---- Shell ---- */

static void print_prompt(void)
{
	vga_puts("C:\\>");
}

static void shell_exec(char *cmd)
{
	while (*cmd == ' ') cmd++;

	if (cmd[0] == '\0') {
		/* empty */
	} else if (my_strcmp(cmd, "help") == 0) {
		vga_puts("Commands:\n");
		vga_puts("  help       - Show this help\n");
		vga_puts("  ver        - Show version\n");
		vga_puts("  clear      - Clear screen\n");
		vga_puts("  echo ..    - Echo text\n");
		vga_puts("  uptime     - Show uptime\n");
		vga_puts("  dir        - List files on disk\n");
		vga_puts("  type FILE  - Display file contents\n");
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
	} else if (my_strcmp(cmd, "dir") == 0 || my_strcmp(cmd, "ls") == 0) {
		cmd_dir();
	} else if (starts_with(cmd, "type ")) {
		cmd_type(cmd + 5);
	} else if (starts_with(cmd, "cat ")) {
		cmd_type(cmd + 4);
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

	pic_init();
	pit_init(TIMER_HZ);
	idt_set_gate(0x20, (unsigned)isr_timer);
	idt_set_gate(0x21, (unsigned)isr_keyboard);
	__asm__ volatile ("sti");

	vga_puts("Chocola Ver0.1\n");
	vga_puts("Type 'help' for available commands.\n\n");
	shell_run();
}
