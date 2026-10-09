# LazyTO — Nintendont

This is the [LazyTO](https://github.com/PranavMin/LazyTO) fork of
[Slippi Nintendont](https://github.com/project-slippi/Nintendont). LazyTO lets players at a
Melee weekly pick, play and report their start.gg sets from the Wii itself.

**What this fork adds** (branch `LazyTO`):

- **Relay EXI device** (`kernel/RelayEXI.c`): the game's requests go through a LazyTO beamer in
  the USB port (its RAM mailbox past the replay partition) to the LazyTO relay, and the answers
  come back through EXI. The Wii's own network is not used by LazyTO; the station number, the
  relay's address and the secret live on the beamer, so every SD card is the same.
- **Record gate**: with the kiosk module loaded, Slippi records only the matches the kiosk starts
  as set games, and tells the kiosk each replay's name. A match that never sent Game End is
  finished, and the next one is still recorded.
- **Early sync**: each recording is synced once after its first data block, so an interrupted
  game is a file with a size that the beamer can collect and erase.
- **Module loader**: copies `lazyto_kiosk.bin` from the SD card into RAM at boot and applies its
  hooks on top of stock Melee 1.02. The venue's own Slippi codesets still apply.
- **Station telemetry**: the kernel log and module load result are sent to the relay status page.
- The homebrew app is installed as `apps/LazyTO`.

Use the CI-built loader from GitHub Actions. A locally built loader fails at "Preparing IOS58
Kernel". Venue setup is in
[LazyTO/docs/wii-setup.md](https://github.com/PranavMin/LazyTO/blob/main/docs/wii-setup.md);
the relay device design is in [docs/relay-exi-report.md](docs/relay-exi-report.md).

| LazyTO repo | Role |
|---|---|
| [LazyTO](https://github.com/PranavMin/LazyTO) | Relay on the venue's Raspberry Pi, the kiosk module (`lazyto_kiosk.bin`), design docs and setup guides |
| **Nintendont** (this repo) | Wii loader |

Licence: GNU General Public License, version 2, like Nintendont itself; see [COPYING](COPYING).

Upstream is merged periodically from `project-slippi/Nintendont` `slippi`. Upstream's README follows.

---

Download the `.zip` from the [latest release](https://github.com/project-slippi/Nintendont/releases/latest)

## Nintendont Slippi
This is a fork of [FIX94/Nintendont](https://github.com/FIX94/Nintendont) specifically
used for supporting Project Slippi.

## Users
See [README.md](https://github.com/FIX94/Nintendont/blob/master/README.md) for standard Nintendont if you're new to Nintendont.

### Replays
See documentation in [SLIPPI.md](SLIPPI.md) for more information.

### Kadano's Wii softmodding guide
I recommend using [Kadano's guide](https://docs.google.com/document/d/1iaPI7Mb5fCzsLLLuEeQuR9-BeR8AOwvHyU-FM8GKmEs) if you're new to Wii homebrew. Many guides in the wild are out of date or weren't very good to begin with.

### Installation Summary
1. Download the `.zip` from the [latest release](https://github.com/project-slippi/Nintendont/releases/latest). Unzip to the root of your SD card such that `boot.dol`, `meta.xml`, and `icon.png` all end up under `/apps/Nintendont Melee`.
2. Copy your vanilla Melee (NTSC 1.02) ISO (as well as any special versions like [Training Mode](https://www.patreon.com/UnclePunch)) to `/games/`.
3. Combine with [Priiloader](http://wiibrew.org/wiki/Priiloader) and [Nintendont Forwarder for Priiloader](https://github.com/jmlee337/Nintendont-Forwarder-for-Priiloader) to autoboot from power-on to Nintendont.
4. Turn on autoboot in Nintendont settings to autoboot all the way to Melee.

## Developers
See [README.md](https://github.com/FIX94/Nintendont/blob/master/README.md) for standard Nintendont if you're new to Nintendont.

### Build Requirements
You will need to install part of the devkitPro toolchain to build Nintendont. Visit [the getting started page](https://devkitpro.org/wiki/Getting_Started) and find the instructions for your system. I recommend installing GBA, Wii, and GC packages. GBA is required to get `devkitARM` which isn't provided in the others. You effectively need `devkitARM` `devkitPPC` and `libogc`.

### Building with Docker
We maintain a docker image for building on CI which can also be used for building locally without the need to install devkitPro. Simply run `docker run --volume=${PWD}:/work --workdir=/work nikhilnarayana/devkitpro-slippi make` from the root repo directory.

### Building on Windows
To build on windows, you need to run the `Incremental Build.bat` file in the Nintendont root.

### Troubleshooting
libwinpthread-1.dll error:
```
make -C multidol
make[1]: Entering directory '../Nintendont/multidol'
 ASSEMBLE    crt0.S
 COMPILE     cache.c
 COMPILE     main.c
 COMPILE     global.c
 COMPILE     apploader.c
 COMPILE     dip.c
 COMPILE     utils.c
 LINK        multidol_ldr.elf
collect2.exe: error: ld returned 53 exit status
make[1]: *** [Makefile:44: multidol_ldr.elf] Error 1
make[1]: Leaving directory '../Nintendont/multidol'
make: *** [Makefile:34: multidol] Error 2
```
For some reason the devkitPro installer might not install things correctly. In order to fix the above error, you will need to go to your `devkitPPC/bin` directory, grab the file called `libwinpthread-1.dll`, and copy it to `devkitPPC/powerpc-eabi/bin/`, this should fix the issue.
