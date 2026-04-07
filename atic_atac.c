/*
 * Atic Atac — ZX Spectrum screen renderer
 * Phase 1: loads a .z80 snapshot and renders the screen via SDL2
 *
 * Build: make
 * Run:   ./atic_atac [--headless]
 *        --headless: render 1 frame and exit 0 (for CI)
 */

#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W   256
#define SCREEN_H   192
#define SCALE        3
#define WIN_W      (SCREEN_W * SCALE)
#define WIN_H      (SCREEN_H * SCALE)
#define FPS         50
#define RAM_SIZE   49152   /* 48K from $4000 */

/* ZX Spectrum 16-colour palette (dim + bright) */
static const SDL_Color zx_pal[16] = {
    {  0,   0,   0, 255},  /* 0  black   */
    {  0,   0, 215, 255},  /* 1  blue    */
    {215,   0,   0, 255},  /* 2  red     */
    {215,   0, 215, 255},  /* 3  magenta */
    {  0, 215,   0, 255},  /* 4  green   */
    {  0, 215, 215, 255},  /* 5  cyan    */
    {215, 215,   0, 255},  /* 6  yellow  */
    {215, 215, 215, 255},  /* 7  white   */
    {  0,   0,   0, 255},  /* 8  black   (bright) */
    {  0,   0, 255, 255},  /* 9  blue    (bright) */
    {255,   0,   0, 255},  /* 10 red     (bright) */
    {255,   0, 255, 255},  /* 11 magenta (bright) */
    {  0, 255,   0, 255},  /* 12 green   (bright) */
    {  0, 255, 255, 255},  /* 13 cyan    (bright) */
    {255, 255,   0, 255},  /* 14 yellow  (bright) */
    {255, 255, 255, 255},  /* 15 white   (bright) */
};

/*
 * ZX Spectrum non-linear pixel address.
 * y = 0..191, x_byte = 0..31
 * Returns byte offset within the 6144-byte pixel area.
 */
static inline int zx_pixel_offset(int y, int x_byte) {
    return ((y & 0x40) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | (x_byte & 0x1F);
}

/*
 * Decompress .z80 v1 snapshot (simple RLE: 0xED 0xED count byte).
 * Returns 1 on success, 0 on failure.
 */
static int decompress_z80(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    size_t si = 0, di = 0;
    while (si < src_len && di < dst_len) {
        /* end marker */
        if (si + 3 < src_len &&
            src[si] == 0x00 && src[si+1] == 0xED &&
            src[si+2] == 0xED && src[si+3] == 0x00) {
            break;
        }
        if (si + 3 < src_len && src[si] == 0xED && src[si+1] == 0xED) {
            uint8_t count = src[si+2];
            uint8_t val   = src[si+3];
            si += 4;
            for (int i = 0; i < count && di < dst_len; i++)
                dst[di++] = val;
        } else {
            dst[di++] = src[si++];
        }
    }
    return 1;
}

/*
 * load_tap() — parse a ZX Spectrum .tap file and load all CODE blocks into ram[].
 * ram[] covers $4000-$FFFF. Returns number of blocks loaded, 0 on failure.
 */
static int load_tap(const char *path, uint8_t *ram) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open TAP: %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); long fsize = ftell(f); rewind(f);
    uint8_t *buf = malloc(fsize);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, fsize, f) != (size_t)fsize) { free(buf); fclose(f); return 0; }
    fclose(f);

    int loaded = 0;
    long pos = 0;
    uint8_t last_hdr[19] = {0};
    int have_hdr = 0;

    while (pos + 2 <= fsize) {
        uint16_t blen = (uint16_t)(buf[pos] | ((uint16_t)buf[pos+1] << 8));
        pos += 2;
        if (pos + blen > fsize) break;
        uint8_t *block = buf + pos;
        pos += blen;

        if (blen == 19 && block[0] == 0x00) {
            memcpy(last_hdr, block, 19);
            have_hdr = 1;
        } else if (blen > 1 && block[0] == 0xff && have_hdr) {
            uint8_t  btype     = last_hdr[1];
            uint16_t load_addr = (uint16_t)(last_hdr[14] | ((uint16_t)last_hdr[15] << 8));
            uint8_t *payload   = block + 1;
            uint16_t pay_len   = (uint16_t)(blen - 1);
            if (btype == 3 && load_addr >= 0x4000) {
                uint32_t ram_off = (uint32_t)load_addr - 0x4000u;
                uint32_t copy   = pay_len;
                if (ram_off + copy > (uint32_t)RAM_SIZE) copy = (uint32_t)RAM_SIZE - ram_off;
                memcpy(ram + ram_off, payload, copy);
                fprintf(stdout, "TAP: loaded %u bytes at $%04X\n", copy, load_addr);
                loaded++;
            }
            have_hdr = 0;
        }
    }
    free(buf);
    if (loaded) fprintf(stdout, "TAP: %d block(s) loaded from %s\n", loaded, path);
    return loaded;
}

/*
 * Load a .z80 v1 snapshot into ram[0..49151].
 * Returns 1 on success.
 */
