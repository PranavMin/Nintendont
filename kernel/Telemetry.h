/* Station telemetry (tournament-reporter protocol.yaml telemetry_hdr):
 * a copy of the kernel log and the tournament module's load result, sent to
 * the relay by RelayEXI's idle thread so a Wii that boots wrong is diagnosed
 * from the relay's status page instead of from its SD card. */
#ifndef __TELEMETRY_H__
#define __TELEMETRY_H__

#include "global.h"

/* dbgprintf: should this message be rendered for telemetry even when no
 * other log output is enabled? (A capture budget keeps a kernel that logs
 * forever from paying for vsprintf forever.) */
bool TelemetryWantsLog(void);
/* dbgprintf: append a rendered message. Never blocks; a message that finds
 * the buffer busy or full is dropped and counted. */
void TelemetryLog(const char *s, u32 len);

/* RelayEXI (the only reader): copy up to max bytes of unsent log into dst,
 * cut after the last '\n' when there is one; returns the byte count. The
 * bytes stay queued until TelemetryConsume. */
u32 TelemetryPeek(char *dst, u32 max);
void TelemetryConsume(u32 n);
u32 TelemetryDropped(void);

/* Patch.c LoadTournamentModule: the outcome, as enum module_state. */
void TelemetrySetModule(u32 state, u32 len, u32 load, u32 patches, u32 arena_hi);
void TelemetryGetModule(u32 *state, u32 *len, u32 *load, u32 *patches, u32 *arena_hi);

#endif
