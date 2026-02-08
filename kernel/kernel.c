/*
 * chocola kernel (C) — VGA graphics mode (640x480x256)
 * 32-bit protected mode, flat memory model.
 */

#define GFX_WIDTH     640
#define GFX_HEIGHT    480
#define CHAR_W        8
#define CHAR_H        14
#define CONSOLE_COLS  (GFX_WIDTH / CHAR_W)   /* 80 */
#define CONSOLE_ROWS  32
#define TASKBAR_Y     (CONSOLE_ROWS * CHAR_H) /* 448 */
#define VGA_BANK_SIZE 65536                    /* 64KB per bank window */

#define COL_BG        1    /* desktop blue */
#define COL_FG        15   /* white */
#define COL_TASKBAR   8    /* dark gray */
#define COL_TBTEXT    14   /* yellow */
#define COL_CURSOR    15   /* white */

#define CMD_BUF_SIZE  64
#define KBD_BUF_SIZE  32
#define TIMER_HZ      100

#define HIST_SIZE     16
#define KEY_UP        '\x01'
#define KEY_DOWN      '\x02'

#define HEAP_START    0x200000
#define HEAP_SIZE     0x200000

#define MAX_TASKS      8
#define TASK_STACK_SIZE 4096

#define FS_DIR_SECTOR 100
#define FS_MAX_FILES  16
#define FILE_BUF_SIZE 2048

static const unsigned char *font;   /* 8x14 BIOS font (pointer at 0x4F8) */

/* ---- I/O port helpers ---- */

static inline unsigned char inb(unsigned short port)
{ unsigned char v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

static inline unsigned short inw(unsigned short port)
{ unsigned short v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(port)); return v; }

static inline void outb(unsigned short port, unsigned char v)
{ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port)); }

static inline void outw(unsigned short port, unsigned short v)
{ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(port)); }

static inline void io_wait(void) { outb(0x80, 0); }

/* ---- VBE banked framebuffer (640x480 at 0xA0000, 64KB window) ---- */

static unsigned char *const fb_win = (unsigned char *)0xA0000;
static volatile int cur_bank = -1;

static void vbe_set_bank(int bank)
{
	if (bank == cur_bank) return;
	cur_bank = bank;
	/* Bochs VBE dispi interface — register 0x05 = BANK */
	outw(0x1CE, 0x05);
	outw(0x1CF, (unsigned short)bank);
}

static void fb_write(unsigned offset, unsigned char val)
{
	unsigned flags;
	__asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
	vbe_set_bank((int)(offset / VGA_BANK_SIZE));
	fb_win[offset % VGA_BANK_SIZE] = val;
	__asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory");
}

static unsigned char fb_read(unsigned offset)
{
	unsigned flags; unsigned char v;
	__asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
	vbe_set_bank((int)(offset / VGA_BANK_SIZE));
	v = fb_win[offset % VGA_BANK_SIZE];
	__asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory");
	return v;
}

/* ---- Scan code table ---- */

static const char sc_to_ascii[128] = {
	 0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
	'\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
	 0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
	 0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
	'*', 0, ' '
};

/* ---- Keyboard ring buffer ---- */

static volatile char kbd_buf[KBD_BUF_SIZE];
static volatile int  kbd_head, kbd_tail;

/* ---- Timer ---- */

static volatile unsigned int ticks;

/* ---- Mouse state ---- */

static volatile int mouse_x = 160, mouse_y = 100;
static volatile unsigned char mouse_btns;

/* ---- GUI state (shared between timer ISR and scroll) ---- */

static int gui_old_mx = -1, gui_old_my = -1;
static volatile int gui_no_cursor;   /* when set, timer skips cursor update */

/* ---- Command history ---- */

static char history[HIST_SIZE][CMD_BUF_SIZE];
static int  hist_count;

/* ---- Disk / FS buffers ---- */

static unsigned char disk_buf[512];
static unsigned char file_buf[FILE_BUF_SIZE];

struct fs_entry {
	char         name[20];
	unsigned int start, size, flags;
} __attribute__((packed));

/* ---- Graphics primitives ---- */

static void gfx_pixel(int x, int y, unsigned char c)
{
	if ((unsigned)x < GFX_WIDTH && (unsigned)y < GFX_HEIGHT)
		fb_write((unsigned)y * GFX_WIDTH + (unsigned)x, c);
}

static void gfx_rect(int x, int y, int w, int h, unsigned char c)
{
	int i, j, x2 = x + w, y2 = y + h;
	if (x < 0) x = 0;  if (y < 0) y = 0;
	if (x2 > GFX_WIDTH) x2 = GFX_WIDTH;
	if (y2 > GFX_HEIGHT) y2 = GFX_HEIGHT;
	for (j = y; j < y2; j++)
		for (i = x; i < x2; i++)
			fb_write((unsigned)j * GFX_WIDTH + (unsigned)i, c);
}

