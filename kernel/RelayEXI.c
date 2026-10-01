/* kernel/RelayEXI.c
 * LazyTO relay EXI device (../tournament-reporter/docs/architecture.md, section
 * LazyTO Nintendont; docs/history/relay-exi-investigation.md).
 *
 * The game (melee lbrelayexi.c) selects channel 1 / device 0 (slot B, shared
 * with Slippi's own device), writes a 4-byte immediate command word
 * (EXI_RELAY_REQ or EXI_RELAY_POLL in the top byte, relay_proto.h), then
 *   REQ:  relay_hdr + payload through EXIImmEx, i.e. EXIImm writes of <= 4
 *         bytes each (melee OSExi.c EXIImmEx, lines 168-184); the patched
 *         EXIImm stub (kernel/asm/EXIImm.S:36-41) stores the bytes as an
 *         immediate word, first byte in the top bits;
 *   POLL: one 4096-byte EXIDma read (lbrelayexi.c:110) that must come back
 *         as lbRelayExi_PollBuf: {exi_poll_hdr, relay_hdr, relay_resp,
 *         payload}.
 *
 * Contexts (investigation section 1/3): EXI writes are serviced by the kernel
 * main loop (kernel/main.c:583 -> kernel/EXI.c EXIUpdateRegistersNEW) with the
 * game frozen inside the transfer until the ack, so the three hooks below only
 * copy bytes and flip `relay_state`. The TCP round trip runs on the dedicated
 * thread spawned by RelayEXIInit(), template kernel/SlippiNetworkBroadcast.c
 * (own stack in kernel.ld, do_thread_create at prio 0x78).
 *
 * State machine (one request buffer, one thread, no retries):
 *   RELAY_IDLE  --REQ complete-->  RELAY_BUSY  --thread-->  RELAY_DONE
 *                                              \--thread-->  RELAY_ERROR
 *   DONE/ERROR are sticky until the next REQ; a REQ while BUSY is dropped.
 *
 * Relay discovery (../tournament-reporter/docs/decisions.md R15, protocol.yaml
 * relay_beacon): tournament.cfg carries no relay address. The relay
 * broadcasts a 12-byte relay_beacon every BEACON_INTERVAL_MS to UDP
 * BEACON_PORT; the same thread, while idle, owns a non-blocking UDP socket
 * bound to that port and takes the latest valid beacon's SOURCE address plus
 * its tcp_port as the relay (relay_ip / relay_port below). Until one arrives
 * both are 0: exi_poll_hdr shows 0 and every request answers ST_INTERNAL
 * "no relay found yet".
 *
 * Shared secret (decisions.md R16, protocol.yaml relay_auth): every TCP request
 * starts with a 20-byte relay_auth carrying tournament.cfg's secret=, then
 * the game's relay_hdr + payload. The game never sees it. A card without a
 * valid secret= answers ST_INTERNAL "no secret in tournament.cfg" locally;
 * a wrong one comes back from the relay as ST_BAD_SECRET.
 *
 * Beamer transport (tournament.cfg transport=beamer, protocol.yaml beamer_*):
 * the same bytes travel through a LazyTO beamer, an ESP32 USB mass-storage
 * stick on the Wi-Fi that is also the Slippi replay drive, and the thread
 * opens no IOS socket at all (no beacon, no UDP). The beamer serves
 * BEAMER_MB_SECTORS mailbox sectors from its RAM right after its first FAT32
 * partition (MBR entry type 0x0B/0x0C, start + size, derived again per USB
 * mount). Idle, the thread reads beamer_hello about once a second: its relay
 * address fills exi_poll_hdr, and no drive or no valid hello is PF_NO_BEAMER.
 * Nothing is written to the mailbox before a valid hello, so a plain USB stick
 * is never written there. A request is beamer_req_hdr + relay_auth + relay_hdr
 * + payload in the request sector under a new seq; the response sector is
 * polled every RELAY_BEAMER_POLL_MS until it carries that seq, within the same
 * RELAY_BUDGET_MS. Telemetry datagrams go to the telemetry sectors, one write
 * per tick. The Slippi file writer thread drives the same drive, so
 * usbstorage.c serializes SCSI cycles with its USB lock; USB is in that mode
 * only with replays on and the game on SD (RelayEXIInit, beamer_usb).
 */

#include "RelayEXI.h"
#include "relay_proto.h"
#include "EXI.h"		/* EXI_WRITE */

#include "global.h"
#include "common.h"
#include "string.h"
#include "debug.h"
#include "syscalls.h"
#include "net.h"
#include "ff_utf8.h"
#include "Telemetry.h"
#include "Config.h"
#include "usbstorage.h"

/* Game-side contract, melee/src/melee/lb/lbrelayexi.h. */
#define RELAY_EXI_BUF_SIZE	4096	/* LB_RELAY_EXI_BUF_SIZE */
#define RELAY_EXI_MAX_PAYLOAD	48	/* LB_RELAY_EXI_MAX_PAYLOAD: report_score_req / end_set_req with 8-byte game_result */
#define RELAY_REQ_MAX		(sizeof(struct relay_hdr) + RELAY_EXI_MAX_PAYLOAD)
#define RELAY_RESP_MAX		(RELAY_EXI_BUF_SIZE - sizeof(struct exi_poll_hdr))	/* after the poll header */

#define RELAY_BUDGET_MS		3000	/* design 4.6: one attempt, 3 s */
#define RELAY_THREAD_CYCLE_MS	1	/* like SlippiNetwork.c THREAD_CYCLE_TIME_MS */
#define RELAY_CFG_PATH		"sd:/tournament.cfg"	/* design 4.3 */
#define RELAY_CFG_MAX		512
#define RELAY_RX_CHUNK		1024
#define RELAY_BEACON_POLL_MS	100	/* idle thread drains the beacon socket this often */
#define RELAY_BEACON_SETUP_MS	1000	/* socket/bind failed or network not up yet: try again this often */
#define RELAY_BEACON_DRAIN_MAX	8	/* datagrams read per poll; beacons come every 2 s */
#define RELAY_BEACON_REQUEST_MS	2000	/* while no beacon has been heard, ask for one this often */
#define RELAY_TELEMETRY_TICK_MS	100	/* idle thread sends telemetry this often */
#define RELAY_BEAMER_HELLO_MS	1000	/* idle thread reads the beamer's hello this often */
#define RELAY_BEAMER_POLL_MS	10	/* a request reads the response sector this often */
/* Shown top-right on the kiosk's set list next to the module's own version
 * (exi_poll_hdr.host_build). Bump by hand when a loader release changes
 * behaviour the TO should be able to tell apart on the TV. */
#define RELAY_HOST_BUILD	3	/* 2: network init off the boot path, PF_NET_JOINING; 3: beamer transport */
#define RELAY_TELEMETRY_CHUNKS	4	/* TM_LOG datagrams per tick at most */

