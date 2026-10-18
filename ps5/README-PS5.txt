Bloodborne for PS5 - test build (based on Supermedo's bloodborne_pc)
====================================================================

What this is
- The PC port (bbport) built for the PS5's own CPU and GPU: the game's original x86-64 code
  runs directly on the console, its graphics go through shadPS4's GPU core to Vulkan, which is
  the RADV driver from PS5_Vulkan linked into this title. It runs as a homebrew title folder,
  like the other PS5_Vulkan titles.
- Default: the scene renders at 1916x1078 and FSR 3.1 upscales it to the PS5's output
  resolution (1080p, 1440p or 4K), 60 FPS.
  BB_FPS=uncap in bbport/bbport-ps5.env tries the uncapped patch with a 120 Hz display.
- Not in this build: online play, the intro and ending movies (no video decoder yet: they are
  skipped), DLSS/FSR 4 and frame generation, keyboard and mouse.

Folder layout (/data/homebrew/PPSA99173/ on the console)
  eboot.bin  sce_sys/  sce_module/libc.prx    the title
  CUSA03173/                                   YOUR game dump, copied in (eboot.bin, sce_sys,
                                               sce_module, dvdroot_ps4, ...; version 1.09)
  bbport/boot-linked.bin, content.bin          made on your PC from that dump
  bbport/patches-60.bin, patches-uncap.bin     the FPS/resolution patches
  bbport/bbport-ps5.env, bbport.ini, input.ini settings
  bbport/user/                                 saves and shader caches (made on first start)
  bbport/last_run.log                          the log of the last start (previous_run.log before it)

Installing
1. Copy this whole PPSA99173 folder to /data/homebrew/PPSA99173/ over FTP (or with
   PS5_Vulkan's tools/deploy-title-folder.py PPSA99173 --host <console IP>).
2. Copy your CUSA03173 folder into it: /data/homebrew/PPSA99173/CUSA03173/ (about 31 GB; the
   folder with eboot.bin directly inside).
3. Let your homebrew setup register the folder as it does for the other titles
   (ShadowMountPlus scans /data/homebrew), and start "Bloodborne".
4. Saves from the PC: copy bbport-windows/user/savedata/ to bbport/user/savedata/ (optional).

Testing
- The first start compiles shaders: expect stutter and long loads for the first minutes.
- L3+R3 opens the port's menu (upscaler, sharpness, FPS counter, button prompts).
- After each test, send bbport/last_run.log (get it over FTP). If the title closes or hangs,
  the log shows how far it got. klog has the same lines, prefixed [bbport].

Settings (bbport/bbport-ps5.env)
- BB_FPS=60 or uncap
- BB_OUTPUT_RES=auto (default): FSR upscales to whatever the TV/monitor runs at, so 1080p,
  1440p and 4K displays each get their own resolution. Or set it yourself:
  BB_OUTPUT_RES=2560x1440 (1440p), 1920x1080 (1080p), 3840x2160 (4K). A lower output than
  the display's is quicker and is stretched to the screen.
- RADV_DEBUG=nohiz,nodcc (default) turns off GPU compression that drew blocky edges.
- BB_DISPLAY_RES: by default the PS5's own output resolution; set 1920x1080, 2560x1440 or
  3840x2160 to force one.
- The scene itself renders at 1916x1078 for every output (the patches); a different render
  resolution needs new patches: ps5/tools/prepare-game.sh with BB_PS5_RENDER_RES on the PC.
- The PS5 itself must output the resolution you want: Settings > Screen and Video > Video
  Output > Resolution (Automatic picks 1440p on a 1440p monitor on current firmware).
