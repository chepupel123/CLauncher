CLauncher

A lightweight native Minecraft launcher for Linux and Windows, written in C++20 with a Dear ImGui interface and OpenGL. Designed and built by a single developer for older hardware: no ads, no telemetry, no store, no account required for offline play.

Requires OpenGL 3.0 or newer. Runs on Linux (Mint, Ubuntu, Debian) and Windows 10/11. The binary is about three megabytes and uses 30 to 50 MB of RAM. The reference development machine is a 2012 desktop computer, and that is a deliberate constraint: if the launcher and the game run smoothly there, they run everywhere.

FEATURES

Vanilla Minecraft installation straight from the launcher: client, libraries, assets and native libraries are downloaded automatically from the official Mojang servers.

Fabric support. Installed through the official Fabric installer, and the loader version is kept current from Fabric Meta.

Performance mods with one checkbox: Sodium, Lithium and FerriteCore. Mod versions are matched automatically to the selected Minecraft version through the Modrinth API.

Your own mods. The My Mods button opens the mods folder for the selected Fabric version. Any jar file you drop there is loaded at game startup. Each version has its own folder, so mods never conflict between versions.

Resource packs. The Resource Packs button opens the shared resource pack folder. Drop a zip pack there and it appears in the in-game resource pack list.

Automatic Java management. The launcher downloads the correct JRE version (8, 17 or 21) for the selected Minecraft version from Adoptium. If a matching Java is already installed in the system, it is used instead. Package managers supported: apt, dnf, pacman, zypper and apk.

Parallel downloads. Minecraft assets are thousands of small files. The launcher downloads them with a pool of sixteen threads and connection reuse, cutting the first installation from about an hour to a few minutes.

Integrity checking. Every downloaded file, including client, libraries, assets and mods, is verified by SHA1. Corrupted and partially downloaded files are re-fetched automatically on the next launch.

Responsive interface. All installation work runs in a background thread, the window never freezes, and progress is shown in real time.

Two interface languages. English and Russian, switchable on the fly, with the choice saved.

Safe game launch. The game starts through execv on Linux and CreateProcessW on Windows, with no shell involved. A long classpath is passed through an argfile, so neither its length nor special characters in the nickname can break the launch.

SUPPORTED VERSIONS

Vanilla: every official release in the Mojang manifest, including the 26.x line.

Fabric: versions from 1.14.x to 1.21.x. Fabric support for the 26.x line is intentionally not included: Mojang moved the modding toolchain to Java 25 and a new Loom, which breaks the current loader ecosystem. Vanilla for 26.x works fully.

System requirements: Linux Mint 22, Ubuntu 20.04 or newer, Debian 10 or newer, or Windows 10 and 11. OpenGL 3.0 or newer.

QUICK START ON LINUX

First install the dependencies. In a terminal run:

sudo apt-get update
sudo apt-get install -y build-essential cmake git libglfw3-dev libgl1-mesa-dev libcurl4-openssl-dev nlohmann-json3-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev fonts-dejavu-core

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

This produces CLauncher.exe. On first launch, SmartScreen or your antivirus may ask for permission. That is normal, the launcher calls PowerShell to unpack Java. Click More info, then Run anyway.

HOW IT WORKS UNDER THE HOOD

When you press Play, the launcher runs the following chain.

First, MinecraftInstaller syncs with the Mojang manifest and downloads whatever is missing: the game client, libraries, several thousand asset files and native components. Downloads run in parallel, every file is checked by SHA1, and partial downloads are saved with a temporary extension and renamed only after a successful check.

If Fabric is selected, the official Fabric installer runs, the loader version is taken live from Fabric Meta, and then Sodium, Lithium and FerriteCore are downloaded from Modrinth, matched to your game version.

If the system has no suitable Java, JavaManager downloads the matching JRE from the Adoptium servers and unpacks it into a local folder. On Linux, the system Java and the package managers of major distributions are also checked.

Finally, JavaLauncher starts the game through execv on Linux or CreateProcessW on Windows. There is no shell between the launcher and the game, and a long classpath is passed through an argfile. After the game starts successfully, the launcher closes itself after a second and a half, and the game process id is saved to the last_game.pid file.

Key architectural decisions. One shared Minecraft folder is used, compatible with the official launcher, so worlds and settings are shared. Each Fabric version has its own mods folder, so mods do not conflict between versions. Every launch runs an integrity check of what is installed, and only what is missing gets downloaded. The asset index for Fabric profiles is resolved from the parent vanilla version through inheritance.

USAGE

Enter a nickname. Latin letters, digits and underscore only; anything else is filtered automatically.

Pick a version from the list. The list comes from the official Mojang manifest and is cached, so it works offline too.

Pick a loader: Vanilla or Fabric. For Fabric there is a performance mods checkbox.

Pick the memory amount. 2048 MB is recommended.

Press Play. The first installation takes a while; launches after that are instant.

The My Mods button opens the mods folder of the current Fabric version. The Resource Packs button opens the resource packs folder. In the top right corner there is the language switch.

WHERE THINGS LIVE

On Linux: the game itself in the .minecraft folder of your home directory, worlds in .minecraft/saves, mods in .minecraft/versions/version-name/mods, resource packs in .minecraft/resourcepacks, downloaded Java in .minecraft-launcher/runtime, launcher settings in .minecraft-launcher/launcher_settings.json.

On Windows: the game in the .minecraft folder of your user directory, mods and worlds the same way, and the downloaded Java and launcher settings in the minecraft-launcher folder inside AppData Roaming.

OFFLINE PLAYER IDENTITY

The launcher generates a deterministic offline identifier from the nickname using the OfflinePlayer scheme, the same way servers do. The identifier is stable between launches, so your progress in worlds is preserved correctly.

KNOWN LIMITATIONS

Fabric for the 26.x line is not supported, because Mojang moved the modding toolchain to Java 25 and a new Loom. Vanilla for 26.x works fully.

Your antivirus may ask for permission on the first Windows launch: the launcher calls PowerShell to unpack Java.

The first installation is slow: several thousand asset files. Later launches are instant, everything is cached and SHA1-checked.

Cyrillic in nicknames is rejected on purpose: the nickname goes to servers and into world files, where only a limited character set is safe.

IF SOMETHING DOES NOT WORK

GLFW init failed error. GLFW is not installed, or you are launching outside a graphical session. Install libglfw3-dev.

Question marks instead of text. A font with Cyrillic support was not found. Install fonts-dejavu-core.

Java download failed. Check your internet, or install Java manually: if the major version matches, the launcher uses the system one.

Missing sounds or empty language list. Press Play for that version once more: the launcher will verify the assets and fetch whatever is missing.

The game crashes right after startup. Most likely a mod built for a different Minecraft version. Remove it from the mods folder.

The download stalled halfway. Press Play again; everything resumes from where it stopped.
