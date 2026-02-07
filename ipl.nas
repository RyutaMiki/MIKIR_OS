; MIKIR-OS IPL (Initial Program Loader)
; Boot sector only. Loads sector 2+ to 0x8000 and jumps to the loader.
; TAB=4

		ORG		0x7c00

; ----------------------------------------------------------------------
; FAT12 BPB (BIOS Parameter Block) - for compatibility
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
; Entry: set segment registers, load sector 1..4 (LBA) to 0x8000, jump to loader
; Use Extended Read (AH=42h) so HDD geometry does not matter.
; ----------------------------------------------------------------------
entry:
		MOV		AX, 0
		MOV		SS, AX
		MOV		SP, 0x7c00
		MOV		DS, AX
		MOV		ES, AX

		; INT 0x13 AH=0x42: Extended Read (LBA). DS:SI = DAP, DL = drive (BIOS)
		MOV		SI, dap
		MOV		AH, 0x42
		INT		0x13
		JC		error

		; Jump to loader at 0x8000:0
		JMP		0x8000:0

; Disk Address Packet for LBA read: sector 1, 4 sectors -> 0x8000:0
dap:
		DB		0x10		; size of DAP
		DB		0			; reserved
		DW		16			; sector count (8KB – enough for loader+kernel)
		DW		0			; buffer offset
		DW		0x8000		; buffer segment
		DD		1			; LBA low (sector 1 = 2nd sector)
		DD		0			; LBA high

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