static void gfx_char(int x, int y, char ch, unsigned char fg, unsigned char bg)
{
	int row, col;
	unsigned char bits;
	unsigned base;
	for (row = 0; row < CHAR_H; row++) {
		bits = font[(unsigned char)ch * CHAR_H + row];
		base = (unsigned)(y + row) * GFX_WIDTH + (unsigned)x;
		for (col = 0; col < 8; col++)
			fb_write(base + (unsigned)col, (bits & (0x80 >> col)) ? fg : bg);
	}
}

static void gfx_text(int x, int y, const char *s, unsigned char fg, unsigned char bg)
{
	while (*s) { gfx_char(x, y, *s++, fg, bg); x += 8; }
}

/* ---- Mouse cursor (forward declarations for scroll) ---- */

static void cursor_hide(int cx, int cy);
static void cursor_show(int cx, int cy);

/* ---- Console (character grid on framebuffer) ---- */

static int cur_x, cur_y;

static void vga_scroll(void)
{
	unsigned total = (unsigned)GFX_WIDTH * (unsigned)(CONSOLE_ROWS - 1) * (unsigned)CHAR_H;
	unsigned src_off = (unsigned)GFX_WIDTH * (unsigned)CHAR_H;
	unsigned flags, i;
	int mx, my;

	/* Prevent timer ISR from touching cursor during scroll */
	gui_no_cursor = 1;
	if (gui_old_mx >= 0)
		cursor_hide(gui_old_mx, gui_old_my);

	/* Disable interrupts — fast bulk copy with no per-pixel overhead */
	__asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");

	i = 0;
	while (i < total) {
		unsigned src = i + src_off;
		int db = (int)(i   / VGA_BANK_SIZE);
		int sb = (int)(src / VGA_BANK_SIZE);
		unsigned dr = VGA_BANK_SIZE - (i   % VGA_BANK_SIZE);
		unsigned sr = VGA_BANK_SIZE - (src % VGA_BANK_SIZE);
		unsigned chunk = dr < sr ? dr : sr;
		if (i + chunk > total) chunk = total - i;

		if (sb == db) {
			/* Same bank — fast 4-byte copy within window */
			unsigned s = src % VGA_BANK_SIZE, d = i % VGA_BANK_SIZE;
			unsigned j, w = chunk >> 2;
			vbe_set_bank(db);
			for (j = 0; j < w; j++)
				((unsigned *)(fb_win + d))[j] = ((unsigned *)(fb_win + s))[j];
			for (j = w << 2; j < chunk; j++)
				fb_win[d + j] = fb_win[s + j];
		} else {
			/* Cross-bank — batch via temp buffer */
			unsigned char tmp[512];
			unsigned s = src % VGA_BANK_SIZE, d = i % VGA_BANK_SIZE;
			unsigned done = 0;
			while (done < chunk) {
				unsigned batch = chunk - done, j;
				if (batch > 512) batch = 512;
				vbe_set_bank(sb);
				for (j = 0; j < batch; j++) tmp[j] = fb_win[s + done + j];
				vbe_set_bank(db);
				for (j = 0; j < batch; j++) fb_win[d + done + j] = tmp[j];
				done += batch;
			}
		}
		i += chunk;
	}

	/* Clear last row (fits in one bank) */
	{
		unsigned start = (unsigned)(CONSOLE_ROWS - 1) * CHAR_H * GFX_WIDTH;
		unsigned count = (unsigned)GFX_WIDTH * CHAR_H;
		unsigned base = start % VGA_BANK_SIZE;
		unsigned j, w = count >> 2;
		vbe_set_bank((int)(start / VGA_BANK_SIZE));
		for (j = 0; j < w; j++)
			((unsigned *)(fb_win + base))[j] = COL_BG * 0x01010101u;
		for (j = w << 2; j < count; j++)
			fb_win[base + j] = COL_BG;
	}
	cur_y = CONSOLE_ROWS - 1;

	__asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory");

	/* Restore cursor at current mouse position */
	mx = mouse_x; my = mouse_y;
	cursor_show(mx, my);
	gui_old_mx = mx; gui_old_my = my;
	gui_no_cursor = 0;
}

