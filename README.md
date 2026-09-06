# ARCHIVED - Support the official release which wasn't announced at the time!

# CloverPit — Nintendo Switch port (Unity 6 / IL2CPP wrapper)
 
This is a native wrapper / loader that runs the original ARM64 Android build of CloverPit on Switch homebrew. It contains no game code and no game assets.

## Install & run
You need CloverPit 1.4.8 extract the .apk and put files in the appropriate directories as shown below

```
sdmc:/switch/cloverpit
├── cloverpit_nx.nro
├── libmain.so  libunity.so  libil2cpp.so  lib_burst_generated.so   <- from config.arm64_v8a
└── assets/                                                          <- from the base APK
    └── bin/Data/data.unity3d, Managed/Metadata/global-metadata.dat, ...
```

## Unholy Fusion DLC
 
CloverPit resolves DLC ownership through Google Play Billing, which does not exist here, so
the DLC reads as unowned no matter who owns it. If you bought it, set
`CP_OWN_UNHOLY_FUSION_DLC` to `1` in `source/config.h` before building — the flag is an
assertion of ownership, not an unlock, and the wrapper cannot verify one.

## Building
 
Requires devkitPro with devkitA64 plus these portlibs:
 
```sh
sudo dkp-pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib switch-libpng
```
 
```sh
export DEVKITPRO=/opt/devkitpro
make
```

## Legal
 
CloverPit is © Panik Arcade. This project is an independent, non-commercial interop wrapper,
not affiliated with or endorsed by Panik Arcade. It distributes no game content; you run it
only with files from your own legally-owned copy. Wrapper code is MIT (see `LICENSE`); the
inherited substrate retains its upstream MIT notices.
 
Running homebrew requires custom firmware, which violates Nintendo's terms of service and
can get a console banned from online services. That is your call to make.
 
## Credits
 
Built on the SoLoader lineage: **badpiggies_nx / colorsheep_nx / laytonbmr_nx / vln_nx**,
and the TheOfficialFloW / Rinnegatamante so-loader tradition. The loader, libc/JNI/OpenSL
shims, crash handler and diagnostics are inherited substrate, MIT, and are not my work. The
Unity 6 / IL2CPP specifics — the GC stop-the-world bridge, the engine fingerprints and
patches, the FMOD OpenSL fixes, input and the main loop — are new to this project.