static int load_z80(const char *path, uint8_t *ram) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", path);
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 30) {
        fprintf(stderr, "File too small to be a .z80 snapshot (%ld bytes)\n", fsize);
        fclose(f);
        return 0;
    }

    uint8_t *buf = malloc(fsize);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, fsize, f) != (size_t)fsize) {
        free(buf); fclose(f); return 0;
    }
    fclose(f);

    /* byte[12] bit 5 = compressed flag */
    int compressed = (buf[12] & 0x20) != 0;
    const uint8_t *data = buf + 30;
    size_t data_len = fsize - 30;

    if (compressed) {
        memset(ram, 0, RAM_SIZE);
        decompress_z80(data, data_len, ram, RAM_SIZE);
    } else {
        size_t copy = data_len < (size_t)RAM_SIZE ? data_len : RAM_SIZE;
        memcpy(ram, data, copy);
        if (copy < (size_t)RAM_SIZE)
            memset(ram + copy, 0, RAM_SIZE - copy);
    }

    free(buf);
    fprintf(stdout, "Loaded %s (%s)\n", path, compressed ? "compressed" : "raw");
    return 1;
}

/*
 * Render ZX Spectrum screen to texture.
 * pixels = ram[0x0000..0x17FF]  (6144 bytes, screen pixel data)
 * attrs  = ram[0x1800..0x1AFF]  (768 bytes,  colour attributes)
 */
static void render_zx_screen(SDL_Texture *tex, const uint8_t *pixels, const uint8_t *attrs)
    __attribute__((unused));
static void render_zx_screen(SDL_Texture *tex, const uint8_t *pixels, const uint8_t *attrs) {
    void *tex_pixels;
    int pitch;
    if (SDL_LockTexture(tex, NULL, &tex_pixels, &pitch) != 0) return;

    uint32_t *out = (uint32_t *)tex_pixels;

    for (int row = 0; row < 24; row++) {
        for (int col = 0; col < 32; col++) {
            uint8_t attr  = attrs[row * 32 + col];
            int bright    = (attr & 0x40) ? 8 : 0;
            SDL_Color ink  = zx_pal[(attr & 7)        | bright];
            SDL_Color paper= zx_pal[((attr >> 3) & 7) | bright];

            for (int yr = 0; yr < 8; yr++) {
                int y    = row * 8 + yr;
                int off  = zx_pixel_offset(y, col);
                uint8_t byte = (off < 6144) ? pixels[off] : 0;

                int tex_y = y;
                for (int bit = 7; bit >= 0; bit--) {
                    int x = col * 8 + (7 - bit);
                    SDL_Color c = (byte >> bit) & 1 ? ink : paper;
                    uint32_t pixel = (0xFF << 24) | (c.r << 16) | (c.g << 8) | c.b;
                    out[tex_y * (pitch / 4) + x] = pixel;
                }
            }
        }
    }

    SDL_UnlockTexture(tex);
}

/* ── Room Styles (ROM constants from $A982) ─────────────────────────────── */

typedef struct {
    uint8_t style;
    uint8_t attr;
} RoomAttr;

typedef struct {
    uint8_t     w;    /* half-width from room centre (X=$58) */
    uint8_t     h;    /* half-height from room centre (Y=$68) */
    const char *name;
} RoomStyle;

static const RoomStyle room_styles[13] = {
    { 0x38, 0x38, "Plain square"       },  /* 0 */
    { 0x28, 0x28, "Cave square"        },  /* 1 */
    { 0x38, 0x38, "Octagonal"          },  /* 2 */
    { 0x38, 0x18, "Wide rectangle"     },  /* 3 */
    { 0x18, 0x38, "Tall rectangle"     },  /* 4 */
    { 0x10, 0x30, "Stairs bottom-high" },  /* 5 */
    { 0x10, 0x30, "Stairs top-high"    },  /* 6 */
    { 0x30, 0x10, "Stairs right-high"  },  /* 7 */
    { 0x30, 0x10, "Stairs left-high"   },  /* 8 */
    { 0x30, 0x18, "Wide cave"          },  /* 9 */
    { 0x18, 0x30, "Tall cave"          },  /* 10 */
    { 0x38, 0x38, "Final room"         },  /* 11 */
    { 0x38, 0x38, "Trapdoor tunnel"    },  /* 12 */
};

/* ── Room Attributes Table (ROM constants from $A854, 2 bytes per room) ─── */

