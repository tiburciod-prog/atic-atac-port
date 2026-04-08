# Atic Atac Port — Execution Plan (Waves 1-5)

## Rules for This Plan

1. **Wave 1** = bug fixes, delegate ALL to Nord in parallel, no new data needed
2. **Wave 2** = data extraction from ROM, Claudia does these herself (NOT Nord), outputs are C arrays
3. **Waves 3-5** = features, delegate to Nord one task at a time, sequential within each wave
4. Each task = one commit, conventional commit format
5. Follow ALL delegation gates in DELEGATION.md — especially Gate 0 (positive mechanics), Gate 0b (anchor uniqueness), Gate 1 (asset check), Gate 2 (research check)
6. For every Nord brief: paste 5-10 lines of surrounding code context, never say "read file X"
7. If Nord returns STATUS=blocked or produces placeholders, diagnose and fix the brief before re-delegating
8. Run `grep -c "anchor_line" atic_atac.c` for every oldText anchor before sending to Nord
9. After every Nord commit: `make clean && make 2>&1` + `SDL_VIDEODRIVER=offscreen ./atic_atac --headless` must pass

## Source File

`/home/tiburcio/projects/atic-atac-port/atic_atac.c` — currently 1629 lines, builds clean.

## Reference Doc

`/home/tiburcio/.openclaw/workspace/skills/zx-spectrum-port/references/atic-atac.md` — all game data specs are here. Slice relevant sections into each Nord brief.

---

## WAVE 1: Bug Fixes (delegate all 5 to Nord in parallel, no dependencies)

### Task 1a: Fix ACG key graphic IDs

The code checks for graphic `0x5C` (Spider) instead of `0x8C`/`0x8D`/`0x8E` (the three ACG key pieces). The win condition can never trigger.

**Change 1** — `render_items()` around line 1266:

Current:
```c
if (e->graphic != 0x5C) continue;
```
Replace with:
```c
if (e->graphic < 0x8C || e->graphic > 0x8E) continue;
```

**Change 2** — `check_item_pickup()` around line 1278:

Current:
```c
if (e->graphic != 0x5C) continue;
```
Replace with:
```c
if (e->graphic < 0x8C || e->graphic > 0x8E) continue;
```

Anchor uniqueness: `e->graphic != 0x5C` appears exactly 2 times in the file (both in these functions). Safe to use.

**Commit:** `fix(items): use correct ACG key graphic IDs ($8C-$8E not $5C)`

---

### Task 1b: Fix score-after-swap bug

In `update_weapon()`, line ~1189 reads `gs->creatures[i].graphic` AFTER the creature was swapped out at lines 1177-1178. It reads the wrong creature's type for scoring.

**Context** (around line 1175):
```c
        if (dx*dx + dy*dy < 144) {  /* 12px hit radius */
            /* Kill creature: remove by swapping with last */
            gs->creatures[i] = gs->creatures[gs->num_creatures - 1];
            gs->num_creatures--;
            gs->weapon_active = 0;
            /* Per-creature score (type from graphic & 0x1F) */
            {
                static const uint32_t score_table[22] = {
```

**Replace** the block from `/* Kill creature` through the score read:
```c
            /* Kill creature: save type before swapping */
            uint8_t killed_graphic = gs->creatures[i].graphic;
            gs->creatures[i] = gs->creatures[gs->num_creatures - 1];
            gs->num_creatures--;
            gs->weapon_active = 0;
            /* Per-creature score (type from graphic & 0x1F) */
            {
                static const uint32_t score_table[22] = {
                    100,100,100,100,
                    150,150,150,150,
                    200,200,200,200,
                    250,250,250,250,
                    300,300,300,300,300,300
                };
                uint8_t ctype = killed_graphic & 0x1Fu;
                if (ctype > 21u) ctype = 0;
                gs->score_val += score_table[ctype];
            }
```

Anchor: `/* Kill creature: remove by swapping with last */` — unique in file.

**Commit:** `fix(score): save creature graphic before swap to get correct score value`

---

### Task 1c: Fix creature damage floor

