Fork of Slippi Nintendont, branch lazyto, the LazyTO Wii loader.
How it works: ../tournament-reporter/docs/architecture.md; decisions (R-numbers): ../tournament-reporter/docs/decisions.md.
New code lives in kernel/RelayEXI.c and kernel/RelayEXI.h (relay EXI device, discovery, telemetry), the module loader in kernel/Patch.c (LoadTournamentModule), and the PPC entry stub change.
The kernel builds locally (docs/build-windows.md), but a loader that runs on a Wii must come from CI: `gh workflow run build.yml --ref lazyto`.
Any kernel change means rebuilding the loader, since it embeds the kernel.
kernel/relay_proto.h is a generated copy from the relay repo (generated/); never hand-edit it.
Never add a waiting lock in dbgprintf: FatFS is not reentrant. Use a try-lock.
Every claim about EXI handler context, blocking, sockets, or memory must cite file:line.