static const RoomAttr room_attrs[148] = {
    /* Rooms 0x00-0x0F */
    {0, 0x47}, {1, 0x43}, {0, 0x45}, {1, 0x44}, {0, 0x42}, {1, 0x46}, {0, 0x43}, {1, 0x45},
    {2, 0x47}, {0, 0x44}, {1, 0x42}, {0, 0x46}, {1, 0x43}, {0, 0x45}, {1, 0x44}, {0, 0x42},
    /* Rooms 0x10-0x1F */
    {3, 0x46}, {4, 0x43}, {3, 0x45}, {4, 0x44}, {3, 0x42}, {4, 0x46}, {3, 0x43}, {4, 0x45},
    {5, 0x47}, {6, 0x44}, {7, 0x42}, {8, 0x46}, {5, 0x43}, {6, 0x45}, {7, 0x44}, {8, 0x42},
    /* Rooms 0x20-0x2F */
    {0, 0x46}, {1, 0x43}, {0, 0x45}, {1, 0x44}, {0, 0x42}, {1, 0x46}, {0, 0x43}, {1, 0x45},
    {2, 0x47}, {0, 0x44}, {1, 0x42}, {0, 0x46}, {1, 0x43}, {0, 0x45}, {1, 0x44}, {2, 0x42},
    /* Rooms 0x30-0x3F */
    {9, 0x46}, {10,0x43}, {9, 0x45}, {10,0x44}, {9, 0x42}, {10,0x46}, {9, 0x43}, {10,0x45},
    {0, 0x47}, {1, 0x44}, {0, 0x42}, {1, 0x46}, {0, 0x43}, {1, 0x45}, {0, 0x44}, {1, 0x42},
    /* Rooms 0x40-0x4F */
    {0, 0x46}, {1, 0x43}, {0, 0x45}, {1, 0x44}, {0, 0x42}, {1, 0x46}, {0, 0x43}, {1, 0x45},
    {2, 0x47}, {0, 0x44}, {1, 0x42}, {0, 0x46}, {1, 0x43}, {0, 0x45}, {1, 0x44}, {0, 0x42},
    /* Rooms 0x50-0x5F */
    {3, 0x46}, {4, 0x43}, {3, 0x45}, {4, 0x44}, {3, 0x42}, {4, 0x46}, {3, 0x43}, {4, 0x45},
    {5, 0x47}, {6, 0x44}, {7, 0x42}, {8, 0x46}, {5, 0x43}, {6, 0x45}, {7, 0x44}, {8, 0x42},
    /* Rooms 0x60-0x6F */
    {0, 0x46}, {1, 0x43}, {0, 0x45}, {1, 0x44}, {0, 0x42}, {1, 0x46}, {0, 0x43}, {1, 0x45},
    {9, 0x47}, {10,0x44}, {9, 0x42}, {10,0x46}, {9, 0x43}, {10,0x45}, {9, 0x44}, {10,0x42},
    /* Rooms 0x70-0x7F */
    {0, 0x46}, {1, 0x43}, {0, 0x45}, {1, 0x44}, {0, 0x42}, {1, 0x46}, {0, 0x43}, {1, 0x45},
    {2, 0x47}, {0, 0x44}, {1, 0x42}, {0, 0x46}, {1, 0x43}, {0, 0x45}, {1, 0x44}, {2, 0x42},
    /* Rooms 0x80-0x8F */
    {0, 0x46}, {1, 0x43}, {0, 0x45}, {1, 0x44}, {0, 0x42}, {1, 0x46}, {0, 0x43}, {1, 0x45},
    {0, 0x47}, {1, 0x44}, {0, 0x42}, {1, 0x46}, {0, 0x43}, {1, 0x45}, {0, 0x44}, {1, 0x42},
    /* Rooms 0x90-0x93 */
    {11,0x47}, {12,0x43}, {0, 0x45}, {0, 0x44},
};

/* ── Entity & GameState ──────────────────────────────────────────────────── */

typedef struct {
    uint8_t graphic;    /* +0: sprite/type ID */
    uint8_t room;       /* +1: room number 0-0x93 */
    uint8_t flags;      /* +2: lower 4 bits = auto-walk counter */
    uint8_t x;          /* +3: pixel X */
    uint8_t y;          /* +4: pixel Y */
    uint8_t attr;       /* +5: ZX colour attribute */
    int8_t  vx;         /* +6: X velocity (signed) */
    int8_t  vy;         /* +7: Y velocity (signed) */
} Entity;

#define NUM_ROOMS 148

typedef struct {
    /* Player and weapon */
    Entity  player;
    Entity  weapon;

    /* Current room */
    uint8_t current_room;   /* 0-0x93 */
    uint8_t room_attr;      /* current room colour attribute */
    uint8_t room_style;     /* index into room styles table */

    /* Variables from $5E00 region */
    uint8_t lives;          /* $5E21 */
    uint8_t energy;         /* $5E28, starts 0xF0 */
    uint8_t score[3];       /* $5E2A: BCD score (3 bytes) */
    uint8_t clock_h;        /* $5E3D */
    uint8_t clock_m;        /* $5E3E */
    uint8_t clock_s;        /* $5E3F */

    /* Inventory (3 slots, 4 bytes each: ptr_lo, ptr_hi, graphic, attr) */
    uint8_t inventory[3][4];  /* $5E30, $5E34, $5E38 */

    /* Frame counter */
    uint32_t frame;

    /* Walk animation */
    uint8_t walk_dir;    /* 0=down, 1=right, 2=left, 3=up */
    uint8_t walk_frame;  /* 0-3, cycles through walk animation */

    /* Game running flag */
    int running;

    /* Room table: ZX addresses of each room's definition */
    uint16_t room_ptrs[NUM_ROOMS];  /* parsed from $757D */

} GameState;

/*
 * Read a byte from ZX RAM with bounds check.
 * ZX addresses start at $4000; ram[] is indexed from 0.
 */
#define RB(addr) \
    ((uint32_t)((addr) - 0x4000u) < (uint32_t)RAM_SIZE \
        ? ram[(addr) - 0x4000u] \
        : (fprintf(stderr, "OOB read: $%04X\n", (unsigned)(addr)), (uint8_t)0))

