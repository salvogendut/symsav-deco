; deco_msx.s — VDP helper for deco screensaver (MSX Screen 7)
; SCC cdecl convention: args on stack, callee preserves IX, IY, BC.
;
; MSX2 V9938 VDP ports:
;   0x98 — VRAM data read/write
;   0x99 — control: address setup, register write
;
; VRAM write address setup:
;   di
;   out (0x99), addr_lo               ; A7:A0
;   out (0x99), (addr_hi & 0x3F)|0x40 ; A13:A8, bits7:6 = 01 = write
;   ei
;
; R#14 selects the 16KB VRAM bank (bits A16:A14).
; Register write: out(0x99, value); out(0x99, 0x80 | regnum).

.z80
.code

; -----------------------------------------------------------------------
; void vdp_fill(unsigned int vram_addr, unsigned char fill_byte,
;               unsigned short len)
;
; Fills len bytes of MSX VRAM starting at vram_addr with fill_byte.
; vram_addr must be < 0x4000 (Screen 7 frame fits in first 16KB bank).
; -----------------------------------------------------------------------
.export _vdp_fill
_vdp_fill:
	push bc
	push ix
	ld   ix, #2
	add  ix, sp

	ld   e, (ix+4)      ; E = vram_addr lo
	ld   d, (ix+5)      ; D = vram_addr hi
	ld   l, (ix+6)      ; L = fill byte
	ld   c, (ix+8)      ; BC = len
	ld   b, (ix+9)

	; Set R#14 = 0 (VRAM bank 0, addresses 0x0000-0x3FFF)
	di
	ld   a, d
	rlca
	rlca
	and  #3
	out  (0x99), a
	ld   a, #0x8E
	out  (0x99), a
	; Set VRAM write address
	ld   a, e
	out  (0x99), a
	ld   a, d
	and  #0x3F
	or   #0x40
	out  (0x99), a
	ei

	ld   a, b
	or   c
	jr   z, _vdp_fill_done
_vdp_fill_loop:
	ld   a, l
	out  (0x98), a
	dec  bc
	ld   a, b
	or   c
	jr   nz, _vdp_fill_loop
_vdp_fill_done:
	pop  ix
	pop  bc
	ret
