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

    gs->current_room = gs->player.room;

    /* Room attributes at $A854: 2 bytes per room (style, attr) */
    uint32_t rap = 0xA854u + (uint32_t)gs->current_room * 2u;
    gs->room_style = RB(rap);
    gs->room_attr  = RB(rap + 1u);

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
    (void)room_id; (void)room_attr_table;
    return &room_styles[0];
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

    /* Load snapshot */
    if (!load_z80("atic_atac.z80", ram)) {
        fprintf(stderr, "Using blank RAM (no valid snapshot)\n");
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

    /* Main loop */
    while (gs.running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) gs.running = 0;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) gs.running = 0;
        }

        render_zx_screen(tex, ram, ram + 0x1800);

        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, &dst);
        SDL_RenderPresent(ren);

        if (headless) {
            fprintf(stdout, "Headless: frame rendered OK\n");
            gs.running = 0;
        } else {
            SDL_Delay(1000 / FPS);
        }

        gs.frame++;
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(ram);
    return 0;
}