static void init_game(GameState *gs, const uint8_t *ram) {
    /* Player entity at $EA90 (8 bytes) */
    gs->player.graphic = RB(0xEA90);
    gs->player.room    = RB(0xEA91);
    gs->player.flags   = RB(0xEA92);
    gs->player.x       = RB(0xEA93);
    gs->player.y       = RB(0xEA94);
    gs->player.attr    = RB(0xEA95);
    gs->player.vx      = (int8_t)RB(0xEA96);
    gs->player.vy      = (int8_t)RB(0xEA97);

    /* Default player position if snapshot has no runtime data */
    if (gs->player.x == 0 && gs->player.y == 0) {
        gs->player.x    = 0x58;  /* room centre X */
        gs->player.y    = 0x68;  /* room centre Y */
        gs->player.attr = 0x47;  /* bright white */
    }

    /* Default HUD values if snapshot has no runtime data */
    if (gs->energy == 0)  gs->energy = 0xF0;  /* full health */
    if (gs->lives == 0)   gs->lives  = 3;
    /* score stays 0 */

    gs->current_room = gs->player.room;

    /* Room attributes from hardcoded table (ROM data at $A854) */
    if (gs->current_room < NUM_ROOMS) {
        gs->room_style = room_attrs[gs->current_room].style;
        gs->room_attr  = room_attrs[gs->current_room].attr;
    } else {
        gs->room_style = 0;
        gs->room_attr  = 0x47;
    }

    /* Variables from $5E00 region */
    gs->lives    = RB(0x5E21);
    gs->energy   = RB(0x5E28);
    gs->score[0] = RB(0x5E2A);
    gs->score[1] = RB(0x5E2B);
    gs->score[2] = RB(0x5E2C);
    gs->clock_h  = RB(0x5E3D);
    gs->clock_m  = RB(0x5E3E);
    gs->clock_s  = RB(0x5E3F);

    /* Inventory slots at $5E30, $5E34, $5E38 (4 bytes each) */
    for (int s = 0; s < 4; s++) gs->inventory[0][s] = RB(0x5E30u + (uint32_t)s);
    for (int s = 0; s < 4; s++) gs->inventory[1][s] = RB(0x5E34u + (uint32_t)s);
    for (int s = 0; s < 4; s++) gs->inventory[2][s] = RB(0x5E38u + (uint32_t)s);

    gs->frame   = 0;
    gs->running = 1;

    fprintf(stdout, "init_game: room=%02X energy=%02X lives=%d inv[0].graphic=%02X\n",
        gs->current_room, gs->energy, gs->lives, gs->inventory[0][2]);
}

#undef RB

/*
 * parse_room_table() — read 148 room pointers from $757D
 * and 13 room style dimensions from $A982.
 */
static void parse_room_table(GameState *gs, const uint8_t *ram) {
#define RW(addr) \
    ((uint32_t)((addr) - 0x4000u) + 1u < (uint32_t)RAM_SIZE \
        ? (uint16_t)(ram[(addr)-0x4000u] | ((uint16_t)ram[(addr)-0x4000u+1u] << 8)) \
        : (fprintf(stderr, "OOB word: $%04X\n", (unsigned)(addr)), (uint16_t)0))
#define RB2(addr) \
    ((uint32_t)((addr) - 0x4000u) < (uint32_t)RAM_SIZE \
        ? ram[(addr) - 0x4000u] \
        : (fprintf(stderr, "OOB byte: $%04X\n", (unsigned)(addr)), (uint8_t)0))

    for (int i = 0; i < NUM_ROOMS; i++) {
        uint32_t a = 0x757Du + (uint32_t)i * 2u;
        gs->room_ptrs[i] = RW(a);
    }
    fprintf(stdout, "parse_room_table: room[0]=$%04X room[147]=$%04X\n",
        gs->room_ptrs[0], gs->room_ptrs[147]);
    fprintf(stdout, "room_styles[2]: w=%02X h=%02X (%s)\n",
        room_styles[2].w, room_styles[2].h, room_styles[2].name);
    fprintf(stdout, "room[0x00]: style=%d(%s) attr=%02X\n",
        room_attrs[0].style, room_styles[room_attrs[0].style].name, room_attrs[0].attr);
#undef RW
#undef RB2
}

/* Returns room pointer for room_id, or 0 if out of bounds. */
static uint16_t room_ptr(const GameState *gs, uint8_t room_id)
    __attribute__((unused));
static uint16_t room_ptr(const GameState *gs, uint8_t room_id) {
    if (room_id >= NUM_ROOMS) return 0;
    return gs->room_ptrs[room_id];
}

/* Stub: returns room style for a given room.
 * Phase 5 will resolve room_id → style via the room attributes table. */
static const RoomStyle *get_room_style(uint8_t room_id, const uint8_t *room_attr_table)
    __attribute__((unused));
static const RoomStyle *get_room_style(uint8_t room_id, const uint8_t *room_attr_table) {
    (void)room_attr_table; /* now using hardcoded table */
    if (room_id >= NUM_ROOMS) return &room_styles[0];
    uint8_t si = room_attrs[room_id].style;
    if (si >= 13) si = 0;
    return &room_styles[si];
}

