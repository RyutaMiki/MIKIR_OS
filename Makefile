# MIKIR-OS build
# Requires: nasm, (gcc -m32 or i686-elf-gcc), ld, objcopy, dd
# On Apple Silicon: brew install i686-elf-gcc  (for x86 32-bit kernel)

ASM		= nasm
ASMFLAGS_BIN	= -f bin
ASMFLAGS_ELF	= -f elf32

# Use i686-elf cross-compiler if available (needed on Mac ARM)
CROSS		:= $(shell command -v i686-elf-gcc >/dev/null 2>&1 && echo i686-elf- || true)
ifneq ($(CROSS),)
  CC		= i686-elf-gcc
  CFLAGS	= -ffreestanding -Wall -Wextra -O2 -c -I.
  LD		= i686-elf-ld
  OBJCOPY	= i686-elf-objcopy
else
  CC		= gcc
  CFLAGS	= -ffreestanding -m32 -Wall -Wextra -O2 -c -I.
  LD		= ld
  OBJCOPY	= objcopy
endif

LDFLAGS		= -m elf_i386 -T link.ld -nostdlib

IPL		= ipl.bin
LOADER		= loader.o
KERNEL_C	= kernel/kernel.c
KERNEL_O	= kernel.o
KERNEL_ELF	= kernel.elf
KERNEL_BIN	= kernel.bin
IMAGE		= mikiros.img
SECTORS		= 32768

.PHONY: all clean run

all: $(IMAGE)

$(IPL): ipl.nas
	$(ASM) $(ASMFLAGS_BIN) -o $(IPL) ipl.nas

$(LOADER): loader.nas
	$(ASM) $(ASMFLAGS_ELF) -o $(LOADER) loader.nas

$(KERNEL_O): $(KERNEL_C)
	$(CC) $(CFLAGS) -o $(KERNEL_O) $(KERNEL_C)

$(KERNEL_ELF): $(LOADER) $(KERNEL_O)
	$(LD) $(LDFLAGS) -o $(KERNEL_ELF) $(LOADER) $(KERNEL_O)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $(KERNEL_BIN)

$(IMAGE): $(IPL) $(KERNEL_BIN)
	dd if=/dev/zero of=$(IMAGE) bs=512 count=$(SECTORS) 2>/dev/null
	dd if=$(IPL) of=$(IMAGE) bs=512 conv=notrunc
	dd if=$(KERNEL_BIN) of=$(IMAGE) bs=512 seek=1 conv=notrunc

clean:
	rm -f $(IPL) $(LOADER) $(KERNEL_O) $(KERNEL_ELF) $(KERNEL_BIN) $(IMAGE)

run: $(IMAGE)
	qemu-system-i386 -boot order=c -drive file=$(IMAGE),format=raw,if=ide,index=0
