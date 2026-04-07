#!/usr/bin/env python3
"""Phase 16 patch: Wizard/Serf sprites + character-specific weapons."""

EM = "\u2014"   # em-dash as it appears in the source file

SRC = "atic_atac.c"

with open(SRC, "r", encoding="utf-8") as f:
    code = f.read()

# ------------------------------------------------------------------ CHANGE 1
# Insert wizard/serf/fireball/spanner arrays just before render_hud()
# so they are defined before render_player() uses them.
OLD1 = (
    "/*\n"
    " * render_hud() " + EM + " draw energy bar, score, and lives outside the room.\n"
    " */\n"
    "static void render_hud(SDL_Renderer *ren, const GameState *gs) {"
)
NEW1 = (
    "/* Wizard placeholder sprites: 16 frames [dir*4+frame], 18 rows, 2 bytes/row */\n"
    "/* All 16 frames identical placeholder -- robe shape */\n"
    "static const uint8_t wizard_sprites[16][18][2] = {\n"
    "    #define WIZ_FRAME \\\n"
    "        {0x06,0x00},{0x0F,0x00},{0x0F,0x00},{0x06,0x00}, \\\n"
    "        {0x1F,0x80},{0x3F,0xC0},{0x7F,0xE0},{0x7F,0xE0}, \\\n"
    "        {0xFF,0xF0},{0xFF,0xF0},{0x7F,0xE0},{0x3F,0xC0}, \\\n"
    "        {0x1F,0x80},{0x0F,0x00},{0x1B,0x80},{0x31,0x80}, \\\n"
    "        {0x61,0x80},{0x43,0x00}\n"
    "    {WIZ_FRAME},{WIZ_FRAME},{WIZ_FRAME},{WIZ_FRAME},\n"
    "    {WIZ_FRAME},{WIZ_FRAME},{WIZ_FRAME},{WIZ_FRAME},\n"
    "    {WIZ_FRAME},{WIZ_FRAME},{WIZ_FRAME},{WIZ_FRAME},\n"
    "    {WIZ_FRAME},{WIZ_FRAME},{WIZ_FRAME},{WIZ_FRAME},\n"
    "    #undef WIZ_FRAME\n"
    "};\n"
    "\n"
    "/* Serf placeholder sprites: 16 frames, 18 rows, 2 bytes/row */\n"
    "static const uint8_t serf_sprites[16][18][2] = {\n"
    "    #define SERF_FRAME \\\n"
    "        {0x0C,0x00},{0x1E,0x00},{0x1E,0x00},{0x0C,0x00}, \\\n"
    "        {0x3F,0x00},{0x7F,0x80},{0x7F,0x80},{0xFF,0xC0}, \\\n"
    "        {0xFF,0xC0},{0x7F,0x80},{0x3F,0x00},{0x1E,0x00}, \\\n"
    "        {0x3F,0x00},{0x6D,0x80},{0x4D,0x80},{0xC1,0x80}, \\\n"
    "        {0x41,0x80},{0x61,0x80}\n"
    "    {SERF_FRAME},{SERF_FRAME},{SERF_FRAME},{SERF_FRAME},\n"
    "    {SERF_FRAME},{SERF_FRAME},{SERF_FRAME},{SERF_FRAME},\n"
    "    {SERF_FRAME},{SERF_FRAME},{SERF_FRAME},{SERF_FRAME},\n"
    "    {SERF_FRAME},{SERF_FRAME},{SERF_FRAME},{SERF_FRAME},\n"
    "    #undef SERF_FRAME\n"
    "};\n"
    "\n"
    "/* Fireball sprite (Wizard weapon): 8x8 */\n"
    "static const uint8_t fireball_sprite[8][2] = {\n"
    "    {0x18,0x00},{0x3C,0x00},{0x7E,0x00},{0xFF,0x00},\n"
    "    {0xFF,0x00},{0x7E,0x00},{0x3C,0x00},{0x18,0x00},\n"
    "};\n"
    "\n"
    "/* Spanner sprite (Serf weapon): 8x8 */\n"
    "static const uint8_t spanner_sprite[8][2] = {\n"
    "    {0xC3,0x00},{0xE7,0x00},{0x7E,0x00},{0x3C,0x00},\n"
    "    {0x3C,0x00},{0x7E,0x00},{0xE7,0x00},{0xC3,0x00},\n"
    "};\n"
    "\n"
    "/*\n"
    " * render_hud() " + EM + " draw energy bar, score, and lives outside the room.\n"
    " */\n"
    "static void render_hud(SDL_Renderer *ren, const GameState *gs) {"
)

assert OLD1 in code, "CHANGE 1 anchor not found"
code = code.replace(OLD1, NEW1, 1)
print("CHANGE 1 applied")