In `update_creatures()`, line ~980, `if (gs->energy >= 32) gs->energy -= 32` means energy can never reach 0 from creature contact (floors at 31). Only the natural drain can kill.

**Current** (around line 978):
```c
        /* Damage player on contact (within 8px) */
        if (dx*dx + dy*dy < 64) {
            if ((gs->frame % 16) == 0 && gs->energy >= 32)
                gs->energy -= 32;
        }
```

**Replace with:**
```c
        /* Damage player on contact (within 8px) */
        if (dx*dx + dy*dy < 64) {
            if ((gs->frame % 16) == 0 && gs->energy > 0) {
                if (gs->energy > 32)
                    gs->energy -= 32;
                else
                    gs->energy = 0;
            }
        }
```

Anchor: `/* Damage player on contact (within 8px) */` — unique in file.

**Commit:** `fix(creatures): allow creature damage to kill player (energy can reach 0)`

---

### Task 1d: Use creature type table

In `spawn_creature()`, line ~953, all creatures spawn as Spider (`0x5C`). The original uses a 16-entry type table at $8B7A, indexed by `FRAMES & 0x0F`.

**Current** (around line 950):
```c
static void spawn_creature(GameState *gs) {
    if (gs->num_creatures >= 3) return;
    Entity *e = &gs->creatures[gs->num_creatures++];
    e->graphic = 0x5C;  /* Spider type */
    e->room    = gs->current_room;
    e->x       = 0x58;  /* room centre */
    e->y       = 0x68;
    e->attr    = 0x44;  /* bright green */
    e->vx      = 0;
    e->vy      = 0;
    e->flags   = 0;
}
```

**Replace with:**
```c
static void spawn_creature(GameState *gs) {
    if (gs->num_creatures >= 3) return;
    /* 16-entry creature type table from ROM $8B7A */
    static const uint8_t creature_table[16] = {
        0x5C, 0x5E, 0x98, 0x98,  /* Spider, Spikey, Bat, Bat      */
        0x90, 0x90, 0x94, 0x94,  /* Witch, Witch, Monk, Monk      */
        0x5C, 0x5E, 0x60, 0x62,  /* Spider, Spikey, Blob, Ghoul   */
        0x4C, 0x4E, 0x68, 0x6A,  /* Pumpkin, Ghostlet, Ghost, Batlet */
    };
    Entity *e = &gs->creatures[gs->num_creatures++];
    e->graphic = creature_table[gs->frame & 0x0F];
    e->room    = gs->current_room;
    e->x       = 0x58;  /* room centre */
    e->y       = 0x68;
    e->attr    = 0x44;  /* bright green */
    e->vx      = 0;
    e->vy      = 0;
    e->flags   = 0;
}
```

Anchor: `e->graphic = 0x5C;  /* Spider type */` — unique in file.

**Commit:** `fix(creatures): use 16-entry creature type table from ROM $8B7A`

---

### Task 1e: Weapon wall bounce + lifetime

The weapon currently flies straight and deactivates off-screen. In the original, weapons bounce off room walls and have a 48-frame lifetime.

**Step 1 — Add `weapon_life` to GameState.** Find:
```c
    /* Weapon state */
    int     weapon_active;  /* 1 = axe in flight */
    uint8_t weapon_room;    /* room axe was fired in */
```
Replace with:
```c
    /* Weapon state */
    int     weapon_active;  /* 1 = axe in flight */
    uint8_t weapon_room;    /* room axe was fired in */
    uint8_t weapon_life;    /* frames remaining (0 = deactivate) */
```

**Step 2 — Set lifetime in `fire_weapon()`.** Find:
```c
    gs->weapon.vx = dvx[gs->walk_dir];
    gs->weapon.vy = dvy[gs->walk_dir];
}
```
(This is the closing of `fire_weapon()`.) Insert before the `}`:
```c
    gs->weapon_life = 48;  /* 48-frame lifetime */
```

