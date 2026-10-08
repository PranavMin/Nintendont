/* kernel/RelayEXI.c
 * LazyTO relay EXI device and record gate (../tournament-reporter/docs/architecture.md,
 * section LazyTO Nintendont; protocol v2: docs/protocol-v2.md and docs/redesign.md in
 * that repo).
 *
 * The game (kiosk lbrelayexi.c) selects channel 1 / device 0 (slot B, shared
 * with Slippi's own device), writes a 4-byte immediate command word
 * (EXI_RELAY_REQ or EXI_RELAY_POLL in the top byte, relay_proto.h), then
 *   REQ:  relay_hdr + payload through EXIImmEx, i.e. EXIImm writes of <= 4
 *         bytes each (melee OSExi.c EXIImmEx, lines 168-184); the patched
 *         EXIImm stub (kernel/asm/EXIImm.S:36-41) stores the bytes as an
 *         immediate word, first byte in the top bits;
 *   POLL: one 4096-byte EXIDma read that must come back as
 *         lbRelayExi_PollBuf: {exi_poll_hdr, relay_hdr, relay_resp, payload}.
 *
 * Contexts: EXI transfers are serviced by the kernel main loop
 * (kernel/main.c:591 EXIUpdateRegistersNEW -> kernel/EXI.c:743, 782, 863) with
 * the game frozen inside the transfer until the ack (kernel/EXI.c:886), so
 * the hooks below only copy bytes and flip `relay_state`. Everything that
 * touches USB runs on the dedicated thread spawned by RelayEXIInit() (own stack
 * in kernel.ld:42-43, do_thread_create at prio 0x78).
 *
 * State machine (one request buffer, one thread, no retries):
 *   RELAY_IDLE  --REQ complete-->  RELAY_BUSY  --thread-->  RELAY_DONE
 *                                              \--thread-->  RELAY_ERROR + last_fail
 *   DONE/ERROR are sticky until the next REQ; a REQ while BUSY is dropped; a
 *   REQ longer than EXI_PAYLOAD_MAX ends RELAY_ERROR with LF_BAD_REQUEST.
 *
 * The beamer is the only link (docs/redesign.md, The Wii side). A LazyTO beamer
 * is an ESP32 USB mass-storage stick on the Wi-Fi that is also the Slippi
 * replay drive. This file opens no IOS socket and reads no file: the station
 * number, the relay's address and the secret all live on the beamer, so every
 * SD card is the same. The beamer serves BEAMER_MB_SECTORS mailbox sectors from
 * its RAM right after its first FAT32 partition (MBR entry type 0x0B/0x0C,
 * start + size, derived again per USB mount).
 *   - Idle, the thread reads beamer_hello about once a second (also while a
 *     game is paused) and publishes it in exi_poll_hdr: the station, the relay,
 *     the beamer's Wi-Fi and storage, and, without a usable v2 hello,
 *     PF_NO_BEAMER with no_beamer_reason. Nothing is written to the mailbox
 *     before a valid v2 hello, so a plain USB stick is never written there.
 *   - A request is beamer_req_hdr + relay_hdr + payload (no relay_auth: the
 *     beamer puts its own in front) in the request sector under a new seq,
 *     with relay_hdr.station stamped from the hello. The response sector is
 *     polled every RELAY_BEAMER_POLL_MS until it carries that seq, within
 *     RELAY_BUDGET_MS. Every failure is RELAY_ERROR with a code in
 *     exi_poll_hdr.last_fail (the beamer's BR_* passed through, or the kernel's
 *     LF_*); the kiosk picks the words.
 *   - Telemetry datagrams go to the telemetry sector, one write per tick.
 *   - The Slippi file writer thread drives the same drive, so usbstorage.c
 *     serializes SCSI cycles with its USB lock; this thread waits for it at
 *     most what is left of the request's budget (RELAY_IDLE_WAIT_MS when idle).
 *     USB is in that mode only with replays on and the game on SD
 *     (RelayEXIInit, beamer_usb).
 * Slippi's own network (net.c NetworkInitAsync, console mirroring) is neither
 * touched nor used here: the loader's Network option means mirroring only.
 *
 * Record gate (protocol.yaml record_gate, docs/redesign.md "Recording only set
 * games"): with the kiosk module loaded, the Slippi file writer records only the
 * matches the kiosk asks for. RelayEXIGateStart runs at each Game Start inside
 * the EXI DMA handler (SlippiMemoryWrite, kernel/EXI.c:870): it reads the
 * kiosk's line of the gate, counts the start, publishes the count and keeps
 * {ring cursor, record, seq} in a small table, with no lock and no wait. The
 * writer thread looks its new match up there (RelayEXIGateChoice) and, once the
 * file is valid, publishes which start it opened a file for
 * (RelayEXIGateOpened), so the kiosk can name the game's replay.
 */

#include "RelayEXI.h"
#include "relay_proto.h"
#include "EXI.h"		/* EXI_WRITE */

#include "global.h"
#include "common.h"
#include "string.h"
#include "debug.h"
#include "syscalls.h"
#include "Telemetry.h"
#include "Config.h"
#include "usbstorage.h"

/* Game-side contract, kiosk lbrelayexi.h. */
#define RELAY_EXI_BUF_SIZE	4096	/* LB_RELAY_EXI_BUF_SIZE */
#define RELAY_REQ_MAX		(sizeof(struct relay_hdr) + EXI_PAYLOAD_MAX)

