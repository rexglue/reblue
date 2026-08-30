<img width="1480" height="662" alt="Untitled-1" src="https://github.com/user-attachments/assets/1779fdfd-bc3a-416d-8b6c-38874d8eae93" />



> [!IMPORTANT]
> re:Blue is an unofficial project


# re:Blue

re:Blue rebuilds Blue Dragon as a native application through static recompilation, translating the original code into something your machine runs directly rather than emulating a console around it. That opens the door to things an emulator cannot reach: higher frame rates, modern resolutions, and real mod support.

## Table of Contents

- [Hardware Requirements](#hardware-requirements)
- [How to Install](#how-to-install)
- [Features](#features)
- [FAQ](#faq)
- [Building](#building)
- [Credits](#credits)
- [License](#license)

## Hardware Requirements

Requires all three retail Blue Dragon discs or their disc images. Steam Deck is supported. 64-bit ARM processors are supported on Linux and macOS. Windows is x86-64 only. 

### Minimum

- OS: Windows 10 version 1909 or later, Ubuntu 24.04 / Fedora 40 / SteamOS 3.6 or later, or macOS 13.3 Ventura or later
- Processor: Intel Core i5-4460 3.2 GHz 4 Core or AMD Ryzen 3 1200 or Apple M1, or equivalent
- Memory: 8 GB RAM
- GPU: Nvidia GTX 1050 Ti or AMD RX 570, or equivalent performance & VRAM. DirectX 12 with Shader Model 6.0, or Vulkan 1.2, or Metal
- Storage: 15 GB available space

### Recommended

- OS: Windows 11, SteamOS 3.6, or macOS 14 Sonoma or later
- Processor: AMD Ryzen 5 5600X or Intel Core i5-12400 or Apple M2, or equivalent performance, 6 physical cores minimum
- Memory: 16 GB RAM
- GPU: Nvidia RTX 2060 or AMD RX 5700, or equivalent performance & VRAM. 8 GB VRAM for 4K with MSAA
- Storage: 15 GB available space

## How to Install

[Download latest release for your platform](https://github.com/zolaware/reblue/releases/latest) or [build yourself](#building)

1. Blue Dragon shipped on three DVDs, and you will need a disc image of each one from your own copy of the game.

2. Run the executable. A setup wizard will guide you through the rest. You will be asked to point it at each of the three disc images in turn, and it will check each one before letting you continue. Once you pick where to install, the program copies itself there and restarts from that location, so you can delete the folder you extracted the zip into.

3. Pick a graphics quality preset. The wizard copies the game files out of the discs, and you are done. You may also install DLC from this installer or from the main menu under the config menu

The wizard only needs to run once. If something later goes missing from your install, launching with `--repair` reopens it on your existing install and copies back only what it needs.

## Features

Everything below is new to re:Blue. All of it is configurable in game, from the title screen or the camp menu.

### Graphics

- Resolutions up to 4K, windowed or fullscreen, on whichever monitor you pick
- Aspect ratios 16:9, 4:3, 16:10, 21:9, 32:9, plus auto and stretch
- Four quality presets, Low through Ultra
- MSAA up to 8x or SSAA up to 4x
- Anisotropic filtering
- Shadow quality and draw distance
- Depth of field adjustment
- Unlocked FPS with optional caps and VSync

### Quality of Life

- Unlocked frame rate, with optional caps at 30, 60, 90, or 120
- Save from the camp menu anywhere instead of only at save points
- Field of view adjustment, 45 through 120 degrees
- Skip the in-game tutorial pages
- Full area map on the world map screen, with zoom, floor switching, and a legend
- Optional map markers for the hidden items, chests, and barriers a floor still has, plus per-floor counts, carried onto the field compass
- The field HUD can fade out once you stop pressing anything, or stay off entirely
- Achievement list viewable in game, with eight new re:Blue achievements alongside the original ones
- Master volume control
- Separate center, rear, and subwoofer levels for 5.1/7.1 tuning
- Fully native keyboard and mouse support with cursor and look modes supported by mouse
- Every controller button rebindable to a key, with mouse sensitivity and cursor opacity of your own
- Menus take the mouse directly: hover a row to move the cursor, click to confirm, wheel to scroll
- Custom input based icons/glyphs for hud elements, following the device you last used or pinned to Xbox, PlayStation, Switch, or Steam Deck
- UI language and voice language chosen separately


### Mods and DLC

- Built-in mod manager
- Official DLC is supported

### Platforms and Languages

- Windows on DX12 or Vulkan
- Linux AMD64 and ARM64, including the Steam Deck and other handhelds
- macOS AMD64 and ARM64
- Custom menus in English, French, German, Italian, and Spanish

## FAQ

### Where is my save data and configuration stored?

Everything lives under the folder you installed to:

- Saves and settings: `profiles\default\`
- Your configuration file: `profiles\default\reblue.toml`
- Game files copied from your discs: `game\`
- Mods: `mods\`

### I want to update the game. Will I lose my save data?

No. Copy a newer build over your existing installation and your saves, settings, and mods are left alone. You do not need to reinstall or point the wizard at your discs again.

### How do I install mods?

Use the mod manager in the config menu. It accepts a mod folder or a zip file and puts everything in the right place for you

### Can I keep more than one set of saves?

Yes. Each profile is its own folder under `profiles\`, holding that profile's saves, settings, achievements, and DLC toggles. Launch with `--profile <name>` to pick one, and anything but `default` starts out fresh.

## Building

re:Blue builds with CMake and vcpkg against the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

As with running the game, building requires the files from your own copy of Blue Dragon.
First, extract the game files using a pre-made build, or any other means you have available, and place the `default.xex` file in `./assets/`.

```sh
export PRESET=win-amd64-release  # or linux-amd64-release

cmake --preset "${PRESET}"
cmake --build --preset "${PRESET}" --target reblue_codegen
cmake --build --preset "${PRESET}"
```

Presets cover `win-amd64`, `win-vk`, `linux-amd64`, `linux-arm64`, `mac-amd64`, and `mac-arm64`, each in Debug, Release, and RelWithDebInfo. A `win-amd64` preset builds both the DX12 executable (`reblue.exe`) and the Vulkan one (`reblue_vk.exe`), and a `win-vk` one builds the Vulkan executable alone.

## Credits

Huge thanks to everyone who has put time into this. re:Blue would not be where it is without you.

### re:Blue Development Team

- **[crack](https://github.com/tomcl7)** project lead and developer

- **[rcold](https://github.com/RC0ld)** developer and has done an absurd amount for this project. A lot of re:Blue looks the way it does because of him.

### Playtesting and Support

- **[infernozotza](https://github.com/Zotza)** - Playtester 
- **baus.98** - Playtester
- **[wolfaeterni](https://github.com/Zolawolf)** - Playtester and French Translations 
- **[griever666.](https://github.com/grv666)** - Playtester
- **[fungus](https://github.com/fungoid-creature)** - Playtester
- **[graine25](https://github.com/Graine25)** - macOS and Linux Development Support
- **[zhyxeryz](https://github.com/Zhyxeryz)** - Playtester and German Translations
- **[Azar42](https://github.com/Azar42)** - Playtesting
- **[ZolaKluke](https://github.com/ZolaKluke)** - Playtester
- **[emersed](https://github.com/RaphyEmersed)** - Playtester
- **[mrcmunir](https://github.com/mrcmunir)** - Spanish Translations
- **[mystixor](https://github.com/mystixor)** - German Translations
- **[toby](https://github.com/TbyDtch)** - Graphic Design

### Special Thanks

- The **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** team, for the toolchain this project is built on.

- The **[hedge-dev](https://github.com/hedge-dev)** team, for [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) and for blazing the trail for Xbox 360 recompilations with [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp).

- The wider **Xbox 360 emulation scene**, and the [Xenia](https://github.com/xenia-project/xenia) project in particular. A lot of the hardest problems were solved long before this project started.

## License

See [LICENSE](LICENSE).
