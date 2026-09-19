# F-Zero (SNES Recompilation) for Nintendo Switch

**Version:** 0.3.0  
**Author:** Thorhax  
**Target:** Nintendo Switch (Atmosphère / Horizon OS)  

---

## Overview

This is a native Nintendo Switch port of **FZeroRecomp**, the static C recompilation of SNES *F-Zero (USA)*. By running natively on the Switch's 64-bit ARM Cortex-A57 CPU without SNES emulation overhead, the game achieves a locked 60 FPS presentation with widescreen rendering.

> [!IMPORTANT]
> **Performance & Overclocking Recommendation:**  
> This game is very CPU-heavy due to native real-time widescreen Mode 7 track transformation and multi-vehicle layer raster composition. For stable and stutter-free 60 FPS performance, it is **strongly recommended to overclock your CPU to 1785 MHz** (e.g. via sys-clk).

---

## Features

- **60 FPS Gameplay**: Smooth 60 FPS performance with recommended CPU overclock.
- **Widescreen Mode 7**: Native 1280×720 widescreen presentation with expanded track rendering and multi-vehicle display.
- **Embedded RomFS**: The reference ROM is bundled directly inside the `.nro` binary—no external files required to play out-of-the-box.
- **External ROM Support**: Optionally loads custom/reference ROMs placed at `sdmc:/switch/fzero/fzero.sfc`.
- **SRAM Save Support**: High scores, lap times, and game progress are saved to `sdmc:/switch/fzero/fzero.srm`.
- **Modern Controller Ergonomics**:
  - Full Nintendo Switch Joy-Con and Pro Controller support.
  - Analog trigger mapping for L and R leaning via ZL and ZR.

---

## Controls

| Switch Button | SNES Function | In-Game Action |
| :--- | :--- | :--- |
| **B** | B Button | Accelerate |
| **Y** | Y Button | Brake |
| **A** | A Button | Super Jet (Boost) |
| **X** | X Button | Brake |
| **L / ZL** | L Shoulder | Lean Left (Hard Turn) |
| **R / ZR** | R Shoulder | Lean Right (Hard Turn) |
| **D-Pad / Left Stick** | D-Pad | Steer / Pitch |
| **+ (Plus) / Start** | Start | Pause Game / Select |
| **- (Minus) / Select** | Select | Select Option |

---

## Installation

1. Copy the `switch/` directory to the root of your Switch's microSD card (so that `fzero_recomp.nro` is located at `sdmc:/switch/fzero/fzero_recomp.nro`).
2. Launch the **Homebrew Menu** on your Nintendo Switch.
3. Select **F-Zero** and enjoy!
