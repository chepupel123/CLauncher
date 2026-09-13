CLauncher

A lightweight native Minecraft launcher for Linux and Windows, written in C++20 with a Dear ImGui interface and OpenGL. Designed and built by a single developer for older hardware: no ads, no telemetry, no store, no account required for offline play.

Requires OpenGL 3.0 or newer. Runs on Linux (Mint, Ubuntu, Debian) and Windows 10/11. The binary is about three megabytes, and the launcher itself idles at roughly 30 to 50 MB of RAM (Minecraft's own memory is configured separately in the UI). The reference development machine is a 2012 desktop computer, used as a practical old-hardware baseline rather than a guarantee for every possible system configuration.

FEATURES

Vanilla Minecraft installation straight from the launcher: client, libraries, assets and native libraries are downloaded automatically from the official Mojang servers.

Fabric support. Fabric is installed through the official Fabric installer (the launcher runs its installer jar), and the loader version is fetched live from Fabric Meta.

Performance mods with one checkbox: Sodium, Lithium and FerriteCore. Mod versions are matched automatically to the selected Minecraft version through the Modrinth API.

Your own mods. The My Mods button opens the mods folder for the selected Fabric version. Any jar file you drop there is loaded at game startup. Each Minecraft version has its own mods folder, so mods from different versions never get mixed together (mods can still be mutually incompatible, as with any Fabric setup).

Resource packs. The Resource Packs button opens the shared resource pack folder. Drop a zip pack there, then enable it in Minecraft's in-game resource pack menu.

Automatic Java management. The launcher first checks whether a compatible system Java exists (via java -version); on Linux it can then install the matching JRE through the distribution package manager (apt, dnf, pacman, zypper or apk), and as a final fallback it always downloads a portable JRE (8, 17 or 21, matched to the Minecraft version) from Adoptium. On Windows only the portable JRE is used.

Parallel downloads. Minecraft assets are thousands of small files. The launcher downloads them with a pool of sixteen threads and connection reuse, cutting the first installation from about an hour to a few minutes.

Integrity checking. Downloaded game files — the client, libraries, native libraries and assets — as well as performance mods from Modrinth are verified by SHA1 whenever Mojang or Modrinth provide a checksum. Corrupted and incomplete files are detected and re-fetched automatically on the next launch.

Responsive interface. All installation work runs in a background thread, the window never freezes, and progress is shown in real time.

Two interface languages. English and Russian, switchable on the fly, with the choice saved.

Safe game launch. The game starts through execv on Linux and CreateProcessW on Windows, with no shell involved. A long classpath is passed through an argfile, so neither its length nor special characters in the nickname can break the launch.

SUPPORTED VERSIONS

Vanilla and Fabric: every 1.x release in the Mojang manifest, through the 1.21.x line (Fabric from 1.14.x onward).

The 26.x line and anything newer are intentionally not supported: they require Java 25 and a new modding toolchain, target substantially newer hardware than this launcher is designed for, and are still fresh and unstable. For 26.x please use the official launcher. Support may be added once the line and its modding ecosystem stabilize.

System requirements: Linux Mint 21+, Ubuntu 22.04+, Debian 12+, or Windows 10 and 11. OpenGL 3.0 or newer. The prebuilt AppImage needs glibc 2.35 or newer; on older systems build from source.

DOWNLOAD

Ready-to-use builds are available on the Releases page:

CLauncher-Linux.zip — Linux x86_64. Contains the self-contained CLauncher-x86_64.AppImage: everything is bundled, no extra packages needed (requires glibc 2.35+; unzip, make executable, run).

CLauncher-Windows-x64.zip — Windows 10/11 x64 (beta, not yet tested on a real Windows system). Contains CLauncher-beta.exe together with the required GLFW/curl DLLs; unzip and run the exe.

If you prefer to build from source, follow the steps below.

QUICK START ON LINUX

First install the dependencies. In a terminal run:

sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config unzip libglfw3-dev libgl1-mesa-dev libcurl4-openssl-dev nlohmann-json3-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev fonts-dejavu-core

Then get the project: click Code → Download ZIP on the repository page (or git clone) and enter the project folder:

git clone https://github.com/chepupel123/CLauncher.git
cd CLauncher

Dear ImGui is already bundled with the project (the extern/imgui folder), so there is nothing extra to download.

Build it:

rm -rf build
mkdir build
cd build
cmake ..
make -j$(nproc)

Run it:

./CLauncher

QUICK START ON WINDOWS

You will need MSYS2. Download the installer from msys2.org and install it to C:\msys64. Open the MSYS2 MINGW64 terminal and run:

pacman -Syu
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-curl mingw-w64-x86_64-glfw mingw-w64-x86_64-nlohmann-json make

Then navigate to the project folder and build:

mkdir build
cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja

This produces CLauncher.exe. It links dynamically against glfw3.dll and libcurl-4.dll from MSYS2, so either run it from the MINGW64 shell or copy those DLLs (and their dependencies) next to the exe — the release zip already bundles them together with a cacert.pem TLS trust store. On first launch SmartScreen or your antivirus may show a warning, because the exe is unsigned: click More info, then Run anyway. Java archives are extracted with the built-in tar.exe (Windows 10 1803+), with PowerShell used only as a fallback on older systems.

HOW IT WORKS UNDER THE HOOD

When you press Play, the launcher runs the following chain.

First, MinecraftInstaller syncs with the Mojang manifest and downloads whatever is missing: the game client, libraries, several thousand asset files and native components. The parallel downloads write each file to a temporary .part name and rename it only after its SHA1 check succeeds; files with no published checksum are still retried on failure.

If Fabric is selected, the official Fabric installer runs, the loader version is taken live from Fabric Meta, and then Sodium, Lithium and FerriteCore are downloaded from Modrinth, matched to your game version.

If the system has no suitable Java, JavaManager first looks for a system Java, then (on Linux) tries to install the matching JRE through the distribution package manager, and finally downloads a portable JRE from the Adoptium servers and unpacks it into a local folder if nothing else is available.

Finally, JavaLauncher starts the game through execv on Linux or CreateProcessW on Windows. There is no shell between the launcher and the game, and a long classpath is passed through an argfile. After the game starts successfully, the launcher closes itself after a second and a half, and the game process id is saved to the last_game.pid file.

Key architectural decisions. CLauncher uses the standard .minecraft directory, compatible with the official launcher, so worlds, resource packs and other game data are shared. Each Fabric version has its own mods folder, so mods from different versions do not get mixed together. Every launch runs an integrity check of what is installed, and only missing or corrupted files get downloaded. The asset index for Fabric profiles is resolved from the parent vanilla version through inheritance (inheritsFrom).

USAGE

Enter a nickname. Latin letters, digits and underscore only; anything else is filtered automatically.

Pick a version from the list. The list comes from the official Mojang manifest and is cached, so it works offline too.

Pick a loader: Vanilla or Fabric. For Fabric there is a performance mods checkbox.

Pick the memory amount. 2048 MB is recommended.

Press Play. The first installation takes a while; later launches only verify the installed files and fetch anything missing, so they are much faster.

The My Mods button opens the mods folder of the current Fabric version. The Resource Packs button opens the resource packs folder. In the top right corner there is the language switch.

WHERE THINGS LIVE

On Linux: the game itself in the .minecraft folder of your home directory, worlds in .minecraft/saves, mods in .minecraft/versions/version-name/mods, resource packs in .minecraft/resourcepacks, downloaded Java in .minecraft-launcher/runtime, launcher settings in .minecraft-launcher/launcher_settings.json.

On Windows: the game in the .minecraft folder of your user directory, mods and worlds the same way, and the downloaded Java and launcher settings in the minecraft-launcher folder inside AppData Roaming.

OFFLINE PLAYER IDENTITY

The launcher generates a deterministic offline UUID from the nickname using Minecraft's standard OfflinePlayer: naming scheme (a name-based MD5 UUID, version 3) — the same identifier an offline-mode server derives from the nickname. It is stable between launches, so your progress in worlds is preserved correctly.

KNOWN LIMITATIONS

The 26.x line is not supported at all (Vanilla included): it requires Java 25, targets newer hardware than this launcher aims at, and is still unstable. Choose 1.21.x or older.

The Windows executable is unsigned, so SmartScreen or your antivirus may show a warning on the first launch (More info → Run anyway). Java is unpacked with Windows' built-in tar.exe (PowerShell is only a fallback).

The first installation is slow: several thousand asset files. Later launches are much faster, because everything is cached and SHA1-checked and only missing files are downloaded.

Cyrillic in nicknames is rejected on purpose: the nickname goes to servers and into world files, where only a limited character set is safe.

IF SOMETHING DOES NOT WORK

GLFW init failed error. GLFW is not installed, or you are launching outside a graphical session. Install libglfw3-dev.

Question marks instead of text. A font with Cyrillic support was not found. Install fonts-dejavu-core.

Java download failed. Check your internet, or install Java manually: if the major version matches, the launcher uses the system one.

Missing sounds or empty language list. Press Play for that version once more: the launcher will verify the assets and fetch whatever is missing.

The game crashes right after startup. Most likely a mod built for a different Minecraft version. Remove it from the mods folder.

The download stalled halfway. Press Play again: already downloaded files are kept and verified by SHA1, and only missing or incomplete files are fetched again.
