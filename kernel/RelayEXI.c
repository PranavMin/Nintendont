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
/* Shown top-right on the kiosk's set list next to the module's own version
 * (exi_poll_hdr.host_build). Bump by hand when a loader release changes
 * behaviour the TO should be able to tell apart on the TV. */
#define RELAY_HOST_BUILD	3	/* 2: network init off the boot path, PF_NET_JOINING; 3: EINPROGRESS 26 and IOS poll bits (connect to a relay on another host) */
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
 * errmap): EAGAIN 6, EALREADY 7, EINPROGRESS 26 (NOT 27, which is EINTR:
 * the table is alphabetical from E2BIG = 1; Dolphin IPC_HLE/WII_Socket.h
 * has the same enum). With 27 every connect() that did not complete
 * synchronously was treated as a failure - against a relay on the same
 * PC that was rare, against the Pi it was every time (NO LINK TO THE
 * RELAY, 2026-10-01). */
#define RELAY_SO_EAGAIN		6
#define RELAY_SO_EALREADY	7
#define RELAY_SO_EINPROGRESS	26
#define RELAY_CONNECT_SLICE_MS	50	/* poll slice while a connect is in progress (see doRoundTrip) */

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
	char	secret[SECRET_LEN];	/* NUL-padded, as relay_auth carries it */
};
static struct RelayCfg cfg;
static char cfg_text[RELAY_CFG_MAX] ALIGNED(32);

/* The relay as the latest valid relay_beacon announced it; 0 = none heard
 * yet. Written only by the relay thread (serviceBeacon); read by the thread
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

/* key=value lines, keys station / stream (design 4.3), both required, unknown
 * keys ignored, blank lines ignored. relay_ip / relay_port from cards written
 * before relay discovery (decisions.md R15) are unknown keys now: ignored, the
 * relay's address comes from its beacon. */
static bool parseCfg(char *text)
{
	bool have_station = false, have_stream = false;
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
		}
		line = next;
	}
	return have_station && have_stream;
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
		dbgprintf("RelayEXI: station %u stream %u secret %s (relay address from its beacon, udp %u)\r\n",
			cfg.station, cfg.stream, cfg.has_secret ? "set" : "MISSING", BEACON_PORT);
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
		 * kiosk can say "no network" instead of waiting for a beacon. */
		ph->flags = (NetworkStarted ? 0
				: ConfigGetConfig(NIN_CFG_NETWORK) ? PF_NET_JOINING : PF_NO_NETWORK)
			| (cfg.ok ? 0 : PF_NO_CFG)
			| (cfg.has_secret ? 0 : PF_NO_SECRET);
		ph->station = cfg.station;
		ph->relay_ip = relay_ip;		/* 0 until a beacon is heard */
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

/* One telemetry datagram: relay_auth + telemetry_hdr + `len` payload bytes,
 * the payload already at tele_buf + headers. A send error other than "would
 * block" closes the socket; the next tick reconnects. */