# ------------------------------------------------------------------ CHANGE 2
# render_player: pick sprite/colour by char_type
OLD2 = (
    "/*\n"
    " * render_player() " + EM + " draw the animated Knight sprite at its current position.\n"
    " * Selects frame based on walk direction and animation cycle.\n"
    " */\n"
    "static void render_player(SDL_Renderer *ren, const GameState *gs) {\n"
    "    static const int walk_cycle[4] = {0, 1, 2, 1};\n"
    "    int sprite_idx = gs->walk_dir * 4 + walk_cycle[gs->walk_frame & 3];\n"
    "    draw_sprite(ren,\n"
    "                knight_sprites[sprite_idx],\n"
    "                18,\n"
    "                gs->player.x,\n"
    "                gs->player.y,\n"
    "                gs->player.attr ? gs->player.attr : 0x47,\n"
    "                1);\n"
    "}"
)
NEW2 = (
    "/*\n"
    " * render_player() " + EM + " draw the animated sprite at its current position.\n"
    " * Selects frame based on walk direction and animation cycle.\n"
    " */\n"
    "static void render_player(SDL_Renderer *ren, const GameState *gs) {\n"
    "    static const int walk_cycle[4] = {0, 1, 2, 1};\n"
    "    int sprite_idx = gs->walk_dir * 4 + walk_cycle[gs->walk_frame & 3];\n"
    "    /* Pick sprite sheet and colour by character type */\n"
    "    const uint8_t (*sheet)[2];\n"
    "    uint8_t attr;\n"
    "    switch (gs->char_type) {\n"
    "        case 1:  sheet = wizard_sprites[sprite_idx]; attr = 0x45; break; /* cyan  */\n"
    "        case 2:  sheet = serf_sprites[sprite_idx];   attr = 0x43; break; /* green */\n"
    "        default: sheet = knight_sprites[sprite_idx]; attr = 0x47; break; /* white */\n"
    "    }\n"
    "    if (gs->player.attr) attr = gs->player.attr;\n"
    "    draw_sprite(ren, sheet, 18, gs->player.x, gs->player.y, attr, 1);\n"
    "}"
)

assert OLD2 in code, "CHANGE 2 anchor not found"
code = code.replace(OLD2, NEW2, 1)
print("CHANGE 2 applied")

# ------------------------------------------------------------------ CHANGE 3
# fire_weapon: weapon attr by char_type
OLD3 = "    gs->weapon.attr   = 0x46;  /* bright yellow */"
NEW3 = (
    "    /* Colour and sprite vary by character */\n"
    "    switch (gs->char_type) {\n"
    "        case 1:  gs->weapon.attr = 0x44; break; /* Wizard: bright cyan  */\n"
    "        case 2:  gs->weapon.attr = 0x42; break; /* Serf:   bright red   */\n"
    "        default: gs->weapon.attr = 0x46; break; /* Knight: bright yellow */\n"
    "    }"
)

assert OLD3 in code, "CHANGE 3 anchor not found"
code = code.replace(OLD3, NEW3, 1)
print("CHANGE 3 applied")

# ------------------------------------------------------------------ CHANGE 4
# render_weapon: pick sprite by char_type
OLD4 = (
    "/*\n"
    " * render_weapon() " + EM + " draw axe if in flight.\n"
    " */\n"
    "static void render_weapon(SDL_Renderer *ren, const GameState *gs) {\n"
    "    if (!gs->weapon_active) return;\n"
    "    if (gs->weapon_room != gs->current_room) return;\n"
    "    draw_sprite(ren, axe_sprite, 8, gs->weapon.x, gs->weapon.y, gs->weapon.attr, 1);\n"
    "}"
)
NEW4 = (
    "/*\n"
    " * render_weapon() " + EM + " draw weapon if in flight.\n"
    " */\n"
    "static void render_weapon(SDL_Renderer *ren, const GameState *gs) {\n"
    "    if (!gs->weapon_active) return;\n"
    "    if (gs->weapon_room != gs->current_room) return;\n"
    "    const uint8_t (*ws)[2];\n"
    "    switch (gs->char_type) {\n"
    "        case 1:  ws = fireball_sprite; break;\n"
    "        case 2:  ws = spanner_sprite;  break;\n"
    "        default: ws = axe_sprite;      break;\n"
    "    }\n"
    "    draw_sprite(ren, ws, 8, gs->weapon.x, gs->weapon.y, gs->weapon.attr, 1);\n"
    "}"
)

assert OLD4 in code, "CHANGE 4 anchor not found"
code = code.replace(OLD4, NEW4, 1)
print("CHANGE 4 applied")

# ------------------------------------------------------------------ write
with open(SRC, "w", encoding="utf-8") as f:
    f.write(code)

print("All changes written successfully.")
