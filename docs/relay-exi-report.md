# RelayEXI report (session 8)

> Written on branch `reporter`; the current branch is `LazyTO`. Section 4 is superseded
> and kept, marked, for history.

What was built on branch `reporter` for `../tournament-reporter/docs/architecture.md` section 6.2,
following `docs/history/relay-exi-investigation.md` (R3: the EXI handler must never block). All
citations are `file:line` in this repo unless noted. No hardware was available; the
on-hardware checklist is at the end.

## 1. What changed

| File | Change |
|---|---|
| `kernel/relay_proto.h` | Verbatim copy of `../tournament-reporter/generated/relay_proto.h` (GENERATED from `protocol.yaml`, never hand-edit; re-copy on protocol changes). |
| `kernel/RelayEXI.h`, `kernel/RelayEXI.c` | The relay device: config, EXI hooks, state machine, relay thread. |
| `kernel/EXI.c:34,736-745,781-790,856-864` | Wiring into the existing EXI dispatch for slot B (EXISelect / EXIImm / EXIDMA). |
| `kernel/main.c:44,328-330` | `RelayEXIInit()` right after the NETWORK_INIT stage. Since host build 2 (2026-09-30) that stage only starts `net.c NetworkInitThread`; `NCDInit()` (IOS `SO_STARTUP`, which blocks until the Wi-Fi join and DHCP finish, no timeout) runs there, the Slippi threads wait on `NetworkStarted`, and the poll header says `PF_NET_JOINING` until it flips. Before that a slow join hung the boot at "Slippi network init". |
| `kernel/kernel.ld:42-43` | `__relay_exi_stack_*`, 0x2000 bytes appended to the thread-stack chain. |
| `kernel/Makefile:42` | `RelayEXI.o` added to `OBJECTS`. |
| `kernel/Patch.c:139-154,170-177,1266-1268,1356` | Version-gate bypass for the tournament build (section 4). |
| `docs/build-windows.md` | Windows build recipe (devkitARM via MSYS2 pacman). |

## 2. Game-side contract this implements

`P:\Projects\melee\src\melee\lb\lbrelayexi.h` / `.c`:

