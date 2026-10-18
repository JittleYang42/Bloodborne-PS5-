# Bloodborne for PS5 (native, upscaled)

**An unofficial PS5 port of Supermedo's PC port of Bloodborne.** It runs the original game on a
jailbroken PS5 at 60 FPS, upscaled with FSR 3.1 to your screen's resolution (1080p, 1440p or 4K),
instead of the PS4 version's 30 FPS at 1080p.

> This project is **based on [Supermedo's bloodborne_pc](https://github.com/Supermedo/bloodborne_pc)**
> ("Bloodborne for Windows"), which is itself built on deadinside28's
> [bbport](https://github.com/deadinside28/bloodborne_pc) and the
> [shadPS4](https://github.com/shadps4-emu/shadPS4) renderer. All of the port's core work (the
> runtime that runs the game's code natively, the graphics translation, the patches, FSR, the
> settings menu) is theirs. This repository adds the PS5 build on top of it. Please support the
> original: **[Supermedo's port](https://github.com/Supermedo/bloodborne_pc)** ·
> [Discord](https://discord.gg/yTMG8c4Bqm). Their original README is in
> [docs/README-PC.md](docs/README-PC.md).

> **No game files are included.** You need your own decrypted dump of Bloodborne (CUSA03173,
> version 1.09). Not affiliated with Sony Interactive Entertainment, FromSoftware or Supermedo.

## Status: test builds

Playable from the start of the game into Yharnam in testing so far. Expect rough edges.

| | |
|---|---|
| Frame rate | 60 FPS patch; about 48–60 FPS depending on the area |
| Resolution | scene at 1916x1078, FSR 3.1 to the PS5's own output resolution |
| Controller | DualSense, PlayStation button prompts, on-screen keyboard for names |
| Saves | yours, in `bbport/user/` (PC saves can be copied over) |
| Not yet | online play, the intro/ending movies (skipped), FSR 4/DLSS/frame generation |

## What's different from the PC port

The game's own x86-64 code runs directly on the PS5's CPU, as it does on PC. Its graphics go
through shadPS4's GPU core to Vulkan, using the RADV driver from PS5_Vulkan linked into the title.
The PS5 build adds:

- a console platform layer (memory, threads, the game's TLS, 16 KB pages, logging) in `ps5/` and
  `src/compat_ps5.h`
- a small SDL3 stand-in for the DualSense, audio and the display (`ps5/sdl3/`)
- the display through VK_KHR_display at the console's output resolution
- an L3+R3 menu with the settings that apply on the console (upscaler, sharpness, FPS counter,
  button prompts)
- home-screen icon and backgrounds

## Installing

See [ps5/README-PS5.txt](ps5/README-PS5.txt). In short:

1. Build the title (`ps5/`, CMake with the PS5 payload SDK and PS5_Vulkan) or use a test build.
2. On your PC, prepare the game files from a working bbport install:
   `ps5/tools/prepare-game.sh <bbport folder> <title folder>`.
3. Copy the title folder to `/data/homebrew/PPSA99173/` and your `CUSA03173` dump into it.
4. Register it with your homebrew loader (e.g. ShadowMountPlus) and start **Bloodborne**.

Settings are in `bbport/bbport-ps5.env` (frame rate, resolution override, diagnostics). After a
problem, send `bbport/last_run.log`.

## Credits

- **[Supermedo](https://github.com/Supermedo)** (Mohammed Albarghouthi): the PC port this is
  based on, [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc).
- **deadinside28**: [bbport](https://github.com/deadinside28/bloodborne_pc), the native port
  Supermedo's builds on.
- **[shadPS4](https://github.com/shadps4-emu/shadPS4)**: the GPU core and renderer.
- **PS5_Vulkan / PS5 payload SDK** authors: RADV on the PS5 and the console toolchain.
- Game patches by Kyo, Lance McDonald, auser1337, illusion, emoose and the Bloodborne community.
- Everything else credited in the original README: [docs/README-PC.md](docs/README-PC.md#credits).

PS5 port by [JittleYang42](https://github.com/JittleYang42).

## License

GNU GPL v2 or later, as the original ([LICENSE](LICENSE)). Third-party components keep their own
licenses.
