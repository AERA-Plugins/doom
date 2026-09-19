# AERA Doom

A standalone native Doom source port for AERA Recovery Project. It does not
use RetroArch, libretro, SDL, Android, Java, or a hardware framebuffer. The
engine writes its own frames and audio into AERA's protected plugin channels,
while AERA retains ownership of the display and touch devices.

The runtime uses PureDOOM at pinned commit
`355cfbd16fac119718879239336ee2ea408886bd` and includes FreeDoom 0.13.0 Phase
1 as freely redistributable game data. If present, a personal IWAD in
`/sdcard/AERA/Doom` takes priority in this order:

- `doom2.wad`
- `doom.wad`
- `doom1.wad`

Configuration and save games persist in the same AERA Doom directory when
internal storage is available. Without decrypted storage, the bundled game
still runs with temporary saves.

## Controls

- Direction pad: walk forward/back and turn
- Fire: fire, and confirm menu choices
- Use: open doors and activate switches
- Map: toggle the automap
- Weapon: cycle weapons 1–7
- Menu: open or leave the Doom menu

## Reproducible build

Set `PUREDOOM_SOURCE` to the pinned checkout, `AERA_CC` to an ARM64 musl C
compiler wrapper, and `AERA_STRIP` to `llvm-strip`. Then run:

```sh
source/build-runtime.sh /tmp/aera-doom-root
source/pack.py /tmp/aera-doom-root build
```

The build script verifies the pinned PureDOOM commit and the SHA-256 of the
FreeDoom 0.13.0 release archive before compiling or packaging anything.

PureDOOM and the AERA platform adaptation are distributed under GPL-2.0-or-
later. FreeDoom game data is distributed under the BSD 3-Clause license; its
license is included inside the runtime.
