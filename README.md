# Demon Blaster PSP

Raycaster FPS homebrew for Sony PSP. 19 levels, procedural brick textures, minimap, HUD, distance-sorted sprites with z-buffer occlusion, and chiptune music per level.

## Build

Requires [Docker](https://www.docker.com/products/docker-desktop) (pulls `pspdev/pspdev` automatically).

**Windows (double-click):**
```
build.bat
```

**CLI:**
```
docker run --rm -v "$PWD":/build pspdev/pspdev sh -c "cd /build && make clean && make"
```

Output: `EBOOT.PBP`

## Install on PSP

Copy to memory stick:
```
PSP/GAME/DemonBlaster/EBOOT.PBP
```

Requires custom firmware (tested on 6.60 PRO-C2).

## Controls

| Button | Action |
|--------|--------|
| D-pad / Analog | Move / Turn |
| L/R Trigger | Strafe |
| Cross | Shoot |
| Start + Select | Quit |

## Project Structure

```
demon_blaster.c   - Game source (single file)
db_all_levels.h   - 19 level definitions (maps, enemies, music)
Makefile           - PSP SDK build config
ICON0.PNG          - XMB game icon (144x80)
build.bat          - Windows build script
```

## Key Build Settings

```makefile
BUILD_PRX = 1
PSP_FW_VERSION = 500
LIBS = -lpspaudio -lm
```

Build artifacts (`.o`, `.elf`, `.prx`, `.PBP`, `PARAM.SFO`) are gitignored.