#define RELAY_BUDGET_MS		3000	/* design 4.6: one attempt, 3 s */
#define RELAY_THREAD_CYCLE_MS	1	/* like SlippiNetwork.c THREAD_CYCLE_TIME_MS */
#define RELAY_TELEMETRY_TICK_MS	100	/* idle thread sends telemetry this often */
#define RELAY_BEAMER_HELLO_MS	1000	/* idle thread reads the beamer's hello this often */
#define RELAY_BEAMER_POLL_MS	10	/* a request reads the response sector this often */
#define RELAY_IDLE_WAIT_MS	50	/* idle USB work waits this long for the USB lock, then tries next time */
#define RELAY_STARTING_MS	45000	/* after boot or a USB change, "no drive" and "not a beamer" are NB_STARTING this long */
/* Shown top-right on the kiosk's set list next to the module's own version
 * (exi_poll_hdr.host_build), and the kiosk's feature gate
 * (RECORD_GATE_HOST_BUILD). Bump by hand when a loader release changes
 * behaviour the TO should be able to tell apart on the TV. */
#define RELAY_HOST_BUILD	7	/* 2: network init off the boot path, PF_NET_JOINING; 3: EINPROGRESS 26 and IOS poll bits (connect to a relay on another host); 4: lazyto_kiosk.bin / lazyto_station.txt, no stream=; 5: beamer transport; 6: the transport bench (branch bench, never merged); 7: protocol v2, the beamer is the only link, the record gate */

RELAY_STATIC_ASSERT(RELAY_HOST_BUILD >= RECORD_GATE_HOST_BUILD, host_build_has_record_gate);
RELAY_STATIC_ASSERT(sizeof(struct exi_poll_hdr) + RELAY_REPLY_MAX == RELAY_EXI_BUF_SIZE, poll_buf_holds_a_reply);

/* Thread state (template: SlippiNetworkBroadcast.c:19-22) */
static u32 RelayEXI_Thread;
extern char __relay_exi_stack_addr, __relay_exi_stack_size;
static u32 RelayEXIThread(void *arg);

/* What exi_poll_hdr says about the beamer (flags, station, relay_ip,
 * relay_port, no_beamer_reason, beamer_wifi, beamer_storage; the poll path adds
 * state, host_opts, host_build and last_fail). The relay thread writes the slot
 * the poll path is not reading and then flips view_cur, so one poll never shows
 * half of one hello and half of the next. */
static struct exi_poll_hdr view[2];
static vu32 view_cur = 0;
static struct exi_poll_hdr shown;	/* the last view published, for the log (relay thread) */
static u32 shown_fw = 0;

/* The beamer as the latest hello read showed it. Relay thread only. */
static bool beamer_usb = false;		/* USB is up as the hotswap replay drive (RelayEXIInit) */
static bool beamer_up = false;		/* the last hello read was a valid v2 hello */
static u8 hello_flags = 0;		/* its beamer_flags; 0 while !beamer_up */
static u16 hello_station = 0;		/* its station, 0 without BF_STATION_SET */
static u32 relay_ip = 0;		/* its relay address, 0 without BF_RELAY */
static u32 starting_ts = 0;		/* HW_TIMER of kernel boot or of the last USB change */
static bool starting = true;		/* within RELAY_STARTING_MS of starting_ts (latched off: HW_TIMER wraps) */
static u32 seen_mount = 0;		/* USBStorage_Mount id the last hello read saw; 0 = no drive */
static u32 mb_mount = 0;		/* USBStorage_Mount id mb_lba belongs to; 0 = none yet */
static u32 mb_lba = 0;			/* first mailbox sector on that mount; 0 = not a beamer */
static u32 mb_ts = 0;			/* HW_TIMER of the last hello read */
static u32 mb_seq = 0;			/* beamer_req_hdr.seq of the last request (serviceBeamer seeds it) */
static u32 mb_seq_mount = 0;		/* mount mb_seq was seeded on */
static u32 mb_tele_seq = 0;		/* beamer_tele_hdr.seq of the last datagram */
static u8 mb_buf[BEAMER_MB_RESP_SECTORS * BEAMER_SECTOR_SIZE] ALIGNED(32);	/* every mailbox read and write */

/* Station telemetry (protocol.yaml telemetry_hdr): the kernel log and the
 * module's load result, through the beamer to the relay's TELEMETRY_PORT.
 * Relay thread only. */
static u32 tele_ip = 0;			/* relay the last TM_STATUS went to */
static u32 tele_ts = 0;			/* HW_TIMER of the last tick */
static u32 tele_uptime = 0;		/* ms since RelayEXIInit, summed per tick (HW_TIMER wraps) */
static u32 tele_status_ms = 0;		/* ms since the last TM_STATUS */
static bool tele_status_due = true;
static u32 tele_sent_state = 0xFFFFFFFF;	/* module_state in the last TM_STATUS */
static u32 tele_seq = 0;
static u8 tele_buf[sizeof(struct telemetry_hdr) + TELEMETRY_TEXT_MAX] ALIGNED(32);

/* Crash mailbox (protocol.yaml crash_mailbox): the game's module writes a
 * crash_report here from its OS error handler; CRASH_MAILBOX_PPC is the PPC's
 * uncached view of this ARM address (PPC = ARM + 0xC0000000). Zeroed and
 * stamped with CRASH_MAGIC at init so the module knows it may write. */
#define CRASH_MAILBOX_ARM	(CRASH_MAILBOX_PPC - 0xC0000000)
static u32 crash_seen_seq = 0;		/* last seq logged */
static u32 crash_send_seq = 0;		/* last seq sent as TM_CRASH */
static struct crash_report crash_copy ALIGNED(32);

/* Record gate (protocol.yaml record_gate): 64 bytes of MEM2, two cache lines.
 * Line 0 is the kiosk's (want), line 1 the kernel's (start_seq, file_seq,
 * file_id). The kernel only invalidates line 0 and only flushes line 1, so a
 * stale copy of the kiosk's word is never written back over it. */