static bool teleSend(u8 kind, u32 len)
{
	struct relay_auth *auth = (struct relay_auth *)tele_buf;
	struct telemetry_hdr *h = (struct telemetry_hdr *)(tele_buf + sizeof(struct relay_auth));
	u32 total = sizeof(struct relay_auth) + sizeof(struct telemetry_hdr) + len;
	s32 res;

	memset(auth, 0, sizeof(*auth));
	auth->magic[0] = AUTH_MAGIC_0;
	auth->magic[1] = AUTH_MAGIC_1;
	memcpy(auth->secret, cfg.secret, SECRET_LEN);
	h->magic[0] = RELAY_MAGIC_0;
	h->magic[1] = TELEMETRY_MAGIC_1;
	h->version = RELAY_PROTO_VERSION;
	h->kind = kind;
	h->station = cfg.station;
	h->len = (u16)len;
	h->seq = tele_seq;
	h->uptime_ms = tele_uptime;
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

/* Idle-thread duty, after serviceBeacon: once the relay is known, a
 * TM_STATUS at least every TELEMETRY_STATUS_MS (and at once when the module
 * state changes or the relay moves), and the unsent kernel log in TM_LOG
 * chunks. Needs tournament.cfg's secret like every request. */
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

static void serviceTelemetry(void)
{
	STACK_ALIGN(struct sockaddr_in, addr, 1, 32);
	u8 *payload = tele_buf + sizeof(struct relay_auth) + sizeof(struct telemetry_hdr);
	u32 dt, n, i, state, len, load, patches, arena;
	s32 sock, flags;

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

	if (!NetworkStarted || relay_ip == 0 || !cfg.ok || !cfg.has_secret)
		return;
	if (tele_sock >= 0 && tele_ip != relay_ip)
	{
		close(top_fd, tele_sock);
		tele_sock = -1;
	}
	if (tele_sock < 0)
	{
		sock = socket(top_fd, AF_INET, SOCK_DGRAM, IPPROTO_IP);
		if (sock < 0)
			return;
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
			return;
		}
		tele_sock = sock;
		tele_ip = relay_ip;
		tele_status_due = true;
		dbgprintf("RelayEXI: telemetry to %u.%u.%u.%u:%u\r\n",
			relay_ip >> 24, (relay_ip >> 16) & 0xFF, (relay_ip >> 8) & 0xFF, relay_ip & 0xFF, TELEMETRY_PORT);
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
		if (tele_sock < 0)
			return;
	}

	if (crash_send_seq != crash_seen_seq)
	{
		memcpy(payload, &crash_copy, sizeof(crash_copy));
		if (teleSend(TM_CRASH, sizeof(crash_copy)))
			crash_send_seq = crash_seen_seq;
		if (tele_sock < 0)
			return;
	}

	for (i = 0; i < RELAY_TELEMETRY_CHUNKS; i++)
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
	u32 polls = 0;	/* reply polls traced (first few) */
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
		/* IOS's poll does not wake when a connecting socket becomes writable:
		 * on hardware (2026-10-01) poll(POLLOUT, 3200 ms) slept the whole
		 * 3200 ms and only then reported revents 0008, the connect having
		 * completed long before, so the reply read ran out of budget. Poll in
		 * short slices instead; completion is seen within one slice. */
		u32 rem;
		fail = "connect timeout";
		while ((rem = remainingMs(start)) > 0)
		{
			pfd[0].socket = sock;
			pfd[0].events = POLLOUT;
			pfd[0].revents = 0;
			res = poll(top_fd, pfd, 1, rem < RELAY_CONNECT_SLICE_MS ? rem : RELAY_CONNECT_SLICE_MS);
			if (res < 0 || (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
				break;
			if (res > 0 && (pfd[0].revents & POLLOUT))
			{
				fail = NULL;
				break;
			}
		}
		dbgprintf("RelayEXI: connect in progress; %s after %u ms (poll %d revents %04x)\r\n",
			fail ? "gave up" : "connected", TimerDiffMs(start), res, (u32)pfd[0].revents);
	}
	else
		dbgprintf("RelayEXI: connect() completed synchronously\r\n");

	if (!fail)
	{
		/* relay_auth then the game's request, in one send (decisions.md R16). */
		struct relay_auth *auth = (struct relay_auth *)send_buf;
		u32 total = sizeof(struct relay_auth) + req_len;
		memset(auth, 0, sizeof(*auth));
		auth->magic[0] = AUTH_MAGIC_0;
		auth->magic[1] = AUTH_MAGIC_1;
		memcpy(auth->secret, cfg.secret, SECRET_LEN);
		memcpy(send_buf + sizeof(struct relay_auth), req_buf, req_len);
		res = sendto(top_fd, sock, send_buf, total, 0);
		dbgprintf("RelayEXI: sendto %u bytes -> %d after %u ms\r\n", total, res, TimerDiffMs(start));
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
		if (polls < 4)
			dbgprintf("RelayEXI: reply poll(POLLIN, %u ms) -> %d revents %04x after %u ms\r\n",
				rem, res, (u32)pfd[0].revents, TimerDiffMs(start));
		polls++;
		if (res < 0)
		{
			fail = "poll";
			break;
		}
		if (res == 0 || !(pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
		{
			/* A poll that returns at once with bits we do not read would spin
			 * until the deadline; a short sleep keeps the log readable. */
			if (res > 0)
				mdelay(10);
			continue;
		}
		if (resp_len >= RELAY_RESP_MAX)
		{
			fail = "response too large";
			break;
		}
		want = RELAY_RESP_MAX - resp_len;
		if (want > RELAY_RX_CHUNK)
			want = RELAY_RX_CHUNK;
		res = recvfrom(top_fd, sock, rx_chunk, want, 0);
		if (polls <= 4)
			dbgprintf("RelayEXI: recvfrom(%u) -> %d after %u ms\r\n", want, res, TimerDiffMs(start));
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

	if (!fail && resp_len < sizeof(struct relay_hdr) + sizeof(struct relay_resp))
		fail = "short response";
	return fail;
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
		else if (!NetworkStarted)
		{
			synthResponse("no network");
			fail = NULL;
		}
		else if (!cfg.has_secret)
		{
			synthResponse("no secret in tournament.cfg");
			fail = NULL;
		}
		else if (relay_ip == 0)
		{
			synthResponse("no relay found yet");
			fail = NULL;
		}
		else
			fail = doRoundTrip(start);

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
	relay_state = RELAY_IDLE;
	tele_ts = read32(HW_TIMER);
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