**Step 3 — Replace `update_weapon()` deactivation logic.** Find:
```c
    gs->weapon.x = (uint8_t)((int)gs->weapon.x + gs->weapon.vx);
    gs->weapon.y = (uint8_t)((int)gs->weapon.y + gs->weapon.vy);

    /* Deactivate if off-screen (ZX screen 0-255 x 0-191) */
    if (gs->weapon.x > 240 || gs->weapon.y > 180) {
        gs->weapon_active = 0;
        return;
    }
```
Replace with:
```c
    gs->weapon.x = (uint8_t)((int)gs->weapon.x + gs->weapon.vx);
    gs->weapon.y = (uint8_t)((int)gs->weapon.y + gs->weapon.vy);

    /* Lifetime countdown */
    if (gs->weapon_life > 0) gs->weapon_life--;
    if (gs->weapon_life == 0) { gs->weapon_active = 0; return; }

    /* Bounce off room walls */
    {
        const RoomStyle *rs = get_room_style(gs->current_room, NULL);
        int cx = 0x58, cy = 0x68;
        int wl = cx - rs->w + 4, wr = cx + rs->w - 4;
        int wt = cy - rs->h + 4, wb = cy + rs->h - 4;
        if ((int)gs->weapon.x <= wl || (int)gs->weapon.x >= wr)
            gs->weapon.vx = -gs->weapon.vx;
        if ((int)gs->weapon.y <= wt || (int)gs->weapon.y >= wb)
            gs->weapon.vy = -gs->weapon.vy;
    }
```

Anchor for Step 3: `/* Deactivate if off-screen (ZX screen 0-255 x 0-191) */` — unique in file.

**Commit:** `fix(weapon): add wall bounce and 48-frame lifetime per original game`

---

## WAVE 2: Data Extraction (Claudia does these herself — NOT Nord)

All outputs are C arrays pasted into subsequent Nord briefs. Use a Python script to load `atic_atac_full.z80`, decompress, and extract.

### Task 2a: Extract door/decoration/item sprites from ROM

**Sprite pointer table at $A4BE:** array of 16-bit LE pointers, indexed by `(graphic_id - 1) * 2`.
Each sprite: 1 byte = height, then `height * 2` bytes of 1bpp pixel data (16px wide).

Extract and output as C arrays for these graphic IDs:
- Doors: $01, $02, $03 (normal), $08-$0B (locked), $20-$23 (open/closed variants)
- Decorations: $10 (clock), $11-$12 (pictures), $15 (trophies), $16 (picture), $17 (bookcase), $18-$19 (trapdoors), $1A (barrel), $1C (shields), $1D-$1E (decorations)
- Items: $80-$8B (leaf, key, wine, coin, wing, whip, frog, jewel, money, skull, crucifix, spanner)
- ACG keys: $8C, $8D, $8E

Format each as:
```c
static const uint8_t sprite_XX[HEIGHT][2] = { ... };
```

Also output a lookup table:
```c
static const struct { const uint8_t (*data)[2]; int height; } sprite_table[] = { ... };
```

### Task 2b: Extract Wizard and Serf sprites from ROM

- Wizard graphic base = $11, 16 frames (4 dirs x 4 anim)
- Serf graphic base = $21, 16 frames
- Use sprite pointer table at $A4BE: pointer for graphic $11 = entry at $A4BE + ($11-1)*2 = $A4DE
- Each frame: look up pointer, read 1 byte height + height*2 bytes data
- 4 directions x 4 frames = graphic IDs $11-$20 for Wizard, $21-$30 for Serf

Output as:
```c
static const uint8_t wizard_sprites[16][18][2] = { ... };
static const uint8_t serf_sprites[16][18][2] = { ... };
```

Replace the placeholder `WIZ_FRAME` / `SERF_FRAME` macros currently in the file.

### Task 2c: Generate letter font (A-Z + space)

Option A (preferred): Extract ZX ROM charset from $3D00 in the snapshot.
- 96 characters, 8 bytes each (8x8 monochrome), ASCII 32-127
- But $3D00 is below $4000 (ROM area) so may not be in the snapshot RAM