#define RECORD_GATE_LINE	32
#define RECORD_CHOICES		4	/* Game Starts the writer may lag behind and still find its choice */
static volatile struct record_gate *const gate = (volatile struct record_gate *)RECORD_GATE_ARM;
/* The choice at each Game Start, keyed by the low word of its ring cursor.
 * Written by the main loop (RelayEXIGateStart), read by the writer thread
 * (RelayEXIGateChoice): seq is cleared first and set last, so a slot being
 * rewritten never matches. */
struct RecordChoice
{
	vu32 seq;	/* its start_seq; 0 = empty or being rewritten */
	vu32 cursor;	/* low word of SlipMemCursor at its RECEIVE_COMMANDS */
	vu32 record;
};
static struct RecordChoice record_choice[RECORD_CHOICES];
static u32 record_next = 0;		/* main loop only */
static u32 start_seq = 0;		/* main loop only; mirrored in gate->start_seq */

/* EXI transaction being received on the main loop (reset by RelayEXISelect) */
static u8 exi_cmd = 0;			/* EXI_RELAY_REQ / EXI_RELAY_POLL / 0 */
static bool exi_dispatched = false;	/* REQ already handed off; ignore trailing bytes */
static u8 stage_buf[RELAY_REQ_MAX] ALIGNED(32);
static u32 stage_len = 0;

/* The one request in flight and its response. Written by the main loop only
 * while relay_state != RELAY_BUSY; read/written by the thread only while
 * relay_state == RELAY_BUSY. last_fail is written before relay_state. */
static vu32 relay_state = RELAY_IDLE;	/* enum exi_poll_state */
static vu32 last_fail = LF_NONE;	/* enum relay_fail or a beamer_result: the last RELAY_ERROR's reason */
static u8 req_buf[RELAY_REQ_MAX] ALIGNED(32);
static u32 req_len = 0;
static u8 resp_buf[RELAY_REPLY_MAX] ALIGNED(32);
static u32 resp_len = 0;
static u8 poll_image[RELAY_EXI_BUF_SIZE] ALIGNED(32);

/* What the mailbox must hold: a request in one sector, a datagram in the
 * telemetry sectors, and every reply the game's poll buffer can take. */
RELAY_STATIC_ASSERT(sizeof(struct beamer_req_hdr) + RELAY_REQ_MAX <= BEAMER_SECTOR_SIZE, beamer_req_fits);
RELAY_STATIC_ASSERT(sizeof(struct beamer_tele_hdr) + sizeof(tele_buf) <= BEAMER_MB_TELE_SECTORS * BEAMER_SECTOR_SIZE, beamer_tele_fits);
RELAY_STATIC_ASSERT(sizeof(struct beamer_resp_hdr) + RELAY_REPLY_MAX <= sizeof(mb_buf), beamer_resp_fits);
RELAY_STATIC_ASSERT(offsetof(struct record_gate, start_seq) == RECORD_GATE_LINE, record_gate_two_lines);

/* ------------------------------------------------------------------------- */
/* EXI hooks: kernel main loop, never block                                  */

void RelayEXISelect(void)
{
	exi_cmd = 0;
	exi_dispatched = false;
	stage_len = 0;
}

/* Hand the staged request to the thread. Main loop context. */
static void dispatchRequest(void)
{
	if (relay_state == RELAY_BUSY)
	{
		/* lbrelayexi.c allows one request in flight; a second one is a game bug. */
		dbgprintf("RelayEXI: request while busy, dropped\r\n");
		return;
	}
	memcpy(req_buf, stage_buf, stage_len);
	req_len = stage_len;
	resp_len = 0;
	relay_state = RELAY_BUSY;	/* the handoff; the thread polls this */
}

/* A request longer than the staging buffer: RELAY_ERROR with LF_BAD_REQUEST,
 * so the kiosk can say so. Main loop context. */
static void rejectRequest(void)
{
	if (relay_state == RELAY_BUSY)
	{
		dbgprintf("RelayEXI: request while busy, dropped\r\n");
		return;
	}
	last_fail = LF_BAD_REQUEST;
	relay_state = RELAY_ERROR;
}

bool RelayEXIImmWrite(u32 data, u32 len, u32 mode)
{
	u32 i;

	if (mode != EXI_WRITE)
		return false;

	if (exi_cmd == 0)
	{
		u8 cmd = data >> 24;
		if (len == 4 && (cmd == EXI_RELAY_REQ || cmd == EXI_RELAY_POLL))
		{
			exi_cmd = cmd;
			exi_dispatched = false;
			stage_len = 0;
			return true;
		}
		return false;	/* memory card traffic */
	}

	/* POLL carries no data; anything after a dispatched REQ is ignored. */
	if (exi_cmd != EXI_RELAY_REQ || exi_dispatched)
		return true;

	/* EXIImmEx chunks are <= 4 bytes, first byte in the top bits (EXIImm.S). */
	if (len > 4)
		len = 4;
	for (i = 0; i < len; i++)
	{
		if (stage_len < RELAY_REQ_MAX)
			stage_buf[stage_len++] = (u8)(data >> (24 - 8 * i));
	}

	if (stage_len >= sizeof(struct relay_hdr))
	{
		const struct relay_hdr *h = (const struct relay_hdr *)stage_buf;
		u32 want = sizeof(struct relay_hdr) + h->len;
		if (want > RELAY_REQ_MAX)
		{
			dbgprintf("RelayEXI: request payload %u over %u bytes, refused\r\n", h->len, EXI_PAYLOAD_MAX);
			rejectRequest();
			exi_dispatched = true;
		}
		else if (stage_len == want)
		{
			dispatchRequest();
			exi_dispatched = true;
		}
	}
	return true;
}

