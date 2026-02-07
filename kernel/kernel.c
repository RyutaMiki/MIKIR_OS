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

#define HEAP_START    0x200000   /* 2 MB */
#define HEAP_SIZE     0x200000   /* 2 MB */

#define MAX_TASKS      4
#define TASK_STACK_SIZE 4096

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

static void vga_puthex(unsigned n)
{
	static const char hex[] = "0123456789abcdef";
	int i;
	vga_puts("0x");
	for (i = 28; i >= 0; i -= 4)
		vga_putchar(hex[(n >> i) & 0xF]);
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

/* ---- E820 memory map (filled by loader in real mode at linear 0x500) ---- */

struct e820_entry {
	unsigned int base_lo, base_hi;
	unsigned int len_lo, len_hi;
	unsigned int type;
	unsigned int acpi;
} __attribute__((packed));

/* ---- Heap allocator ---- */

struct heap_block {
	unsigned int size;
	unsigned int used;
	struct heap_block *next;
};

static struct heap_block *heap_head;

static void heap_init(void)
{
	heap_head = (struct heap_block *)HEAP_START;
	heap_head->size = HEAP_SIZE - sizeof(struct heap_block);
	heap_head->used = 0;
	heap_head->next = 0;
}

static void *kmalloc(unsigned size)
{
	struct heap_block *block, *nb;

	size = (size + 3) & ~3;   /* align to 4 bytes */

	for (block = heap_head; block; block = block->next) {
		if (!block->used && block->size >= size) {
			/* split if enough room for another block */
			if (block->size > size + sizeof(struct heap_block) + 4) {
				nb = (struct heap_block *)((unsigned char *)block
				      + sizeof(struct heap_block) + size);
				nb->size = block->size - size - sizeof(struct heap_block);
				nb->used = 0;
				nb->next = block->next;
				block->size = size;
				block->next = nb;
			}
			block->used = 1;
			return (void *)((unsigned char *)block + sizeof(struct heap_block));
		}
	}
	return 0;   /* out of memory */
}

static void kfree(void *ptr)
{
	struct heap_block *block;
	if (!ptr) return;
	block = (struct heap_block *)((unsigned char *)ptr - sizeof(struct heap_block));
	block->used = 0;

	/* coalesce with next free block(s) */
	while (block->next && !block->next->used) {
		block->size += sizeof(struct heap_block) + block->next->size;
		block->next = block->next->next;
	}
}

/* ---- Task management (preemptive multitasking) ---- */

struct task {
	unsigned esp;
	int      active;
	char     name[16];
};

static struct task tasks[MAX_TASKS];
static int current_task;
static int num_tasks;

static void my_strcpy(char *dst, const char *src)
{
	while (*src) *dst++ = *src++;
	*dst = 0;
}

static void task_exit(void)
{
	tasks[current_task].active = 0;
	for (;;) __asm__ volatile ("hlt");
}

static void task_init_main(void)
{
	my_strcpy(tasks[0].name, "shell");
	tasks[0].active = 1;
	tasks[0].esp    = 0;   /* will be saved on first context switch */
	current_task = 0;
	num_tasks    = 1;
}

static int task_create(void (*func)(void), const char *name)
{
	unsigned *sp;
	int id;

	if (num_tasks >= MAX_TASKS) return -1;

	id = num_tasks;
	sp = (unsigned *)((unsigned char *)kmalloc(TASK_STACK_SIZE) + TASK_STACK_SIZE);

	/* fake stack: when POPAD + IRET execute, the task starts at func() */
	*(--sp) = (unsigned)task_exit;   /* return addr when func() returns */
	*(--sp) = 0x202;                /* EFLAGS  (IF=1) */
	*(--sp) = 0x08;                 /* CS */
	*(--sp) = (unsigned)func;       /* EIP */
	*(--sp) = 0;  /* EAX */
	*(--sp) = 0;  /* ECX */
	*(--sp) = 0;  /* EDX */
	*(--sp) = 0;  /* EBX */
	*(--sp) = 0;  /* ESP (ignored by POPAD) */
	*(--sp) = 0;  /* EBP */
	*(--sp) = 0;  /* ESI */
	*(--sp) = 0;  /* EDI */

	tasks[id].esp    = (unsigned)sp;
	tasks[id].active = 1;
	my_strcpy(tasks[id].name, name);
	num_tasks++;
	return id;
}

/* Background task: clock display in the top-right corner */
static void task_clock(void)
{
	unsigned last_sec = 0xFFFFFFFF;
	for (;;) {
		unsigned t = ticks;
		unsigned sec = t / TIMER_HZ;
		if (sec != last_sec) {
			unsigned min, hr;
			volatile unsigned short *v = (volatile unsigned short *)0xb8000;
			int col = VGA_COLS - 8;
			last_sec = sec;
			min = sec / 60;
			hr  = min / 60;
			sec %= 60;
			min %= 60;
			v[col+0] = 0x0e00 | ('0' + hr/10);
			v[col+1] = 0x0e00 | ('0' + hr%10);
			v[col+2] = 0x0e00 | ':';
			v[col+3] = 0x0e00 | ('0' + min/10);
			v[col+4] = 0x0e00 | ('0' + min%10);
			v[col+5] = 0x0e00 | ':';
			v[col+6] = 0x0e00 | ('0' + sec/10);
			v[col+7] = 0x0e00 | ('0' + sec%10);
		}
		__asm__ volatile ("hlt");
	}
}

/* ---- Interrupt handlers ---- */

extern void isr_timer(void);
extern void isr_keyboard(void);

unsigned timer_handler(unsigned esp)
{
	int next;
	ticks++;

	if (num_tasks <= 1) return esp;

	tasks[current_task].esp = esp;

	/* round-robin: find next active task */
	next = current_task;
	do {
		next = (next + 1) % num_tasks;
	} while (!tasks[next].active && next != current_task);

	current_task = next;
	return tasks[current_task].esp;
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

/* ---- File write / delete commands ---- */

static void cmd_write(const char *filename)
{
	struct fs_entry *entries;
	int buf_pos, line_start, slot, i, j;
	unsigned free_sector, end, sectors_needed;
	char c;

	/* Read directory: find free slot, check duplicates, find free sector */
	ata_read_sector(FS_DIR_SECTOR, disk_buf);
	entries = (struct fs_entry *)disk_buf;

	slot = -1;
	free_sector = 110;   /* data area starts at sector 110 */

	for (i = 0; i < FS_MAX_FILES; i++) {
		if (entries[i].name[0] == '\0') {
			if (slot < 0) slot = i;
			continue;
		}
		if (my_strcmp(entries[i].name, filename) == 0) {
			vga_puts("File exists. Use 'del' first.\n");
			return;
		}
		end = entries[i].start + (entries[i].size + 511) / 512;
		if (end > free_sector)
			free_sector = end;
	}

	if (slot < 0) {
		vga_puts("Directory full.\n");
		return;
	}

	/* Prompt user for text input */
	vga_puts("Enter text (blank line to save):\n");
	buf_pos = 0;

	for (;;) {
		vga_puts("> ");
		line_start = buf_pos;

		for (;;) {
			c = kbd_getchar();
			if (c == '\n') {
				vga_putchar('\n');
				break;
			} else if (c == '\b') {
				if (buf_pos > line_start) {
					buf_pos--;
					vga_putchar('\b');
				}
			} else if (buf_pos < FILE_BUF_SIZE - 2) {
				file_buf[buf_pos++] = (unsigned char)c;
				vga_putchar(c);
			}
		}

		if (buf_pos == line_start)
			break;
		if (buf_pos < FILE_BUF_SIZE - 1)
			file_buf[buf_pos++] = '\n';
	}

	if (buf_pos == 0) {
		vga_puts("Empty file, not saved.\n");
		return;
	}

	/* Write file data to disk, sector by sector */
	sectors_needed = ((unsigned)buf_pos + 511) / 512;
	for (i = 0; i < (int)sectors_needed; i++) {
		int offset = i * 512;
		int to_copy = buf_pos - offset;
		if (to_copy > 512) to_copy = 512;
		for (j = 0; j < 512; j++)
			disk_buf[j] = (j < to_copy) ? file_buf[offset + j] : 0;
		ata_write_sector(free_sector + (unsigned)i, disk_buf);
	}

	/* Update directory on disk */
	ata_read_sector(FS_DIR_SECTOR, disk_buf);
	entries = (struct fs_entry *)disk_buf;
	for (j = 0; j < 20; j++)
		entries[slot].name[j] = 0;
	for (j = 0; filename[j] && j < 19; j++)
		entries[slot].name[j] = filename[j];
	entries[slot].start = free_sector;
	entries[slot].size  = (unsigned)buf_pos;
	entries[slot].flags = 0;
	ata_write_sector(FS_DIR_SECTOR, disk_buf);

	vga_puts("Saved: ");
	vga_puts(filename);
	vga_puts(" (");
	vga_putint((unsigned)buf_pos);
	vga_puts(" bytes)\n");
}

static void cmd_del(const char *filename)
{
	struct fs_entry *entries;
	int i, j;

	ata_read_sector(FS_DIR_SECTOR, disk_buf);
	entries = (struct fs_entry *)disk_buf;

	for (i = 0; i < FS_MAX_FILES; i++) {
		if (entries[i].name[0] == '\0')
			continue;
		if (my_strcmp(entries[i].name, filename) == 0) {
			for (j = 0; j < 32; j++)
				((unsigned char *)&entries[i])[j] = 0;
			ata_write_sector(FS_DIR_SECTOR, disk_buf);
			vga_puts("Deleted: ");
			vga_puts(filename);
			vga_putchar('\n');
			return;
		}
	}

	vga_puts("File not found: ");
	vga_puts(filename);
	vga_putchar('\n');
}

/* ---- Task commands ---- */

static void cmd_ps(void)
{
	int i, len;
	const char *p;
	vga_puts("  ID  Name         Status\n");
	for (i = 0; i < num_tasks; i++) {
		vga_puts("  ");
		vga_putint((unsigned)i);
		vga_puts("   ");
		vga_puts(tasks[i].name);
		len = 0; p = tasks[i].name;
		while (*p++) len++;
		while (len++ < 13) vga_putchar(' ');
		if (i == current_task)      vga_puts("running\n");
		else if (tasks[i].active)   vga_puts("ready\n");
		else                        vga_puts("stopped\n");
	}
}

/* ---- Memory commands ---- */

static void cmd_mem(void)
{
	unsigned short e820_count = *(volatile unsigned short *)0x500;
	struct e820_entry *e = (struct e820_entry *)0x504;
	unsigned total_kb = 0;
	struct heap_block *block;
	unsigned heap_used = 0, heap_free = 0;
	int i;

	vga_puts("Memory Map (E820):\n");
	for (i = 0; i < e820_count && i < 20; i++) {
		vga_puts("  ");
		vga_puthex(e[i].base_lo);
		vga_puts(" - ");
		vga_puthex(e[i].base_lo + e[i].len_lo);
		switch (e[i].type) {
		case 1:  vga_puts(" usable");   total_kb += e[i].len_lo / 1024; break;
		case 2:  vga_puts(" reserved"); break;
		case 3:  vga_puts(" ACPI");     break;
		default: vga_puts(" other");    break;
		}
		vga_putchar('\n');
	}
	if (e820_count == 0)
		vga_puts("  (not available)\n");

	vga_puts("Total usable: ");
	vga_putint(total_kb);
	vga_puts(" KB (");
	vga_putint(total_kb / 1024);
	vga_puts(" MB)\n\n");

	vga_puts("Heap (2 MB at 0x200000):\n");
	for (block = heap_head; block; block = block->next) {
		if (block->used) heap_used += block->size;
		else             heap_free += block->size;
	}
	vga_puts("  Used: ");  vga_putint(heap_used);  vga_puts(" bytes\n");
	vga_puts("  Free: ");  vga_putint(heap_free);  vga_puts(" bytes\n");
}

static void cmd_memtest(void)
{
	void *a, *b, *c;

	vga_puts("malloc(100)... ");
	a = kmalloc(100);
	if (a) { vga_puts("OK at "); vga_puthex((unsigned)a); vga_putchar('\n'); }
	else   { vga_puts("FAIL\n"); return; }

	vga_puts("malloc(200)... ");
	b = kmalloc(200);
	if (b) { vga_puts("OK at "); vga_puthex((unsigned)b); vga_putchar('\n'); }
	else   { vga_puts("FAIL\n"); return; }

	vga_puts("free(first)... ");
	kfree(a);
	vga_puts("OK\n");

	vga_puts("malloc(50)...  ");
	c = kmalloc(50);
	if (c) {
		vga_puts("OK at "); vga_puthex((unsigned)c);
		if (c == a) vga_puts(" (reused!)");
		vga_putchar('\n');
	} else { vga_puts("FAIL\n"); return; }

	kfree(b);
	kfree(c);
	vga_puts("All tests passed.\n");
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
		vga_puts("  help        - Show this help\n");
		vga_puts("  ver         - Show version\n");
		vga_puts("  clear       - Clear screen\n");
		vga_puts("  echo ..     - Echo text\n");
		vga_puts("  uptime      - Show uptime\n");
		vga_puts("  dir / ls    - List files\n");
		vga_puts("  type FILE   - Display file\n");
		vga_puts("  write FILE  - Create file\n");
		vga_puts("  del FILE    - Delete file\n");
		vga_puts("  mem         - Memory info\n");
		vga_puts("  memtest     - Test malloc/free\n");
		vga_puts("  clock       - Start clock task\n");
		vga_puts("  ps          - List tasks\n");
		vga_puts("  kill N      - Kill task N\n");
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
	} else if (starts_with(cmd, "write ")) {
		cmd_write(cmd + 6);
	} else if (starts_with(cmd, "del ")) {
		cmd_del(cmd + 4);
	} else if (my_strcmp(cmd, "mem") == 0) {
		cmd_mem();
	} else if (my_strcmp(cmd, "memtest") == 0) {
		cmd_memtest();
	} else if (my_strcmp(cmd, "clock") == 0) {
		if (task_create(task_clock, "clock") >= 0)
			vga_puts("Clock task started.\n");
		else
			vga_puts("Cannot create task (max reached).\n");
	} else if (my_strcmp(cmd, "ps") == 0) {
		cmd_ps();
	} else if (starts_with(cmd, "kill ")) {
		int id = cmd[5] - '0';
		if (id > 0 && id < num_tasks && tasks[id].active) {
			tasks[id].active = 0;
			vga_puts("Killed task ");
			vga_putint((unsigned)id);
			vga_putchar('\n');
		} else {
			vga_puts("Invalid task ID.\n");
		}
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

	heap_init();
	task_init_main();
	pic_init();
	pit_init(TIMER_HZ);
	idt_set_gate(0x20, (unsigned)isr_timer);
	idt_set_gate(0x21, (unsigned)isr_keyboard);
	__asm__ volatile ("sti");

	vga_puts("Chocola Ver0.1\n");
	vga_puts("Type 'help' for available commands.\n\n");
	shell_run();
}
