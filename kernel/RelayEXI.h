/* kernel/RelayEXI.h
 * Tournament relay EXI device: the ARM side of the fake EXI device that the
 * Melee decomp build (lbrelayexi.c) talks to for the tournament reporter.
 * Design: ../tournament-reporter/docs/design.md section 6.2; investigation:
 * docs/relay-exi-investigation.md.
 *
 * Two contexts touch this module:
 *   - the kernel main loop, through EXIUpdateRegistersNEW() (kernel/EXI.c),
 *     which must never block: RelayEXISelect/ImmWrite/DMARead only copy
 *     bytes and flip a state word;
 *   - one dedicated kernel thread (RelayEXIInit spawns it) that does the
 *     socket -> connect -> send -> recv -> close round trip.
 */
#ifndef __RELAY_EXI_H__
#define __RELAY_EXI_H__

#include "global.h"

/* Read sd:/tournament.cfg (station, stream) and spawn the relay thread. Call
 * once at boot after the SD card is mounted; networking may or may not be up.
 * The thread finds the relay itself: once the network is up it listens for
 * the relay's UDP beacon (design R15) and uses the latest one's address. */
void RelayEXIInit(void);

/* EXISelect on the relay's channel: forget any half-received transaction. */
void RelayEXISelect(void);

/* EXIImm write on the relay's channel (main loop). `data` is the immediate
 * value (EXIImm.S stores the word itself for writes of <= 4 bytes), `len` the
 * byte count, `mode` the EXI mode (EXI_WRITE == 1). Returns true when the
 * write belongs to a relay transaction and the caller must ack the transfer
 * without touching memory card emulation. */
bool RelayEXIImmWrite(u32 data, u32 len, u32 mode);

/* EXIDMA read on the relay's channel (main loop). If an EXI_RELAY_POLL command
 * word preceded it, fills the game's buffer with {exi_poll_hdr, response}
 * (lbRelayExi_PollBuf layout; relay_ip/relay_port 0 until a beacon is heard)
 * and syncs it; returns true in that case. */
bool RelayEXIDMARead(u8 *ptr, u32 len);

#endif /* __RELAY_EXI_H__ */
