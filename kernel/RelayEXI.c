/* kernel/RelayEXI.c
 * Tournament relay EXI device (../tournament-reporter/docs/design.md section
 * 6.2, docs/relay-exi-investigation.md).
 *
 * The game (melee lbrelayexi.c) selects channel 1 / device 0 (slot B, shared
 * with Slippi's own device), writes a 4-byte immediate command word
 * (EXI_RELAY_REQ or EXI_RELAY_POLL in the top byte, relay_proto.h), then
 *   REQ:  relay_hdr + payload through EXIImmEx, i.e. EXIImm writes of <= 4
 *         bytes each (melee OSExi.c EXIImmEx, lines 168-184); the patched
 *         EXIImm stub (kernel/asm/EXIImm.S:36-41) stores the bytes as an
 *         immediate word, first byte in the top bits;
 *   POLL: one 4096-byte EXIDma read (lbrelayexi.c:110) that must come back
 *         as lbRelayExi_PollBuf: {state u8, pad[3], relay_hdr, relay_resp,
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

/* Game-side contract, melee/src/melee/lb/lbrelayexi.h. */
#define RELAY_EXI_BUF_SIZE	4096	/* LB_RELAY_EXI_BUF_SIZE */
#define RELAY_EXI_MAX_PAYLOAD	28	/* LB_RELAY_EXI_MAX_PAYLOAD */
#define RELAY_REQ_MAX		(sizeof(struct relay_hdr) + RELAY_EXI_MAX_PAYLOAD)
#define RELAY_RESP_MAX		(RELAY_EXI_BUF_SIZE - 4)	/* after the state word */

#define RELAY_BUDGET_MS		3000	/* design 4.6: one attempt, 3 s */
#define RELAY_THREAD_CYCLE_MS	1	/* like SlippiNetwork.c THREAD_CYCLE_TIME_MS */
#define RELAY_CFG_PATH		"sd:/tournament.cfg"	/* design 4.3 */
#define RELAY_CFG_MAX		512
#define RELAY_RX_CHUNK		1024

/* IOCTL_SO_FCNTL (net.h:105) usage copied from libogc network_wii.c
 * net_fcntl(): params = {socket, cmd, flags}, ioctl input length 12, no
 * output. cmd is the POSIX F_GETFL (3) / F_SETFL (4) passed straight through;
 * IOS_O_NONBLOCK is libogc's "(O_NONBLOCK >> 16)" = 0x04. Nothing else in
 * this kernel uses FCNTL (investigation section 3 caveat, design R9). */
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
	u32	ip;		/* relay_ip, host order (kernel is big-endian, same as wire) */
	u16	port;		/* relay_port */
	u16	station;	/* station */
	u8	stream;		/* stream (0/1) */
	bool	ok;		/* false: missing or malformed -> ST_INTERNAL "no tournament.cfg" */
};
static struct RelayCfg cfg;
static char cfg_text[RELAY_CFG_MAX] ALIGNED(32);

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

/* Dotted quad -> u32. */
static bool parseIp(const char *s, u32 *out)
{
	u32 ip = 0;
	int part;
	for (part = 0; part < 4; part++)
	{
		u32 v = 0;
		u32 n = 0;
		while (*s >= '0' && *s <= '9')
		{
			v = v * 10 + (u32)(*s - '0');
			if (v > 255)
				return false;
			s++;
			n++;
		}
		if (n == 0)
			return false;
		ip = (ip << 8) | v;
		if (part < 3)
		{
			if (*s != '.')
				return false;
			s++;
		}
	}
	while (*s == ' ' || *s == '\t' || *s == '\r')
		s++;
	if (*s != 0)
		return false;
	*out = ip;
	return true;
}

/* key=value lines, keys relay_ip / relay_port / station / stream (design 4.3),
 * all four required, unknown keys ignored, blank lines ignored. */
static bool parseCfg(char *text)
{
	bool have_ip = false, have_port = false, have_station = false, have_stream = false;
	char *line = text;

	while (*line)
	{
		char *next = strchr(line, '\n');
		char *eq;
		u32 v;
		if (next)
			*next++ = 0;
		else
			next = line + strlen(line);

		eq = strchr(line, '=');
		if (eq)
		{
			const char *val = eq + 1;
			*eq = 0;
			if (strcmp(line, "relay_ip") == 0)
				have_ip = parseIp(val, &cfg.ip);
			else if (strcmp(line, "relay_port") == 0)
			{
				have_port = parseU32(val, 65535, &v) && v != 0;
				cfg.port = (u16)v;
			}
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
	return have_ip && have_port && have_station && have_stream;
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
		dbgprintf("RelayEXI: relay %u.%u.%u.%u:%u station %u stream %u\r\n",
			cfg.ip >> 24, (cfg.ip >> 16) & 0xFF, (cfg.ip >> 8) & 0xFF, cfg.ip & 0xFF,
			cfg.port, cfg.station, cfg.stream);
	else
		dbgprintf("RelayEXI: %s malformed\r\n", RELAY_CFG_PATH);
}

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
	if (exi_cmd != EXI_RELAY_POLL)
		return false;
	exi_cmd = 0;

	if (len > RELAY_EXI_BUF_SIZE)
		len = RELAY_EXI_BUF_SIZE;

	/* Build in kernel RAM, then one aligned copy into MEM1 (Starlet needs
	 * 32-bit MEM1 writes, kernel/common.h:36-42; same shape as
	 * EXIReadFontFile, kernel/EXI.c:903-907). Zero buffer unless DONE. */
	memset(poll_image, 0, len);
	poll_image[0] = (u8)relay_state;
	if (relay_state == RELAY_DONE && len > 4)
	{
		u32 n = resp_len;
		if (n > len - 4)
			n = len - 4;
		memcpy(poll_image + 4, resp_buf, n);
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

	sock = socket(top_fd, AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock < 0)
		return "socket";

	/* R9: connect() has no timeout in this kernel, so make the socket
	 * non-blocking and wait for writability with the same deadline. */
	flags = relay_fcntl(sock, RELAY_F_GETFL, 0);
	if (flags < 0)
		flags = 0;
	relay_fcntl(sock, RELAY_F_SETFL, (u32)flags | RELAY_IOS_O_NONBLOCK);

	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = cfg.port;
	addr->sin_addr.s_addr = cfg.ip;
	res = connect(top_fd, sock, (struct sockaddr *)addr);
	if (res < 0 && res != -RELAY_SO_EINPROGRESS && res != -RELAY_SO_EAGAIN && res != -RELAY_SO_EALREADY)
		fail = "connect";
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
		res = sendto(top_fd, sock, req_buf, req_len, 0);
		if (res != (s32)req_len)
			fail = "send";
	}

	/* The relay serves one request per connection and closes after its reply
	 * (design section 5), so EOF ends the response. */
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

	RelayEXI_Thread = do_thread_create(
		RelayEXIThread,
		((u32 *)&__relay_exi_stack_addr),
		((u32)(&__relay_exi_stack_size)),
		0x78);
	thread_continue(RelayEXI_Thread);
	dbgprintf("RelayEXI: thread started\r\n");
}