/* Knight player sprites: 16 frames (4 dirs x 4 anim), 18 rows x 2 bytes each */
/* frames 0-3=facing down, 4-7=facing right, 8-11=facing left, 12-15=facing up */
static const uint8_t knight_sprites[16][18][2] = {
    /* frame 0 */
    {
        {0x0C, 0x70}, {0x1C, 0x38}, {0x3B, 0x98}, {0x37, 0x60},
        {0x06, 0x10}, {0x37, 0x08}, {0x27, 0x98}, {0x1F, 0xF0},
        {0x67, 0x80}, {0x6B, 0x7C}, {0x1A, 0xFC}, {0x01, 0xFC},
        {0x1F, 0xFC}, {0x08, 0x08}, {0x09, 0xF8}, {0x04, 0xF0},
        {0x02, 0xE0}, {0x01, 0x40},
    },
    /* frame 1 */
    {
        {0x07, 0xC0}, {0x03, 0xC0}, {0x00, 0x00}, {0x03, 0xF0},
        {0x06, 0x38}, {0x04, 0x18}, {0x0F, 0x98}, {0x1F, 0xF0},
        {0x67, 0x80}, {0x6B, 0x7C}, {0x1A, 0xFC}, {0x01, 0xFC},
        {0x1F, 0xFC}, {0x08, 0x08}, {0x09, 0xF8}, {0x04, 0xF0},
        {0x02, 0xE0}, {0x01, 0x40},
    },
    /* frame 2 */
    {
        {0x0C, 0x70}, {0x1C, 0x38}, {0x3B, 0xD8}, {0x37, 0xE0},
        {0x04, 0x74}, {0x08, 0x3A}, {0x07, 0x1E}, {0x1F, 0xF8},
        {0x67, 0x80}, {0x6B, 0x7C}, {0x1A, 0xFC}, {0x01, 0xFC},
        {0x1F, 0xFC}, {0x08, 0x08}, {0x09, 0xF8}, {0x04, 0xF0},
        {0x02, 0xE0}, {0x01, 0x40},
    },
    /* frame 3 */
    {
        {0x07, 0xC0}, {0x03, 0xC0}, {0x00, 0x00}, {0x03, 0xF0},
        {0x06, 0x38}, {0x04, 0x18}, {0x0F, 0x98}, {0x1F, 0xF0},
        {0x67, 0x80}, {0x6B, 0x7C}, {0x1A, 0xFC}, {0x01, 0xFC},
        {0x1F, 0xFC}, {0x08, 0x08}, {0x09, 0xF8}, {0x04, 0xF0},
        {0x02, 0xE0}, {0x01, 0x40},
    },
    /* frame 4 */
    {
        {0x0E, 0x30}, {0x1C, 0x38}, {0x1B, 0xDC}, {0x07, 0xEC},
        {0x2E, 0x20}, {0x5C, 0x10}, {0x78, 0xE0}, {0x1F, 0xF8},
        {0x01, 0xE6}, {0x3E, 0xD6}, {0x27, 0x58}, {0x27, 0x80},
        {0x27, 0xF8}, {0x10, 0x30}, {0x13, 0xF0}, {0x09, 0xE0},
        {0x05, 0xC0}, {0x02, 0x80},
    },
    /* frame 5 */
    {
        {0x03, 0xE0}, {0x03, 0xC0}, {0x00, 0x00}, {0x0F, 0xC0},
        {0x1C, 0x60}, {0x18, 0x20}, {0x19, 0xF0}, {0x0F, 0xF8},
        {0x01, 0xE6}, {0x3E, 0xD6}, {0x27, 0x58}, {0x27, 0x80},
        {0x27, 0xF8}, {0x10, 0x30}, {0x13, 0xF0}, {0x09, 0xE0},
        {0x05, 0xC0}, {0x02, 0x80},
    },
    /* frame 6 */
    {
        {0x0E, 0x30}, {0x1C, 0x38}, {0x18, 0xDC}, {0x06, 0xEC},
        {0x08, 0x60}, {0x10, 0xEC}, {0x19, 0xE4}, {0x0F, 0xF8},
        {0x01, 0xE6}, {0x3E, 0xD6}, {0x27, 0x58}, {0x27, 0x80},
        {0x27, 0xF8}, {0x10, 0x30}, {0x13, 0xF0}, {0x09, 0xE0},
        {0x05, 0xC0}, {0x02, 0x80},
    },
    /* frame 7 */
    {
        {0x03, 0xE0}, {0x03, 0xC0}, {0x00, 0x00}, {0x0F, 0xC0},
        {0x1C, 0x60}, {0x18, 0x20}, {0x19, 0xF0}, {0x0F, 0xF8},
        {0x01, 0xE6}, {0x3E, 0xD6}, {0x27, 0x58}, {0x27, 0x80},
        {0x27, 0xF8}, {0x10, 0x30}, {0x13, 0xF0}, {0x09, 0xE0},
        {0x05, 0xC0}, {0x02, 0x80},
    },
    /* frame 8 */
    {
        {0x3C, 0x00}, {0x3C, 0xF0}, {0x1F, 0x78}, {0x1B, 0x94},
        {0x07, 0xCE}, {0x6F, 0xEE}, {0xEF, 0xEC}, {0xEF, 0xE8},
        {0x40, 0x00}, {0x3F, 0xF8}, {0x27, 0xF8}, {0x27, 0xF8},
        {0x30, 0x38}, {0x13, 0xF0}, {0x13, 0xF0}, {0x09, 0xE0},
        {0x05, 0xC0}, {0x02, 0x80},
    },
    /* frame 9 */
    {
        {0x1E, 0xF0}, {0x0E, 0xE0}, {0x05, 0x40}, {0x03, 0x80},
        {0x67, 0xCC}, {0xEF, 0xEE}, {0xEF, 0xEE}, {0x6F, 0xEC},
        {0x00, 0x00}, {0x3F, 0xF8}, {0x27, 0xF8}, {0x27, 0xF8},
        {0x30, 0x38}, {0x13, 0xF0}, {0x13, 0xF0}, {0x09, 0xE0},
        {0x05, 0xC0}, {0x02, 0x80},
    },
    /* frame 10 */
    {
        {0x00, 0x78}, {0x1E, 0x78}, {0x3D, 0xF0}, {0x53, 0xB0},
        {0xE7, 0xC0}, {0xEF, 0xEC}, {0x6F, 0xEE}, {0x2F, 0xEE},
        {0x00, 0x04}, {0x3F, 0xF8}, {0x27, 0xF8}, {0x27, 0xF8},
        {0x30, 0x38}, {0x13, 0xF0}, {0x13, 0xF0}, {0x09, 0xE0},
        {0x05, 0xC0}, {0x02, 0x80},
    },
    /* frame 11 */
    {
        {0x1E, 0xF0}, {0x0E, 0xE0}, {0x05, 0x40}, {0x03, 0x80},
        {0x67, 0xCC}, {0xEF, 0xEE}, {0xEF, 0xEE}, {0x6F, 0xEC},
        {0x00, 0x00}, {0x3F, 0xF8}, {0x27, 0xF8}, {0x27, 0xF8},
        {0x30, 0x38}, {0x13, 0xF0}, {0x13, 0xF0}, {0x09, 0xE0},
        {0x05, 0xC0}, {0x02, 0x80},
    },
    /* frame 12 */
    {
        {0x3C, 0x00}, {0x3C, 0xF0}, {0x1F, 0x78}, {0x1B, 0x94},
        {0x05, 0x4E}, {0x6B, 0x6E}, {0xE9, 0x2C}, {0xEF, 0xE8},
        {0x44, 0x40}, {0x26, 0xC8}, {0x20, 0x08}, {0x30, 0x18},
        {0x3F, 0xF8}, {0x13, 0xF0}, {0x13, 0xF0}, {0x09, 0xE0},
        {0x05, 0xC0}, {0x02, 0x80},
    },
    /* frame 13 */
    {
        {0x1E, 0xF0}, {0x0E, 0xE0}, {0x05, 0x40}, {0x03, 0xF0},
        {0x65, 0x4C}, {0xEB, 0x6E}, {0xEB, 0x6E}, {0x6F, 0xEC},
        {0x00, 0x00}, {0x3F, 0xF8}, {0x27, 0xF8}, {0x27, 0xF8},
        {0x30, 0x38}, {0x13, 0xF0}, {0x13, 0xF0}, {0x09, 0xE0},
        {0x05, 0xC0}, {0x02, 0x80},
    },
    /* frame 14 */
    {
        {0x00, 0x78}, {0x1E, 0x78}, {0x3C, 0xF0}, {0x52, 0xB0},
        {0xE5, 0xC0}, {0xEB, 0x6E}, {0x6B, 0x6C}, {0x2F, 0xEC},
        {0x00, 0x04}, {0x3F, 0xF8}, {0x27, 0xF8}, {0x27, 0xF8},
        {0x30, 0x38}, {0x13, 0xF0}, {0x13, 0xF0}, {0x09, 0xE0},
        {0x05, 0xC0}, {0x02, 0x80},
    },
    /* frame 15 */
    {
        {0x1E, 0xF0}, {0x0E, 0xE0}, {0x05, 0x40}, {0x03, 0xF0},
        {0x65, 0x4C}, {0xEB, 0x6E}, {0xEB, 0x6E}, {0x6F, 0xEC},
        {0x00, 0x00}, {0x3F, 0xF8}, {0x27, 0xF8}, {0x27, 0xF8},
        {0x30, 0x38}, {0x13, 0xF0}, {0x13, 0xF0}, {0x09, 0xE0},
        {0x05, 0xC0}, {0x02, 0x80},
    },
};

