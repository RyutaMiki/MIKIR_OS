# MIKIR-OS build
# Requires: nasm, gcc (m32), ld, objcopy, dd

ASM		= nasm
ASMFLAGS_BIN	= -f bin
ASMFLAGS_ELF	= -f elf32

CC		= gcc
CFLAGS		= -ffreestanding -m32 -Wall -Wextra -O2 -c -I.

LD		= ld
LDFLAGS		= -m elf_i386 -T link.ld -nostdlib

IPL		= ipl.bin
LOADER		= loader.o
KERNEL_C	= kernel/kernel.c
KERNEL_O	= kernel.o
KERNEL_ELF	= kernel.elf
KERNEL_BIN	= kernel.bin
IMAGE		= mikiros.img
SECTORS		= 2880

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
	objcopy -O binary $(KERNEL_ELF) $(KERNEL_BIN)

$(IMAGE): $(IPL) $(KERNEL_BIN)
	dd if=/dev/zero of=$(IMAGE) bs=512 count=$(SECTORS) 2>/dev/null
	dd if=$(IPL) of=$(IMAGE) bs=512 conv=notrunc
	dd if=$(KERNEL_BIN) of=$(IMAGE) bs=512 seek=1 conv=notrunc

clean:
	rm -f $(IPL) $(LOADER) $(KERNEL_O) $(KERNEL_ELF) $(KERNEL_BIN) $(IMAGE)

run: $(IMAGE)
	qemu-system-i386 -fda $(IMAGE)