Option B: Create a minimal 4x6 bitmap font for A-Z (26 glyphs), matching the existing `digit_font` format:
```c
static const uint8_t letter_font[26][6] = {
    {0x6,0x9,0xF,0x9,0x9,0x0}, /* A */
    ...
};
```

Claudia generates this data. Do NOT delegate font generation to Nord.

### Task 2d: Extract chicken energy bar sprite

In the original, `draw_chicken` at $8B8A draws a chicken sprite that shrinks proportionally to energy.
- Find the chicken sprite data near $8B8A in the disassembly
- Extract as a C array
- Document how the original clips it based on `energy >> 3`

---

## WAVE 3: Visual Fidelity (Nord — sequential, depends on Wave 2 output)

### Task 3a: Real door sprites

Using door sprite arrays from Task 2a, replace the rectangle rendering in `render_decorations()` (lines 993-1013) with `draw_sprite()` calls.

**Context to paste in brief** (current render_decorations door code):
```c
if (e->graphic >= 0x01 && e->graphic <= 0x03) {
    SDL_SetRenderDrawColor(ren, 200, 200, 200, 255);
    SDL_Rect dr = { e->x * SCALE, e->y * SCALE, 16 * SCALE, 16 * SCALE };
    SDL_RenderDrawRect(ren, &dr);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_Rect inner = { (e->x + 3) * SCALE, (e->y + 2) * SCALE, 10 * SCALE, 14 * SCALE };
    SDL_RenderFillRect(ren, &inner);
    continue;
}
```

Replace with `draw_sprite(ren, door_sprite_XX, height, e->x, e->y, e->attr, 1)` using the appropriate sprite for the graphic ID.

Same for locked doors ($08-$0B) — replace the double-rectangle outline with real sprites.

**Commit:** `feat(render): real door sprites from ROM sprite table`

### Task 3b: Real decoration sprites

Using decoration sprite arrays from Task 2a, replace the 8x8 colour square rendering in `render_decorations()` (lines 1015-1023):

```c
if (e->graphic < 0x10 || e->graphic >= 0x80) continue;
/* ... */
SDL_Rect r = { e->x * SCALE, e->y * SCALE, 8 * SCALE, 8 * SCALE };
SDL_RenderFillRect(ren, &r);
```

Replace with a lookup into the sprite table and `draw_sprite()` call. For graphic IDs that don't have an extracted sprite, keep the coloured square as fallback.

**Commit:** `feat(render): real decoration sprites (clocks, shields, bookcases, etc.)`

### Task 3c: Real item rendering

Add rendering for collectible items ($80-$8E) in rooms. These are entities in the room entity list with graphic IDs $80-$8E. Use extracted item sprites from Task 2a.

Update `render_items()` to handle all item types, not just ACG keys.

**Commit:** `feat(render): render collectible items with ROM sprites`

### Task 3d: Real Wizard/Serf sprites

Replace the placeholder `WIZ_FRAME` / `SERF_FRAME` macro arrays (lines 676-703) with the real extracted data from Task 2b. Paste the complete arrays in the brief.

**Commit:** `feat(sprites): real Wizard and Serf sprites from ROM`

### Task 3e: Text rendering

Using font from Task 2c, add a `draw_text()` function and replace placeholder rectangles in `show_char_select()`:
- Title "ATIC ATAC" — currently outline rectangles (line 1067-1072)
- "SELECT CHARACTER" — currently grey bar (line 1075-1077)
- Character names "KNIGHT"/"WIZARD"/"SERF" — currently grey bars (line 1103-1106)

Also add text to game over screen (currently just a red overlay).

**Commit:** `feat(ui): real text rendering with bitmap font`

---

## WAVE 4: Gameplay Features (Nord — sequential, include reference doc slices)

### Task 4a: Food pickup + energy restore

Food items have graphic IDs matching leaf ($80) through skull ($89). When player is within 12px:
- Remove item (zero the graphic in room_entities)
- Add +64 energy, capped at $F0
- Add 100 to score

Slice from reference doc S8:
> Food restore: +64 (all 8 food types), capped at $F0

