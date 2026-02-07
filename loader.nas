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

		; Tell user we reached the loader (real mode)
		MOV		SI, msg_loader
		CALL	putstr_16
		; Load GDT
		LGDT	[gdtr]

		; Enable A20 (port 0x92)
		IN		AL, 0x92
		OR		AL, 2
		OUT		0x92, AL

		; Disable interrupts before PMode (no IDT yet -> timer would triple-fault)
		CLI

		; Protected mode
		MOV		EAX, CR0
		OR		EAX, 1
		MOV		CR0, EAX

		; Far jump to 32-bit code (selector 0x08 = code)
		JMP		DWORD 0x08:start_32

putstr_16:
		PUSH	SI
.1:		LODSB
		CMP		AL, 0
		JE		.2
		MOV		AH, 0x0e
		MOV		BX, 0x000f
		INT		0x10
		JMP		.1
.2:		POP		SI
		RET
msg_loader:
		DB		0x0a, 0x0a, "Loader OK (16bit)", 0x0a, 0

; GDT: code at 0x80000, data flat 0..4GB
align 8
gdt:
		DQ		0
gdt_code:
		DW		0xffff
		DW		0x0000
		DB		0x08
		DB		0x9a
		DB		0xcf
		DB		0x00
gdt_data:
		DW		0xffff
		DW		0x0000
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

		; Clear screen then write "32OK" at top-left (VGA 0xb8000)
		MOV		EDI, 0xb8000
		MOV		ECX, 80*25
		MOV		AX, 0x0f20
.clr:	MOV		[EDI], AX
		ADD		EDI, 2
		LOOP	.clr
		MOV		EDI, 0xb8000
		MOV		WORD [EDI], 0x0f33
		MOV		WORD [EDI+2], 0x0f32
		MOV		WORD [EDI+4], 0x0f4f
		MOV		WORD [EDI+6], 0x0f4b

		CALL	setup_idt
		PUSH	0x80000
		CALL	kernel_main
		ADD		ESP, 4

		CLI
.hlt:	HLT
		JMP		.hlt

; Fill IDT (256 entries) at 0x81000, all pointing to idt_stub
setup_idt:
		MOV		EDI, 0x81000
		MOV		ECX, 256
		MOV		EAX, idt_stub
		ADD		EAX, 0x80000
.fill:
		MOV		[EDI], AX
		MOV		WORD [EDI+2], 0x08
		MOV		BYTE [EDI+4], 0
		MOV		BYTE [EDI+5], 0x8e
		PUSH	EAX
		SHR		EAX, 16
		MOV		[EDI+6], AX
		POP		EAX
		ADD		EDI, 8
		LOOP	.fill
		SUB		ESP, 6
		MOV		WORD [ESP], 256*8 - 1
		MOV		DWORD [ESP+2], 0x81000
		LIDT	[ESP]
		ADD		ESP, 6
		RET

idt_stub:
		CLI
		HLT
		JMP		idt_stub
