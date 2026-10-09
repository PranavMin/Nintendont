/* kernel/RelayEXI.h
 * Tournament relay EXI device: the ARM side of the fake EXI device that the
 * LazyTO kiosk module (lbrelayexi.c) talks to, and the record gate through
 * which the kiosk chooses the matches Slippi records.
 * Design: ../tournament-reporter/docs/architecture.md and, for protocol v2,
 * docs/protocol-v2.md there; investigation: docs/relay-exi-investigation.md.
 *
 * Three contexts touch this module:
 *   - the kernel main loop, through EXIUpdateRegistersNEW() (kernel/EXI.c),
 *     which must never block: RelayEXISelect/ImmWrite/DMARead only copy
 *     bytes and flip a state word, and RelayEXIGateStart (from
 *     SlippiMemoryWrite in the EXI DMA handler, kernel/EXI.c:826, 870)
 *     reads and writes one cache line each and a small table;
 *   - one dedicated kernel thread (RelayEXIInit spawns it) that carries each
 *     request through the beamer's USB mailbox (RelayEXI.c header);
 *   - the Slippi file writer thread (SlippiFileWriter.c), which calls
 *     RelayEXIGateChoice and RelayEXIGateOpened.
 */
#ifndef __RELAY_EXI_H__
#define __RELAY_EXI_H__

#include "global.h"

/* Zero the record gate and spawn the relay thread. Call once at boot after
 * the SD card is mounted and USB started, before the game runs. The thread
 * finds the beamer on USB itself, and takes the station and the relay's
 * address from its hello. */
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

/* Record gate (protocol.yaml record_gate). SlippiMemoryWrite, at a Game Start
 * (RECEIVE_COMMANDS first in a DMA buffer; main loop, EXI DMA handler): decide
 * whether that match is recorded - always without the kiosk module, else only
 * when the kiosk's want is RECORD_THIS_MATCH - count it, publish the count, and
 * keep the choice under `cursor` (the low word of the ring cursor at its
 * RECEIVE_COMMANDS). Gives its seq; returns the choice. No lock, no wait. */
bool RelayEXIGateStart(u32 cursor, u32 *seq);

/* The writer thread, at a new match: the choice kept for `cursor` and its seq.
 * A cursor not in the table (not seen at a DMA buffer's start, or overwritten
 * by four later Game Starts) records, with seq 0. */
bool RelayEXIGateChoice(u32 cursor, u32 *seq);

/* The writer thread, once a recorded match's file is valid: publish its seq and
 * the file's gameStartTime for the kiosk (file_id, then file_seq). */
void RelayEXIGateOpened(u32 seq, u32 file_id);

#endif /* __RELAY_EXI_H__ */
