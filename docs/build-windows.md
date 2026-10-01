# Building Slippi Nintendont on Windows

Recipe for branch `LazyTO` (named `vanilla-module` until 2026-10-01) (kernel + loader; first used 2026-09-22 on the old branch `reporter`). CI builds in the
`nikhilnarayana/devkitpro-slippi` Docker image (`.github/workflows/build.yml`); this is the
native equivalent with devkitPro's pacman inside MSYS2, installed under `C:\devkitPro` so the
layout matches devkitPro's own Windows installer.

## Known requirements (hardware, 2026-09-30)

Two kernel fixes are required on a real Wii. Keep them when merging or rebasing:

- **BootInfo `0x80000034` arena word.** Nintendont can leave it 0. The module loader
  (`kernel/Patch.c` `LoadTournamentModule`) must handle that case before checking room for
  the module and writing `load_addr` there.
- **FatFS try-lock in `kernel/vsprintf.c`.** `dbgprintf` must only try the log lock, never wait
  on it: FatFS is not reentrant and a waiting lock deadlocks the kernel.

Kernel changes only reach a Wii through a rebuilt loader (`loader/data/kernel.zip` is
embedded), and that loader must come from CI (see the IOS58 note below).

## 1. Toolchain install (once)

1. **MSYS2 under `C:\devkitPro\msys2`** (the installer is a Qt IFW exe; it takes a silent
   command line):

   ```bat
   msys2-x86_64-20260611.exe in --confirm-command --accept-messages --root C:/devkitPro/msys2
   ```

   Installer: https://github.com/msys2/msys2-installer/releases (also `winget install MSYS2.MSYS2`,
   which defaults to `C:\msys64`; then use that path below).

2. **Map `/opt/devkitpro` to `C:\devkitPro`** so packages land next to msys2, as the official
   installer does. In an MSYS2 shell (`C:\devkitPro\msys2\usr\bin\bash.exe -l`):

   ```bash
   echo "C:/devkitPro /opt/devkitpro" >> /etc/fstab
   pacman -Syu --noconfirm            # run twice if it asks to restart the shell
   pacman -S --noconfirm --needed make zip git
   ```

3. **devkitPro repositories and keyring** (devkitPro wiki "devkitPro pacman", section for an
   existing pacman install):

   ```bash
   pacman-key --recv BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com
   pacman-key --lsign BC26F752D25B92CE272E0F44F7FD5492264BB9D0
   pacman -U --noconfirm https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.xz
   cat >> /etc/pacman.conf <<'EOF'

   [dkp-libs]
   Server = https://pkg.devkitpro.org/packages

   [dkp-windows]
   Server = https://pkg.devkitpro.org/packages/windows/$arch/
   EOF
   pacman -Sy
   ```

   Note: `pkg.devkitpro.org` answers 403 to plain `curl`/browser user agents; pacman's own
   downloader is allowed. `pacman -U` may leave a `dirmngr.exe` running that keeps the shell
   open; it is harmless (`taskkill /F /IM dirmngr.exe`).

4. **Packages**:

   ```bash
   pacman -S --noconfirm --needed devkitARM devkitarm-rules devkitPPC devkitppc-rules libogc \
       general-tools gamecube-tools ppc-libpng ppc-freetype ppc-zlib
   ```

   Installed 2026-09-22: devkitARM r68 (GCC 16.1.0), devkitPPC r50 (GCC 16.1.0), libogc 3.1.0.

5. **Older libogc for the loader.** The loader includes `<ogc/lwp_threads.h>` and calls
   `__lwp_thread_stopmultitasking` / `__lwp_thread_closeall` (`loader/source/global.c:28,283`,
   `loader/source/main.c:24,1348`). libogc 3.x removed that header and those symbols (commit
   "libogc: adapt to tuxedo, removing most low-level system support code"); the last tag with
   them is v2.14.1. Build it from source into a side prefix, leaving the pacman libogc alone:

   ```bash
   git clone --filter=blob:none https://github.com/devkitPro/libogc.git ~/libogc-src
   cd ~/libogc-src && git config core.autocrlf false && git checkout -f v2.14.1
   export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
   make -j8
   make install DEVKITPRO=/c/devkitPro/legacy     # -> C:\devkitPro\legacy\libogc
   ```

   The kernel does not need this; only `make -C loader`.

