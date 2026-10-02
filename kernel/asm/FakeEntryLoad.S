#include <asm.h>

.set	RESET_STATUS,	0xD3003420
.set	FLUSH_LEN,		0xD3003424
.set	FLUSH_ADDR,		0xD3003428
.set	MOD_FLUSH_ADDR,	0xD300342C
.set	MOD_FLUSH_LEN,	0xD3003430
.set	MOD_SRC,		0xD3003434

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

	# The tournament module (kernel Patch.c LoadTournamentModule) lives
	# outside the DOL range. The ARM writing it straight into MEM1 left the
	# PPC executing stale cache lines (zeros from multidol RAMInit: "Illegal
	# instruction at 817E88D8" on the first hardware runs, 2026-09-30, even
	# after a dcbi/icbi pass here). So the kernel only stages it in MEM2 and
	# the PPC copies it in itself, uncached source, through its own caches,
	# flushing each line - the same path every other PPC-written byte takes.
	# MOD_FLUSH_LEN is the line count (0 = no module), MOD_FLUSH_ADDR the
	# cached destination, MOD_SRC the uncached source.
	lwz		r4,	MOD_FLUSH_LEN@l(r3)
	cmpwi	r4,	0
	beq		NoModule
	mtctr	r4
	lwz		r4,	MOD_FLUSH_ADDR@l(r3)
	lwz		r5,	MOD_SRC@l(r3)

	# BootInfo arenaHi (0x80000034) must end the game's heap BELOW the module,
	# and only the PPC can do it: the apploader wrote it (the FST base,
	# 0x817F8AC0 on Melee) through the PPC data cache, so the ARM kernel read a
	# stale 0 and its own write would have been buried by that dirty line.
	# Melee's heap setup then zeroed the module one second after entry (RAM
	# watch, 2026-09-30). Lower it to the module base when it is above it;
	# leave 0 alone (OSInit then uses its 0x81700000 default, also below).
	lis		r6,	0x8000
	lwz		r7,	0x34(r6)
	cmplw	r7,	r4
	ble		ArenaDone
	stw		r4,	0x34(r6)
	addi	r7,	r6,	0x20
	dcbf	0,	r7
	sync
ArenaDone:
CopyModule:
	lwz		r0,	0(r5)
	lwz		r6,	4(r5)
	lwz		r7,	8(r5)
	lwz		r8,	12(r5)
	lwz		r9,	16(r5)
	lwz		r10,	20(r5)
	lwz		r11,	24(r5)
	lwz		r12,	28(r5)
	stw		r0,	0(r4)
	stw		r6,	4(r4)
	stw		r7,	8(r4)
	stw		r8,	12(r4)
	stw		r9,	16(r4)
	stw		r10,	20(r4)
	stw		r11,	24(r4)
	stw		r12,	28(r4)
	dcbf	0,	r4
	icbi	0,	r4
	addi	r4,	r4,	0x20
	addi	r5,	r5,	0x20
	bdnz	CopyModule
	sync
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
