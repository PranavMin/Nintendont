> Superseded: session-3 investigation on the retired branch `reporter`; see docs/relay-exi-report.md.

# RelayEXI investigation (design.md §6.2, R3)

Read-only investigation of Slippi Nintendont (branch `reporter`) answering the five questions
from `../tournament-reporter/docs/sessions.md` session 3. All citations are `file:line` in this
repo unless noted.

Bottom line for R3: **the EXI handler cannot block.** It runs on the kernel's single main loop,
which also services pads, disc, and audio, and the PPC side (the game) is frozen inside the
patched EXI transfer until the handler acks. The safe shape is the one Slippi itself uses: the
EXI handler only copies a buffer and flips a state flag; a dedicated kernel thread does all
socket work. That is still one code path (§3 below).

---

## 1. Where EXI writes from the game are handled, and in what context

There is no interrupt handler. Nintendont emulates the EXI device by **polling a mailbox in
shared memory from the kernel's main loop**:

- The fake EXI register block lives at `0x13026800` (`EXI_BASE`, kernel/EXI.h:36) with the
  command word at `EXI_CMD_0` and the data/pointer word at `EXI_CMD_1` (kernel/EXI.h:38–39).
  The game's patched EXI functions write command words here (PPC address `0x93026800`) and wait.
- `EXIUpdateRegistersNEW()` (kernel/EXI.c:695) reads `EXI_CMD_0` (kernel/EXI.c:703–704) and
  dispatches on the top byte: `0x10` EXISelect, `0x11` EXIImm, `0x12` EXIDMA
  (kernel/EXI.c:710, 754, 793).
- The Slippi write path is the EXIDMA case for the memcard slot: when `slippi_use_port_a` and
  `mode == 1` (write), the DMA'd bytes are copied into the SlipMem ring buffer via
  `SlippiMemoryWrite(ptr, len)` (kernel/EXI.c:805–809; port-B variant at 841–845). The handler
  then acks by clearing `EXI_CMD_0` and scheduling a fake EXI IRQ back to the PPC
  (kernel/EXI.c:822–829: `IRQ_Cause[0] = 10`, `write32(EXI_CMD_0, 0)`, `EXI_IRQ = true`).
- `EXIUpdateRegistersNEW()` is called from the **main kernel loop** in `kernel/main.c:583`,
  inside the `while (1)` at kernel/main.c:476. It is the kernel *main thread*, not an ISR and
  not a dedicated thread.
- The fake EXI IRQ is delivered on a later iteration of the same loop: `EXI_IRQ` is checked at
  kernel/main.c:490–494 and fired by `EXIInterrupt()` (kernel/EXI.c:187) after `CurrentTiming`
  ticks — default 1900 ticks ≈ 1 ms (`EXI_IRQ_DEFAULT`, kernel/EXI.c:38).

Everything else the console does is serviced by this same loop iteration: disc
(`DIUpdateRegisters`, kernel/main.c:581), controller pads ~240 Hz (`SIInterrupt`,
kernel/main.c:496–504), audio streaming (`StreamUpdateRegisters`, kernel/main.c:601), BT/HID
(kernel/main.c:585–586), and the replay file writer (kernel/main.c:588–596). The loop's only
pause is a `udelay(20)` (kernel/main.c:536).

Note for `EXI_RELAY_POLL`: the current EXIDMA path for the Slippi port never copies data *to*
the PPC — a read (mode 0) is just acked (kernel/EXI.c:820–836). The read-back pattern to reuse
is the memcard/SRAM one: write into the game's DMA pointer, then `sync_after_write(Data, Length)`
(e.g. kernel/EXI.c:499–517, and `GCNCard_Read` at kernel/EXI.c:395).

## 2. How the broadcast feature opens its socket and sends data

**Network API:** thin wrappers in `kernel/net.c` around ioctls to the IOS socket driver
`/dev/net/ip/top`. The single shared descriptor `top_fd` (kernel/net.c:14) is opened once in
`NCDInit()` (kernel/net.c:66–70) and shared by every Slippi thread (comment at
kernel/net.c:13, 62–64). Wrappers: `socket()` net.c:117, `sendto()` net.c:221 (an
`IOS_Ioctlv(IOCTLV_SO_SENDTO)` that first copies the payload into a 32-byte-aligned buffer from
IOS heap 0, net.c:234–252), `recvfrom()` net.c:263, `connect()` net.c:291, `setsockopt()`
net.c:314, `poll()` net.c:337. Every one of these **blocks the calling kernel thread** inside an
IOS ioctl until the IOS network module answers.

**Which thread:** its own. `SlippiNetworkBroadcastInit()` spawns a dedicated thread at priority
0x78 with a 0x400-byte stack (kernel/SlippiNetworkBroadcast.c:52–61; stack carved out in
kernel/kernel.ld:33–34). The thread body (kernel/SlippiNetworkBroadcast.c:110–123) loops with
`mdelay(5000)`.