Add to `check_item_pickup()` or create a new `check_food_pickup()`.

**Commit:** `feat(items): food pickup restores +64 energy`

### Task 4b: Item inventory management

3 inventory slots. When player contacts a collectible item ($80-$8E):
- If inventory has empty slot: pick up item (store graphic + attr)
- If inventory full: drop oldest item at player position, pick up new one
- Keys ($81) consumed when opening a matching locked door (already have `has_key_for_door()`)
- Display inventory items in HUD (bottom area, using item sprites)

Slice from reference doc S1:
> inventory1-3 at $5E30/$5E34/$5E38, 4B each: ptr(2B) + graphic(1B) + attr(1B)

**Commit:** `feat(inventory): 3-slot item pickup, drop, and HUD display`

### Task 4c: Creature spawn delay + pre-spawn animation

Current: creatures appear instantly at room centre. Original has:
- `creature_delay` counter at $5E27 — frames until next spawn attempt
- Sparkle animation before creature becomes active ($8CB7)

Implementation:
1. Add `creature_delay` to GameState, init to 100
2. In `game_tick()`, decrement each frame; only call `spawn_creature()` when it hits 0, then reset to 100-200 (random)
3. New creatures start with a `spawn_timer` field (add to Entity or use flags). For first 30 frames, render as a flashing dot. After that, enable movement and damage.

**Commit:** `feat(creatures): spawn delay timer and pre-spawn animation`

### Task 4d: Secret passages

Each character type can use one kind of passage:
- Knight ($01 base) -> Clock entities ($10)
- Wizard ($11 base) -> Bookcase entities ($17)
- Serf ($21 base) -> Barrel entities ($1A)

Check: in movement code, when player is within 12px of a room entity:
```c
// char_bases: Knight=0x01, Wizard=0x11, Serf=0x21
uint8_t char_base = (gs->char_type == 1) ? 0x11 : (gs->char_type == 2) ? 0x21 : 0x01;
// Secret passage types: Clock=0x10, Bookcase=0x17, Barrel=0x1A
if (entity_graphic == passage_type_for_char) {
    do_room_transition(gs, entity->zx_addr);
}
```

Slice from reference doc S4:
> Secret passage check: Subtracts character base from graphic ID. If result < $10, passage opens.

**Commit:** `feat(gameplay): secret passages (Knight=clock, Wizard=bookcase, Serf=barrel)`

### Task 4e: Boss creatures

5 boss types with unique behavior. Add to `update_creatures()`:

| Boss | Graphic | Damage | Special |
|------|---------|--------|---------|
| Mummy | $70 | 8/hit | Attracted to Leaf ($80) in inventory |
| Dracula | $7C | 8/hit | Repelled by Crucifix ($8A) — runs away |
| Frankenstein | $74 | 8/hit | Killed by Spanner ($8B) -> 1000 pts |
| Devil | $78 | 8/hit | Always chases, no item interaction |
| Hunchback | $9C | 16/hit | Double damage, destroys floor items |

Implementation:
1. In `update_creatures()`, check `e->graphic & 0xF0` — if $70/$90 range, use boss AI
2. Boss AI: roam within 52-60px of room centre ($58, $68)
3. Check inventory for item interactions each frame
4. Boss damage is 8 (not 32 like normal creatures), Hunchback is 16

Slice from reference doc S5 for full details.

**Commit:** `feat(creatures): boss creature AI with item interactions`

### Task 4f: Trapdoor mechanics

Open trapdoor entity ($19): when player contacts it, teleport to the room stored in the linked entity's byte +1 (same door-pair mechanism).

```c
if (e->graphic == 0x19) {
    // Proximity check
    if (dx*dx + dy*dy < 144) {
        do_room_transition(gs, e->zx_addr);
    }
}
```

Add to the door check loop in the main movement code.

**Commit:** `feat(rooms): trapdoor teleportation to linked room`

### Task 4g: ACG key random placement