/* IOCTL_SO_FCNTL (net.h:105) usage copied from libogc network_wii.c
 * net_fcntl(): params = {socket, cmd, flags}, ioctl input length 12, no
 * output. cmd is the POSIX F_GETFL (3) / F_SETFL (4) passed straight through;
 * IOS_O_NONBLOCK is libogc's "(O_NONBLOCK >> 16)" = 0x04. Nothing else in
 * this kernel uses FCNTL (investigation section 3 caveat, decisions.md R9). */
#define RELAY_F_GETFL		3
#define RELAY_F_SETFL		4
#define RELAY_IOS_O_NONBLOCK	0x04

/* IOS socket error codes, negated on return. Dolphin
 * Source/Core/Core/IOS/Network/Socket.h `WiiSockets` (same order as libogc's
 * errmap): EAGAIN 6, EALREADY 7, EINPROGRESS 27. */
#define RELAY_SO_EAGAIN		6
#define RELAY_SO_EALREADY	7
#define RELAY_SO_EINPROGRESS	27

/* From kernel/net.c */
extern s32 top_fd;
extern u32 NetworkStarted;

/* Thread state (template: SlippiNetworkBroadcast.c:19-22) */
static u32 RelayEXI_Thread;
extern char __relay_exi_stack_addr, __relay_exi_stack_size;
static u32 RelayEXIThread(void *arg);

/* sd:/tournament.cfg */
struct RelayCfg {
	u16	station;	/* station */
	u8	stream;		/* stream (0/1) */
	bool	ok;		/* false: missing or malformed -> ST_INTERNAL "no tournament.cfg" */
	bool	has_secret;	/* false: no valid secret= -> ST_INTERNAL "no secret in tournament.cfg" */
	bool	beamer;		/* transport=beamer: the USB mailbox instead of IOS sockets */
	char	secret[SECRET_LEN];	/* NUL-padded, as relay_auth carries it */
};
static struct RelayCfg cfg;
static char cfg_text[RELAY_CFG_MAX] ALIGNED(32);

/* The relay as the latest valid relay_beacon announced it (with the beamer:
 * as the latest valid beamer_hello reports it); 0 = none heard yet. Written
 * only by the relay thread (serviceBeacon, serviceBeamer); read by the thread
 * for the next round trip and by the poll path (main loop) for
 * exi_poll_hdr. Host order = wire order (the kernel is big-endian). */
static vu32 relay_ip = 0;
static vu32 relay_port = 0;
static s32 beacon_sock = -1;		/* UDP socket bound to BEACON_PORT, -1 until set up */
/* Beacon request (protocol.yaml relay_beacon): some access points never
 * deliver the relay's broadcast to this power-saving Wi-Fi client (first
 * venue-style Wi-Fi test, 2026-09-30: pings answered, no beacon in minutes).
 * So while relay_ip is 0 we broadcast a relay_beacon with tcp_port 0 to
 * TELEMETRY_PORT every RELAY_BEACON_REQUEST_MS; the relay answers unicast to
 * our BEACON_PORT, where serviceBeacon already listens. A connected UDP socket
 * to 255.255.255.255, the way SlippiNetworkBroadcast.c sends its own. */
static s32 request_sock = -1;
static u32 request_ts = 0;
static u32 request_count = 0;
static struct relay_beacon request_msg ALIGNED(32);
static struct sockaddr_in request_addr ALIGNED(32) = {
	.sin_family = AF_INET,
	.sin_port = TELEMETRY_PORT,
	{
		.s_addr = 0xffffffff,
	},
};
static u32 beacon_ts = 0;		/* HW_TIMER of the last setup attempt or drain */
static u8 beacon_rx[32] ALIGNED(32);	/* recvfrom target; > sizeof(relay_beacon) so oversize datagrams show */

/* Station telemetry (protocol.yaml telemetry_hdr): the kernel log and the
 * module's load result, to the relay's TELEMETRY_PORT. Relay thread only. */
static s32 tele_sock = -1;		/* UDP socket connected to tele_ip:TELEMETRY_PORT */
static u32 tele_ip = 0;
static u32 tele_ts = 0;			/* HW_TIMER of the last tick */
static u32 tele_uptime = 0;		/* ms since RelayEXIInit, summed per tick (HW_TIMER wraps) */
static u32 tele_status_ms = 0;		/* ms since the last TM_STATUS */
static bool tele_status_due = true;
static u32 tele_sent_state = 0xFFFFFFFF;	/* module_state in the last TM_STATUS */
static u32 tele_seq = 0;
static u8 tele_buf[sizeof(struct relay_auth) + sizeof(struct telemetry_hdr) + TELEMETRY_TEXT_MAX] ALIGNED(32);

/* Crash mailbox (protocol.yaml crash_mailbox): the game's module writes a
 * crash_report here from its OS error handler; CRASH_MAILBOX_PPC is the PPC's
 * uncached view of this ARM address (PPC = ARM + 0xC0000000). Zeroed and
 * stamped with CRASH_MAGIC at init so the module knows it may write. */
#define CRASH_MAILBOX_ARM	(CRASH_MAILBOX_PPC - 0xC0000000)
static u32 crash_seen_seq = 0;		/* last seq logged */
static u32 crash_send_seq = 0;		/* last seq sent as TM_CRASH */
static struct crash_report crash_copy ALIGNED(32);

/* Beamer transport (protocol.yaml beamer_*). Relay thread only, except
 * beamer_up, which the poll path reads for PF_NO_BEAMER. */
static bool beamer_usb = false;		/* USB is up as the hotswap replay drive (RelayEXIInit) */
static vu32 beamer_up = 0;		/* the last hello read was valid */
static u32 mb_mount = 0;		/* USBStorage_Mount id mb_lba belongs to; 0 = none yet */
static u32 mb_lba = 0;			/* first mailbox sector on that mount; 0 = not a beamer */
static u32 mb_hello_logged = 0;		/* mount whose missing hello was logged */
static u32 mb_fw = 0;			/* fw_build of the last valid hello, for the log */
static u32 mb_ts = 0;			/* HW_TIMER of the last hello read */
static u32 mb_seq = 0;			/* beamer_req_hdr.seq of the last request (serviceBeamer seeds it) */
static u32 mb_seq_mount = 0;		/* mount mb_seq was seeded on */
static u32 mb_tele_seq = 0;		/* beamer_tele_hdr.seq of the last datagram */
static u8 mb_buf[BEAMER_MB_RESP_SECTORS * BEAMER_SECTOR_SIZE] ALIGNED(32);	/* every mailbox read and write */

/* EXI transaction being received on the main loop (reset by RelayEXISelect) */
static u8 exi_cmd = 0;			/* EXI_RELAY_REQ / EXI_RELAY_POLL / 0 */
static bool exi_dispatched = false;	/* REQ already handed off; ignore trailing bytes */
static u8 stage_buf[RELAY_REQ_MAX] ALIGNED(32);
static u32 stage_len = 0;