static void vga_putchar(char c)
{
	if (c == '\n') {
		cur_x = 0; cur_y++;
	} else if (c == '\b') {
		if (cur_x > 0) { cur_x--; gfx_char(cur_x*CHAR_W, cur_y*CHAR_H, ' ', COL_FG, COL_BG); }
	} else if (c == '\t') {
		cur_x = (cur_x + 4) & ~3;
		if (cur_x >= CONSOLE_COLS) { cur_x = 0; cur_y++; }
	} else {
		gfx_char(cur_x*CHAR_W, cur_y*CHAR_H, c, COL_FG, COL_BG);
		cur_x++;
		if (cur_x >= CONSOLE_COLS) { cur_x = 0; cur_y++; }
	}
	if (cur_y >= CONSOLE_ROWS) vga_scroll();
}

static void vga_puts(const char *s) { while (*s) vga_putchar(*s++); }

static void vga_putint(unsigned n)
{
	char buf[12]; int i = 0;
	if (n == 0) { vga_putchar('0'); return; }
	while (n) { buf[i++] = '0' + n % 10; n /= 10; }
	while (--i >= 0) vga_putchar(buf[i]);
}

static void vga_puthex(unsigned n)
{
	static const char h[] = "0123456789abcdef";
	int i; vga_puts("0x");
	for (i = 28; i >= 0; i -= 4) vga_putchar(h[(n >> i) & 0xF]);
}

static void vga_clear(void)
{
	gfx_rect(0, 0, GFX_WIDTH, TASKBAR_Y, COL_BG);
	cur_x = cur_y = 0;
}

/* ---- Desktop (taskbar + palette) ---- */

static void desktop_init(void)
{
	/* Custom palette */
	outb(0x3C8, 1); outb(0x3C9, 0x08); outb(0x3C9, 0x10); outb(0x3C9, 0x28); /* blue bg */
	outb(0x3C8, 8); outb(0x3C9, 0x12); outb(0x3C9, 0x12); outb(0x3C9, 0x12); /* taskbar gray */

	/* Background */
	gfx_rect(0, 0, GFX_WIDTH, TASKBAR_Y, COL_BG);
	/* Taskbar */
	gfx_rect(0, TASKBAR_Y, GFX_WIDTH, GFX_HEIGHT - TASKBAR_Y, COL_TASKBAR);
	gfx_text(8, TASKBAR_Y + 9, "Chocola", COL_TBTEXT, COL_TASKBAR);
}

/* ---- PIC ---- */

static void pic_init(void)
{
	outb(0x20,0x11); io_wait(); outb(0xA0,0x11); io_wait();
	outb(0x21,0x20); io_wait(); outb(0xA1,0x28); io_wait();
	outb(0x21,0x04); io_wait(); outb(0xA1,0x02); io_wait();
	outb(0x21,0x01); io_wait(); outb(0xA1,0x01); io_wait();
	outb(0x21, 0xF8);   /* master: unmask IRQ0,1,2 */
	outb(0xA1, 0xEF);   /* slave:  unmask IRQ12 (mouse) */
}

/* ---- PIT ---- */

static void pit_init(unsigned hz)
{
	unsigned short d = (unsigned short)(1193182 / hz);
	outb(0x43,0x34); outb(0x40, d & 0xFF); outb(0x40, (d>>8) & 0xFF);
}

/* ---- IDT ---- */

struct idt_entry { unsigned short ol; unsigned short sel; unsigned char z, ta; unsigned short oh; } __attribute__((packed));

static void idt_set_gate(int n, unsigned h)
{
	volatile struct idt_entry *idt = (volatile struct idt_entry *)0x70000;
	idt[n].ol = h & 0xFFFF; idt[n].sel = 0x08; idt[n].z = 0; idt[n].ta = 0x8E; idt[n].oh = (h>>16) & 0xFFFF;
}

/* ---- Heap allocator ---- */

struct heap_block { unsigned int size, used; struct heap_block *next; };
static struct heap_block *heap_head;

static void heap_init(void)
{
	heap_head = (struct heap_block *)HEAP_START;
	heap_head->size = HEAP_SIZE - sizeof(struct heap_block);
	heap_head->used = 0; heap_head->next = 0;
}

static void *kmalloc(unsigned sz)
{
	struct heap_block *b, *nb;
	sz = (sz + 3) & ~3;
	for (b = heap_head; b; b = b->next) {
		if (!b->used && b->size >= sz) {
			if (b->size > sz + sizeof(struct heap_block) + 4) {
				nb = (struct heap_block *)((unsigned char *)b + sizeof(struct heap_block) + sz);
				nb->size = b->size - sz - sizeof(struct heap_block);
				nb->used = 0; nb->next = b->next;
				b->size = sz; b->next = nb;
			}
			b->used = 1;
			return (void *)((unsigned char *)b + sizeof(struct heap_block));
		}
	}
	return 0;
}

static void kfree(void *p)
{
	struct heap_block *b;
	if (!p) return;
	b = (struct heap_block *)((unsigned char *)p - sizeof(struct heap_block));
	b->used = 0;
	while (b->next && !b->next->used) {
		b->size += sizeof(struct heap_block) + b->next->size;
		b->next = b->next->next;
	}
}