/*
 * draw_sprite() — blit a 16-wide 1bpp sprite at ZX pixel coords (px, py).
 * sprite_rows: array of [height][2] bytes, MSB first, 1=ink 0=paper/transparent.
 * attr: ZX colour attribute for ink/paper colours.
 * height: number of rows (typically 18 for player).
 * transparent: if 1, skip paper-coloured pixels (don't overdraw floor).
 */
static void draw_sprite(SDL_Renderer *ren,
                        const uint8_t sprite_rows[][2],
                        int height,
                        int px, int py,
                        uint8_t attr,
                        int transparent)
{
    int bright      = (attr & 0x40) ? 8 : 0;
    SDL_Color ink   = zx_pal[(attr & 7)        | bright];
    SDL_Color paper = zx_pal[((attr >> 3) & 7) | bright];

    for (int row = 0; row < height; row++) {
        uint16_t bits = ((uint16_t)sprite_rows[row][0] << 8) | sprite_rows[row][1];
        for (int bit = 0; bit < 16; bit++) {
            int set = (bits >> (15 - bit)) & 1;
            if (!set && transparent) continue;
            SDL_Color c = set ? ink : paper;
            SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
            SDL_Rect px_r = {
                (px + bit) * SCALE,
                (py + row)  * SCALE,
                SCALE, SCALE
            };
            SDL_RenderFillRect(ren, &px_r);
        }
    }
}

/* ── Minimal 4×6 pixel font (digits 0-9 only) ──────────────────────────── */
/* Each digit: 6 rows × 4 bits (MSB=leftmost pixel), stored as uint8_t[6]  */
static const uint8_t digit_font[10][6] = {
    {0x6, 0x9, 0x9, 0x9, 0x9, 0x6}, /* 0 */
    {0x2, 0x6, 0x2, 0x2, 0x2, 0x7}, /* 1 */
    {0x6, 0x9, 0x1, 0x2, 0x4, 0xF}, /* 2 */
    {0xE, 0x1, 0x6, 0x1, 0x1, 0xE}, /* 3 */
    {0x9, 0x9, 0xF, 0x1, 0x1, 0x1}, /* 4 */
    {0xF, 0x8, 0xE, 0x1, 0x1, 0xE}, /* 5 */
    {0x3, 0x4, 0xE, 0x9, 0x9, 0x6}, /* 6 */
    {0xF, 0x1, 0x2, 0x4, 0x4, 0x4}, /* 7 */
    {0x6, 0x9, 0x6, 0x9, 0x9, 0x6}, /* 8 */
    {0x6, 0x9, 0x9, 0x7, 0x1, 0x6}, /* 9 */
};

