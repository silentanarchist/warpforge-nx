# Warpforge — Nintendo Switch wrapper (Unity 6 / IL2CPP)

**Status: experimental, not yet run on hardware.** First milestone is reaching the login
screen and making one https request to the Warpforge Revival server.

This is a native wrapper / loader that runs the original ARM64 Android build of
*Warhammer 40,000: Warpforge* 1.35.0 on Switch homebrew (custom firmware). It loads the game's
own libraries (`libmain.so`, `libunity.so`, `libil2cpp.so`, `lib_burst_generated.so`,
`libRF_CNative_andr.so`) and recreates, natively, the Android layer Unity expects. It
contains no game code and no game assets; you supply them from your own copy of the game.

It is part of the [Warpforge Revival](https://github.com/silentanarchist/warpforge-revival-mod)
fan project, which keeps the game playable on its own server now that the official one is
closed. Unofficial; not affiliated with Everguild or Games Workshop.

## Install & run

1. Get the three APKs of Warpforge 1.35.0 (base, `config.arm64_v8a`, `UnityDataAssetPack`),
   or the `.xapk` that holds them.
2. Run `prepare for switch.bat` from the Revival project's `android patch\switch` folder
   (`prepare_switch.py --apks <folder or .xapk> --out warpforge_nx --server <host>`). It
   extracts the libraries and assets into the layout below and makes two data edits so the
   stock game talks to the Revival server instead of PlayFab and Photon.
3. Copy the folder to `sdmc:/switch/warpforge_nx/` and put `warpforge_nx.nro` (from the
   [releases](../../releases)) in it:

```
sdmc:/switch/warpforge_nx
├── warpforge_nx.nro
├── libmain.so  libunity.so  libil2cpp.so  lib_burst_generated.so  libRF_CNative_andr.so
├── assets/bin/Data/     <- base APK + data pack, merged (split files, hashed resource files)
├── assets/aa/Android/   <- Addressables bundles and catalog, from the data pack
└── debug.log            <- written at run time; attach it to bug reports
```

4. Launch over a game: hold **R** while starting any installed title. Not from the album —
   Unity needs more memory than the album applet gets.

Controls: touchscreen in handheld; docked, the left stick drives a cursor (A to tap) and a
USB mouse works. The game is touch-only, so there is no button mapping beyond that.

## What is different from a phone

- No Google Play services: sign-in, billing, notifications, Helpshift and Firebase answer
  "unavailable" and the game takes its offline paths. Accounts come from the Revival
  server's website instead.
- The loading video is not played (the Android media decoder is not available); the game
  waits out the clip length and continues.
- Matches go to the Revival server's match service over plain TCP, which that server allows
  from its own network only; see the Revival server README.

## Building

Pushes build automatically in GitHub Actions inside devkitPro's `devkita64` image, which
already carries every library the Makefile needs; a `v*` tag publishes the `.nro` on a
release. Locally:

```sh
export DEVKITPRO=/opt/devkitpro     # devkitA64 + switch-portlibs
make
```

`tools/gen_offsets.py <libunity.so>` regenerates `source/cloverpit_offsets.h` from the game's
engine library: every patch site carries a masked fingerprint and a guard word, and a site
that cannot be verified is skipped rather than guessed.

## Credits

Forked from [cloverpit_nx](https://github.com/ChanseyIsTheBest/cloverpit_nx) by
ChanseyIsTheBest (same Unity 6000.2.6f2 engine), which builds on the open-source Switch
.so-loader lineage: Andy Nguyen, fgsfds, Rinnegatamante and TheOfficialFloW's loader
tradition, with pieces from the badpiggies_nx / colorsheep_nx / laytonbmr_nx / vln_nx /
pvz_fusion_nx ports. All MIT. The managed-method name resolution, the Warpforge offsets and
data layout are this project's.

## Legal

*Warhammer 40,000: Warpforge* is © Everguild / Games Workshop. This project is an
independent, non-commercial interop wrapper: it ships no game code or assets and requires
your own copy of the game. MIT licence for the wrapper's own code; see LICENSE.