/* The one request in flight and its response. Written by the main loop only
 * while relay_state != RELAY_BUSY; read/written by the thread only while
 * relay_state == RELAY_BUSY. */
static vu32 relay_state = RELAY_IDLE;	/* enum exi_poll_state */
static u8 req_buf[RELAY_REQ_MAX] ALIGNED(32);
static u32 req_len = 0;
static u8 send_buf[sizeof(struct relay_auth) + RELAY_REQ_MAX] ALIGNED(32);	/* relay_auth + req_buf, one sendto */
static u8 resp_buf[RELAY_RESP_MAX] ALIGNED(32);
static u32 resp_len = 0;
static u8 rx_chunk[RELAY_RX_CHUNK] ALIGNED(32);	/* 32-byte aligned recvfrom target */
static u8 poll_image[RELAY_EXI_BUF_SIZE] ALIGNED(32);

/* What the mailbox must hold: a request in one sector, a datagram in the
 * telemetry sectors, and every reply the game's poll buffer can take. */
RELAY_STATIC_ASSERT(sizeof(struct beamer_req_hdr) + sizeof(send_buf) <= BEAMER_SECTOR_SIZE, beamer_req_fits);
RELAY_STATIC_ASSERT(sizeof(struct beamer_tele_hdr) + sizeof(tele_buf) <= BEAMER_MB_TELE_SECTORS * BEAMER_SECTOR_SIZE, beamer_tele_fits);
RELAY_STATIC_ASSERT(sizeof(struct beamer_resp_hdr) + RELAY_RESP_MAX == sizeof(mb_buf), beamer_resp_fits);

/* ------------------------------------------------------------------------- */
/* tournament.cfg                                                            */

/* Parse an unsigned decimal <= max: digits only, at least one, then only
 * trailing whitespace. */
static bool parseU32(const char *s, u32 max, u32 *out)
{
	u32 v = 0;
	u32 n = 0;
	while (*s >= '0' && *s <= '9')
	{
		v = v * 10 + (u32)(*s - '0');
		if (v > max)
			return false;
		s++;
		n++;
	}
	if (n == 0)
		return false;
	while (*s == ' ' || *s == '\t' || *s == '\r')
		s++;
	*out = v;
	return *s == 0;
}

/* secret=: 8 to SECRET_LEN of A-Z a-z 0-9 - _ (the relay's config.ts rule),
 * then only trailing whitespace. Stored NUL-padded. */
static bool parseSecret(const char *s, char *out)
{
	u32 n = 0;
	memset(out, 0, SECRET_LEN);
	while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
	       *s == '-' || *s == '_')
	{
		if (n == SECRET_LEN)
			return false;
		out[n++] = *s++;
	}
	while (*s == ' ' || *s == '\t' || *s == '\r')
		s++;
	return *s == 0 && n >= 8;
}

/* Exactly `word`, then only trailing whitespace. */
static bool parseWord(const char *s, const char *word)
{
	u32 n = strlen(word);
	if (strncmp(s, word, n) != 0)
		return false;
	s += n;
	while (*s == ' ' || *s == '\t' || *s == '\r')
		s++;
	return *s == 0;
}

/* key=value lines, keys station / stream (design 4.3), both required, unknown
 * keys ignored, blank lines ignored. relay_ip / relay_port from cards written
 * before relay discovery (decisions.md R15) are unknown keys now: ignored, the
 * relay's address comes from its beacon. transport= is network (also when
 * absent, so older cards keep working) or beamer; anything else is malformed. */
static bool parseCfg(char *text)
{
	bool have_station = false, have_stream = false, transport_ok = true;
	char *line = text;

	while (*line)
	{
		char *next = strchr(line, '\n');
		char *eq;
		u32 v = 0;	/* parseU32 leaves it untouched on failure */
		if (next)
			*next++ = 0;
		else
			next = line + strlen(line);

		eq = strchr(line, '=');
		if (eq)
		{
			const char *val = eq + 1;
			*eq = 0;
			if (strcmp(line, "secret") == 0)
				cfg.has_secret = parseSecret(val, cfg.secret);
			else if (strcmp(line, "station") == 0)
			{
				have_station = parseU32(val, 65535, &v);
				cfg.station = (u16)v;
			}
			else if (strcmp(line, "stream") == 0)
			{
				have_stream = parseU32(val, 1, &v);
				cfg.stream = (u8)v;
			}
			else if (strcmp(line, "transport") == 0)
			{
				cfg.beamer = parseWord(val, "beamer");
				transport_ok = cfg.beamer || parseWord(val, "network");
			}
		}
		line = next;
	}
	return have_station && have_stream && transport_ok;
}

/* Pattern B from the investigation (kernel/Config.c ConfigInit: FatFS open +
 * f_read at boot), except that a missing/short/malformed file sets cfg.ok =
 * false instead of Shutdown(). */