- Channel 1, device 0, frequency 4 (slot B, shared with Slippi's own device).
- REQ: `EXILock`, `EXISelect`, 4-byte immediate word `EXI_RELAY_REQ << 24`, then
  `relay_hdr` + payload via `EXIImmEx` (lbrelayexi.c:63-77). The SDK's `EXIImmEx` is a loop of
  `EXIImm` writes of at most 4 bytes (melee `libs/dolphin/src/dolphin/os/OSExi.c:168-184`), and
  Nintendont's patched `EXIImm` stub stores a write of <= 4 bytes as the *value* in `EXI_CMD_1`,
  first byte in the top bits (`kernel/asm/EXIImm.S:36-41`). So the kernel sees the request as a
  sequence of 4-byte immediate words on the memcard-B device.
- POLL: immediate word `EXI_RELAY_POLL << 24`, then one 4096-byte `EXIDma` read
  (lbrelayexi.c:106-110). Read-back layout is `lbRelayExi_PollBuf`: `{state u8, pad[3],
  relay_hdr, relay_resp, payload}`. Anything but DONE/ERROR keeps the game polling; it times out
  after 5 s; on ERROR the buffer is zeroed.
- The driver checks the return of `EXISelect`, `EXISync` and `EXIDeselect` (lbrelayexi.c:66-76).
  Nintendont patches `EXISync`, `EXIDeselect` and `EXIUnlock` with the always-1 `EXILock` stub
  (`kernel/patches.c:307-335`, `kernel/asm/EXILock.S`), so those pass. `EXISelect` is answered by
  the kernel (`kernel/EXI.c:736-745`) and previously returned `GCNCard_IsEnabled(1)`, i.e. 0
  without a slot-B card image, which would have failed every request; see section 3.2.

## 3. RelayEXI design

### 3.1 Contexts

- **Main loop, never blocks.** `EXIUpdateRegistersNEW()` (`kernel/EXI.c:696`) runs from the
  kernel main loop (`kernel/main.c:583`) with the game frozen inside the transfer until the
  mailbox ack (investigation section 1). The three hooks it calls only copy bytes and flip a
  state word:
  - `RelayEXISelect()` (`kernel/RelayEXI.c:247`), from the EXISelect case for slot B
    (`kernel/EXI.c:743`): forgets any half-received transaction.
  - `RelayEXIImmWrite()` (`kernel/RelayEXI.c:269`), from the EXIImm case for slot B
    (`kernel/EXI.c:781`): recognises the command word, stages REQ bytes, and dispatches when
    `sizeof(relay_hdr) + hdr.len` bytes have arrived. Returns true when the write is relay
    traffic; EXI.c then acks the transfer itself (`kernel/EXI.c:787-788`, the same two writes
    `EXIDeviceMemoryCard` ends with at `kernel/EXI.c:403-404`). This matters because
    `EXIDeviceMemoryCard` returns *without* acking when the slot has no card
    (`kernel/EXI.c:232-236`), which would spin the game forever.
  - `RelayEXIDMARead()` (`kernel/RelayEXI.c:320`), from the EXIDMA case for slot B on a read
    (`kernel/EXI.c:856-864`): builds the poll image in kernel RAM and copies it in one aligned
    `memcpy` + `sync_after_write` (the `EXIReadFontFile` shape, `kernel/EXI.c:928-932`; Starlet
    needs 32-bit MEM1 writes, `kernel/common.h:36-42`). The existing ack + fake EXI IRQ for slot-B
    DMA (`kernel/EXI.c:881-889`) follows unchanged.
- **Relay thread, may block.** `RelayEXIThread()` (`kernel/RelayEXI.c:477`) is spawned by
  `RelayEXIInit()` (`kernel/RelayEXI.c:533`) with `do_thread_create` at priority 0x78 on its own
  0x2000 stack (`kernel/kernel.ld:42-43`), the `SlippiNetworkBroadcastInit` template
  (`kernel/SlippiNetworkBroadcast.c:52-61`). It sleeps 1 ms between checks of the state word
  (`kernel/SlippiNetwork.c:31` uses the same cycle).

### 3.2 EXISelect on slot B now returns 1

`kernel/EXI.c:736-745`. Without a slot-B card image the old answer was 0 and `lbRelayExi_Request`
bails on it (lbrelayexi.c:66). Melee's CARD library only ever selects slot B after `EXIProbe`
says a card is present, and the patched `EXIProbe` returns 0 for slot B unless
`NIN_CFG_MC_SLOTB` is set (`kernel/asm/EXIProbe.S:19-25`), i.e. unless a card image loaded
(`kernel/GCNCard.c:251-256`). So without a card image only the relay driver ever sees this
select; with one, the answer was already 1.

### 3.3 State machine

`relay_state` (`kernel/RelayEXI.c:98`) is the poll state byte, `enum exi_poll_state`:

```
RELAY_IDLE ---REQ complete (main loop)---> RELAY_BUSY ---thread---> RELAY_DONE  (resp_buf valid)
                                                      \--thread---> RELAY_ERROR (poll returns zeros)
DONE / ERROR stay until the next REQ. A REQ while BUSY is dropped and logged.
```

- Handoff main loop -> thread is `relay_state = RELAY_BUSY` (`kernel/RelayEXI.c:266`) after the
  staged bytes were copied into the single request buffer; the thread publishes the result with
  `relay_state = result` after `resp_buf`/`resp_len` are final (`kernel/RelayEXI.c:528`). One
  in-order core, one variable, no other synchronisation needed.
- The poll image copies `resp_buf` only when DONE (`kernel/RelayEXI.c:333-339`); BUSY/IDLE/ERROR
  give a zeroed buffer after the state byte, matching the contract and the Dolphin forwarder
  (`Ishiiruka Source/Core/Core/HW/EXI_DeviceSlippi.cpp:3788-3808`).

### 3.4 The round trip (`doRoundTrip`, `kernel/RelayEXI.c:384-473`)

One code path, no retries, 3 s total budget (`RELAY_BUDGET_MS`) measured from the moment the
thread picks the request up:

1. `socket(top_fd, AF_INET, SOCK_STREAM, IPPROTO_IP)` on the shared IOS socket fd
   (`kernel/net.c:14`, `kernel/net.c:117`).
2. **R9, non-blocking connect.** `connect()` (`kernel/net.c:291`) has no timeout and nothing in
   this kernel made an outbound TCP connect before. The socket is switched to non-blocking with
   `IOCTL_SO_FCNTL` (`kernel/net.h:105`): `relay_fcntl()` (`kernel/RelayEXI.c:364`) is libogc's
   `net_fcntl()` from `network_wii.c` (params `{socket, cmd, flags}`, input length 12, cmd =
   POSIX `F_GETFL` 3 / `F_SETFL` 4, `IOS_O_NONBLOCK` = 0x04, libogc's `(O_NONBLOCK >> 16)`).
   `connect()` then returns 0 or a negative in-progress code; anything other than
   `-EINPROGRESS`/`-EAGAIN`/`-EALREADY` (IOS values 27/6/7, Dolphin
   `Source/Core/Core/IOS/Network/Socket.h` `WiiSockets`, same order as libogc's error map) is a
   failure. In-progress waits in `poll(POLLOUT)` (`kernel/net.c:337`) with the remaining budget;
   POLLERR/POLLHUP/no POLLOUT is "connect timeout".
3. `sendto()` of the stamped request (`kernel/net.c:221`); a short send fails.
4. `poll(POLLIN)` with the remaining budget, then `recvfrom()` into a 32-byte-aligned chunk
   buffer, appended until the relay closes (recv returns 0), the pattern of
   `waitForMessage`/`getClientMessage` (`kernel/SlippiNetwork.c:96-175`). The relay serves one
   request per connection and closes after the reply (architecture.md), the same end condition
   the Dolphin forwarder uses (`EXI_DeviceSlippi.cpp:3695-3698`). `-EAGAIN` just re-polls; a
   response over 4092 bytes (the poll buffer minus the state word) is an error.
5. `close()` always; a response shorter than `relay_hdr + relay_resp` is "short response".

Kernel-generated answers (`synthResponse`, `kernel/RelayEXI.c:351`) are a DONE with the request's
header echoed, `relay_resp.status = ST_INTERNAL` and a message: `"no tournament.cfg"` when the
config is missing/malformed (design 6.2 item 1), `"no network"` when Nintendont's networking was
not enabled (`NIN_CFG_NETWORK` off, `kernel/main.c:315-326`). The game shows the message.

### 3.5 Stamping and logging

- `hdr.station` on every request and `start_set_req.stream` on `CMD_START_SET` are overwritten
  from the config before sending (`kernel/RelayEXI.c:497-499`; design 5.3, 6.2 item 2).
- Every request logs one line through `dbgprintf` (`kernel/RelayEXI.c:516-525`):
  `RelayEXI: cmd N len N -> DONE status S resp B bytes in T ms` or
  `RelayEXI: cmd N len N -> ERROR (reason) after T ms`. `dbgprintf` goes to the USB Gecko
  (`slippi_use_port_a`), the SD log (`NIN_CFG_LOG`) or the UDP debug socket under
  `SLIPPI_DEBUG` (`kernel/vsprintf.c:309-345`). Boot logs the parsed config.

### 3.6 `sd:/tournament.cfg`

`loadCfg()` (`kernel/RelayEXI.c:214`) is the investigation's Pattern B (`ConfigInit`,
`kernel/Config.c:13-39`: FatFS open + `f_read` once at boot) with the SD path spelled out like the
loader's nickname read (`SD_SLIPPI_DAT_FILE`, `common/include/Slippi.h:13`) and without the
`Shutdown()` on failure. Format (design 4.3): `key=value` lines, `\r\n` tolerated, unknown keys
ignored, both of `station` (0-65535) and `stream` (0 or 1) required. Missing file, file >= 512
bytes, or either key missing/invalid -> `cfg.ok = false`. Since 2026-09-25 (decisions.md R15) the card
has no relay address: `relay_ip`/`relay_port` from older cards are unknown keys and ignored.

### 3.7 Relay discovery (decisions.md R15)

The relay broadcasts a 12-byte `relay_beacon` (`relay_proto.h`) every `BEACON_INTERVAL_MS` to UDP
`BEACON_PORT`. The relay thread, whenever it is idle (`RelayEXIThread`, the `relay_state !=
RELAY_BUSY` branch), calls `serviceBeacon()`: once `NetworkStarted` it creates a UDP socket
bound to `BEACON_PORT` on any address and sets it non-blocking with the same `IOCTL_SO_FCNTL` as the
TCP connect (`beaconSetup()`; a failed socket/bind is logged and tried again after 1 s); then every
100 ms it drains up to 8 datagrams. One counts only if it is exactly `sizeof(struct
relay_beacon)` bytes with magic `MT`, `RELAY_PROTO_VERSION` and a non-zero `tcp_port`; its
**source address** and `tcp_port` become `relay_ip`/`relay_port`, the latest one winning, and a
change logs `RelayEXI: relay is a.b.c.d:port (event N)`.

`kernel/net.c:263` `recvfrom()` cannot report the source address (its third vector is NULL,
net.c:278-279), so `RelayEXI.c` has its own `recvfromAddr()` with libogc's vector layout for
`IOCTLV_SO_RECVFROM` (`network_wii.c` `net_recvfrom`: one input vector {socket, flags}, two
outputs {data, source sockaddr}). **Verified on hardware 2026-09-30**: a real Wii heard the
beacon, learned the relay address and ran LIST_SETS.

**Beacon request.** Some access points never deliver the relay's broadcast to a power-saving
Wi-Fi client. So while `relay_ip` is 0, the relay thread broadcasts a `relay_beacon` with
`tcp_port` 0 to `TELEMETRY_PORT` (UDP 29472) every 2 s (`requestBeacon()`, `kernel/RelayEXI.c`
near the `request_sock` declaration). The relay answers unicast to `BEACON_PORT` (UDP 29471),
where `serviceBeacon()` already listens. The beacon carries the TCP port (29470). This path was
also heard on hardware 2026-09-30.

Until a beacon is heard, `exi_poll_hdr.relay_ip`/`relay_port` are 0 and every request answers
`ST_INTERNAL` `"no relay found yet"` without touching the network. Each round trip snapshots
the address once (`doRoundTrip`), so a relay that moves mid-request is used from the next one.

Note: the kernel mounts `sd:` only when the game boots from SD or Slippi replays are enabled
(`kernel/main.c:197-198,238-249`). Booting the ISO from USB with replays off leaves `sd:`
unmounted and every relay command answers `"no tournament.cfg"`.

## 4. Version-gate bypass (decisions.md R12) - SUPERSEDED 2026-09-24

**Superseded.** The kiosk no longer ships a shifted DOL: the ISO is stock 1.02 and the kiosk
code is `sd:/tournament.bin`, loaded by `LoadTournamentModule()` in `kernel/Patch.c` inside the
same full-DOL patch pass, after the GCT/Slippi-core block and before `PatchState =
PATCH_STATE_DONE`, only when `MeleeVersion == MELEE_VERSION_NTSC_2`. It checks the `TMOD`
header, the guard word (`0x8016D800 == 0x7C0802A6`) and that `*(0x80000034)` (arenaHi) is
still above the load address, reads the blob to MEM1, applies the patch words and writes the
load address to `0x80000034`. A missing file logs one line and changes nothing. The
`254853c` gate below is reverted; Slippi core, MeleeCodes and hotswap apply as stock. The
section is kept for the history of the shifted-DOL era.

**Problem (historical).** `GetMeleeVersion()` (`kernel/Patch.c:160`) keyed only on the disc header
(`GAME_ID`, byte 7), and the tournament ISO keeps a vanilla GALE01 v1.02 header, so Nintendont
detected `MELEE_VERSION_NTSC_2` and would have written the Slippi core codeset, the toggled
MeleeCodes, the tournament-mode redirect `write32(0x0022D638, ...)` and the codehandler at
vanilla 1.02 addresses into the shifted decomp DOL (`kernel/Patch.c:3300-3361,3378-3454`). Every venue mod
is compiled into that DOL, so the correct behaviour is to apply nothing.

**What the DOL and header actually contain** (`SmashTournament-v24.iso`, read with a script):

| | stock GALE01 v1.02 | SmashTournament-v24.iso |
|---|---|---|
| header 0x000-0x007 | `GALE01`, version 2 | identical |
| header 0x420 DOL offset | 0x1E800 (sys area, below the FST) | **0x57058000** (past end-of-data) |
| header 0x424 FST offset | 0x456E00 | 0x456E00 |
| main.dol size | 4,425,184 | 4,450,912 |
| strings | none of ours | `TOURNAMENT`, `START THIS SET?`, `RELAY LINK ERROR`, `SENDING...` (menu text, absent from the stock DOL) |

**Chosen: the DOL's disc offset**, captured where Nintendont already recognises the DOL header
read. `DoPatches(Buffer, Length, DiscOffset)` is called for every DI read (`kernel/DI.c:899`) and
identifies the 0x100-byte DOL header the apploader fetches (`kernel/Patch.c:1353-1411`); that
call's `DiscOffset` is the header's DOL offset, stored in `DOLDiscOffset`
(`kernel/Patch.c:1356`). `GetMeleeVersion()` returns `MELEE_VERSION_NONE` when it is at or beyond
`TOURNAMENT_DOL_MIN_OFFSET` = 16 MiB (`kernel/Patch.c:153,173-177`). A stock disc cannot place its
DOL 16 MiB into the image (its DOL precedes the FST at 0x456E00), so no false positive on a real
1.02 disc; the appended DOL is at 0x57058000, so no false negative as long as the ISO is built the
same way. The marker-string alternative was rejected because it depends on menu text that changes
with UI work and needs a scan of the 4.4 MB DOL; the offset is structural and O(1).

**Effect of `MELEE_VERSION_NONE`** (all in `kernel/Patch.c`): no GCT/Slippi core/MeleeCodes block
(`3378-3454`), no codehandler install and no tournament-mode redirect (`3300-3361`), no
`OSSleepThread` hook removal (`1715-1716`) and no `OSExceptionInit` hook (`2566`),
`isMeleeWidescreen` forced off (`1266-1268`, a MeleeCodes option that also drives the VI patches at
`2210` and `2282`). Generic, pattern-matched patches (EXI/SI/DVD/AR functions, `kernel/patches.c`)
still apply; they are what make the memcard, pad, disc and relay EXI emulation work, and the decomp
build has the same SDK function bodies as the stock DOL. Also untouched: `EXISetTimings`, the
Slippi replay threads (they simply never receive data from this build), the loader's GALE01
revision warning (`loader/source/menu.c:849-856`, UI only).

## 5. What remains for the on-hardware test

**Hardware result, 2026-09-30:** items 1, 3 and 4 below are verified on a Wii (first LIST_SETS
answered in 55 ms, set list shown, `.slp` not yet checked); the beacon's `recvfromAddr` works.
What it took is in tournament-reporter `docs/wii-setup.md` section 6: a CI-built loader (the local
build fails at the IOS58 step), and the PPC entry stub lowering BootInfo arenaHi to the module
base, because the ARM cannot see or change that word (it lives in the PPC data cache) and Melee
zeroes its heap up to it.


Nothing in this session ran on a Wii. Open points to verify, in order:

1. **EXI path end to end.** The game's `EXIImmEx` request arrives as 4-byte immediate words on
   channel 1 and the 4096-byte `EXIDma` read returns the poll image. Watch the boot log for
   `RelayEXI: relay ...` (config parsed) and per-request lines.
2. **IOS non-blocking connect semantics.** Which code `IOCTL_SO_CONNECT` returns on a non-blocking
   socket (expected `-27` EINPROGRESS) and whether `poll(POLLOUT)` reports writability / errors
   as assumed (`kernel/RelayEXI.c:406-416`). If IOS returns a code not in the accepted set, the
   log shows `ERROR (connect) after 0 ms`. Also measure the unreachable-relay case: the log must
   show `ERROR (connect timeout) after ~3000 ms`, not a multi-second stall.
3. **Round-trip time** under 500 ms per session 8's done-when.
4. **Module load.** Boot log shows `Patch:Apply Slippi core` *and* the tournament-module line
   with its load address (needs `DEBUG_PATCH`, on by default in `kernel/global.h:27`); the game
   reaches the set list, and a game writes a `.slp` to the USB drive. Unplug/replug the USB drive
   between games: the next game still records (hotswap untouched).
5. **Slot-B EXISelect answering 1** does not disturb anything else in this build (no card image in
   slot B at the venue).
6. `EXI_RELAY_POLL` = 0xF1 collides with the memory-card *erase* command byte in Nintendont's
   memcard emulation (`kernel/EXI.c:322`, `0xF1xxxxxx` with a 4-byte immediate). The relay hook
   claims the word only when it is exactly `0xF1000000` at the start of a select window, which a
   card erase would send only for block offset 0 during a format of a slot-B card. Not reachable
   at the venue (no slot-B image), but a protocol note for the user: moving the poll command off
   0xF1 (and REQ off 0xF0, which is unused by the memcard code today) would remove the overlap.

## 6. SD card setup

`sd:/tournament.cfg` in the root of the SD card the game boots from, one per Wii:

```
station=3
stream=1
secret=<the relay's RELAY_SECRET>
```

- `secret`: the relay's shared secret (decisions.md R16), the same on every card; 8-16 of `A-Z a-z 0-9 - _`.
  Sent as a `relay_auth` block ahead of every request (`doRoundTrip`). Missing or malformed: every
  action shows `no secret in tournament.cfg` without touching the network; wrong: the relay answers
  `ST_BAD_SECRET` ("wrong relay secret") and its status page counts the refusal.
- No relay address: the Wii finds the relay from its UDP beacon (section 3.7, decisions.md R15).
  The Wii and the Pi must be on the same network, and it must not isolate clients.
- `station`: the physical station number on the label; stamped into every request.
- `stream`: `1` on **exactly one** Wii, the stream station, `0` everywhere else. The relay refuses
  `START_SET` with `stream=1` from any station other than its configured stream station
  (`ST_NOT_STREAM`), so a mis-copied card cannot hijack the stream, but two cards with `stream=1`
  still means one station gets that error on every start.
- Plain ASCII, LF or CRLF, no spaces around `=`. Any missing or malformed key disables the relay
  for that Wii: the menu shows `no tournament.cfg` on every action.
- Nintendont's own **Network** option must be on (it brings up IOS networking,
  `kernel/main.c:315-326`); otherwise every action shows `no network`.
- Boot the ISO from the SD card (or enable Slippi replays) so `sd:` is mounted in the kernel
  (section 3.6).

## 7. Contract notes for the user (nothing changed in the other repos)

- EXI command bytes 0xF0/0xF1 overlap Nintendont's memcard command space (section 5, item 6).
- The kernel returns kernel-generated `ST_INTERNAL` replies as `RELAY_DONE` with a normal
  `relay_resp` (design 6.2 says "returns ST_INTERNAL"), and transport failures as `RELAY_ERROR`
  with a zeroed buffer, matching `lbrelayexi.h`.
- The kernel reads until the relay closes the connection (like the forwarder); it does not check
  `hdr.len` against the byte count. The game's own "BAD RESPONSE" check stays the arbiter.
