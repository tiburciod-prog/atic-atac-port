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

int main(int argc, char *argv[]) {
    int headless = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) headless = 1;
    }

    /* Allocate RAM */
    uint8_t *ram = calloc(RAM_SIZE, 1);
    if (!ram) { fprintf(stderr, "Out of memory\n"); return 1; }

    /* Load snapshot */
    if (!load_z80("atic_atac.z80", ram)) {
        fprintf(stderr, "Using blank RAM (no valid snapshot)\n");
        /* ram is already zeroed — will render black screen */
    }

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
    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = 0;
        }

        render_zx_screen(tex, ram, ram + 0x1800);

        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, &dst);
        SDL_RenderPresent(ren);

        if (headless) {
            fprintf(stdout, "Headless: frame rendered OK\n");
            running = 0;
        } else {
            SDL_Delay(1000 / FPS);
        }
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(ram);
    return 0;
}
