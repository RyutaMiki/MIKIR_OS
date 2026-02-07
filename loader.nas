; MIKIR-OS Loader (second stage)
; Loaded at 0x8000:0 by IPL. Switches to 32-bit protected mode and calls kernel_main (C).
; TAB=4

[SECTION .text.boot]
[BITS 16]
		GLOBAL	_start
		EXTERN	kernel_main

_start:
		MOV		AX, CS
		MOV		DS, AX
		MOV		ES, AX
		MOV		SS, AX
		MOV		SP, 0x7c00

		; Load GDT
		LGDT	[gdtr]

		; Enable A20 (port 0x92)
		IN		AL, 0x92
		OR		AL, 2
		OUT		0x92, AL

		; Protected mode
		MOV		EAX, CR0
		OR		EAX, 1
		MOV		CR0, EAX

		; Far jump to 32-bit code (selector 0x08 = code)
		JMP		DWORD 0x08:start_32

; GDT: code at 0x80000 (64K), data flat 0..4GB (for VGA 0xb8000)
align 8
gdt:
		DQ		0
gdt_code:
		DW		0xffff
		DW		0x0000
		DB		0x00
		DB		0x9a
		DB		0xcf
		DB		0x08
gdt_data:
		DW		0xffff
		DW		0xffff
		DB		0x00
		DB		0x92
		DB		0xcf
		DB		0x00
gdt_end:

gdtr:
		DW		gdt_end - gdt - 1
		DD		0x80000 + (gdt - _start)

[SECTION .text]
[BITS 32]
		GLOBAL	start_32
start_32:
		MOV		AX, 0x10
		MOV		DS, AX
		MOV		ES, AX
		MOV		FS, AX
		MOV		GS, AX
		MOV		SS, AX
		MOV		ESP, 0x90000

		CALL	kernel_main

		CLI
.hlt:	HLT
		JMP		.hlt