bool RelayEXIDMARead(u8 *ptr, u32 len)
{
	u32 state;

	if (exi_cmd != EXI_RELAY_POLL)
		return false;
	exi_cmd = 0;

	if (len > RELAY_EXI_BUF_SIZE)
		len = RELAY_EXI_BUF_SIZE;

	/* Build in kernel RAM, then one aligned copy into MEM1 (Starlet needs
	 * 32-bit MEM1 writes, kernel/common.h:36-42; same shape as
	 * EXIReadFontFile, kernel/EXI.c:928-931). Zero buffer unless DONE. */
	memset(poll_image, 0, len);
	state = relay_state;	/* before last_fail: the thread writes last_fail first */
	if (len >= sizeof(struct exi_poll_hdr))
	{
		/* exi_poll_hdr (protocol.yaml): the beamer's state from its latest
		 * hello, so the kiosk can say what is wrong even while the relay
		 * never answers. The kernel is big-endian like the wire, so the
		 * struct is written as is. */
		struct exi_poll_hdr *ph = (struct exi_poll_hdr *)poll_image;
		memcpy(ph, &view[view_cur], sizeof(*ph));
		ph->state = (u8)state;
		ph->host_opts = (ConfigGetConfig(NIN_CFG_MELEE_MUSIC) ? HO_MUSIC_ON : 0)
			| (ConfigGetConfig(NIN_CFG_MELEE_STEREO) ? HO_STEREO : 0);
		ph->host_build = RELAY_HOST_BUILD;
		ph->last_fail = (u8)last_fail;
	}
	else
		poll_image[0] = (u8)state;
	if (state == RELAY_DONE && len > sizeof(struct exi_poll_hdr))
	{
		u32 n = resp_len;
		if (n > len - sizeof(struct exi_poll_hdr))
			n = len - sizeof(struct exi_poll_hdr);
		memcpy(poll_image + sizeof(struct exi_poll_hdr), resp_buf, n);
	}
	memcpy(ptr, poll_image, len);
	sync_after_write(ptr, len);
	return true;
}

/* ------------------------------------------------------------------------- */
/* Record gate: Game Start in the EXI DMA handler, the writer thread after   */

bool RelayEXIGateStart(u32 cursor, u32 *seq)
{
	struct RecordChoice *c;
	u32 state, len, load, patches, arena;
	bool record;

	/* No kiosk module: record everything, as plain Slippi Nintendont does.
	 * With it: only what the kiosk asked for at this very Game Start. */
	TelemetryGetModule(&state, &len, &load, &patches, &arena);
	sync_before_read((void *)RECORD_GATE_ARM, RECORD_GATE_LINE);
	record = state != MOD_LOADED || gate->want == RECORD_THIS_MATCH;

	gate->start_seq = ++start_seq;
	sync_after_write((void *)(RECORD_GATE_ARM + RECORD_GATE_LINE), RECORD_GATE_LINE);

	c = &record_choice[record_next++ % RECORD_CHOICES];
	c->seq = 0;
	c->cursor = cursor;
	c->record = record;
	c->seq = start_seq;

	*seq = start_seq;
	return record;
}

bool RelayEXIGateChoice(u32 cursor, u32 *seq)
{
	u32 i;

	for (i = 0; i < RECORD_CHOICES; i++)
	{
		const struct RecordChoice *c = &record_choice[i];
		u32 s = c->seq;
		bool record;
		if (s == 0 || c->cursor != cursor)
			continue;
		record = c->record != 0;
		if (c->seq != s)
			continue;	/* rewritten while we read it */
		*seq = s;
		return record;
	}
	*seq = 0;
	return true;
}

void RelayEXIGateOpened(u32 seq, u32 file_id)
{
	/* file_id reaches memory before file_seq, so a kiosk that sees its
	 * match_seq in file_seq reads that match's file_id. */
	gate->file_id = file_id;
	sync_after_write((void *)(RECORD_GATE_ARM + RECORD_GATE_LINE), RECORD_GATE_LINE);
	gate->file_seq = seq;
	sync_after_write((void *)(RECORD_GATE_ARM + RECORD_GATE_LINE), RECORD_GATE_LINE);
}

/* ------------------------------------------------------------------------- */
/* Relay thread: the beamer's hello                                          */

