/*
	TinyLoad - a simple region free (original) game launcher in 4k

# This code is licensed to you under the terms of the GNU GPL, version 2;
# see file COPYING or http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt
*/

/* This code comes from HBC's stub which was based on dhewg's geckoloader stub */
// Copyright 2008-2009  Andre Heider  <dhewg@wiibrew.org>
// Copyright 2008-2009  Hector Martin  <marcan@marcansoft.com>

#include "cache.h"
#include "dip.h"
#include "utils.h"
#include "global.h"
#include "apploader.h"

/* Top of MEM1, where the kernel puts the tournament module (sd:/tournament.bin,
 * linked at 0x817E0000; kernel/Patch.c LoadTournamentModule) and where the
 * game's FST lands. */
#define TOP_MEM1_START	0x81700000
#define TOP_MEM1_END	0x81800000

u32 _main()
{
	u32 entry, a;

	RAMInit();
	entry = Apploader_Run();

	/* RAMInit zeroed this range through the PPC caches, then the ARM kernel
	 * wrote the module straight into RAM while the apploader ran. The PPC
	 * still held the stale zero lines (L1, and the unified L2 that an
	 * instruction fetch refills from), so the first call into the module
	 * executed zeros: "Illegal instruction at 817E88D8", the first
	 * instruction of tm_bootOnLoad, on the first hardware run (2026-09-30).
	 * dcbf writes back anything the apploader really changed here (the FST,
	 * BI2) and drops the rest; icbi drops stale instructions. The DOL itself
	 * never had the problem: the apploader's DVD reads invalidate it. */
	for (a = TOP_MEM1_START; a < TOP_MEM1_END; a += 32)
		asm volatile("dcbf 0,%0 ; icbi 0,%0" : : "b"(a) : "memory");
	asm volatile("sync ; isync");

	return entry;
}