**Socket lifecycle:** `startBroadcast()` (kernel/SlippiNetworkBroadcast.c:67–88) creates a UDP
socket — `socket(top_fd, AF_INET, SOCK_DGRAM, IPPROTO_IP)` at line 79 — and `connect()`s it to
the broadcast address `255.255.255.255:20582` (the `discover` sockaddr, lines 41–47). After
that, `do_broadcast()` (lines 94–104) just calls `sendto(top_fd, discover_sock, &ready_msg, …)`
every 10 s.

The bigger sibling is the replay server in `kernel/SlippiNetwork.c`: same pattern, own thread
with a 0x2000 stack (SlippiNetwork.c:79–84, kernel.ld:27–28), a **TCP** server socket on port
51441 (`socket`/`bind`/`listen`, SlippiNetwork.c:296–328), blocking `accept()`
(SlippiNetwork.c:443), and `sendto`/`recvfrom` with `poll()`-based timeouts. All Slippi network
threads are spawned at boot, before the game starts, in kernel/main.c:313–325.

## 3. Is a blocking connect/send/recv with 3 s timeout safe from the EXI handler?

**No.** Two independent reasons:

1. **It stalls the whole console.** The EXI dispatch runs on the main kernel loop (§1). Blocking
   there for up to 3 s means: no pad servicing (kernel/main.c:496–504), no disc reads
   (kernel/main.c:581), no audio streaming (kernel/main.c:601), and no fake-IRQ delivery
   (kernel/main.c:490–494) for the duration. Melee polls pads every frame; a 3 s stall is a
   visibly frozen console — exactly what design.md §4.6 forbids.
2. **The game itself is frozen inside the transfer.** The PPC-side patched EXI code is waiting
   for the mailbox ack / fake EXI IRQ that only this same loop produces (kernel/EXI.c:822–829,
   kernel/main.c:490–494). The "poll once per frame, never block" plan in design.md §6.1 only
   works if every EXI transaction is acked in microseconds, as the Slippi write path does today.

Note also that even on a dedicated thread the socket wrappers are *thread*-blocking IOS ioctls
(§2); Slippi's own measurement is ~10 ms per `sendto` (comment at kernel/SlippiNetwork.c:18–24).
That's fine on a private thread, fatal on the main loop.

### Minimal safe shape — one code path

Mirror `SlippiNetwork.c` exactly; this is the structure §6.2 already sketches
(`IDLE|BUSY|DONE|ERROR`):

- **Boot:** `RelayEXIInit()` spawns one thread (`do_thread_create`, global.h:362–366) with its
  own stack added to kernel/kernel.ld next to `__slippi_network_stack_*` (kernel.ld:27–28);
  0x2000 like SlippiNetwork's is the proven size. Init is called from the NETWORK_INIT stage in
  kernel/main.c:313–325, after `NCDInit()`.
- **`EXI_RELAY_REQ` (in the EXIDMA dispatch, next to kernel/EXI.c:805):** `sync_before_read` the
  DMA'd request, memcpy into a static request buffer, set `state = BUSY`, ack the EXI transfer
  immediately (same as lines 822–829). Nothing else. Reject if state is already BUSY.
- **Relay thread loop:** sleep while IDLE (`mdelay`, like SlippiNetworkBroadcast.c:120); on BUSY
  do the single sequence `socket → connect → sendto → poll/recvfrom loop → close`, store the
  response and `state = DONE` or `ERROR` + status code. The receive timeout uses exactly the
  existing pattern: `poll()` with a timeout (`waitForMessage`, SlippiNetwork.c:96–113) inside a
  `TimerDiffMs` budget loop (`getClientMessage`, SlippiNetwork.c:122–175), with the 3 s budget
  covering send+recv.
- **`EXI_RELAY_POLL`:** copy `{state, status, response}` into the game's DMA read pointer and
  `sync_after_write` (read-back pattern, §1). Never blocks.

This is still "one path": there is exactly one request buffer, one thread, one
socket-per-request sequence, no retry, no queue. The state machine is four states and lives in
one file.

**One caveat to log during session 8:** `connect()` (net.c:291–309) takes no timeout and there
is no non-blocking wrapper in net.c (IOS does expose `IOCTL_SO_FCNTL`, net.h:105, but nothing in
this codebase uses it — Slippi never makes an outbound TCP connect; its only `connect()` is the
UDP broadcast one, which completes immediately). If the relay host is unreachable, the thread
may sit in `connect()` for however long IOS's internal TCP timeout is; the game is unaffected
(it just keeps seeing BUSY and shows its own timeout error), but the station can't issue a new
request until IOS gives up. Measure this on real hardware; if IOS's timeout is unacceptable, the
fix is a small `fcntl(O_NONBLOCK)` wrapper + `poll(POLLOUT)` — still one path, just a bounded
connect.