/* Little-endian u32, as the MBR stores it (the kernel is big-endian). */
static u32 le32(const u8 *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static const char *const reason_names[] = {
	"usb drive not answering",		/* NB_UNKNOWN */
	"replays off or the game on usb",	/* NB_REPLAYS_OFF */
	"no usb drive",				/* NB_NO_DRIVE */
	"not a lazyto beamer",			/* NB_NOT_LAZYTO */
	"old beamer firmware",			/* NB_OLD_FIRMWARE */
	"waiting for the beamer",		/* NB_STARTING */
	"newer beamer firmware than this loader",	/* NB_NEW_FIRMWARE */
};

/* Publish what exi_poll_hdr is to say about the beamer, and log a change. */
static void publishView(const struct exi_poll_hdr *v, u32 fw)
{
	if (memcmp(v, &shown, sizeof(shown)) != 0 || fw != shown_fw)
	{
		if (v->flags & PF_NO_BEAMER)
			dbgprintf("RelayEXI: no beamer: %s\r\n",
				v->no_beamer_reason < sizeof(reason_names) / sizeof(reason_names[0])
					? reason_names[v->no_beamer_reason] : "?");
		else
			dbgprintf("RelayEXI: beamer fw %u, station %u%s%s%s, wifi %u, storage %u, relay %u.%u.%u.%u:%u\r\n",
				fw, v->station,
				(v->flags & PF_NO_STATION) ? " (none set)" : "",
				(v->flags & PF_NO_SECRET) ? ", no secret" : "",
				(v->flags & PF_RELAY_STALE) ? ", beacon stale" : "",
				v->beamer_wifi, v->beamer_storage,
				v->relay_ip >> 24, (v->relay_ip >> 16) & 0xFF, (v->relay_ip >> 8) & 0xFF,
				v->relay_ip & 0xFF, v->relay_port);
		memcpy(&shown, v, sizeof(shown));
		shown_fw = fw;
	}
	memcpy(&view[view_cur ^ 1], v, sizeof(*v));
	view_cur ^= 1;
}

/* True while kernel boot or the last USB change is less than
 * RELAY_STARTING_MS ago. serviceBeamer asks on every pass (about once a
 * second, beamer up or not), so it latches off long before HW_TIMER wraps. */
static bool startingWindow(void)
{
	if (starting && TimerDiffMs(starting_ts) >= RELAY_STARTING_MS)
		starting = false;
	return starting;
}

/* No usable beamer: PF_NO_BEAMER and why. Within the starting window "no
 * drive" and "not a beamer" are NB_STARTING instead: a beamer that is still
 * booting, erasing or joining the Wi-Fi is not missing. */
static void beamerDown(u8 reason)
{
	struct exi_poll_hdr v;

	if ((reason == NB_NO_DRIVE || reason == NB_NOT_LAZYTO) && startingWindow())
		reason = NB_STARTING;
	beamer_up = false;
	hello_flags = 0;
	hello_station = 0;
	relay_ip = 0;
	memset(&v, 0, sizeof(v));
	v.flags = PF_NO_BEAMER;
	v.no_beamer_reason = reason;
	publishView(&v, 0);
}

/* A hello read that could not run: the USB lock stayed taken
 * (USB_MB_BUSY), or the mount went away under it (USB_MB_GONE). It changes
 * nothing, except that "starting" must not outlast its window: a drive that
 * has not let one hello read through by then is not starting, it is not
 * answering (the writer stuck in a SCSI cycle, say), and the kiosk is to say
 * so instead of waiting on NB_STARTING for good. */
static void helloMissed(void)
{
	if ((shown.flags & PF_NO_BEAMER) && shown.no_beamer_reason == NB_STARTING && !startingWindow())
	{
		dbgprintf("RelayEXI: no hello read got the usb drive within %u ms\r\n", RELAY_STARTING_MS);
		beamerDown(NB_UNKNOWN);
	}
}

/* A valid v2 hello: its state for the requests, the telemetry and the kiosk. */
static void beamerUp(const struct beamer_hello *h)
{
	struct exi_poll_hdr v;

	beamer_up = true;
	hello_flags = h->flags;
	hello_station = (h->flags & BF_STATION_SET) ? h->station : 0;
	relay_ip = (h->flags & BF_RELAY) ? h->relay_ip : 0;

	memset(&v, 0, sizeof(v));
	v.flags = ((h->flags & BF_STATION_SET) ? 0 : PF_NO_STATION)
		| ((h->flags & BF_SECRET) ? 0 : PF_NO_SECRET)
		| (((h->flags & BF_RELAY) && h->beacon_age_s > BEACON_STALE_S) ? PF_RELAY_STALE : 0);
	v.station = hello_station;
	v.relay_ip = relay_ip;
	v.relay_port = (h->flags & BF_RELAY) ? h->relay_port : 0;
	v.beamer_wifi = h->wifi;
	v.beamer_storage = h->storage;
	publishView(&v, h->fw_build);
}

/* The mailbox of `mount`, derived once per mount (the MBR is read again after
 * a failed read): the first MBR partition entry of type 0x0B or 0x0C (FAT32),
 * end = start + size, the way the beamer firmware places it. USB_MB_OK with
 * mb_lba 0: this drive cannot be a beamer. */
static s32 beamerMailbox(u32 mount, u32 size)
{
	s32 res;
	u32 i;

	if (mount == mb_mount)
		return USB_MB_OK;
	mb_lba = 0;
	if (size != BEAMER_SECTOR_SIZE)
	{
		dbgprintf("RelayEXI: usb drive has %u-byte sectors, not a beamer\r\n", size);
		mb_mount = mount;
		return USB_MB_OK;
	}
	res = USBStorage_ReadMounted(mount, 0, 1, mb_buf, RELAY_IDLE_WAIT_MS);
	if (res != USB_MB_OK)
		return res;
	mb_mount = mount;
	if (mb_buf[510] == 0x55 && mb_buf[511] == 0xAA)
	{
		for (i = 0; i < 4; i++)
		{
			const u8 *e = mb_buf + 0x1BE + 16 * i;
			if (e[4] == 0x0B || e[4] == 0x0C)
			{
				mb_lba = le32(e + 8) + le32(e + 12);
				break;
			}
		}
	}
	if (mb_lba)
		dbgprintf("RelayEXI: usb drive: beamer mailbox would be at sector %u\r\n", mb_lba);
	else
		dbgprintf("RelayEXI: usb drive has no FAT32 partition, not a beamer\r\n");
	return USB_MB_OK;
}

/* Idle-thread duty, every RELAY_BEAMER_HELLO_MS: read the hello sector and
 * publish what it says (protocol.yaml beamer_hello, no_beamer_reason).
 * USB_MB_BUSY or USB_MB_GONE leaves everything as it was until the next read
 * (helloMissed: except a "starting" past its window). Finding the beamer (the first valid hello, and again on a new USB mount)
 * also reads the response sector: the beamer outlives a Wii reboot and keeps
 * its last response, so the next request seq starts one past that one
 * (protocol.yaml beamer_req_hdr), or at 1 when the sector holds none. */
static void serviceBeamer(void)
{
	const struct beamer_hello *h = (const struct beamer_hello *)mb_buf;
	const struct beamer_resp_hdr *r = (const struct beamer_resp_hdr *)(mb_buf + BEAMER_SECTOR_SIZE);
	u32 mount, size;
	s32 res;

	if (!beamer_usb || TimerDiffMs(mb_ts) < RELAY_BEAMER_HELLO_MS)
		return;
	mb_ts = read32(HW_TIMER);
	startingWindow();	/* latch it off in time, whatever the hello says */

	if (USBStorage_Mount(RELAY_IDLE_WAIT_MS, &mount, &size) != USB_MB_OK)
	{
		helloMissed();
		return;
	}
	if (mount != seen_mount)
	{
		/* The drive went away or another one came: a beamer that is
		 * rebooting gets its starting time again. */
		if (seen_mount != 0)
		{
			starting_ts = read32(HW_TIMER);
			starting = true;
		}
		seen_mount = mount;
	}
	if (mount == 0)
	{
		beamerDown(NB_NO_DRIVE);
		return;
	}

	res = beamerMailbox(mount, size);
	if (res == USB_MB_OK && mb_lba != 0)
		res = USBStorage_ReadMounted(mount, mb_lba + BEAMER_MB_HELLO, 1, mb_buf, RELAY_IDLE_WAIT_MS);
	if (res == USB_MB_BUSY || res == USB_MB_GONE)
	{
		helloMissed();
		return;
	}
	if (res != USB_MB_OK)
	{
		beamerDown(NB_UNKNOWN);
		return;
	}
	if (mb_lba == 0 || memcmp(h->magic, "LAZYTOMB", sizeof(h->magic)) != 0)
	{
		beamerDown(NB_NOT_LAZYTO);
		return;
	}
	/* fw_build is at offset 20 in mailbox v1 and v2 alike. */
	if (h->version != BEAMER_MB_VERSION || h->fw_build < BEAMER_FW_MIN)
	{
		if (shown.no_beamer_reason != NB_OLD_FIRMWARE && shown.no_beamer_reason != NB_NEW_FIRMWARE)
			dbgprintf("RelayEXI: beamer mailbox v%u fw %u; this loader needs v%u fw %u or later\r\n",
				h->version, h->fw_build, BEAMER_MB_VERSION, BEAMER_FW_MIN);
		beamerDown(h->version > BEAMER_MB_VERSION ? NB_NEW_FIRMWARE : NB_OLD_FIRMWARE);
		return;
	}

	if (!beamer_up || mount != mb_seq_mount)
	{
		res = USBStorage_ReadMounted(mount, mb_lba + BEAMER_MB_RESP, 1, mb_buf + BEAMER_SECTOR_SIZE, RELAY_IDLE_WAIT_MS);
		if (res == USB_MB_BUSY || res == USB_MB_GONE)
		{
			helloMissed();
			return;
		}
		if (res != USB_MB_OK)
		{
			beamerDown(NB_UNKNOWN);
			return;
		}
		mb_seq = (r->magic[0] == RELAY_MAGIC_0 && r->magic[1] == 'R') ? r->seq : 0;
		mb_seq_mount = mount;
		dbgprintf("RelayEXI: beamer found, next request seq %u\r\n", mb_seq + 1);
	}
	beamerUp(h);
}

/* ------------------------------------------------------------------------- */
/* Relay thread: telemetry                                                   */

/* The beamer's telemetry sector(s): beamer_tele_hdr under a new seq, then the
 * datagram in tele_buf, in one USB write. */
static s32 beamerTele(u32 total)
{
	struct beamer_tele_hdr *t = (struct beamer_tele_hdr *)mb_buf;
	u32 n = (sizeof(*t) + total + BEAMER_SECTOR_SIZE - 1) / BEAMER_SECTOR_SIZE;

	memset(mb_buf, 0, n * BEAMER_SECTOR_SIZE);
	t->magic[0] = RELAY_MAGIC_0;
	t->magic[1] = 'E';
	t->seq = ++mb_tele_seq;
	t->len = (u16)total;
	memcpy(mb_buf + sizeof(*t), tele_buf, total);
	return USBStorage_WriteMounted(mb_mount, mb_lba + BEAMER_MB_TELE, n, mb_buf, RELAY_IDLE_WAIT_MS);
}

/* One telemetry datagram: telemetry_hdr + `len` payload bytes, the payload
 * already at tele_buf + the header, stamped with the beamer's station. */
static bool teleSend(u8 kind, u32 len)
{
	struct telemetry_hdr *h = (struct telemetry_hdr *)tele_buf;

	h->magic[0] = RELAY_MAGIC_0;
	h->magic[1] = TELEMETRY_MAGIC_1;
	h->version = RELAY_PROTO_VERSION;
	h->kind = kind;
	h->station = hello_station;
	h->len = (u16)len;
	h->seq = tele_seq;
	h->uptime_ms = tele_uptime;
	if (beamerTele(sizeof(*h) + len) != USB_MB_OK)
		return false;
	tele_seq++;
	return true;
}

/* Log a new crash report as soon as the module writes one (beamer or not),
 * and remember it for TM_CRASH. */
static void watchCrash(void)
{
	volatile struct crash_mailbox *mb = (volatile struct crash_mailbox *)CRASH_MAILBOX_ARM;
	u32 seq, i;

	if (mb->magic != CRASH_MAGIC)
		return;
	seq = mb->seq;
	if (seq == crash_seen_seq)
		return;
	memcpy(&crash_copy, (const void *)&mb->report, sizeof(crash_copy));
	crash_seen_seq = seq;
	dbgprintf("TMOD:CRASH #%u error %u at %08x srr1 %08x lr %08x sp %08x dsisr %08x dar %08x\r\n",
		crash_copy.count, crash_copy.error, crash_copy.srr0, crash_copy.srr1, crash_copy.lr,
		crash_copy.sp, crash_copy.dsisr, crash_copy.dar);
	dbgprintf("TMOD:CRASH words at srr0: %08x %08x %08x %08x\r\n",
		crash_copy.fetched[0], crash_copy.fetched[1], crash_copy.fetched[2], crash_copy.fetched[3]);
	for (i = 0; i < CRASH_STACK_DEPTH && crash_copy.stack[i]; i++)
		dbgprintf("TMOD:CRASH stack[%u] %08x\r\n", i, crash_copy.stack[i]);
}

/* The beamer forwards a datagram only with a number, a secret and a relay
 * (protocol.yaml beamer_tele_hdr); until it has all three the log stays
 * queued here instead of being written and dropped. */
#define TELE_NEEDS	(BF_STATION_SET | BF_SECRET | BF_RELAY)

/* Idle-thread duty, after serviceBeamer: once the beamer can forward, a
 * TM_STATUS at least every TELEMETRY_STATUS_MS (and at once when the module
 * state changes or the relay moves), a TM_CRASH, and the unsent kernel log in
 * TM_LOG chunks. A USB write costs a few ms: one datagram per tick, and none
 * while a request waits for this thread. */
static void serviceTelemetry(void)
{
	u8 *payload = tele_buf + sizeof(struct telemetry_hdr);
	u32 dt, n, state, len, load, patches, arena;

	dt = TimerDiffMs(tele_ts);
	if (dt < RELAY_TELEMETRY_TICK_MS)
		return;
	if (dt == UINT_MAX)
		dt = RELAY_TELEMETRY_TICK_MS;	/* HW_TIMER wrapped */
	tele_ts = read32(HW_TIMER);
	tele_uptime += dt;
	tele_status_ms += dt;

	TelemetryGetModule(&state, &len, &load, &patches, &arena);
	watchCrash();

	if (!beamer_up || (hello_flags & TELE_NEEDS) != TELE_NEEDS || relay_state == RELAY_BUSY)
		return;
	if (tele_ip != relay_ip)
	{
		tele_ip = relay_ip;
		tele_status_due = true;
	}

	if (tele_status_due || state != tele_sent_state || tele_status_ms >= TELEMETRY_STATUS_MS)
	{
		struct station_status *st = (struct station_status *)payload;
		memset(st, 0, sizeof(*st));
		st->module_state = (u8)state;
		st->module_patches = (u16)patches;
		st->module_len = len;
		st->module_load = load;
		st->arena_hi = arena;
		st->log_dropped = TelemetryDropped();
		if (teleSend(TM_STATUS, sizeof(*st)))
		{
			tele_status_due = false;
			tele_sent_state = state;
			tele_status_ms = 0;
		}
		return;
	}

	if (crash_send_seq != crash_seen_seq)
	{
		memcpy(payload, &crash_copy, sizeof(crash_copy));
		if (teleSend(TM_CRASH, sizeof(crash_copy)))
			crash_send_seq = crash_seen_seq;
		return;
	}

	n = TelemetryPeek((char *)payload, TELEMETRY_TEXT_MAX);
	if (n != 0 && teleSend(TM_LOG, n))
		TelemetryConsume(n);
}

/* ------------------------------------------------------------------------- */
/* Relay thread: one request                                                 */

static u32 remainingMs(u32 start)
{
	u32 elapsed = TimerDiffMs(start);
	return elapsed >= RELAY_BUDGET_MS ? 0 : RELAY_BUDGET_MS - elapsed;
}

/* A mailbox call that did not do its work, as the request's last_fail. */
static u8 usbFail(s32 res, u8 failed)
{
	if (res == USB_MB_BUSY)
		return LF_USB_BUSY;
	if (res == USB_MB_GONE)
		return LF_BEAMER_LOST;
	return failed;
}

static const char *failName(u8 fail)
{
	switch (fail)
	{
		case BR_NO_RELAY:	return "beamer: no relay";
		case BR_NO_WIFI:	return "beamer: no wi-fi";
		case BR_CONNECT:	return "beamer: connect";
		case BR_TIMEOUT:	return "beamer: relay timeout";
		case BR_TOO_LARGE:	return "beamer: reply too large";
		case BR_BAD_REQ:	return "beamer: bad request";
		case BR_NO_STATION:	return "no station number";
		case BR_NO_SECRET:	return "beamer: no secret";
		case LF_NO_BEAMER:	return "no beamer";
		case LF_USB_WRITE:	return "usb write";
		case LF_USB_READ:	return "usb read";
		case LF_USB_BUSY:	return "usb busy";
		case LF_NO_ANSWER:	return "no answer";
		case LF_BAD_REPLY:	return "bad reply";
		case LF_BAD_REQUEST:	return "bad request";
		case LF_BEAMER_LOST:	return "beamer lost";
		default:		return "?";
	}
}

/* One mailbox round trip: the request into the request sector under a new seq
 * (the beamer sends each seq to the relay once, behind its relay_auth), then
 * the first response sector every RELAY_BEAMER_POLL_MS until it carries that
 * seq, all inside RELAY_BUDGET_MS, waits for the USB lock included. Returns
 * LF_NONE with the relay's reply in resp_buf, or why not (relay_fail, or the
 * beamer's own beamer_result passed through). */
static u8 beamerRoundTrip(u32 start)
{
	struct beamer_req_hdr *q = (struct beamer_req_hdr *)mb_buf;
	const struct beamer_resp_hdr *r = (const struct beamer_resp_hdr *)mb_buf;
	const struct relay_hdr *req = (const struct relay_hdr *)req_buf;
	const struct relay_hdr *reply = (const struct relay_hdr *)(mb_buf + sizeof(struct beamer_resp_hdr));
	u32 seq, len, n;
	s32 res;

	memset(mb_buf, 0, BEAMER_SECTOR_SIZE);
	q->magic[0] = RELAY_MAGIC_0;
	q->magic[1] = 'Q';
	q->seq = seq = ++mb_seq;
	q->len = (u16)req_len;
	memcpy(mb_buf + sizeof(*q), req_buf, req_len);
	res = USBStorage_WriteMounted(mb_mount, mb_lba + BEAMER_MB_REQ, 1, mb_buf, remainingMs(start));
	if (res != USB_MB_OK)
		return usbFail(res, LF_USB_WRITE);

	while (1)
	{
		if (remainingMs(start) == 0)
			return LF_NO_ANSWER;
		mdelay(RELAY_BEAMER_POLL_MS);
		res = USBStorage_ReadMounted(mb_mount, mb_lba + BEAMER_MB_RESP, 1, mb_buf, remainingMs(start));
		if (res != USB_MB_OK)
			return usbFail(res, LF_USB_READ);
		if (r->magic[0] == RELAY_MAGIC_0 && r->magic[1] == 'R' && r->seq == seq)
			break;
	}

	if (r->result != BR_OK)
		return r->result < LF_NO_BEAMER ? r->result : LF_BAD_REPLY;

	/* The reply may run on into the other response sectors. */
	len = r->len;
	if (len > RELAY_REPLY_MAX || len < sizeof(struct relay_hdr) + sizeof(struct relay_resp))
		return LF_BAD_REPLY;
	n = (sizeof(struct beamer_resp_hdr) + len + BEAMER_SECTOR_SIZE - 1) / BEAMER_SECTOR_SIZE;
	if (n > 1)
	{
		res = USBStorage_ReadMounted(mb_mount, mb_lba + BEAMER_MB_RESP + 1, n - 1,
			mb_buf + BEAMER_SECTOR_SIZE, remainingMs(start));
		if (res != USB_MB_OK)
			return usbFail(res, LF_USB_READ);
	}
	if (reply->magic[0] != RELAY_MAGIC_0 || reply->magic[1] != RELAY_MAGIC_1 || reply->cmd != req->cmd)
		return LF_BAD_REPLY;
	memcpy(resp_buf, mb_buf + sizeof(struct beamer_resp_hdr), len);
	resp_len = len;
	return LF_NONE;
}

/* The request the main loop handed over (relay_state == RELAY_BUSY). */
static void serveRequest(void)
{
	struct relay_hdr *h = (struct relay_hdr *)req_buf;
	u32 start = read32(HW_TIMER);
	u8 fail;

	resp_len = 0;
	if (!beamer_up)
		fail = LF_NO_BEAMER;
	else if (!(hello_flags & BF_STATION_SET))
		fail = BR_NO_STATION;	/* nothing is written: the beamer would refuse it too */
	else
	{
		/* Station stamping: the game sends 0. */
		h->station = hello_station;
		fail = beamerRoundTrip(start);
	}

	if (fail != LF_NONE)
	{
		dbgprintf("RelayEXI: cmd %u len %u -> ERROR %u (%s) after %u ms\r\n",
			h->cmd, h->len, fail, failName(fail), TimerDiffMs(start));
		resp_len = 0;
		last_fail = fail;
		relay_state = RELAY_ERROR;
	}
	else
	{
		const struct relay_resp *r = (const struct relay_resp *)(resp_buf + sizeof(struct relay_hdr));
		dbgprintf("RelayEXI: cmd %u len %u -> DONE status %u resp %u bytes in %u ms\r\n",
			h->cmd, h->len, r->status, resp_len, TimerDiffMs(start));
		last_fail = LF_NONE;
		relay_state = RELAY_DONE;	/* publishes resp_buf/resp_len to the poll path */
	}
}

static u32 RelayEXIThread(void *arg)
{
	while (1)
	{
		if (relay_state == RELAY_BUSY)
		{
			serveRequest();
			continue;
		}
		serviceBeamer();
		serviceTelemetry();
		mdelay(RELAY_THREAD_CYCLE_MS);
	}
	return 0;
}

void RelayEXIInit(void)
{
	struct exi_poll_hdr v;

	/* The beamer is the Slippi replay drive: kernel/main.c:193-203 starts USB
	 * in hotswap mode only with replays on and the game on SD. Booting from
	 * USB, the main loop and the DI thread read the drive without the USB lock
	 * (usbstorage.c), so the relay thread must not touch it then. */
	beamer_usb = ConfigGetConfig(NIN_CFG_SLIPPI_REPLAYS) && !ConfigGetUseUSB();
	if (!beamer_usb)
		dbgprintf("RelayEXI: Slippi replays are off or the game is on USB, so USB is not the replay drive: no beamer\r\n");
	relay_state = RELAY_IDLE;
	last_fail = LF_NONE;
	tele_ts = read32(HW_TIMER);
	mb_ts = tele_ts;
	starting_ts = tele_ts;

	/* Until the first hello read: replays off is final; otherwise the beamer
	 * may still be starting. */
	memset(&v, 0, sizeof(v));
	v.flags = PF_NO_BEAMER;
	v.no_beamer_reason = beamer_usb ? NB_STARTING : NB_REPLAYS_OFF;
	publishView(&v, 0);

	{
		/* Hand the module its crash mailbox: zero, then the magic. */
		volatile struct crash_mailbox *mb = (volatile struct crash_mailbox *)CRASH_MAILBOX_ARM;
		memset((void *)mb, 0, sizeof(*mb));
		mb->magic = CRASH_MAGIC;
	}

	/* The record gate starts empty, before the game runs: nothing wanted, no
	 * Game Start, no file. */
	memset((void *)gate, 0, sizeof(*gate));
	sync_after_write((void *)RECORD_GATE_ARM, sizeof(*gate));

	RelayEXI_Thread = do_thread_create(
		RelayEXIThread,
		((u32 *)&__relay_exi_stack_addr),
		((u32)(&__relay_exi_stack_size)),
		0x78);
	thread_continue(RelayEXI_Thread);
	dbgprintf("RelayEXI: thread started\r\n");
}
