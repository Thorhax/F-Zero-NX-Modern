#include "fzero_layers.h"
#include <string.h>

/* Skip capture passes when none of the selected slots can reach this line.
 * 64 is the largest supported sprite dimension; this is only an optimisation. */
static bool RangeOnLine(const Ppu *ppu, int y, int first, int count) {
    for (int slot = first; slot < first + count; ++slot)
        if ((uint8_t)(y - (ppu->oam[slot * 2] >> 8)) < 64) return true;
    return false;
}

static void CaptureSprites(FZeroLayers *layers, const Ppu *ppu, int line,
                           int first, int count, bool *mask) {
    (void)layers;
    int y = line - 1;
    if (!(ppu->screenEnabled[0] & 0x10) || !RangeOnLine(ppu, y, first, count)) return;

    static const uint8_t spriteSizes[8][2] = {
        {8, 16}, {8, 32}, {8, 64}, {16, 32},
        {16, 64}, {32, 64}, {16, 32}, {16, 32}
    };

    uint8_t start_index = PPU_objPriority(ppu) ? (ppu->oamaddl & 0xfe) : 0;
    int spritesFound = 0;
    int tilesFound = 0;
    uint8_t foundSprites[128];
    uint8_t index = start_index;

    for (int i = 0; i < 128; i++) {
        uint8_t sprite_y = ppu->oam[index] >> 8;
        uint8_t row = y - sprite_y;
        int spriteSize = spriteSizes[PPU_objSize(ppu)][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
        int spriteHeight = PPU_objInterlace(ppu) ? spriteSize / 2 : spriteSize;
        if (row < spriteHeight) {
            int x = ppu->oam[index] & 0xff;
            x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
            if (x >= 256) x -= 512;
            if (x + spriteSize > 0) {
                spritesFound++;
                if (spritesFound > 32 && !(ppu->renderFlags & kPpuRenderFlags_NoSpriteLimits)) {
                    spritesFound = 32;
                    break;
                }
                foundSprites[spritesFound - 1] = index;
            }
        }
        index += 2;
    }

    PpuZbufType overlay[256] = {0};

    for (int i = spritesFound; i > 0; i--) {
        index = foundSprites[i - 1];
        int slot = index >> 1;
        uint8_t row = y - (ppu->oam[index] >> 8);
        int spriteSize = spriteSizes[PPU_objSize(ppu)][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
        int x = ppu->oam[index] & 0xff;
        x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
        if (x >= 256) x -= 512;

        if (PPU_objInterlace(ppu)) row = row * 2 + (ppu->evenFrame ? 0 : 1);
        int oam1 = ppu->oam[index + 1];
        int objAdr = (oam1 & 0x100) ? PPU_objTileAdr2(ppu) : PPU_objTileAdr1(ppu);
        if (oam1 & 0x8000) row = spriteSize - 1 - row;
        int paletteBase = 0x80 + 16 * ((oam1 & 0xe00) >> 9);
        int prio = SPRITE_PRIO_TO_PRIO((oam1 & 0x3000) >> 12, (oam1 & 0x800) == 0);
        PpuZbufType z = paletteBase + (prio << 8);

        bool in_range = (slot >= first && slot < first + count);

        for (int col = 0; col < spriteSize; col += 8) {
            if (col + x <= -8 || col + x >= 256) continue;
            tilesFound++;
            if (tilesFound > 34 && !(ppu->renderFlags & kPpuRenderFlags_NoSpriteLimits)) break;
            int usedCol = (oam1 & 0x4000) ? (spriteSize - 1 - col) : col;
            int usedTile = ((((oam1 & 0xff) >> 4) + (row >> 3)) << 4) | (((oam1 & 0xf) + (usedCol >> 3)) & 0xf);
            const uint16_t *addr = &PpuRenderVram(ppu)[(objAdr + usedTile * 16 + (row & 7)) & 0x7fff];
            uint32_t plane = addr[0] | ((uint32_t)addr[8] << 16);
            int px_left = col + x < 0 ? -(col + x) : 0;
            int px_right = (col + x + 8 > 256) ? (256 - (col + x)) : 8;

            for (int px = px_left; px < px_right; px++) {
                int shift = (oam1 & 0x4000) ? px : (7 - px);
                uint32_t bits = plane >> shift;
                int pixel = ((bits >> 0) & 1) | ((bits >> 7) & 2) |
                            ((bits >> 14) & 4) | ((bits >> 21) & 8);
                if (pixel == 0) continue;
                int screen_x = col + x + px;
                if (in_range) {
                    overlay[screen_x] = z + pixel;
                }
            }
        }
        if (tilesFound > 34 && !(ppu->renderFlags & kPpuRenderFlags_NoSpriteLimits)) break;
    }

    for (int x = 0; x < FZERO_LAYER_WIDTH; ++x) {
        int index = x + kPpuExtraLeftRight;
        PpuZbufType selected = overlay[x];
        if ((selected & 0xff) && selected == ppu->bgBuffers[0].data[index])
            mask[x] = true;
    }
}

/* Slot 47 is the car's slide spark, between boost and rank instruments.
 * Earlier HUD slots win an equal-priority OBJ overlap; the spark in turn
 * wins over later HUD slots. Keep that ordering when coverage colours match. */
static void CaptureHudSprites(FZeroLayers *layers, const Ppu *ppu, int line,
                               int first, bool *mask) {
    bool spark[256]={0}, later[256]={0};
    CaptureSprites(layers,ppu,line,first,47-first,mask);
    /* Crash animation puts explosion and smoke pieces in slots 48 onward,
     * including the tail. Keep those pieces in the filtered scene. */
    if(layers->crash_layout) return;
    CaptureSprites(layers,ppu,line,47,1,spark);
    CaptureSprites(layers,ppu,line,48,4,later);
    /* Racing uploads use the entire tail for shadows. Only full native
     * uploads can put counters in the final two slots. */
    if(layers->native_oam) CaptureSprites(layers,ppu,line,126,2,later);
    for(int x=0;x<256;++x) mask[x] |= later[x] && !spark[x];
}

/* The racing panoramas are packed into seven-tile-high strips, with the
 * next strip duplicated in the right tilemap page for native scrolling.
 * Unwrap horizontal positions before selecting a strip. A hardware 512-pixel
 * wrap alone selects the wrong strip at the extended screen edges. */
static void ExtendPanorama(Ppu *copy, const Ppu *ppu, int line, int layer, bool intro) {
    const int first_row = layer == 0 ? 4 : 11;
    const int first_scroll = layer == 0 ? 36 : 92;
    const int period = layer == 0 ? 896 : 768;
    const int base = layer == 0 ? 0x7800 : 0x7000;
    /* The intro adds a vertical displacement as its horizon enters the view.
     * Integer division retains the packed strip phase; the row below includes
     * the displacement. Ordinary racing still requires an aligned strip. */
    int vertical = (int)ppu->vScroll[layer] - first_scroll;
    if (line > 47 || ppu->bgXsc[layer] != (base >> 8 | 1) ||
        PPU_bigTiles(ppu, layer) || ppu->hScroll[layer] > 255 ||
        vertical < 0 || (!intro && vertical % 56) || vertical / 56 > 3)
        return;
    int phase = vertical / 56;
    if (phase * 256 >= period) return;
    int row = (ppu->vScroll[layer] + line) / 8;
    int strip_row = row - first_row - phase * 7;
    if (strip_row < 0 || strip_row >= 7) return;
    /* +512 makes division well-defined for the negative left margin. */
    int left = ((int)ppu->hScroll[layer] - FZERO_WIDE_MARGIN + 512) / 8 - 64;
    int right = (ppu->hScroll[layer] + 255 + FZERO_WIDE_MARGIN) / 8;
    for (int tile = left; tile <= right; ++tile) {
        int position = (phase * 256 + tile * 8 + period) % period;
        int source = base + (first_row + (position / 256) * 7 + strip_row) * 32 +
                     (position % 256) / 8;
        unsigned column = (unsigned)tile & 63;
        int destination = base + (column / 32) * 1024 + row * 32 + column % 32;
        copy->vram[destination] = ppu->vram[source];
    }
}

/* The racing OAM layout assigns six eight-piece vehicle groups to 68..115,
 * followed by shadows through slot 127. Full native uploads have a separate
 * side-rendering path. Hints distinguish right-side pieces from the
 * hardware's signed, nine-bit offscreen coordinates. */
static void ExtendVehicleSprites(Ppu *copy) {
    uint8_t hints[16] = {0};
    for (int slot = 0; slot < 128; ++slot) {
        int index = slot * 2;
        int shift = index & 7;
        unsigned high = (copy->highOam[index >> 3] >> shift) & 3;
        bool vehicle = slot >= 68;
        /* Empty vehicle and shadow entries use this parked coordinate.
         * A large sprite there could otherwise reach the far left margin. */
        bool parked = copy->oam[index] == 0x8080 && (high & 1);
        /* Unused pieces in an active vehicle group have Y=0. Their remaining
         * attributes can be stale, including an X high bit that would turn a
         * hidden entry into a fragment at the upper right edge. */
        bool unused = slot < 116 && (copy->oam[index] >> 8) == 0;
        if (vehicle && !parked && !unused) {
            hints[slot >> 3] |= 1u << (slot & 7);
        } else {
            /* With no right hint, x=256 decodes to -256. Unlike hiding by Y,
             * this cannot wrap back onto the top scanlines. */
            copy->oam[index] &= 0xff00;
            copy->highOam[index >> 3] |= 1u << shift;
        }
    }
    PpuWsSetOamLeftHints(copy, hints);
    PpuWsSetOamRightHints(copy, hints);
    /* Extra side pieces must not consume the authentic sprite budget.
     * Only this disposable side pass bypasses the hardware limits. */
    copy->renderFlags |= kPpuRenderFlags_NoSpriteLimits;
}

/* Intro and result layouts use sprites and BG3 for lettering and counters.
 * Use the visible source so hidden text does not mask the scenery above it. */
static bool TextOverlayPixel(const Ppu *ppu, int x, bool native_oam) {
    unsigned layer = (ppu->bgBuffers[0].data[x + kPpuExtraLeftRight] >> 8) & 15;
    /* Source 6 is OBJ palettes 0..3, which bypass SNES colour math. */
    return layer == kPpuOverlaySource_Bg3 ||
        (native_oam && (layer == kPpuOverlaySource_Obj || layer == 6));
}

/* Side backgrounds and vehicles are rendered on a copy. The authentic centre
 * and live sprite evaluation retain their original width and hardware limits.
 * Capture also runs with widescreen off so a paused menu can switch immediately. */
static inline void CopyPpuExceptMode2(Ppu *dst, const Ppu *src) {
    memcpy(dst, src, offsetof(Ppu, wsMode2Capture));
    const uint8_t *src_bytes = (const uint8_t *)src;
    uint8_t *dst_bytes = (uint8_t *)dst;
    size_t start = offsetof(Ppu, wsLayerClamp);
    size_t end = offsetof(Ppu, vram);
    memcpy(dst_bytes + start, src_bytes + start, end - start);
}

static void BuildWideLine(FZeroLayers *layers, const Ppu *ppu, int line,
                          bool supported, bool hud_layout) {
    int y = line - 1;
    uint32_t *world = layers->wide_world[y], *hud = layers->wide_hud[y];
    memset(world, 0, sizeof(layers->wide_world[y]));
    for (int x = 0; x < FZERO_WIDE_WIDTH; ++x) hud[x] = 0xff000000;
    if (supported) {
        Ppu *copy = &layers->scratch;
        CopyPpuExceptMode2(copy, ppu);
        memcpy(copy->vram, ppu->vram, sizeof(copy->vram));
        copy->renderBuffer = (uint8_t *)layers->wide_capture;
        copy->renderPitch = sizeof(layers->wide_capture[0]);
        PpuClearOverlayBindings(copy);
        PpuSetExtraSpace(copy, FZERO_WIDE_MARGIN);
        PpuSetWidescreenLayerClamp(copy, y < 48 ? 4 : 0);
        if (PPU_mode(ppu) == 1) {
            ExtendPanorama(copy, ppu, line, 0, layers->intro_panorama);
            ExtendPanorama(copy, ppu, line, 1, layers->intro_panorama);
        }
        if (layers->native_oam) {
            /* Full-screen effects use the native list, not racing vehicle
             * groups. Preserve its signed coordinates and current artwork. */
            uint8_t hints[16] = {0};
            PpuWsSetOamLeftHints(copy, hints);
            PpuWsSetOamRightHints(copy, hints);
            copy->renderFlags |= kPpuRenderFlags_NoSpriteLimits;
        } else if (layers->vehicles.ready) FZeroVehiclesApply(&layers->vehicles, copy);
        else ExtendVehicleSprites(copy);
        if (!FZeroGroundRenderLine(&layers->ground, copy, line)) ppu_runLine(copy, line);
        for (int x = 0; x < FZERO_WIDE_WIDTH; ++x) {
            if (x >= FZERO_WIDE_MARGIN && x < FZERO_WIDE_MARGIN + FZERO_NATIVE_WIDTH) continue;
            /* A full-centre fallback must also protect the sides. Otherwise
             * Enhanced grades only the margins and exposes the old viewport. */
            if (hud_layout || ((layers->intro_panorama || layers->results_layout) &&
                !TextOverlayPixel(copy, x - FZERO_WIDE_MARGIN, layers->native_oam))) {
                world[x] = layers->wide_capture[y][x];
                hud[x] = 0;
            } else {
                hud[x] = layers->wide_capture[y][x] | 0xff000000u;
            }
        }
        ++layers->wide_lines;
    }
    memcpy(world + FZERO_WIDE_MARGIN, layers->world[y], sizeof(layers->world[y]));
    memcpy(hud + FZERO_WIDE_MARGIN, layers->hud[y], sizeof(layers->hud[y]));
}

static void HideHudSlot(Ppu *copy, unsigned slot) {
    unsigned shift=(slot&3)*2;
    copy->oam[slot*2] &= 0xff00;
    copy->highOam[slot/4] |= 1u<<shift; /* Signed X=-256, never a Y wrap. */
}

/* Move only the racing instrumentation. Messages and repair sprites occupy
 * separate slots and retain their original positions and colour protection. */
static void MoveWideHud(FZeroLayers *layers, const Ppu *ppu, int line, bool protect_scene) {
    int y=line-1;
    bool native_text=layers->native_oam &&
        (layers->intro_panorama || layers->results_layout);
    bool gp_bottom=!native_text && layers->results_layout && y>=48;
    bool slots[52]={0};
    for(unsigned slot=20;slot<52;++slot) {
        int x=ppu->oam[slot*2]&255;
        if(ppu->highOam[slot/4] & (1u<<((slot&3)*2))) x-=256;
        /* The ending first retains the map, markers and counters, then
         * reuses their slots for the central results table. Racing corner
         * groups start outside x=48..183; results lettering stays inside. */
        slots[slot]=slot!=47 && (!layers->crash_layout || slot<48) &&
            (!gp_bottom || x<48 || x>=184);
    }
    bool tail[2]={layers->native_oam && !layers->crash_layout,
                  layers->native_oam && !layers->crash_layout};
    bool move[256]={0}, centred[256]={0};
    if(native_text) {
        /* Intro, race results and the crashed-out page share a reduced HUD.
         * Practice course selection instead uses the tail for map pieces.
         * Only tail sprites in the lower-right counter area are lives. */
        CaptureSprites(layers,ppu,line,0,126,centred);
        for(unsigned slot=126;slot<128;++slot) {
            unsigned position=ppu->oam[slot*2];
            unsigned x=position&255, y=position>>8;
            bool offscreen=ppu->highOam[slot/4] & (1u<<((slot&3)*2));
            tail[slot-126]=!offscreen && x>=192 && y>=184 && y<216;
            CaptureSprites(layers,ppu,line,slot,1,tail[slot-126]?move:centred);
        }
    } else CaptureSprites(layers,ppu,line,0,20,centred);
    if(gp_bottom) {
        /* Capture and remove the same selected groups. Keep result lettering
         * in the clean scene beneath an instrument, including equal colours. */
        for(int first=20;first<52;) {
            int end=first+1;
            while(end<52 && slots[end]==slots[first]) ++end;
            CaptureSprites(layers,ppu,line,first,end-first,slots[first]?move:centred);
            first=end;
        }
        CaptureSprites(layers,ppu,line,52,16,centred);
    } else if(!native_text) CaptureHudSprites(layers,ppu,line,20,move);
    bool top=PPU_mode(ppu)==1 && (native_text ?
        layers->results_layout && y<32 : y<48);
    bool power_band=!native_text && top && y>=18 && y<30;
    bool power_math=power_band && y<28;
    bool any=power_band;
    for(int x=0;x<256;++x) {
        unsigned layer=(ppu->bgBuffers[0].data[x+kPpuExtraLeftRight]>>8)&15;
        if(top && layer==2 && (!native_text || x<64)) move[x]=true;
        if(centred[x]) move[x]=false;
        any |= move[x];
    }
    if(!any) return;

    Ppu *copy=&layers->scratch;
    /* Re-render the live scene beneath the old HUD. No shifted screenshot or
     * neighbouring pixel can recover a car or track detail hidden by it. */
    CopyPpuExceptMode2(copy, ppu);
    copy->renderVram = (uint16_t *)PpuRenderVram(ppu);
    copy->renderBuffer=(uint8_t*)layers->capture;
    copy->renderPitch=sizeof(layers->capture[0]);
    PpuClearOverlayBindings(copy);
    if(!native_text)
        for(unsigned slot=20;slot<52;++slot) if(slots[slot]) HideHudSlot(copy,slot);
    for(unsigned slot=126;slot<128;++slot)
        if(tail[slot-126]) HideHudSlot(copy,slot);
    if(top) {
        copy->screenEnabled[0] &= ~4;
        copy->screenEnabled[1] &= ~4;
    }
    if(power_math) {
        /* The meter fill is produced by a colour window over the sky.
         * Remove that operation only in this clean-scene pass. */
        copy->cgwsel &= 15;
        copy->cgadsub=0;
        copy->fixedColor=0;
    }
    ppu_runLine(copy,line);
    const uint32_t *original=(const uint32_t*)(ppu->renderBuffer+y*ppu->renderPitch);
    uint32_t *world=layers->wide_world[y], *hud=layers->wide_hud[y];
    for(int x=0;x<256;++x) {
        bool old_meter=power_band && x>=174 && x<242 && !centred[x];
        if(old_meter && power_math && x>=ppu->window1left && x<=ppu->window1right)
            move[x]=true;
        if(!move[x] && !old_meter) continue;
        bool protected_pixel=protect_scene ||
            (native_text && TextOverlayPixel(copy,x,true));
        world[x+FZERO_WIDE_MARGIN]=protected_pixel?0:layers->capture[y][x];
        hud[x+FZERO_WIDE_MARGIN]=protected_pixel?(layers->capture[y][x]|0xff000000u):0;
    }
    /* Clear all sources before drawing destinations: the two regions can
     * overlap when a wide HUD group extends back into the original centre. */
    for(int x=0;x<256;++x) if(move[x]) {
        int destination=x+(x<128?0:2*FZERO_WIDE_MARGIN);
        hud[destination]=original[x]|0xff000000u;
    }
}

void FZeroLayersProcessLine(FZeroLayers *layers, const Ppu *ppu, int line,
                            bool racing, bool hud_layout) {
    if (line < 1 || line > FZERO_LAYER_HEIGHT) return;
    int y = line - 1;
    const uint32_t *original = (const uint32_t *)(ppu->renderBuffer + y * ppu->renderPitch);
    bool supported = racing && (PPU_mode(ppu) == 1 || PPU_mode(ppu) == 7) &&
        !PPU_forcedBlank(ppu) && !ppu->extraLeftRight && !PPU_objInterlace(ppu) &&
        (ppu->renderFlags & kPpuRenderFlags_NewRenderer);
    bool mask[FZERO_LAYER_WIDTH] = {0};
    bool text_layout = layers->intro_panorama || layers->results_layout;
    if (supported && text_layout) {
        /* Keep the selected filter through intro and results. The GP ending
         * retains racing vehicles; protect its text slots but not those cars.
         * Only the GP ending retains the racing power-window mask. */
        if (!layers->native_oam) {
            CaptureSprites(layers, ppu, line, 0, 68, mask);
        }
        for (int x = 0; x < FZERO_LAYER_WIDTH; ++x)
            mask[x] |= TextOverlayPixel(ppu, x, layers->native_oam);
        if (layers->move_hud && !layers->native_oam &&
            PPU_mode(ppu) == 1 && y >= 18 && y < 30)
            for (int x = 174; x < 242; ++x) mask[x] = true;
    } else if (supported && hud_layout) {
        /* Racing messages, map, times and counters. Exhaust, sparks, vehicles
         * and all twelve shadow slots remain in the scene. */
        CaptureHudSprites(layers, ppu, line, 0, mask);
        for (int x = 0; x < FZERO_LAYER_WIDTH; ++x) {
            unsigned layer = (ppu->bgBuffers[0].data[x + kPpuExtraLeftRight] >> 8) & 15;
            if (y < 48 && PPU_mode(ppu) == 1 && layer == 2) mask[x] = true;
            /* The power fill is a colour-window effect, not a tile layer. */
            if (y >= 18 && y < 30 && x >= 174 && x < 242) mask[x] = true;
        }
    } else {
        /* Menus and unclassified HUD layouts retain Original
         * in the centre. Background expansion has a separate eligibility. */
        memset(mask, 1, sizeof(mask));
        ++layers->protected_lines;
    }
    unsigned pixels = 0;
    for (int x = 0; x < FZERO_LAYER_WIDTH; ++x) {
        layers->hud[y][x] = mask[x] ? original[x] | 0xff000000u : 0;
        layers->world[y][x] = mask[x] ? 0 : original[x];
        pixels += mask[x];
    }
    BuildWideLine(layers, ppu, line, supported, hud_layout);
    if(supported && layers->move_hud)
        MoveWideHud(layers,ppu,line,!hud_layout && !text_layout);
    if (supported && (hud_layout || text_layout) && pixels) ++layers->extracted_lines;
    layers->hud_pixels += pixels;
}