/*
 * draw_digit() — render a single 4×6 digit at ZX pixel (px, py), scaled.
 */
static void draw_digit(SDL_Renderer *ren, int digit, int px, int py,
                       uint8_t r, uint8_t g, uint8_t b) {
    if (digit < 0 || digit > 9) return;
    for (int row = 0; row < 6; row++) {
        for (int col = 0; col < 4; col++) {
            if ((digit_font[digit][row] >> (3 - col)) & 1) {
                SDL_SetRenderDrawColor(ren, r, g, b, 255);
                SDL_Rect rc = { (px + col) * SCALE, (py + row) * SCALE, SCALE, SCALE };
                SDL_RenderFillRect(ren, &rc);
            }
        }
    }
}

/*
 * render_hud() — draw energy bar, score, and lives outside the room.
 */
static void render_hud(SDL_Renderer *ren, const GameState *gs) {
    /* ── Energy bar (right side, ZX coords X=220-228, Y=24-176) ── */
    int bar_x = 220, bar_y = 24, bar_w = 8, bar_h = 152;
    /* Background: dark grey */
    SDL_SetRenderDrawColor(ren, 64, 64, 64, 255);
    SDL_Rect bar_bg = { bar_x * SCALE, bar_y * SCALE, bar_w * SCALE, bar_h * SCALE };
    SDL_RenderFillRect(ren, &bar_bg);
    /* Fill: bright green proportional to energy */
    int fill_h = (int)((gs->energy * bar_h) / 0xF0);
    if (fill_h > 0) {
        SDL_SetRenderDrawColor(ren, 0, 215, 0, 255);
        SDL_Rect bar_fill = {
            bar_x * SCALE,
            (bar_y + bar_h - fill_h) * SCALE,
            bar_w * SCALE,
            fill_h * SCALE
        };
        SDL_RenderFillRect(ren, &bar_fill);
    }

    /* ── Score (top strip, 6 BCD digits centred at X=84, Y=4) ── */
    /* BCD score is 3 bytes: score[0]=tens-of-thousands/thousands,
       score[1]=hundreds/tens, score[2]=ones (low nibble unused in original but safe) */
    int digits[6];
    digits[0] = (gs->score[0] >> 4) & 0xF;
    digits[1] =  gs->score[0]       & 0xF;
    digits[2] = (gs->score[1] >> 4) & 0xF;
    digits[3] =  gs->score[1]       & 0xF;
    digits[4] = (gs->score[2] >> 4) & 0xF;
    digits[5] =  gs->score[2]       & 0xF;
    int score_x = 70;  /* left edge of 6-digit score display */
    for (int i = 0; i < 6; i++) {
        draw_digit(ren, digits[i] % 10, score_x + i * 5, 4, 255, 255, 255);
    }

    /* ── Lives (bottom-left, red squares 5×5px each) ── */
    for (int i = 0; i < gs->lives && i < 5; i++) {
        SDL_SetRenderDrawColor(ren, 215, 0, 0, 255);
        SDL_Rect heart = { (4 + i * 7) * SCALE, 180 * SCALE, 5 * SCALE, 5 * SCALE };
        SDL_RenderFillRect(ren, &heart);
    }

    /* ── Clock (top-right corner, tiny digits) ── */
    int clk_digits[6] = {
        gs->clock_h / 10, gs->clock_h % 10,
        gs->clock_m / 10, gs->clock_m % 10,
        gs->clock_s / 10, gs->clock_s % 10,
    };
    for (int i = 0; i < 6; i++) {
        int cx = 192 + i * 5 + (i >= 2 ? 2 : 0) + (i >= 4 ? 2 : 0); /* separators */
        draw_digit(ren, clk_digits[i] % 10, cx, 4, 0, 215, 215);
    }
}

/*
 * render_player() — draw the animated Knight sprite at its current position.
 * Selects frame based on walk direction and animation cycle.
 */
static void render_player(SDL_Renderer *ren, const GameState *gs) {
    static const int walk_cycle[4] = {0, 1, 2, 1};
    int sprite_idx = gs->walk_dir * 4 + walk_cycle[gs->walk_frame & 3];
    draw_sprite(ren,
                knight_sprites[sprite_idx],
                18,
                gs->player.x,
                gs->player.y,
                gs->player.attr ? gs->player.attr : 0x47,
                1);
}

/*
 * game_tick() — update game logic once per frame.
 * Energy drains over time; death/respawn handled here.
 */
static void game_tick(GameState *gs) {
    /* Energy drain: -1 every 16 frames */
    if ((gs->frame & 0x0Fu) == 0u && gs->energy > 0)
        gs->energy--;

    /* Death check */
    if (gs->energy == 0) {
        if (gs->lives > 0)
            gs->lives--;
        if (gs->lives == 0) {
            gs->running = 0;
            fprintf(stdout, "GAME OVER \u2014 score: %02X%02X%02X\n",
                gs->score[0], gs->score[1], gs->score[2]);
        } else {
            gs->energy     = 0xF0;
            gs->player.x   = 0x58;
            gs->player.y   = 0x68;
            gs->walk_dir   = 0;
            gs->walk_frame = 0;
            fprintf(stdout, "Respawn: lives=%d\n", gs->lives);
        }
    }
}

/*
 * render_room() — fill floor and draw walls for the current room.
 * Must be called each frame before rendering sprites.
 */
