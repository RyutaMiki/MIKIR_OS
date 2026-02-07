; chocola Loader (second stage)
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

		; Detect memory map (INT 15h, E820) — store at linear 0x500
		PUSH	ES
		XOR		AX, AX
		MOV		ES, AX
		XOR		BP, BP			; entry count
		MOV		DI, 0x504		; entries start at 0x504
		XOR		EBX, EBX		; continuation = 0
.e820:
		MOV		EAX, 0xE820
		MOV		ECX, 24
		MOV		EDX, 0x534D4150	; 'SMAP'
		INT		0x15
		JC		.e820_end
		CMP		EAX, 0x534D4150
		JNE		.e820_end
		INC		BP
		ADD		DI, 24
		TEST	EBX, EBX
		JNZ		.e820
.e820_end:
		MOV		[ES:0x500], BP	; store count (16-bit)
		MOV		WORD [ES:0x502], 0
		POP		ES

		; Disable interrupts before PMode
		CLI

		; Protected mode
		MOV		EAX, CR0
		OR		EAX, 1
		MOV		CR0, EAX

		; Far jump to 32-bit code (selector 0x08 = flat code)
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

; ---------------------------------------------------------------------------
; GDT: both code and data are flat (base=0, limit=4GB)
; ---------------------------------------------------------------------------
align 8
gdt:
		DQ		0
gdt_code:
		DW		0xffff			; limit 15:0
		DW		0x0000			; base 15:0
		DB		0x00			; base 23:16
		DB		0x9a			; P=1 DPL=0 S=1 type=exec/read
		DB		0xcf			; G=1 D=1 limit 19:16=0xf
		DB		0x00			; base 31:24
gdt_data:
		DW		0xffff
		DW		0x0000
		DB		0x00
		DB		0x92			; P=1 DPL=0 S=1 type=data/write
		DB		0xcf
		DB		0x00
gdt_end:

gdtr:
		DW		gdt_end - gdt - 1
		DD		0x80000 + (gdt - _start)

; ---------------------------------------------------------------------------
; 32-bit protected mode
; ---------------------------------------------------------------------------
[SECTION .text]
[BITS 32]
		GLOBAL	start_32
		GLOBAL	isr_timer
		GLOBAL	isr_keyboard
		EXTERN	timer_handler
		EXTERN	keyboard_handler

start_32:
		MOV		AX, 0x10
		MOV		DS, AX
		MOV		ES, AX
		MOV		FS, AX
		MOV		GS, AX
		MOV		SS, AX
		MOV		ESP, 0x90000

		CALL	setup_idt
		CALL	kernel_main

		CLI
.hlt:	HLT
		JMP		.hlt

; ---------------------------------------------------------------------------
; ISR stubs: save all regs, call C handler, send EOI, restore, IRET
; ---------------------------------------------------------------------------
isr_timer:
		PUSHAD
		PUSH	ESP				; arg: current ESP (-> PUSHAD frame)
		CALL	timer_handler	; returns new ESP in EAX
		MOV		ESP, EAX		; switch stack (may be same or different task)
		MOV		AL, 0x20
		OUT		0x20, AL		; EOI to master PIC
		POPAD
		IRET

isr_keyboard:
		PUSHAD
		CALL	keyboard_handler
		MOV		AL, 0x20
		OUT		0x20, AL		; EOI to master PIC
		POPAD
		IRET

; ---------------------------------------------------------------------------
; Fill IDT (256 entries) at 0x70000, all pointing to idt_stub
; ---------------------------------------------------------------------------
setup_idt:
		MOV		EDI, 0x70000
		MOV		ECX, 256
		MOV		EAX, idt_stub		; VMA already = linear addr (flat GDT)
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
		MOV		DWORD [ESP+2], 0x70000
		LIDT	[ESP]
		ADD		ESP, 6
		RET

idt_stub:
		CLI
		HLT
		JMP		idt_stub