/* ---- Task management ---- */

struct task { unsigned esp; int active; char name[16]; };
static struct task tasks[MAX_TASKS];
static int current_task, num_tasks;

static void my_strcpy(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; }

static void task_exit(void) { tasks[current_task].active = 0; for(;;) __asm__ volatile("hlt"); }

static void task_init_main(void)
{
	my_strcpy(tasks[0].name, "shell");
	tasks[0].active = 1; tasks[0].esp = 0;
	current_task = 0; num_tasks = 1;
}

static int task_create(void (*fn)(void), const char *name)
{
	unsigned *sp; int id;
	if (num_tasks >= MAX_TASKS) return -1;
	id = num_tasks;
	sp = (unsigned *)((unsigned char *)kmalloc(TASK_STACK_SIZE) + TASK_STACK_SIZE);
	*(--sp) = (unsigned)task_exit;
	*(--sp) = 0x202; *(--sp) = 0x08; *(--sp) = (unsigned)fn;
	*(--sp)=0; *(--sp)=0; *(--sp)=0; *(--sp)=0;
	*(--sp)=0; *(--sp)=0; *(--sp)=0; *(--sp)=0;
	tasks[id].esp = (unsigned)sp;
	tasks[id].active = 1;
	my_strcpy(tasks[id].name, name);
	num_tasks++;
	return id;
}

/* ---- Mouse cursor (10x14 arrow) ---- */

#define CUR_W 10
#define CUR_H 14

/* 1=white pixel, 2=black outline */
static const unsigned char cursor_data[CUR_H][CUR_W] = {
	{2,0,0,0,0,0,0,0,0,0},
	{2,2,0,0,0,0,0,0,0,0},
	{2,1,2,0,0,0,0,0,0,0},
	{2,1,1,2,0,0,0,0,0,0},
	{2,1,1,1,2,0,0,0,0,0},
	{2,1,1,1,1,2,0,0,0,0},
	{2,1,1,1,1,1,2,0,0,0},
	{2,1,1,1,1,1,1,2,0,0},
	{2,1,1,1,1,1,1,1,2,0},
	{2,1,1,1,1,2,2,2,2,0},
	{2,1,1,2,1,2,0,0,0,0},
	{2,1,2,0,2,1,2,0,0,0},
	{2,2,0,0,2,1,2,0,0,0},
	{2,0,0,0,0,2,2,0,0,0},
};

static unsigned char cursor_save[CUR_W * CUR_H];

static void cursor_hide(int cx, int cy)
{
	int r, c, idx = 0;
	unsigned off;
	for (r = 0; r < CUR_H; r++)
		for (c = 0; c < CUR_W; c++, idx++)
			if (cursor_data[r][c] && (unsigned)(cx+c) < GFX_WIDTH && (unsigned)(cy+r) < GFX_HEIGHT) {
				off = (unsigned)(cy+r)*GFX_WIDTH + (unsigned)(cx+c);
				fb_write(off, cursor_save[idx]);
			}
}

static void cursor_show(int cx, int cy)
{
	int r, c, idx = 0;
	unsigned off;
	for (r = 0; r < CUR_H; r++)
		for (c = 0; c < CUR_W; c++, idx++) {
			if ((unsigned)(cx+c) >= GFX_WIDTH || (unsigned)(cy+r) >= GFX_HEIGHT) continue;
			off = (unsigned)(cy+r)*GFX_WIDTH + (unsigned)(cx+c);
			cursor_save[idx] = fb_read(off);
			if (cursor_data[r][c] == 2)
				fb_write(off, 0);
			else if (cursor_data[r][c] == 1)
				fb_write(off, 15);
		}
}

/* ---- Interrupt handlers ---- */

extern void isr_timer(void);
extern void isr_keyboard(void);
extern void isr_mouse(void);