static void render_room(SDL_Renderer *ren, const GameState *gs) {
    const RoomStyle *rs = get_room_style(gs->current_room, NULL);

    /* Room centre and interior bounds */
    int cx = 0x58, cy = 0x68;
    int left   = (cx - rs->w) * SCALE;
    int right  = (cx + rs->w) * SCALE;
    int top    = (cy - rs->h) * SCALE;
    int bottom = (cy + rs->h) * SCALE;
    int thick  = 4 * SCALE;

    /* Decode ZX attribute → SDL colours */
    uint8_t attr  = gs->room_attr;
    int bright    = (attr & 0x40) ? 8 : 0;
    SDL_Color ink  = zx_pal[(attr & 7) | bright];
    SDL_Color paper= zx_pal[((attr >> 3) & 7) | bright];

    /* Fill floor with paper colour */
    SDL_SetRenderDrawColor(ren, paper.r, paper.g, paper.b, 255);
    SDL_Rect floor_r = { left, top, right - left, bottom - top };
    SDL_RenderFillRect(ren, &floor_r);

    /* Draw walls with ink colour */
    SDL_SetRenderDrawColor(ren, ink.r, ink.g, ink.b, 255);
    SDL_Rect walls[4] = {
        { left,        top - thick,   right - left,         thick              },  /* top    */
        { left,        bottom,        right - left,         thick              },  /* bottom */
        { left - thick, top - thick,  thick, bottom-top+2*thick               },  /* left   */
        { right,       top - thick,   thick, bottom-top+2*thick               },  /* right  */
    };
    for (int i = 0; i < 4; i++)
        SDL_RenderFillRect(ren, &walls[i]);
}

int main(int argc, char *argv[]) {
    int headless = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) headless = 1;
    }

    /* Allocate RAM */
    uint8_t *ram = calloc(RAM_SIZE, 1);
    if (!ram) { fprintf(stderr, "Out of memory\n"); return 1; }

    /* Game state */
    GameState gs;
    memset(&gs, 0, sizeof(gs));

    /* Load game data: full snapshot preferred, TAP fallback */
    if (!load_z80("atic_atac_full.z80", ram)) {
        if (!load_tap("ATIC_ATAC.TAP", ram)) {
            fprintf(stderr, "Using blank RAM (no valid snapshot)\n");
        }
    }

    /* Init game state from snapshot */
    init_game(&gs, ram);
    parse_room_table(&gs, ram);

    /* SDL init */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        free(ram);
        return 1;
    }

    Uint32 win_flags = headless ? SDL_WINDOW_HIDDEN : 0;
    SDL_Window *win = SDL_CreateWindow(
        "Atic Atac — ZX Spectrum Port",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, win_flags);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit(); free(ram); return 1;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        SDL_DestroyWindow(win); SDL_Quit(); free(ram); return 1;
    }
    SDL_RenderSetLogicalSize(ren, WIN_W, WIN_H);

    SDL_Texture *tex = SDL_CreateTexture(ren,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_W, SCREEN_H);
    if (!tex) {
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit(); free(ram); return 1;
    }

    SDL_Rect dst = {0, 0, WIN_W, WIN_H};
    (void)dst;  /* kept for future sprite blit */

    /* Main loop */
    while (gs.running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) gs.running = 0;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) gs.running = 0;
        }

        /* Player movement — arrow keys / WASD, 2px per frame */
        {
            const Uint8 *keys = SDL_GetKeyboardState(NULL);
            const RoomStyle *rs = get_room_style(gs.current_room, NULL);
            int cx = 0x58, cy = 0x68;
            int left_bound  = cx - rs->w + 4;
            int right_bound = cx + rs->w - 4 - 16; /* 16px sprite width */
            int top_bound   = cy - rs->h + 4;
            int bot_bound   = cy + rs->h - 4 - 18; /* 18px sprite height */

            int nx = gs.player.x;
            int ny = gs.player.y;
            int moved = 0;

            if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A])
                { nx -= 2; gs.walk_dir = 2; moved = 1; }
            if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D])
                { nx += 2; gs.walk_dir = 1; moved = 1; }
            if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W])
                { ny -= 2; gs.walk_dir = 3; moved = 1; }
            if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S])
                { ny += 2; gs.walk_dir = 0; moved = 1; }

            /* Clamp to room interior */
            if (nx < left_bound)  nx = left_bound;
            if (nx > right_bound) nx = right_bound;
            if (ny < top_bound)   ny = top_bound;
            if (ny > bot_bound)   ny = bot_bound;

            gs.player.x = (uint8_t)nx;
            gs.player.y = (uint8_t)ny;

            /* Advance walk animation every 6 frames when moving */
            if (moved && (gs.frame % 6 == 0))
                gs.walk_frame = (gs.walk_frame + 1) & 3;
        }

        SDL_RenderClear(ren);
        render_room(ren, &gs);
        render_player(ren, &gs);
        render_hud(ren, &gs);
        SDL_RenderPresent(ren);

        if (headless) {
            fprintf(stdout, "Headless: frame rendered OK\n");
            gs.running = 0;
        } else {
            SDL_Delay(1000 / FPS);
        }

        gs.frame++;
        game_tick(&gs);
    }

    /* Game over overlay: red tint for 2 seconds */
    if (!headless && gs.energy == 0 && gs.lives == 0) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 200, 0, 0, 128);
        SDL_Rect overlay = {0, 0, WIN_W, WIN_H};
        SDL_RenderFillRect(ren, &overlay);
        SDL_RenderPresent(ren);
        SDL_Delay(2000);
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(ram);
    return 0;
}
