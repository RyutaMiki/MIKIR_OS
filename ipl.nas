; MIKIR-OS IPL (Initial Program Loader)
; Boot sector only. Loads sector 2+ to 0x8000 and jumps to the loader.
; TAB=4

		ORG		0x7c00

; ----------------------------------------------------------------------
; FAT12 BPB (BIOS Parameter Block) - required for floppy
; ----------------------------------------------------------------------
		JMP		entry
		DB		0x90
		DB		"HELLOIPL"
		DW		512
		DB		1
		DW		1
		DB		2
		DW		224
		DW		2880
		DB		0xf0
		DW		9
		DW		18
		DW		2
		DD		0
		DD		2880
		DB		0, 0, 0x29
		DD		0xffffffff
		DB		"MIKIR-OS   "
		DB		"FAT12   "
		RESB	18

; ----------------------------------------------------------------------
; Entry: set segment registers, load sector 2 to 0x8000, jump to loader
; ----------------------------------------------------------------------
entry:
		MOV		AX, 0
		MOV		SS, AX
		MOV		SP, 0x7c00
		MOV		DS, AX
		MOV		ES, AX

		; INT 0x13 AH=0x02: read sectors
		; CH=cylinder(0), CL=sector(2, 1-based), DH=head(0), DL=drive(BIOS)
		; ES:BX = buffer -> 0x8000:0
		MOV		AX, 0x0800
		MOV		ES, AX
		MOV		CH, 0
		MOV		DH, 0
		MOV		CL, 2
		MOV		BX, 0
		MOV		AL, 4
		MOV		AH, 0x02
		INT		0x13
		JC		error

		; Jump to loader at 0x8000:0
		JMP		0x8000:0

error:
		MOV		SI, msg_error
		CALL	putloop
		HLT
		JMP		$

putloop:
		MOV		AL, [SI]
		ADD		SI, 1
		CMP		AL, 0
		JE		putloop_end
		MOV		AH, 0x0e
		MOV		BX, 0x000f
		INT		0x10
		JMP		putloop
putloop_end:
		RET

msg_error:
		DB		0x0a, 0x0a
		DB		"Load error."
		DB		0x0a, 0

		RESB	0x1fe - ($ - $$)
		DB		0x55, 0xaa