unsigned timer_handler(unsigned esp)
{
	static unsigned gui_last_sec = 0xFFFFFFFF;
	int saved_bank = cur_bank;          /* save shell's bank state */

	ticks++;

	/* ---- GUI: clock (once per second) ---- */
	{
		unsigned total_sec = ticks / TIMER_HZ;
		if (total_sec != gui_last_sec) {
			unsigned sec = total_sec % 60;
			unsigned min = (total_sec / 60) % 60;
			unsigned hr  = total_sec / 3600;
			char tb[9];
			tb[0] = '0'+hr/10; tb[1] = '0'+hr%10; tb[2] = ':';
			tb[3] = '0'+min/10; tb[4] = '0'+min%10; tb[5] = ':';
			tb[6] = '0'+sec/10; tb[7] = '0'+sec%10; tb[8] = 0;
			gfx_text(GFX_WIDTH - 72, TASKBAR_Y + 9, tb, COL_TBTEXT, COL_TASKBAR);
			gui_last_sec = total_sec;
		}
	}

	/* ---- GUI: mouse cursor (skip during scroll) ---- */
	if (!gui_no_cursor) {
		int mx = mouse_x, my = mouse_y;
		if (mx != gui_old_mx || my != gui_old_my) {
			if (gui_old_mx >= 0) cursor_hide(gui_old_mx, gui_old_my);
			cursor_show(mx, my);
			gui_old_mx = mx; gui_old_my = my;
		}
	}

	cur_bank = saved_bank;              /* restore shell's bank state */
	if (saved_bank >= 0) {              /* restore hardware bank too */
		outw(0x1CE, 0x05);
		outw(0x1CF, (unsigned short)saved_bank);
	}

	/* ---- Task switching ---- */
	if (num_tasks <= 1) return esp;
	tasks[current_task].esp = esp;
	{
		int next = current_task;
		do { next = (next+1) % num_tasks; } while (!tasks[next].active && next != current_task);
		current_task = next;
	}
	return tasks[current_task].esp;
}

void keyboard_handler(void)
{
	static int e0_flag = 0;
	unsigned char sc = inb(0x60);
	int next;

	if (sc == 0xE0) { e0_flag = 1; return; }

	if (e0_flag) {
		e0_flag = 0;
		if (sc & 0x80) return;          /* release of extended key */
		char ch = 0;
		if (sc == 0x48) ch = KEY_UP;    /* up arrow */
		else if (sc == 0x50) ch = KEY_DOWN; /* down arrow */
		if (ch) {
			next = (kbd_head+1) % KBD_BUF_SIZE;
			if (next != kbd_tail) { kbd_buf[kbd_head] = ch; kbd_head = next; }
		}
		return;
	}

	if (sc & 0x80) return;
	if (sc < sizeof(sc_to_ascii) && sc_to_ascii[sc]) {
		next = (kbd_head+1) % KBD_BUF_SIZE;
		if (next != kbd_tail) { kbd_buf[kbd_head] = sc_to_ascii[sc]; kbd_head = next; }
	}
}

void mouse_handler(void)
{
	static int cycle = 0;
	static unsigned char bytes[3];
	int dx, dy;

	/* Only read if data is from auxiliary device (mouse, not keyboard) */
	if (!(inb(0x64) & 0x20)) { inb(0x60); return; }

	bytes[cycle] = inb(0x60);

	/* Byte 0 must have bit 3 set (PS/2 always-1 bit); resync if not */
	if (cycle == 0 && !(bytes[0] & 0x08)) return;

	cycle++;
	if (cycle < 3) return;
	cycle = 0;

	mouse_btns = bytes[0] & 7;
	dx = bytes[1]; dy = bytes[2];
	if (bytes[0] & 0x10) dx -= 256;
	if (bytes[0] & 0x20) dy -= 256;
	mouse_x += dx; mouse_y -= dy;
	if (mouse_x < 0) mouse_x = 0;
	if (mouse_x > GFX_WIDTH - CUR_W) mouse_x = GFX_WIDTH - CUR_W;
	if (mouse_y < 0) mouse_y = 0;
	if (mouse_y > TASKBAR_Y - 1) mouse_y = TASKBAR_Y - 1;
}

/* ---- Keyboard ---- */

static char kbd_getchar(void)
{
	char c;
	while (kbd_head == kbd_tail) __asm__ volatile("hlt");
	c = kbd_buf[kbd_tail]; kbd_tail = (kbd_tail+1) % KBD_BUF_SIZE;
	return c;
}

/* ---- ATA PIO ---- */

static void ata_read_sector(unsigned lba, void *buf)
{
	int i; unsigned short *p = (unsigned short *)buf;
	while (inb(0x1F7) & 0x80);
	outb(0x1F6, 0xE0|((lba>>24)&0xF));
	outb(0x1F2,1); outb(0x1F3,lba&0xFF); outb(0x1F4,(lba>>8)&0xFF); outb(0x1F5,(lba>>16)&0xFF);
	outb(0x1F7,0x20);
	while (!(inb(0x1F7) & 0x08));
	for (i=0;i<256;i++) p[i]=inw(0x1F0);
}

