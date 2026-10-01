/* Station telemetry log buffer and module status; see Telemetry.h.
 *
 * Several kernel threads call dbgprintf and there is no mutex in this kernel,
 * so the ring is guarded by a try-lock built on the ARM9's SWP instruction.
 * Nobody ever waits on it: a writer that finds it held drops its message
 * (counted), the reader skips a tick. Waiting could deadlock a high-priority
 * thread spinning on a lock held by a preempted low-priority one. */
#include "Telemetry.h"
#include "relay_proto.h"
#include "string.h"

#define TLOG_SIZE		16384		/* unsent log bytes held */
#define TLOG_CAPTURE_BUDGET	(64 * 1024)	/* bytes rendered for telemetry alone */

static char tlog_buf[TLOG_SIZE];
static vu32 tlog_head = 0;	/* total bytes ever written (writers) */
static vu32 tlog_tail = 0;	/* total bytes ever consumed (reader) */
static vu32 tlog_lock = 0;
static vu32 tlog_dropped = 0;
static vu32 tlog_captured = 0;

static vu32 tmod_state = MOD_PENDING;
static vu32 tmod_len = 0, tmod_load = 0, tmod_patches = 0, tmod_arena = 0;

static inline u32 tlog_swap(vu32 *p, u32 v)
{
	u32 old;
	__asm__ volatile("swp %0, %1, [%2]" : "=&r"(old) : "r"(v), "r"(p) : "memory");
	return old;
}

static inline bool tlog_trylock(void)
{
	return tlog_swap(&tlog_lock, 1) == 0;
}

static inline void tlog_unlock(void)
{
	__asm__ volatile("" ::: "memory");
	tlog_lock = 0;
}

bool TelemetryWantsLog(void)
{
	return tlog_captured < TLOG_CAPTURE_BUDGET;
}

void TelemetryLog(const char *s, u32 len)
{
	u32 head, i;

	if (len == 0)
		return;
	if (!tlog_trylock())
	{
		tlog_dropped += len;
		return;
	}
	tlog_captured += len;
	head = tlog_head;
	if (len > TLOG_SIZE - (head - tlog_tail))
	{
		tlog_dropped += len;
		tlog_unlock();
		return;
	}
	for (i = 0; i < len; i++)
		tlog_buf[(head + i) % TLOG_SIZE] = s[i];
	tlog_head = head + len;
	tlog_unlock();
}

u32 TelemetryPeek(char *dst, u32 max)
{
	u32 avail, n, i, cut;

	if (!tlog_trylock())
		return 0;
	avail = tlog_head - tlog_tail;
	n = avail < max ? avail : max;
	for (i = 0; i < n; i++)
		dst[i] = tlog_buf[(tlog_tail + i) % TLOG_SIZE];
	tlog_unlock();

	/* A full chunk is cut after its last line break so lines arrive whole;
	 * a single line longer than the chunk goes as it is (the relay joins). */
	if (n == max)
	{
		for (cut = n; cut > 0 && dst[cut - 1] != '\n'; cut--)
			;
		if (cut > 0)
			n = cut;
	}
	return n;
}

void TelemetryConsume(u32 n)
{
	/* Only the reader writes tail; a writer that reads a stale tail just
	 * sees less free space. */
	tlog_tail += n;
}

u32 TelemetryDropped(void)
{
	return tlog_dropped;
}

void TelemetrySetModule(u32 state, u32 len, u32 load, u32 patches, u32 arena_hi)
{
	tmod_len = len;
	tmod_load = load;
	tmod_patches = patches;
	tmod_arena = arena_hi;
	tmod_state = state;
}

void TelemetryGetModule(u32 *state, u32 *len, u32 *load, u32 *patches, u32 *arena_hi)
{
	*state = tmod_state;
	*len = tmod_len;
	*load = tmod_load;
	*patches = tmod_patches;
	*arena_hi = tmod_arena;
}
