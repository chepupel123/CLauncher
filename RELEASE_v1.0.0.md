# CLauncher v1.0.0

A lightweight native Minecraft launcher for Linux and Windows, written in C++20 with a Dear ImGui interface and OpenGL.

No ads, no telemetry, no store, and no account login required for local or offline profile-based play.

## Build status

- **Linux — tested.** The full installation and launch cycle has been verified on Linux Mint.
- **Windows 10/11 — beta.** The Windows build is available, but testing on a physical Windows system is still pending. Please report any issues you encounter.

## Features

- **Vanilla Minecraft installation:** downloads the client, libraries, assets, and native libraries from official Mojang servers.
- **Fabric support:** installs Fabric through the official Fabric installer. The loader version is retrieved from Fabric Meta.
- **Performance mods:** one-click installation of Sodium, Lithium, and FerriteCore. Compatible versions are selected automatically through Modrinth.
- **Custom mods and resource packs:** each Fabric installation has its own mods folder, helping prevent conflicts between Minecraft versions.
- **Automatic Java management:** downloads a compatible Java runtime from Adoptium or uses an installed system Java runtime.
- **Parallel downloads:** uses a 16-thread download pool, SHA-1 integrity checking, and automatic retry and recovery of interrupted downloads.
- **Responsive interface:** installation runs in the background, keeping the launcher window responsive.
- **Bilingual interface:** English and Russian.
- **Native process launching:** uses `execv` on Linux and `CreateProcessW` on Windows. The Java classpath is passed through an argument file.

## Supported versions

- **Vanilla:** Minecraft releases listed in the Mojang version manifest, up to the 1.21.x series.
- **Fabric:** supported from Minecraft 1.14.x through the 1.21.x series.

Minecraft 26.x and newer are not currently supported: according to Mojang's own version metadata these releases require Java 25 and an updated modding toolchain, and they target substantially newer hardware.

For Minecraft 26.x and newer, please use the official Minecraft Launcher. Support may be added once the new version series and its modding ecosystem become stable.

## System requirements

### Linux

- Linux Mint 21 or newer
- Ubuntu 22.04 or newer
- Debian 12 or newer
- x86_64 processor
- glibc 2.35 or newer for the AppImage
- OpenGL 3.0 or newer

### Windows

- Windows 10 or newer
- 64-bit system
- OpenGL 3.0 or newer

## Downloads

### CLauncher-Linux.zip

Linux x86_64.

The archive contains a self-contained `CLauncher-x86_64.AppImage`, which bundles the launcher and its required libraries. Minecraft files and Java runtimes are downloaded separately on first use.

To run:

1. Extract the archive.
2. Make the AppImage executable:

   ```bash
   chmod +x CLauncher-x86_64.AppImage
   ```

3. Launch it (double-click, or from a terminal):

   ```bash
   ./CLauncher-x86_64.AppImage
   ```

If the AppImage does not start, the system may be missing FUSE 2 support. On Debian/Ubuntu/Mint install it with `sudo apt install libfuse2` (on Fedora the package is `fuse-libs`). Alternatively, run it without FUSE:

   ```bash
   ./CLauncher-x86_64.AppImage --appimage-extract-and-run
   ```

### CLauncher-Windows-x64.zip

Windows 10/11 x64.

Beta: not yet tested on a physical Windows system.

The archive contains `CLauncher-beta.exe`.

Extract the archive and run the executable.

The Windows executable is currently unsigned, so Windows SmartScreen or antivirus software may display a warning ("More info" → "Run anyway"). The archive includes the required GLFW/curl DLLs and a `cacert.pem` TLS trust store — keep all files in one folder. Downloaded Java runtimes are extracted with the `tar.exe` built into Windows 10/11 (PowerShell is only used as a fallback).

## Notes

- The first launch may take some time because Minecraft requires thousands of asset files to be downloaded.
- Subsequent launches are fast: downloaded files are cached and verified using SHA-1 checksums, and only missing or corrupted files are re-downloaded.
- The Linux AppImage requires glibc 2.35 or newer and OpenGL 3.0 or newer.
- On older Linux systems, build CLauncher from source as described in the README.