static void ata_write_sector(unsigned lba, const void *buf)
{
	int i; const unsigned short *p = (const unsigned short *)buf;
	while (inb(0x1F7)&0x80);
	outb(0x1F6,0xE0|((lba>>24)&0xF));
	outb(0x1F2,1); outb(0x1F3,lba&0xFF); outb(0x1F4,(lba>>8)&0xFF); outb(0x1F5,(lba>>16)&0xFF);
	outb(0x1F7,0x30);
	while (!(inb(0x1F7)&0x08));
	for (i=0;i<256;i++) outw(0x1F0,p[i]);
	outb(0x1F7,0xE7); while (inb(0x1F7)&0x80);
}

/* ---- String helpers ---- */

static int my_strcmp(const char *a, const char *b)
{ while (*a && *a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }

static int starts_with(const char *s, const char *p)
{ while(*p){if(*s++!=*p++)return 0;} return 1; }

/* ---- PS/2 mouse init ---- */

static void mouse_wait_in(void)  { int i; for(i=0;i<100000;i++) if(!(inb(0x64)&2)) return; }
static void mouse_wait_out(void) { int i; for(i=0;i<100000;i++) if(inb(0x64)&1) return; }

static void mouse_init(void)
{
	unsigned char st;

	/* Flush any stale data in the controller buffer */
	while (inb(0x64) & 1) inb(0x60);

	mouse_wait_in(); outb(0x64, 0xA8);
	mouse_wait_in(); outb(0x64, 0x20);
	mouse_wait_out(); st = inb(0x60);
	st |= 2;
	mouse_wait_in(); outb(0x64, 0x60);
	mouse_wait_in(); outb(0x60, st);
	mouse_wait_in(); outb(0x64, 0xD4);
	mouse_wait_in(); outb(0x60, 0xF6);
	mouse_wait_out(); inb(0x60);
	mouse_wait_in(); outb(0x64, 0xD4);
	mouse_wait_in(); outb(0x60, 0xF4);
	mouse_wait_out(); inb(0x60);
}

/* ---- Filesystem commands ---- */

static void cmd_dir(void)
{
	struct fs_entry *e; int i, count = 0;
	ata_read_sector(FS_DIR_SECTOR, disk_buf);
	e = (struct fs_entry *)disk_buf;
	for (i = 0; i < FS_MAX_FILES; i++) {
		if (!e[i].name[0]) break;
		vga_puts("  "); vga_puts(e[i].name);
		{ int l=0; const char *p=e[i].name; while(*p++)l++; while(l++<20) vga_putchar(' '); }
		vga_putint(e[i].size); vga_puts(" bytes\n"); count++;
	}
	if (!count) vga_puts("  (no files)\n");
	vga_putint((unsigned)count); vga_puts(" file(s)\n");
}

static void cmd_type(const char *fn)
{
	struct fs_entry *e; int i; unsigned rem, sec, tp, j;
	ata_read_sector(FS_DIR_SECTOR, disk_buf); e = (struct fs_entry *)disk_buf;
	for (i=0;i<FS_MAX_FILES;i++) {
		if (!e[i].name[0]) break;
		if (my_strcmp(e[i].name,fn)==0) {
			rem=e[i].size; sec=e[i].start;
			while (rem) { ata_read_sector(sec,disk_buf); tp=rem>512?512:rem;
				for(j=0;j<tp;j++) vga_putchar((char)disk_buf[j]); rem-=tp; sec++; }
			return;
		}
	}
	vga_puts("File not found: "); vga_puts(fn); vga_putchar('\n');
}

static void cmd_write(const char *fn)
{
	struct fs_entry *e; int bp=0,ls,sl=-1,i,j; unsigned fs=110,end,sn; char c;
	ata_read_sector(FS_DIR_SECTOR,disk_buf); e=(struct fs_entry*)disk_buf;
	for(i=0;i<FS_MAX_FILES;i++){
		if(!e[i].name[0]){if(sl<0)sl=i;continue;}
		if(my_strcmp(e[i].name,fn)==0){vga_puts("File exists. Use 'del' first.\n");return;}
		end=e[i].start+(e[i].size+511)/512; if(end>fs)fs=end;
	}
	if(sl<0){vga_puts("Directory full.\n");return;}
	vga_puts("Enter text (blank line to save):\n");
	for(;;){
		vga_puts("> "); ls=bp;
		for(;;){c=kbd_getchar();if(c=='\n'){vga_putchar('\n');break;}
			else if(c=='\b'){if(bp>ls){bp--;vga_putchar('\b');}}
			else if(bp<FILE_BUF_SIZE-2){file_buf[bp++]=(unsigned char)c;vga_putchar(c);}}
		if(bp==ls)break; if(bp<FILE_BUF_SIZE-1)file_buf[bp++]='\n';
	}
	if(!bp){vga_puts("Empty file, not saved.\n");return;}
	sn=((unsigned)bp+511)/512;
	for(i=0;i<(int)sn;i++){int o=i*512,tc=bp-o;if(tc>512)tc=512;
		for(j=0;j<512;j++)disk_buf[j]=(j<tc)?file_buf[o+j]:0;
		ata_write_sector(fs+(unsigned)i,disk_buf);}
	ata_read_sector(FS_DIR_SECTOR,disk_buf);e=(struct fs_entry*)disk_buf;
	for(j=0;j<20;j++)e[sl].name[j]=0;
	for(j=0;fn[j]&&j<19;j++)e[sl].name[j]=fn[j];
	e[sl].start=fs;e[sl].size=(unsigned)bp;e[sl].flags=0;
	ata_write_sector(FS_DIR_SECTOR,disk_buf);
	vga_puts("Saved: ");vga_puts(fn);vga_puts(" (");vga_putint((unsigned)bp);vga_puts(" bytes)\n");
}

static void cmd_del(const char *fn)
{
	struct fs_entry *e; int i,j;
	ata_read_sector(FS_DIR_SECTOR,disk_buf);e=(struct fs_entry*)disk_buf;
	for(i=0;i<FS_MAX_FILES;i++){
		if(!e[i].name[0])continue;
		if(my_strcmp(e[i].name,fn)==0){
			for(j=0;j<32;j++)((unsigned char*)&e[i])[j]=0;
			ata_write_sector(FS_DIR_SECTOR,disk_buf);
			vga_puts("Deleted: ");vga_puts(fn);vga_putchar('\n');return;
		}
	}
	vga_puts("File not found: ");vga_puts(fn);vga_putchar('\n');
}

/* ---- Task commands ---- */

static void cmd_ps(void)
{
	int i,l; const char *p;
	vga_puts("  ID  Name         Status\n");
	for(i=0;i<num_tasks;i++){
		vga_puts("  ");vga_putint((unsigned)i);vga_puts("   ");vga_puts(tasks[i].name);
		l=0;p=tasks[i].name;while(*p++)l++;while(l++<13)vga_putchar(' ');
		if(i==current_task)vga_puts("running\n");
		else if(tasks[i].active)vga_puts("ready\n");
		else vga_puts("stopped\n");
	}
}

/* ---- Memory commands ---- */

struct e820_entry { unsigned int blo,bhi,llo,lhi,type,acpi; } __attribute__((packed));

static void cmd_mem(void)
{
	unsigned short e820n=*(volatile unsigned short*)0x500;
	struct e820_entry *e=(struct e820_entry*)0x504;
	unsigned tkb=0; struct heap_block *b; unsigned hu=0,hf=0; int i;
	vga_puts("Memory Map (E820):\n");
	for(i=0;i<e820n&&i<20;i++){
		vga_puts("  ");vga_puthex(e[i].blo);vga_puts(" - ");vga_puthex(e[i].blo+e[i].llo);
		switch(e[i].type){case 1:vga_puts(" usable");tkb+=e[i].llo/1024;break;
			case 2:vga_puts(" reserved");break;default:vga_puts(" other");break;}
		vga_putchar('\n');
	}
	if(!e820n)vga_puts("  (not available)\n");
	vga_puts("Total: ");vga_putint(tkb);vga_puts(" KB (");vga_putint(tkb/1024);vga_puts(" MB)\n\n");
	vga_puts("Heap:\n");
	for(b=heap_head;b;b=b->next){if(b->used)hu+=b->size;else hf+=b->size;}
	vga_puts("  Used: ");vga_putint(hu);vga_puts("  Free: ");vga_putint(hf);vga_putchar('\n');
}

static void cmd_memtest(void)
{
	void *a,*b,*c;
	vga_puts("malloc(100)... ");a=kmalloc(100);
	if(a){vga_puts("OK ");vga_puthex((unsigned)a);vga_putchar('\n');}else{vga_puts("FAIL\n");return;}
	vga_puts("malloc(200)... ");b=kmalloc(200);
	if(b){vga_puts("OK ");vga_puthex((unsigned)b);vga_putchar('\n');}else{vga_puts("FAIL\n");return;}
	vga_puts("free(first)... ");kfree(a);vga_puts("OK\n");
	vga_puts("malloc(50)...  ");c=kmalloc(50);
	if(c){vga_puts("OK ");vga_puthex((unsigned)c);if(c==a)vga_puts(" (reused!)");vga_putchar('\n');}
	else{vga_puts("FAIL\n");return;}
	kfree(b);kfree(c);vga_puts("All tests passed.\n");
}

/* ---- Shell ---- */

static void print_prompt(void) { vga_puts("C:\\>"); }

static void shell_exec(char *cmd)
{
	while(*cmd==' ')cmd++;
	if(!cmd[0]){}
	else if(my_strcmp(cmd,"help")==0){
		vga_puts("Commands:\n");
		vga_puts("  help ver clear echo uptime history\n");
		vga_puts("  dir ls type cat write del\n");
		vga_puts("  mem memtest ps kill\n");
	}
	else if(my_strcmp(cmd,"history")==0){
		int i;
		if(hist_count==0){ vga_puts("(no history)\n"); }
		else for(i=0;i<hist_count;i++){
			vga_puts("  ");vga_putint((unsigned)(i+1));vga_puts("  ");
			vga_puts(history[i]);vga_putchar('\n');
		}
	}
	else if(my_strcmp(cmd,"ver")==0) vga_puts("Chocola Ver0.1\n");
	else if(my_strcmp(cmd,"clear")==0) vga_clear();
	else if(starts_with(cmd,"echo ")) { vga_puts(cmd+5); vga_putchar('\n'); }
	else if(my_strcmp(cmd,"echo")==0) vga_putchar('\n');
	else if(my_strcmp(cmd,"uptime")==0){
		unsigned t=ticks,s=t/TIMER_HZ,m=s/60;s%=60;
		vga_putint(m);vga_puts("m ");vga_putint(s);vga_puts("s (");vga_putint(t);vga_puts(" ticks)\n");
	}
	else if(my_strcmp(cmd,"dir")==0||my_strcmp(cmd,"ls")==0) cmd_dir();
	else if(starts_with(cmd,"type ")) cmd_type(cmd+5);
	else if(starts_with(cmd,"cat ")) cmd_type(cmd+4);
	else if(starts_with(cmd,"write ")) cmd_write(cmd+6);
	else if(starts_with(cmd,"del ")) cmd_del(cmd+4);
	else if(my_strcmp(cmd,"mem")==0) cmd_mem();
	else if(my_strcmp(cmd,"memtest")==0) cmd_memtest();
	else if(my_strcmp(cmd,"ps")==0) cmd_ps();
	else if(starts_with(cmd,"kill ")){
		int id=cmd[5]-'0';
		if(id>0&&id<num_tasks&&tasks[id].active){tasks[id].active=0;
			vga_puts("Killed task ");vga_putint((unsigned)id);vga_putchar('\n');}
		else vga_puts("Invalid task ID.\n");
	}
	else { vga_puts("Unknown: "); vga_puts(cmd); vga_putchar('\n'); }
}

static void shell_run(void)
{
	char buf[CMD_BUF_SIZE]; int pos, hist_nav, i; char c;
	for(;;){
		print_prompt(); pos=0; hist_nav=hist_count;
		for(;;){
			c=kbd_getchar();
			if(c=='\n'){
				vga_putchar('\n'); buf[pos]=0;
				/* save non-empty command to history */
				if(pos>0){
					if(hist_count>=HIST_SIZE){
						for(i=0;i<HIST_SIZE-1;i++) my_strcpy(history[i],history[i+1]);
						hist_count=HIST_SIZE-1;
					}
					my_strcpy(history[hist_count],buf);
					hist_count++;
				}
				break;
			}
			else if(c=='\b'){if(pos>0){pos--;vga_putchar('\b');}}
			else if(c==KEY_UP){
				if(hist_nav>0){
					hist_nav--;
					for(i=0;i<pos;i++) vga_putchar('\b');
					my_strcpy(buf, history[hist_nav]);
					pos=0; while(buf[pos]) pos++;
					vga_puts(buf);
				}
			}
			else if(c==KEY_DOWN){
				if(hist_nav<hist_count-1){
					hist_nav++;
					for(i=0;i<pos;i++) vga_putchar('\b');
					my_strcpy(buf, history[hist_nav]);
					pos=0; while(buf[pos]) pos++;
					vga_puts(buf);
				}
				else if(hist_nav<hist_count){
					hist_nav=hist_count;
					for(i=0;i<pos;i++) vga_putchar('\b');
					pos=0; buf[0]=0;
				}
			}
			else if(pos<CMD_BUF_SIZE-1){buf[pos++]=c;vga_putchar(c);}
		}
		shell_exec(buf);
	}
}

/* ---- Kernel entry ---- */

void kernel_main(void)
{
	font = (const unsigned char *)(*(unsigned int *)0x4F8);

	heap_init();
	task_init_main();
	pic_init();
	pit_init(TIMER_HZ);
	mouse_init();

	idt_set_gate(0x20, (unsigned)isr_timer);
	idt_set_gate(0x21, (unsigned)isr_keyboard);
	idt_set_gate(0x2C, (unsigned)isr_mouse);
	__asm__ volatile("sti");

	desktop_init();

	vga_puts("Chocola Ver0.1\n");
	vga_puts("Type 'help' for commands.\n\n");
	shell_run();
}