## 4. Can a second TCP socket coexist with the broadcast socket? Memory limits?

**Yes — coexistence is already the status quo.** With networking enabled the kernel runs three
sockets concurrently on the one shared `top_fd`: the TCP server socket (SlippiNetwork.c:296),
the accepted client socket (SlippiNetwork.c:443), and the UDP broadcast socket
(SlippiNetworkBroadcast.c:79), used from two different threads at once. Sockets are IOS file
descriptors managed by the IOS network module, not kernel memory; nothing in this codebase caps
their count. A fourth, short-lived TCP client socket for the relay follows the identical pattern.

**Memory, concretely (kernel/kernel.ld:44–47):**

| Region | Size | Use |
|---|---|---|
| code @ `0x12F00000` | 0x60000 | kernel text |
| data @ `0x12F60000` | 0x80000 (512 KB) | all static buffers |
| stack @ `0x12FE0000` | 0x10000 (64 KB) | *all* thread stacks, chained in kernel.ld:6–41 |

- Existing static network buffers for scale: SlipMem ring buffer is 3 MB at its own reservation
  `0x12B80000` (SlippiMemory.c:15–16, outside the kernel data region), `readBuf` is
  `MAX_TX_SIZE` = 25000 bytes (SlippiNetwork.c:466, SlippiNetwork.h:17), `clientMsg` is 1 KB
  (SlippiNetwork.c:121). RelayEXI's two 4 KB static buffers (request + response, per
  design.md §6.1/§6.2) are noise next to these — they fit in the data region trivially.
- Thread stacks are the scarce resource: 64 KB total, currently ~0x9400 allocated
  (kernel.ld:6–41). A 0x2000 RelayEXI stack fits, but it must be added to the chain in
  kernel.ld, not malloc'd.
- Per-call transient cost: every `sendto` heap-allocates an aligned copy of the payload from IOS
  heap 0 (net.c:234–255) and frees it after; a ≤4 KB request is well inside what the 25 KB
  replay sends already exercise.

## 5. How the Slippi config / nickname is read from SD at boot

Two patterns exist; both are usable for `tournament.cfg`:

**Pattern A — loader reads the file, drops the struct at a fixed MEM2 address (the nickname
path):**
- The loader (PPC, before the game boots) reads `sd:/slippi_console.dat` (`SLIPPI_DAT_FILE`,
  common/include/Slippi.h:12) with FatFS into a fixed address: `LoadSlippiDat()`,
  loader/source/global.c:364–385 — `slippi_settings` is a raw pointer to `0x93003500`, the file
  is `f_read` straight into it, and the nickname is force-NUL-terminated (global.c:382).
- Fallback + flush: if the nickname is empty the loader copies the System Menu console name in
  (loader/source/main.c:432–472) and flushes the struct to RAM for the ARM side
  (`DCStoreRange`, loader/source/main.c:1052).
- The kernel then reads it with zero I/O through a static pointer to the same physical memory:
  `slippi_settings` at `0x13003500` and `SlippiGetConsoleNick()` (kernel/Config.h:55–56),
  consumed at e.g. SlippiNetworkBroadcast.c:71–77.

**Pattern B — kernel reads the file itself at boot (the nincfg path):**
- `ConfigInit()` (kernel/Config.c:13–45), called from kernel/main.c:333, opens
  `/slippi_nincfg.bin` on the main drive with the kernel's own FatFS (`f_open_main_drive`,
  ff_utf8), reads it into the config struct, and **fails fast with `Shutdown()`** if the file is
  missing or short (Config.c:23–37).

**Recommendation for `tournament.cfg`: Pattern B.** §6.2 says "parse `tournament.cfg` at boot"
in the kernel, and Pattern B keeps the whole feature in `kernel/RelayEXI.c` — no loader changes,
no new fixed-address contract, and the kernel already has FatFS up at that boot stage
(ConfigInit runs at kernel/main.c:333, after storage init and after the network threads spawn at
313–325; RelayEXI can read the file in its own init). One deviation from Pattern B's behavior:
per §6.2 a missing/malformed file must *not* `Shutdown()` — set a `no config` flag and have
every relay command answer `ST_INTERNAL` / "no tournament.cfg" instead.

---

## Answer to the R3 question for design.md

§6.2 should read "kernel-side state machine", not "blocking receive from the EXI handler". The
EXI handler context is the shared main loop and must stay microsecond-cheap; the blocking
3 s-budget TCP transaction moves onto one dedicated RelayEXI thread using the existing
`poll()`-with-deadline pattern from SlippiNetwork.c. This changes nothing else in the design:
the game still polls once per frame, there is still exactly one request in flight, one code
path, no retries. The only open hardware question is IOS's `connect()` timeout to an unreachable
host (§3 caveat) — measure in session 8.