At game start, select one of 8 room sets for the 3 key pieces:
```c
static const uint8_t key_rooms[8][3] = {
    {0x81, 0x45, 0x7C}, {0x85, 0x49, 0x2B},
    {0x6A, 0x3B, 0x7C}, {0x69, 0x71, 0x2B},
    {0x67, 0x85, 0x7C}, {0x68, 0x7F, 0x2B},
    {0x4D, 0x73, 0x7C}, {0x17, 0x10, 0x2B},
};
int set = gs->frame & 7;  // or use SDL_GetTicks() at init
```

Place key entities ($8C, $8D, $8E) in the selected rooms at game start. This means modifying `init_game()` to write key entities into the room entity data, or maintaining a separate key placement array checked during rendering and pickup.

Slice from reference doc S7 for win condition:
> Inventory slot 1=$8C, slot 2=$8D, slot 3=$8E. Must be in order.

**Commit:** `feat(items): randomized ACG key placement from 8 room sets`

---

## WAVE 5: Polish (Nord — after Waves 3-4)

### Task 5a: Death/spawn animation

When player dies (energy=0): flash the player sprite for ~30 frames before respawning. When game over: display score with text (requires font from Task 2c).

**Commit:** `feat(ui): death flash animation and game over score display`

### Task 5b: Chicken energy bar

Replace the green bar in `render_hud()` with extracted chicken sprite from Task 2d. The chicken is drawn at full size and clipped proportionally: `display_height = (energy / 0xF0) * full_height`. Draw from the bottom up.

**Commit:** `feat(hud): chicken energy bar sprite replacing green bar`

### Task 5c: Room visited tracking

Add `uint8_t visited_rooms[19]` (148 bits = 19 bytes) to GameState. Set the bit for each room entered. Display exploration percentage in HUD.

```c
// On room entry:
gs->visited_rooms[room_id / 8] |= (1 << (room_id & 7));
// Count:
int visited = 0;
for (int i = 0; i < 19; i++)
    for (int b = 0; b < 8; b++)
        if (gs->visited_rooms[i] & (1 << b)) visited++;
int percent = (visited * 100) / 148;
```

**Commit:** `feat(hud): room visited tracking and exploration percentage`

### Task 5d: Sound

Add SDL_mixer. In-game music is a square wave, note table at $858C (64 bytes, looping):
```c
float freq = 3500000.0f / (8.0f * note_value);
```
Use SDL audio callback with `AUDIO_S16SYS`. Generate square wave: `+4000` first half-period, `-4000` second. Advance one note per game frame.

Add to Makefile: `LDFLAGS += -lSDL2_mixer`

**Commit:** `feat(audio): square wave music from ROM note table`

### Task 5e: Score flash timer

When score changes, set `flash_timer = 16`. While flash_timer > 0, alternate score digit colour between white and yellow every 4 frames. Decrement flash_timer each frame.

**Commit:** `feat(hud): score flash animation on point gain`

---

## Execution Summary

```
WAVE 1 (5 bug fixes, Nord parallel)
  1a: ACG key IDs
  1b: score-after-swap
  1c: creature damage floor
  1d: creature type table
  1e: weapon bounce + lifetime
    |
    v
WAVE 2 (4 data extractions, Claudia herself)
  2a: door/decoration/item sprites
  2b: Wizard/Serf sprites
  2c: letter font
  2d: chicken sprite
    |
    v
WAVE 3 (5 visual tasks, Nord sequential)
  3a: real door sprites
  3b: real decoration sprites
  3c: real item rendering
  3d: real Wizard/Serf sprites
  3e: text rendering
    |
    v
WAVE 4 (7 gameplay tasks, Nord sequential)
  4a: food pickup
  4b: item inventory
  4c: creature spawn delay
  4d: secret passages
  4e: boss creatures
  4f: trapdoor mechanics
  4g: ACG key placement
    |
    v
WAVE 5 (5 polish tasks, Nord sequential)
  5a: death animation
  5b: chicken energy bar
  5c: room visited tracking
  5d: sound
  5e: score flash
```

Total: 26 tasks. Wave 1 = ~1 Nord session. Wave 2 = Claudia. Waves 3-5 = ~20 Nord sessions.
