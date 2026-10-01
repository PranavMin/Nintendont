#include <asm.h>

.set	RESET_STATUS,	0xD3003420
.set	FLUSH_LEN,		0xD3003424
.set	FLUSH_ADDR,		0xD3003428
.set	MOD_FLUSH_ADDR,	0xD300342C
.set	MOD_FLUSH_LEN,	0xD3003430

FakeEntryLoad:
	lis		r3,	RESET_STATUS@h
	li		r0,	0x4DEA
	stw		r0,	RESET_STATUS@l(r3)
wait_loop:
	lwz		r4,	RESET_STATUS@l(r3)
	cmplw	r4,	r0
	beq		wait_loop
	mtlr	r4

	lwz		r4,	FLUSH_LEN@l(r3)
	mtctr	r4

	lwz		r4,	FLUSH_ADDR@l(r3)
InvalidateRange1:
	dcbi	0,	r4
	icbi	0,	r4
	addi	r4,	r4,	0x20
	bdnz	InvalidateRange1

	# The tournament module (kernel Patch.c LoadTournamentModule) is written
	# by the ARM outside the DOL range, so the PPC still holds stale lines for
	# it (zeros from multidol RAMInit): first hardware run, 2026-09-30,
	# "Illegal instruction at 817E88D8". The kernel passes its range here;
	# 0 lines = no module.
	lwz		r4,	MOD_FLUSH_LEN@l(r3)
	cmpwi	r4,	0
	beq		NoModule
	mtctr	r4
	lwz		r4,	MOD_FLUSH_ADDR@l(r3)
InvalidateModule:
	dcbi	0,	r4
	icbi	0,	r4
	addi	r4,	r4,	0x20
	bdnz	InvalidateModule
NoModule:

	li		r4,	0x100
	mtctr	r4
	lis		r4,	0x8000
	ori		r4,	r4,	0x1000

InvalidateRange2:
	dcbi	0,	r4
	icbi	0,	r4
	addi	r4,	r4,	0x20
	bdnz	InvalidateRange2

	sync
	isync

	blr
