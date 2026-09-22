# Building Slippi Nintendont on Windows

Recipe used on 2026-09-22 for branch `reporter` (kernel + loader). CI builds in the
`nikhilnarayana/devkitpro-slippi` Docker image (`.github/workflows/build.yml`); this is the
native equivalent with devkitPro's pacman inside MSYS2, installed under `C:\devkitPro` so the
layout matches devkitPro's own Windows installer.

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
               LIBOGC_LIB=/c/devkitPro/legacy/libogc/lib/wii
                                     # -> loader/loader.dol and nintendont/boot.dol
```

Why not the root `make`: its `bin2h` target runs `make -C kernel/bin2h`, whose Makefile only
knows Linux/macOS (`kernel/bin2h/Makefile:10-29`) and errors on Windows; the tracked
`kernel/bin2h/bin2h.exe` is used directly by the kernel Makefile (`kernel/Makefile:68-69`).
Everything else is exactly the root Makefile's subproject order (`Makefile:14-17`).

`LIBOGC_INC`/`LIBOGC_LIB` on the loader command line override the `export ... :=` in
`$(DEVKITPPC)/wii_rules` (command-line variables beat makefile assignments).

## 3. Warnings baseline (GCC 16.1.0, kernel)

`make -C kernel` ends with 4 warnings, all in pre-existing code untouched on this branch except
for line moves: `Patch.c:164` (`*(u8*)0x00000007`, `-Warray-bounds`), `Patch.c:934`,
`Patch.c:3469`, `Patch.c:3481` (`-Wmaybe-uninitialized`), plus the usual `ubj/*` noise earlier in
the log. `RelayEXI.c`, `EXI.c`, `main.c` compile clean with the kernel's `-Wall`.

## 4. Output

- `kernel/kernel.bin`: the ARM kernel; `loader/data/kernel.zip` is what the loader embeds.
- `nintendont/boot.dol` (copy of `loader/loader.dol`): put it at
  `sd:/apps/Slippi Nintendont/boot.dol` on the Wii's SD card, next to `icon.png`/`meta.xml`
  from `nintendont/`. That is what CI packages (`.github/workflows/build.yml`, "Package" step).