static void loadCfg(void)
{
	FIL fp;
	UINT read = 0;

	memset(&cfg, 0, sizeof(cfg));
	if (f_open_char(&fp, RELAY_CFG_PATH, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		dbgprintf("RelayEXI: %s not found\r\n", RELAY_CFG_PATH);
		return;
	}
	if (fp.obj.objsize >= RELAY_CFG_MAX)
	{
		dbgprintf("RelayEXI: %s too large (%u bytes)\r\n", RELAY_CFG_PATH, (u32)fp.obj.objsize);
		f_close(&fp);
		return;
	}
	f_read(&fp, cfg_text, RELAY_CFG_MAX - 1, &read);
	f_close(&fp);
	cfg_text[read] = 0;

	cfg.ok = parseCfg(cfg_text);
	if (cfg.ok)
		dbgprintf("RelayEXI: station %u stream %u secret %s, %s\r\n",
			cfg.station, cfg.stream, cfg.has_secret ? "set" : "MISSING",
			cfg.beamer ? "transport beamer (relay address from the beamer on usb)"
				: "transport network (relay address from its beacon)");
	else
		dbgprintf("RelayEXI: %s malformed\r\n", RELAY_CFG_PATH);
}

/* ------------------------------------------------------------------------- */
/* EXI hooks: kernel main loop, never block                                  */

static u32 trace_select = 0, trace_imm = 0, trace_dma = 0;	/* first-N traces of the EXI hooks */

void RelayEXISelect(void)
{
	exi_cmd = 0;
	exi_dispatched = false;
	stage_len = 0;
	if (trace_select < 3)
	{
		trace_select++;
		dbgprintf("RelayEXI: slot B selected (#%u)\r\n", trace_select);
	}
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
			if (trace_imm < 6)
			{
				trace_imm++;
				dbgprintf("RelayEXI: command word %02x (#%u)\r\n", cmd, trace_imm);
			}
			return true;
		}
		if (trace_imm < 6)
		{
			trace_imm++;
			dbgprintf("RelayEXI: slot B imm write %08x len %u not ours (#%u)\r\n", data, len, trace_imm);
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
			dbgprintf("RelayEXI: bad request len %u, ignored\r\n", h->len);
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
	if (trace_dma < 6)
	{
		trace_dma++;
		dbgprintf("RelayEXI: slot B dma read ptr %08x len %u, cmd %02x (#%u)\r\n", (u32)ptr, len, exi_cmd, trace_dma);
	}
	if (exi_cmd != EXI_RELAY_POLL)
		return false;
	exi_cmd = 0;

	if (len > RELAY_EXI_BUF_SIZE)
		len = RELAY_EXI_BUF_SIZE;

	/* Build in kernel RAM, then one aligned copy into MEM1 (Starlet needs
	 * 32-bit MEM1 writes, kernel/common.h:36-42; same shape as
	 * EXIReadFontFile, kernel/EXI.c:903-907). Zero buffer unless DONE. */
	memset(poll_image, 0, len);
	/* exi_poll_hdr (protocol.yaml): state plus where this station is, so the
	 * game can print STATION n / RELAY a.b.c.d even while the relay is down.
	 * The kernel is big-endian like the wire, so the struct is written as is. */
	if (len >= sizeof(struct exi_poll_hdr))
	{
		struct exi_poll_hdr *ph = (struct exi_poll_hdr *)poll_image;
		ph->state = (u8)relay_state;
		/* What this host already knows is wrong (exi_poll_flags), so the
		 * kiosk can say "no network" instead of waiting for a beacon. The
		 * beamer transport never uses the Wii's network: only PF_NO_BEAMER. */
		ph->flags = (cfg.beamer ? (beamer_up ? 0 : PF_NO_BEAMER)
				: NetworkStarted ? 0
				: ConfigGetConfig(NIN_CFG_NETWORK) ? PF_NET_JOINING : PF_NO_NETWORK)
			| (cfg.ok ? 0 : PF_NO_CFG)
			| (cfg.has_secret ? 0 : PF_NO_SECRET);
		ph->station = cfg.station;
		ph->relay_ip = relay_ip;		/* 0 until a beacon (or a hello with BF_RELAY) is heard */
		ph->relay_port = (u16)relay_port;
		ph->host_opts = (ConfigGetConfig(NIN_CFG_MELEE_MUSIC) ? HO_MUSIC_ON : 0)
			| (ConfigGetConfig(NIN_CFG_MELEE_STEREO) ? HO_STEREO : 0);
		ph->host_build = RELAY_HOST_BUILD;
	}
	else
		poll_image[0] = (u8)relay_state;
	if (relay_state == RELAY_DONE && len > sizeof(struct exi_poll_hdr))
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
/* Relay thread                                                              */

/* A kernel-generated failure the game should show as text: relay_hdr echoing
 * the request, relay_resp with ST_INTERNAL and msg. */
static void synthResponse(const char *msg)
{
	struct relay_hdr *h = (struct relay_hdr *)resp_buf;
	struct relay_resp *r = (struct relay_resp *)(resp_buf + sizeof(struct relay_hdr));

	memcpy(h, req_buf, sizeof(struct relay_hdr));
	h->len = sizeof(struct relay_resp);
	memset(r, 0, sizeof(struct relay_resp));
	r->status = ST_INTERNAL;
	strncpy(r->msg, msg, MSG_LEN);
	resp_len = sizeof(struct relay_hdr) + sizeof(struct relay_resp);
}

/* relay_auth with tournament.cfg's secret= at dst (decisions.md R16): ahead
 * of every request and every telemetry datagram, on either transport. */
static void putAuth(u8 *dst)
{
	struct relay_auth *auth = (struct relay_auth *)dst;
	memset(auth, 0, sizeof(*auth));
	auth->magic[0] = AUTH_MAGIC_0;
	auth->magic[1] = AUTH_MAGIC_1;
	memcpy(auth->secret, cfg.secret, SECRET_LEN);
}

/* relay_auth then the game's request at dst: the bytes either transport
 * hands to the relay. Returns their count. */
static u32 putRequest(u8 *dst)
{
	putAuth(dst);
	memcpy(dst + sizeof(struct relay_auth), req_buf, req_len);
	return sizeof(struct relay_auth) + req_len;
}

static s32 relay_fcntl(s32 sock, u32 cmd, u32 flags)
{
	STACK_ALIGN(u32, params, 3, 32);
	params[0] = (u32)sock;
	params[1] = cmd;
	params[2] = flags;
	return IOS_Ioctl(top_fd, IOCTL_SO_FCNTL, params, 12, NULL, 0);
}

/* recvfrom that also returns the sender's address, which kernel/net.c:263
 * recvfrom does not (it passes a NULL third vector, net.c:278-279). Vector
 * layout from libogc network_wii.c net_recvfrom: one input vector (socket,
 * flags) and two outputs (data, source sockaddr). `mem` and `from` must be
 * 32-byte aligned. */
static s32 recvfromAddr(s32 sock, void *mem, s32 len, struct sockaddr_in *from)
{
	STACK_ALIGN(u32, params, 2, 32);
	STACK_ALIGN(ioctlv, vec, 3, 32);

	params[0] = (u32)sock;
	params[1] = 0;
	vec[0].data = params;
	vec[0].len = 8;
	vec[1].data = mem;
	vec[1].len = len;
	vec[2].data = from;
	vec[2].len = 8;
	return IOS_Ioctlv(top_fd, IOCTLV_SO_RECVFROM, 1, 2, vec);
}

/* UDP socket on BEACON_PORT, any address, non-blocking (same FCNTL as the TCP
 * connect, relay_fcntl below). On failure beacon_sock stays -1 and
 * serviceBeacon tries again after RELAY_BEACON_SETUP_MS. */
static s32 relay_fcntl(s32 sock, u32 cmd, u32 flags);
static void beaconSetup(void)
{
	STACK_ALIGN(struct sockaddr_in, addr, 1, 32);
	s32 sock, res, flags;

	sock = socket(top_fd, AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock < 0)
	{
		dbgprintf("RelayEXI: beacon socket failed (%d)\r\n", sock);
		return;
	}
	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = BEACON_PORT;
	addr->sin_addr.s_addr = INADDR_ANY;
	res = bind(top_fd, sock, (struct sockaddr *)addr);
	if (res < 0)
	{
		dbgprintf("RelayEXI: beacon bind udp %u failed (%d)\r\n", BEACON_PORT, res);
		close(top_fd, sock);
		return;
	}
	flags = relay_fcntl(sock, RELAY_F_GETFL, 0);
	if (flags < 0)
		flags = 0;
	relay_fcntl(sock, RELAY_F_SETFL, (u32)flags | RELAY_IOS_O_NONBLOCK);
	beacon_sock = sock;
	dbgprintf("RelayEXI: listening for relay beacons on udp %u\r\n", BEACON_PORT);

	/* The request sender: a UDP socket connected to the broadcast address. */
	sock = socket(top_fd, AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock >= 0)
	{
		if (connect(top_fd, sock, (struct sockaddr *)&request_addr) < 0)
		{
			dbgprintf("RelayEXI: beacon request socket connect failed\r\n");
			close(top_fd, sock);
		}
		else
		{
			memset(&request_msg, 0, sizeof(request_msg));
			request_msg.magic[0] = RELAY_MAGIC_0;
			request_msg.magic[1] = RELAY_MAGIC_1;
			request_msg.version = RELAY_PROTO_VERSION;
			request_sock = sock;	/* tcp_port 0, event_id 0 = "please send it" */
			request_ts = read32(HW_TIMER);
		}
	}
}

/* While no beacon has arrived: ask. Nothing to do once relay_ip is known. */
static void requestBeacon(void)
{
	s32 res;
	if (request_sock < 0 || relay_ip != 0 || TimerDiffMs(request_ts) < RELAY_BEACON_REQUEST_MS)
		return;
	request_ts = read32(HW_TIMER);
	res = sendto(top_fd, request_sock, &request_msg, sizeof(request_msg), 0);
	request_count++;
	if (request_count <= 3 || (request_count % 30) == 0)
		dbgprintf("RelayEXI: beacon request #%u broadcast to udp %u (%d)\r\n", request_count, TELEMETRY_PORT, res);
}

/* Idle-thread duty: set the socket up once the network is, then every
 * RELAY_BEACON_POLL_MS read what arrived. A datagram counts only if it is
 * exactly one relay_beacon with our magic, version and a non-zero port; the
 * latest one wins, so a relay that moves (new DHCP lease) is followed. */
static void serviceBeacon(void)
{
	STACK_ALIGN(struct sockaddr_in, from, 1, 32);
	const struct relay_beacon *b = (const struct relay_beacon *)beacon_rx;
	u32 n;

	if (!NetworkStarted)
		return;
	if (TimerDiffMs(beacon_ts) < (beacon_sock < 0 ? RELAY_BEACON_SETUP_MS : RELAY_BEACON_POLL_MS))
		return;
	beacon_ts = read32(HW_TIMER);
	if (beacon_sock < 0)
	{
		beaconSetup();
		return;
	}
	requestBeacon();

	for (n = 0; n < RELAY_BEACON_DRAIN_MAX; n++)
	{
		s32 res;
		memset(from, 0, sizeof(*from));
		from->sin_len = 8;
		from->sin_family = AF_INET;
		res = recvfromAddr(beacon_sock, beacon_rx, sizeof(beacon_rx), from);
		if (res < 0)
			break;	/* -EAGAIN: nothing more queued */
		if (res != (s32)sizeof(struct relay_beacon) ||
		    b->magic[0] != RELAY_MAGIC_0 || b->magic[1] != RELAY_MAGIC_1 ||
		    b->version != RELAY_PROTO_VERSION || b->tcp_port == 0 ||
		    from->sin_addr.s_addr == 0)
			continue;
		if (from->sin_addr.s_addr != relay_ip || b->tcp_port != relay_port)
		{
			u32 ip = from->sin_addr.s_addr;
			dbgprintf("RelayEXI: relay is %u.%u.%u.%u:%u (event %u)\r\n",
				ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
				b->tcp_port, b->event_id);
			relay_port = b->tcp_port;
			relay_ip = ip;
		}
	}
}

/* The beamer's telemetry sectors: beamer_tele_hdr under a new seq, then the
 * datagram in tele_buf, all BEAMER_MB_TELE_SECTORS in one USB write. */
static bool beamerTele(u32 total)
{
	struct beamer_tele_hdr *t = (struct beamer_tele_hdr *)mb_buf;

	memset(mb_buf, 0, BEAMER_MB_TELE_SECTORS * BEAMER_SECTOR_SIZE);
	t->magic[0] = RELAY_MAGIC_0;
	t->magic[1] = 'E';
	t->seq = ++mb_tele_seq;
	t->len = (u16)total;
	memcpy(mb_buf + sizeof(*t), tele_buf, total);
	return USBStorage_WriteMounted(mb_mount, mb_lba + BEAMER_MB_TELE, BEAMER_MB_TELE_SECTORS, mb_buf);
}

/* One telemetry datagram: relay_auth + telemetry_hdr + `len` payload bytes,
 * the payload already at tele_buf + headers. A send error other than "would
 * block" closes the socket; the next tick reconnects. The beamer transport
 * writes it to the mailbox instead (beamerTele). */
static bool teleSend(u8 kind, u32 len)
{
	struct telemetry_hdr *h = (struct telemetry_hdr *)(tele_buf + sizeof(struct relay_auth));
	u32 total = sizeof(struct relay_auth) + sizeof(struct telemetry_hdr) + len;
	s32 res;

	putAuth(tele_buf);
	h->magic[0] = RELAY_MAGIC_0;
	h->magic[1] = TELEMETRY_MAGIC_1;
	h->version = RELAY_PROTO_VERSION;
	h->kind = kind;
	h->station = cfg.station;
	h->len = (u16)len;
	h->seq = tele_seq;
	h->uptime_ms = tele_uptime;
	if (cfg.beamer)
	{
		if (!beamerTele(total))
			return false;
		tele_seq++;
		return true;
	}
	res = sendto(top_fd, tele_sock, tele_buf, total, 0);
	if (res == (s32)total)
	{
		tele_seq++;
		return true;
	}
	if (res != -RELAY_SO_EAGAIN)
	{
		close(top_fd, tele_sock);
		tele_sock = -1;
	}
	return false;
}

/* Idle-thread duty, after serviceBeacon / serviceBeamer: once the relay is
 * known, a TM_STATUS at least every TELEMETRY_STATUS_MS (and at once when the
 * module state changes or the relay moves), and the unsent kernel log in
 * TM_LOG chunks. Needs tournament.cfg's secret like every request. */
/* Module integrity watch: once a second for the first minute after the module
 * is loaded, the ARM reads the module back from MEM1 and logs the first word
 * and word sum whenever they change (and once at the start). A crash whose
 * log then still shows the right sum means the PPC ran stale cache, not
 * overwritten memory. */
static u32 tmod_watch_ms = 0, tmod_watch_sum = 0, tmod_watch_n = 0;
/* Per-line sums of the last sample, to report WHICH part changed. */
#define TMOD_WATCH_LINES 4096
static u32 tmod_line_sum[TMOD_WATCH_LINES];

static void watchModule(u32 state, u32 load, u32 len)
{
	u32 i, sum = 0, first, lines, lo = 0xFFFFFFFF, hi = 0, changed = 0;
	if (state != MOD_LOADED || tmod_watch_n > 60 || tele_uptime - tmod_watch_ms < 1000)
		return;
	tmod_watch_ms = tele_uptime;
	lines = (len + 31) >> 5;
	if (lines > TMOD_WATCH_LINES)
		lines = TMOD_WATCH_LINES;
	sync_before_read((void*)P2C(load), lines << 5);
	first = read32(P2C(load));
	for (i = 0; i < lines; i++)
	{
		u32 k, ls = 0, a = P2C(load) + (i << 5);
		for (k = 0; k < 32; k += 4)
			ls += read32(a + k);
		sum += ls;
		if (tmod_watch_n > 0 && ls != tmod_line_sum[i])
		{
			changed++;
			if (i < lo) lo = i;
			if (i > hi) hi = i;
		}
		tmod_line_sum[i] = ls;
	}
	/* Only the module region is invalidated and read here. Never touch low
	 * memory or the EXI mailbox from this thread: an ARM cache invalidate
	 * racing the EXI handler's write + flush of an ack or an interrupt cause
	 * word drops it, and the PPC then spins forever in its EXI stub - the
	 * intermittent black screen at launch seen 2026-09-30 while a boot-word
	 * dump lived here. */
	if (tmod_watch_n == 0 || sum != tmod_watch_sum)
		dbgprintf("TMOD:RAM watch #%u at %u ms: first %08x, sum %08x; %u lines changed, %08x..%08x\r\n",
			tmod_watch_n, tele_uptime, first, sum, changed,
			changed ? load + (lo << 5) : 0, changed ? load + (hi << 5) + 31 : 0);
	tmod_watch_sum = sum;
	tmod_watch_n++;
}

/* Log a new crash report as soon as the module writes one (network or not),
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

/* The telemetry socket (network transport): connected to the relay's
 * TELEMETRY_PORT, set up again when the relay moves. False: not now. */
static bool teleConnect(void)
{
	STACK_ALIGN(struct sockaddr_in, addr, 1, 32);
	s32 sock, flags;

	if (!NetworkStarted)
		return false;
	if (tele_sock >= 0 && tele_ip != relay_ip)
	{
		close(top_fd, tele_sock);
		tele_sock = -1;
	}
	if (tele_sock >= 0)
		return true;
	sock = socket(top_fd, AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock < 0)
		return false;
	flags = relay_fcntl(sock, RELAY_F_GETFL, 0);
	if (flags < 0)
		flags = 0;
	relay_fcntl(sock, RELAY_F_SETFL, (u32)flags | RELAY_IOS_O_NONBLOCK);
	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = TELEMETRY_PORT;
	addr->sin_addr.s_addr = relay_ip;
	if (connect(top_fd, sock, (struct sockaddr *)addr) < 0)
	{
		close(top_fd, sock);
		return false;
	}
	tele_sock = sock;
	tele_ip = relay_ip;
	tele_status_due = true;
	dbgprintf("RelayEXI: telemetry to %u.%u.%u.%u:%u\r\n",
		relay_ip >> 24, (relay_ip >> 16) & 0xFF, (relay_ip >> 8) & 0xFF, relay_ip & 0xFF, TELEMETRY_PORT);
	return true;
}

static void serviceTelemetry(void)
{
	u8 *payload = tele_buf + sizeof(struct relay_auth) + sizeof(struct telemetry_hdr);
	u32 dt, n, i, state, len, load, patches, arena;

	dt = TimerDiffMs(tele_ts);
	if (dt < RELAY_TELEMETRY_TICK_MS)
		return;
	if (dt == UINT_MAX)
		dt = RELAY_TELEMETRY_TICK_MS;	/* HW_TIMER wrapped */
	tele_ts = read32(HW_TIMER);
	tele_uptime += dt;
	tele_status_ms += dt;

	TelemetryGetModule(&state, &len, &load, &patches, &arena);
	watchModule(state, load, len);
	watchCrash();

	if (relay_ip == 0 || !cfg.ok || !cfg.has_secret)
		return;
	if (cfg.beamer)
	{
		/* A USB write costs a few ms: one per tick, and none while a request
		 * waits for this thread. */
		if (!beamer_up || relay_state == RELAY_BUSY)
			return;
		if (tele_ip != relay_ip)
		{
			tele_ip = relay_ip;
			tele_status_due = true;
		}
	}
	else if (!teleConnect())
		return;

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
		if (cfg.beamer || tele_sock < 0)
			return;
	}

	if (crash_send_seq != crash_seen_seq)
	{
		memcpy(payload, &crash_copy, sizeof(crash_copy));
		if (teleSend(TM_CRASH, sizeof(crash_copy)))
			crash_send_seq = crash_seen_seq;
		if (cfg.beamer || tele_sock < 0)
			return;
	}

	for (i = 0; i < (cfg.beamer ? 1 : RELAY_TELEMETRY_CHUNKS); i++)
	{
		n = TelemetryPeek((char *)payload, TELEMETRY_TEXT_MAX);
		if (n == 0 || !teleSend(TM_LOG, n))
			break;
		TelemetryConsume(n);
	}
}

static u32 remainingMs(u32 start)
{
	u32 elapsed = TimerDiffMs(start);
	return elapsed >= RELAY_BUDGET_MS ? 0 : RELAY_BUDGET_MS - elapsed;
}

/* One TCP round trip: socket -> non-blocking connect -> send -> recv until the
 * relay closes -> close, all inside RELAY_BUDGET_MS. On success resp_buf holds
 * the relay's bytes and NULL is returned; otherwise a short reason. The
 * poll()-with-deadline shape is SlippiNetwork.c waitForMessage/
 * getClientMessage (lines 96-175). */
static const char *doRoundTrip(u32 start)
{
	STACK_ALIGN(struct pollsd, pfd, 1, 32);
	STACK_ALIGN(struct sockaddr_in, addr, 1, 32);
	const char *fail = NULL;
	s32 sock, res, flags;
	/* One snapshot per round trip: a beacon that moves the relay mid-request
	 * takes effect on the next request. */
	const u32 ip = relay_ip;
	const u16 port = (u16)relay_port;

	sock = socket(top_fd, AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock < 0)
	{
		dbgprintf("RelayEXI: socket() returned %d\r\n", sock);
		return "socket";
	}

	/* R9: connect() has no timeout in this kernel, so make the socket
	 * non-blocking and wait for writability with the same deadline. */
	flags = relay_fcntl(sock, RELAY_F_GETFL, 0);
	if (flags < 0)
		flags = 0;
	relay_fcntl(sock, RELAY_F_SETFL, (u32)flags | RELAY_IOS_O_NONBLOCK);

	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = port;
	addr->sin_addr.s_addr = ip;
	res = connect(top_fd, sock, (struct sockaddr *)addr);
	if (res < 0 && res != -RELAY_SO_EINPROGRESS && res != -RELAY_SO_EAGAIN && res != -RELAY_SO_EALREADY)
	{
		/* Seen on hardware 2026-09-30: two REPORT_SCOREs failed here within
		 * 25 ms while the relay was up; the IOS code tells Wi-Fi drop from
		 * socket exhaustion from a refused port. */
		dbgprintf("RelayEXI: connect() to %u.%u.%u.%u:%u returned %d (socket %d)\r\n",
			ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port, res, sock);
		fail = "connect";
	}
	else if (res < 0)
	{
		pfd[0].socket = sock;
		pfd[0].events = POLLOUT;
		pfd[0].revents = 0;
		res = poll(top_fd, pfd, 1, remainingMs(start));
		if (res <= 0 || (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) || !(pfd[0].revents & POLLOUT))
			fail = "connect timeout";
	}

	if (!fail)
	{
		/* relay_auth then the game's request, in one send (decisions.md R16). */
		u32 total = putRequest(send_buf);
		res = sendto(top_fd, sock, send_buf, total, 0);
		if (res != (s32)total)
			fail = "send";
	}

	/* The relay serves one request per connection and closes after its reply
	 * (architecture.md), so EOF ends the response. */
	while (!fail)
	{
		u32 rem = remainingMs(start);
		u32 want;
		if (rem == 0)
		{
			fail = "timeout";
			break;
		}
		pfd[0].socket = sock;
		pfd[0].events = POLLIN;
		pfd[0].revents = 0;
		res = poll(top_fd, pfd, 1, rem);
		if (res < 0)
		{
			fail = "poll";
			break;
		}
		if (res == 0 || !(pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
			continue;
		if (resp_len >= RELAY_RESP_MAX)
		{
			fail = "response too large";
			break;
		}
		want = RELAY_RESP_MAX - resp_len;
		if (want > RELAY_RX_CHUNK)
			want = RELAY_RX_CHUNK;
		res = recvfrom(top_fd, sock, rx_chunk, want, 0);
		if (res == 0)
			break;	/* clean close = end of the one response */
		if (res == -RELAY_SO_EAGAIN)
			continue;
		if (res < 0)
		{
			fail = "recv";
			break;
		}
		memcpy(resp_buf + resp_len, rx_chunk, res);
		resp_len += res;
	}

	close(top_fd, sock);
	return fail;
}

/* ------------------------------------------------------------------------- */
/* Beamer transport (protocol.yaml beamer_hello / beamer_req_hdr / ...)      */

/* Little-endian u32, as the MBR stores it (the kernel is big-endian). */
static u32 le32(const u8 *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

/* The mailbox of the drive mounted now, derived once per mount (and again
 * after a USB error): the first MBR partition entry of type 0x0B or 0x0C
 * (FAT32), end = start + size, the way the beamer firmware places it. Returns
 * the mount id, 0 when there is no drive or it cannot be a beamer. */
static u32 beamerMailbox(void)
{
	u32 mount, size, i;

	mount = USBStorage_Mount(&size);
	if (mount == 0)
		return 0;
	if (mount == mb_mount)
		return mb_lba ? mount : 0;
	mb_lba = 0;
	if (size != BEAMER_SECTOR_SIZE)
	{
		dbgprintf("RelayEXI: usb drive has %u-byte sectors, not a beamer\r\n", size);
		mb_mount = mount;
		return 0;
	}
	if (!USBStorage_ReadMounted(mount, 0, 1, mb_buf))
		return 0;	/* read the MBR again next time */
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
	return mb_lba ? mount : 0;
}

/* Idle-thread duty (beamer transport), every RELAY_BEAMER_HELLO_MS: read the
 * hello sector. A valid one (magic, BEAMER_MB_VERSION) makes the mailbox
 * writable and gives the relay address for exi_poll_hdr (0 until the beamer
 * has heard the beacon, BF_RELAY); anything else - no drive, no FAT32
 * partition, a USB error, an ordinary stick - is PF_NO_BEAMER.
 * Finding the beamer (the first valid hello, and again on a new USB mount)
 * also reads the response sector: the beamer outlives a Wii reboot and keeps
 * its last response, so the next request seq starts one past that one
 * (protocol.yaml beamer_req_hdr), or at 1 when the sector holds none. */
static void serviceBeamer(void)
{
	const struct beamer_hello *h = (const struct beamer_hello *)mb_buf;
	const struct beamer_resp_hdr *r = (const struct beamer_resp_hdr *)(mb_buf + BEAMER_SECTOR_SIZE);
	u32 mount, ip, port;
	bool found;

	if (!beamer_usb || TimerDiffMs(mb_ts) < RELAY_BEAMER_HELLO_MS)
		return;
	mb_ts = read32(HW_TIMER);
	mount = beamerMailbox();
	found = !beamer_up || mount != mb_seq_mount;
	if (mount == 0 || !USBStorage_ReadMounted(mount, mb_lba + BEAMER_MB_HELLO, 1, mb_buf) ||
	    memcmp(h->magic, "LAZYTOMB", sizeof(h->magic)) != 0 || h->version != BEAMER_MB_VERSION ||
	    (found && !USBStorage_ReadMounted(mount, mb_lba + BEAMER_MB_RESP, 1, mb_buf + BEAMER_SECTOR_SIZE)))
	{
		if (beamer_up)
			dbgprintf("RelayEXI: beamer lost\r\n");
		else if (mount != 0 && mb_hello_logged != mount)
			dbgprintf("RelayEXI: no beamer hello at sector %u\r\n", mb_lba + BEAMER_MB_HELLO);
		if (mount != 0)
			mb_hello_logged = mount;
		beamer_up = 0;
		relay_ip = 0;
		relay_port = 0;
		return;
	}
	if (!beamer_up || h->fw_build != mb_fw)
		dbgprintf("RelayEXI: beamer fw %u (station %u) on usb, flags %u\r\n", h->fw_build, h->station, h->flags);
	mb_fw = h->fw_build;
	ip = (h->flags & BF_RELAY) ? h->relay_ip : 0;
	port = (h->flags & BF_RELAY) ? h->relay_port : 0;
	if (ip != relay_ip || port != relay_port)
		dbgprintf("RelayEXI: relay is %u.%u.%u.%u:%u (from the beamer)\r\n",
			ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port);
	relay_port = port;
	relay_ip = ip;
	if (found)
	{
		mb_seq = (r->magic[0] == RELAY_MAGIC_0 && r->magic[1] == 'R') ? r->seq : 0;
		mb_seq_mount = mount;
		dbgprintf("RelayEXI: beamer found, next request seq %u\r\n", mb_seq + 1);
	}
	beamer_up = 1;
}

/* One mailbox round trip, doRoundTrip's contract over USB: relay_auth + the
 * request into the request sector under a new seq (the beamer sends each seq
 * to the relay once), then the first response sector every
 * RELAY_BEAMER_POLL_MS until it carries that seq, inside RELAY_BUDGET_MS.
 * BR_NO_RELAY / BR_NO_WIFI become text for the game like the kernel's own;
 * every other result and any USB error is a transport failure. */
static const char *beamerRoundTrip(u32 start)
{
	struct beamer_req_hdr *q = (struct beamer_req_hdr *)mb_buf;
	const struct beamer_resp_hdr *r = (const struct beamer_resp_hdr *)mb_buf;
	u32 seq, len, n;

	memset(mb_buf, 0, BEAMER_SECTOR_SIZE);
	q->magic[0] = RELAY_MAGIC_0;
	q->magic[1] = 'Q';
	q->seq = seq = ++mb_seq;
	q->len = (u16)putRequest(mb_buf + sizeof(*q));
	if (!USBStorage_WriteMounted(mb_mount, mb_lba + BEAMER_MB_REQ, 1, mb_buf))
		return "usb write";

	while (1)
	{
		if (remainingMs(start) == 0)
			return "timeout";
		mdelay(RELAY_BEAMER_POLL_MS);
		if (!USBStorage_ReadMounted(mb_mount, mb_lba + BEAMER_MB_RESP, 1, mb_buf))
			return "usb read";
		if (r->magic[0] == RELAY_MAGIC_0 && r->magic[1] == 'R' && r->seq == seq)
			break;
	}

	switch (r->result)
	{
		case BR_OK:
			break;
		case BR_NO_RELAY:
			synthResponse("no relay found yet");
			return NULL;
		case BR_NO_WIFI:
			synthResponse("beamer not on wi-fi");
			return NULL;
		case BR_CONNECT:
			return "beamer: connect";
		case BR_TIMEOUT:
			return "beamer: relay timeout";
		case BR_TOO_LARGE:
			return "beamer: response too large";
		case BR_BAD_REQ:
			return "beamer: bad request";
		default:
			dbgprintf("RelayEXI: beamer result %u\r\n", r->result);
			return "beamer: unknown result";
	}

	/* The reply may run on into the other response sectors. */
	len = r->len;
	if (len > RELAY_RESP_MAX)
		return "beamer: response too large";
	n = (sizeof(struct beamer_resp_hdr) + len + BEAMER_SECTOR_SIZE - 1) / BEAMER_SECTOR_SIZE;
	if (n > 1 && !USBStorage_ReadMounted(mb_mount, mb_lba + BEAMER_MB_RESP + 1, n - 1, mb_buf + BEAMER_SECTOR_SIZE))
		return "usb read";
	memcpy(resp_buf, mb_buf + sizeof(struct beamer_resp_hdr), len);
	resp_len = len;
	return NULL;
}

static u32 RelayEXIThread(void *arg)
{
	while (1)
	{
		struct relay_hdr *h;
		const struct relay_resp *r;
		const char *fail;
		u32 start, result;

		if (relay_state != RELAY_BUSY)
		{
			if (cfg.beamer)
				serviceBeamer();
			else
				serviceBeacon();
			serviceTelemetry();
			mdelay(RELAY_THREAD_CYCLE_MS);
			continue;
		}

		start = read32(HW_TIMER);
		h = (struct relay_hdr *)req_buf;
		resp_len = 0;

		/* Station/stream stamping (design 5.3, 6.2 item 2): the game sends 0. */
		h->station = cfg.station;
		if (h->cmd == CMD_START_SET && req_len >= sizeof(struct relay_hdr) + sizeof(struct start_set_req))
			((struct start_set_req *)(req_buf + sizeof(struct relay_hdr)))->stream = cfg.stream;

		if (!cfg.ok)
		{
			synthResponse("no tournament.cfg");
			fail = NULL;
		}
		else if (cfg.beamer && !beamer_up)
		{
			synthResponse("no beamer on usb");
			fail = NULL;
		}
		else if (!cfg.beamer && !NetworkStarted)
		{
			synthResponse("no network");
			fail = NULL;
		}
		else if (!cfg.has_secret)
		{
			synthResponse("no secret in tournament.cfg");
			fail = NULL;
		}
		else if (cfg.beamer)
			fail = beamerRoundTrip(start);	/* the beamer answers BR_NO_RELAY itself */
		else if (relay_ip == 0)
		{
			synthResponse("no relay found yet");
			fail = NULL;
		}
		else
			fail = doRoundTrip(start);

		if (!fail && resp_len < sizeof(struct relay_hdr) + sizeof(struct relay_resp))
			fail = "short response";
		if (fail)
		{
			dbgprintf("RelayEXI: cmd %u len %u -> ERROR (%s) after %u ms\r\n",
				h->cmd, h->len, fail, TimerDiffMs(start));
			resp_len = 0;
			result = RELAY_ERROR;
		}
		else
		{
			r = (const struct relay_resp *)(resp_buf + sizeof(struct relay_hdr));
			dbgprintf("RelayEXI: cmd %u len %u -> DONE status %u resp %u bytes in %u ms\r\n",
				h->cmd, h->len, r->status, resp_len, TimerDiffMs(start));
			result = RELAY_DONE;
		}
		relay_state = result;	/* publishes resp_buf/resp_len to the poll path */
	}
	return 0;
}

void RelayEXIInit(void)
{
	loadCfg();
	/* The beamer is the Slippi replay drive: kernel/main.c:197-203 starts USB
	 * in hotswap mode only with replays on and the game on SD. Booting from
	 * USB, the main loop and the DI thread read the drive without the USB lock
	 * (usbstorage.c), so the relay thread must not touch it then. */
	beamer_usb = ConfigGetConfig(NIN_CFG_SLIPPI_REPLAYS) && !ConfigGetUseUSB();
	if (cfg.beamer && !beamer_usb)
		dbgprintf("RelayEXI: transport=beamer needs Slippi replays on and the game on SD; USB is not the replay drive, no beamer\r\n");
	relay_state = RELAY_IDLE;
	tele_ts = read32(HW_TIMER);
	mb_ts = tele_ts;
	{
		/* Hand the module its crash mailbox: zero, then the magic. */
		volatile struct crash_mailbox *mb = (volatile struct crash_mailbox *)CRASH_MAILBOX_ARM;
		memset((void *)mb, 0, sizeof(*mb));
		mb->magic = CRASH_MAGIC;
	}

	RelayEXI_Thread = do_thread_create(
		RelayEXIThread,
		((u32 *)&__relay_exi_stack_addr),
		((u32)(&__relay_exi_stack_size)),
		0x78);
	thread_continue(RelayEXI_Thread);
	dbgprintf("RelayEXI: thread started\r\n");
}