6. **Old start files for the loader.** devkitPPC r50's `-mrvl` spec links `crtmain.o` (which
   calls the libogc 3 `SYS_PreMain`) and its `rvl.ld` (entry `__app_start`); libogc 2.x wants the
   pre-tuxedo `rvl.ld` (`__stack_addr`, `__intrstack_addr`, `__gxregs`, entry `_start` from
   libogc's own `ogc_crt0`) and a `__crtmain` that just runs `main` and exits. Put both in a
   side directory that `-B` will search before the toolchain's own:

   ```bash
   git clone --filter=blob:none https://github.com/devkitPro/devkitppc-crtls.git ~/crtls-src
   cd ~/crtls-src && git config core.autocrlf false && git checkout -f v1.0.0   # just gcn/ogc/rvl.ld
   mkdir -p /c/devkitPro/legacy/crtls/lib && cp *.ld /c/devkitPro/legacy/crtls/lib/
   cat > /c/devkitPro/legacy/crtls/crtmain.c <<'EOF'
   /* crtmain for libogc 2.x with devkitPPC r50: the old devkitPPC crtmain.o
      (__crtmain called from libogc lwp.c __lwp_sysinit): run main, then exit. */
   #include <stdlib.h>
   #include <ogc/system.h>
   extern int main(int argc, char **argv);
   void __crtmain(void)
   {
   	int ret;
   	if (__system_argv->argvMagic == ARGV_MAGIC)
   		ret = main(__system_argv->argc, __system_argv->argv);
   	else
   		ret = main(0, NULL);
   	exit(ret);
   }
   EOF
   cd /c/devkitPro/legacy/crtls && $DEVKITPPC/bin/powerpc-eabi-gcc -DGEKKO -mrvl -mcpu=750 -meabi \
       -mhard-float -O2 -I/c/devkitPro/legacy/libogc/include -c crtmain.c -o lib/crtmain.o
   ```

   `ecrti.o`, `crtbegin.o`, `crtend.o`, `ecrtn.o`, `libsysbase` and newlib still come from
   devkitPPC r50. This mix (GCC 16, newlib 4.6, libogc 2.14.1) is **not** what CI ships; the
   loader it produces links but **does not work on a Wii**: first hardware run 2026-09-30 stopped
   at "Preparing IOS58 Kernel" with `Failed to load IOS58 from NAND: ES_GetStoredTMDSize()
   returned -4352` on a Wii whose stock Nintendont runs fine. -4352 is libogc's `ES_ENOTINIT`
   (ES never opened), not a missing IOS58 - the startup path differs from the real toolchain.
   **Ship only CI-built loaders:** on the fork, `gh workflow run build.yml --ref LazyTO`
   (Actions enabled 2026-09-30), then `gh run download` the `release-*` artifact; its
   `apps/Slippi Nintendont/boot.dol` goes on the SD card. The local build stays useful for
   compiling the kernel (`kernel/kernel.bin`) and checking it builds.

## 2. Build

Everything below in the MSYS2 shell, from the repo root. `git` must be on PATH (the kernel
Makefile embeds `git describe` in `NIN_GIT_VERSION`).

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export DEVKITPPC=/opt/devkitpro/devkitPPC
cd /p/Projects/Nintendont            # or the worktree

# kernel (ARM) and what it depends on, in the root Makefile's order
make -C kernel/asm                   # PPC stubs -> kernel/asm/*.h (uses bin2h.exe, checked in)
make -C fatfs -f Makefile.arm
make -C codehandler
make -C kernel                       # -> kernel/kernel.bin, loader/data/kernel.zip

# loader (PPC), embeds loader/data/kernel.zip
make -C multidol
make -C resetstub
make -C kernelboot
make -C fatfs -f Makefile.ppc
make -C loader/source/ppc
make -C loader LIBOGC_INC=/c/devkitPro/legacy/libogc/include \
               LIBOGC_LIB=/c/devkitPro/legacy/libogc/lib/wii \
               MACHDEP="-DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -B/c/devkitPro/legacy/crtls/lib/"
                                     # -> loader/loader.dol and nintendont/boot.dol
```

If `loader/build/` holds objects from an earlier attempt against the pacman libogc, `rm -rf
loader/build` first: stale objects compiled against libogc 3 headers fail to link (`PPCDCache*`).

Why not the root `make`: its `bin2h` target runs `make -C kernel/bin2h`, whose Makefile only
knows Linux/macOS (`kernel/bin2h/Makefile:10-29`) and errors on Windows; the tracked
`kernel/bin2h/bin2h.exe` is used directly by the kernel Makefile (`kernel/Makefile:68-69`).
Everything else is exactly the root Makefile's subproject order (`Makefile:14-17`).

`LIBOGC_INC`/`LIBOGC_LIB`/`MACHDEP` on the loader command line override the assignments in
`$(DEVKITPPC)/wii_rules` (command-line variables beat makefile assignments); `-B` makes gcc
resolve `rvl.ld%s` and `crtmain.o%s` from the legacy directory first.

## 3. Warnings baseline (GCC 16.1.0, kernel)

`make -C kernel` ends with 4 warnings, all in pre-existing code untouched on this branch except
for line moves: `Patch.c:164` (`*(u8*)0x00000007`, `-Warray-bounds`), `Patch.c:934`,
`Patch.c:3469`, `Patch.c:3481` (`-Wmaybe-uninitialized`), plus the usual `ubj/*` noise earlier in
the log. `RelayEXI.c`, `EXI.c`, `main.c` compile clean with the kernel's `-Wall`.

## 4. Output

Built 2026-09-22 on branch `reporter`: kernel 474,324 bytes, `loader.dol` 1,580,384 bytes,
`loader/data/kernel.zip` regenerated before the loader step so the loader embeds the new kernel.

- `kernel/kernel.bin`: the ARM kernel; `loader/data/kernel.zip` is what the loader embeds.
- `nintendont/boot.dol` (copy of `loader/loader.dol`): put it at
  `sd:/apps/Slippi Nintendont/boot.dol` on the Wii's SD card, next to `icon.png`/`meta.xml`
  from `nintendont/`. That is what CI packages (`.github/workflows/build.yml`, "Package" step).
