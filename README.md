# FN Skate 3

Skate 3's skating inside offline Fortnite (Chapter 4 Season OG, v27.11). Press J in a match and your
character drops onto a Skate 3 board. You get Skate 3's own physics, tricks, grinds, bails, camera,
sounds, trick display, Hall of Meat and object dropper.

**Just want to play?** Download the `.rar` from the [Releases](../../releases/latest) page and follow the
README inside it. This repository is the source code, for people who want to read or change the mod.

No Fortnite or Skate 3 files are included here or in the release. The setup program builds the Skate 3
data the mod needs from the player's own copy of Skate 3.

## What's in here

| Folder | What it is |
|---|---|
| `SkateMod/src` | The mod DLL (C++). It is injected into Fortnite and runs skate mode, the camera, the HUD, audio, the object dropper and keybinds. |
| `SkateMod/padreader` | `PadReader.exe`: reads the controller outside the game and shares its state with the mod. |
| `SkateEngine` | `skate_engine.dll` (Rust): a C interface around the Skate 3 engine. |
| `third_party/skate` | The Skate 3 engine crates from the 2010 Rust Rewrite Mashup / SK8-ENGINE, with this mod's changes. |
| `third_party/minhook`, `third_party/miniaudio` | Hooking library and audio library. |
| `setup` | The setup program's scripts (`fnskate_setup.py` and `lib/`) plus the manifest that checks every file it builds. |
| `skate-data` | The mod's own default settings and sound configs (no Skate 3 content). |
| `bin` | The GameInput service helper scripts. Built files also go here. |
| `tools/Dumper-7_Fortnite-27.11.patch` | The changes Dumper-7 needs to dump Fortnite 27.11's SDK. |

## Building from source

You need:

1. **Visual Studio 2022 or newer** with the "Desktop development with C++" workload.
2. **Rust** (stable, from https://rustup.rs).
3. **A Fortnite 27.11 SDK** made with [Dumper-7](https://github.com/Encryqed/Dumper-7). It isn't included
   because it is generated from Epic's game. Stock Dumper-7 can't read 27.11's names, so:
   - Clone Dumper-7 at revision `dd8fe34` and apply `tools/Dumper-7_Fortnite-27.11.patch`
     (`git apply --ignore-whitespace tools/Dumper-7_Fortnite-27.11.patch`).
     The patch decodes 27.11's XOR-obfuscated names and sets the name-table offsets for this build.
   - Build it and inject it into Fortnite 27.11 (build CL-29739262).
   - Copy the dumped folder (the one that contains `CppSDK`) into `SDK/` in this repository.

Then run:

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

The script puts `SkateMod.dll`, `PadReader.exe` and `skate_engine.dll` in `bin/`.

To run your build, copy those three files over the ones in an installed release's `bin` folder. The
release also contains what the setup program runs on, which isn't stored here:

- the embedded Python 3.9 with NumPy (`setup/python`)
- vgmstream (`setup/vgmstream`)
- iw4l-skate-convert (`setup/skate-convert`). This one can be built with `third_party/skate/converter/build.ps1`.

## Licence

The mod is licensed under the Apache License 2.0 (see `LICENSE`). The third-party parts keep their own
licences; they are listed in `NOTICE.txt` and `third_party/skate/NOTICE`.

This is an unofficial fan project. It is not affiliated with, endorsed by or supported by Epic Games or
Electronic Arts. Fortnite is a trademark of Epic Games. Skate and Skate 3 are trademarks of Electronic
Arts.
