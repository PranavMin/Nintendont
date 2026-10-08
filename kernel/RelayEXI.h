/* kernel/RelayEXI.h
 * Tournament relay EXI device: the ARM side of the fake EXI device that the
 * LazyTO kiosk module (lbrelayexi.c) talks to.
 * Design: ../tournament-reporter/docs/architecture.md and, for protocol v2,
 * docs/protocol-v2.md there; investigation: docs/relay-exi-investigation.md.
 *
 * Two contexts touch this module:
 *   - the kernel main loop, through EXIUpdateRegistersNEW() (kernel/EXI.c),
 *     which must never block: RelayEXISelect/ImmWrite/DMARead only copy
 *     bytes and flip a state word;
 *   - one dedicated kernel thread (RelayEXIInit spawns it) that carries each
 *     request through the beamer's USB mailbox (RelayEXI.c header).
 */
#ifndef __RELAY_EXI_H__
#define __RELAY_EXI_H__

#include "global.h"

/* Spawn the relay thread. Call once at boot after the SD card is mounted and
 * USB started, before the game runs. The thread finds the beamer on USB
 * itself, and takes the station and the relay's address from its hello. */
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
 * (lbRelayExi_PollBuf layout; the beamer's state from its latest hello) and
 * syncs it; returns true in that case. */
bool RelayEXIDMARead(u8 *ptr, u32 len);

#endif /* __RELAY_EXI_H__ */
