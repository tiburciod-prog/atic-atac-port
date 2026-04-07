#!/usr/bin/env python3
"""Apply Phase 15 changes: character select screen."""

import sys

path = "/home/tiburcio/projects/atic-atac-port/atic_atac.c"

with open(path, "r") as f:
    src = f.read()

# ------------------------------------------------------------------
# CHANGE 1: add char_type field before Walk animation comment
# ------------------------------------------------------------------
OLD1 = "    /* Walk animation */\n    uint8_t walk_dir;"
NEW1 = "    /* Character type: 0=Knight, 1=Wizard, 2=Serf */\n    uint8_t char_type;\n\n    /* Walk animation */\n    uint8_t walk_dir;"

if OLD1 not in src:
    print("ERROR: CHANGE 1 anchor not found"); sys.exit(1)
src = src.replace(OLD1, NEW1, 1)
print("CHANGE 1 applied")

# ------------------------------------------------------------------
# CHANGE 2: insert show_char_select() before fire_weapon()
# ------------------------------------------------------------------
# Use the em-dash character that's already in the file
FIRE_ANCHOR = "/*\n * fire_weapon() \u2014 launch axe from player position in walk_dir direction."

CHAR_SELECT_FN = r"""/*
 * show_char_select() - blocking character selection screen.
 * Returns 0=Knight, 1=Wizard, 2=Serf. ESC/Q defaults to Knight.
 */
static int show_char_select(SDL_Renderer *ren) {
    static const char *names[3] = {"KNIGHT", "WIZARD", "SERF"};
    static const uint8_t colours[3] = {0x47, 0x45, 0x43}; /* white, cyan, green */
    int selected = 0;
    int done = 0;

    while (!done) {
        /* Clear to black */
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);

        /* Draw title "ATIC ATAC" in top area */
        static const char *title = "ATIC ATAC";
        int tx = 60;
        for (int i = 0; title[i]; i++) {
            /* Draw each letter as a bright white 6x10 block */
            SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
            SDL_Rect r = { (tx + i*9) * SCALE, 20 * SCALE, 6 * SCALE, 10 * SCALE };
            SDL_RenderDrawRect(ren, &r);
        }

        /* Draw "SELECT CHARACTER" prompt */
        SDL_SetRenderDrawColor(ren, 200, 200, 200, 255);
        SDL_Rect prompt = { 55 * SCALE, 55 * SCALE, 150 * SCALE, 6 * SCALE };
        SDL_RenderFillRect(ren, &prompt);

        /* Draw 3 character options */
        for (int i = 0; i < 3; i++) {
            uint8_t attr = colours[i];
            int bright = (attr & 0x40) ? 255 : 180;
            int ci     = attr & 0x07;
            int r = (ci & 2) ? bright : 0;
            int g = (ci & 4) ? bright : 0;
            int b = (ci & 1) ? bright : 0;

            int oy = 80 + i * 30;

            /* Selection indicator */
            if (i == selected) {
                SDL_SetRenderDrawColor(ren, 255, 255, 0, 255);
                SDL_Rect arrow = { 50 * SCALE, oy * SCALE, 6 * SCALE, 10 * SCALE };
                SDL_RenderFillRect(ren, &arrow);
            }

            /* Character colour block */
            SDL_SetRenderDrawColor(ren, r, g, b, 255);
            SDL_Rect box = { 60 * SCALE, oy * SCALE, 12 * SCALE, 12 * SCALE };
            SDL_RenderFillRect(ren, &box);

            /* Name as a bar (placeholder for text) */
            SDL_SetRenderDrawColor(ren, 200, 200, 200, 255);
            SDL_Rect name_bar = { 80 * SCALE, (oy+2) * SCALE,
                                  (int)(strlen(names[i]) * 7) * SCALE, 8 * SCALE };
            SDL_RenderFillRect(ren, &name_bar);
        }

        SDL_RenderPresent(ren);

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) return 0;
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                    case SDLK_UP:   selected = (selected + 2) % 3; break;
                    case SDLK_DOWN: selected = (selected + 1) % 3; break;
                    case SDLK_RETURN:
                    case SDLK_SPACE: done = 1; break;
                    case SDLK_ESCAPE:
                    case SDLK_q:    return 0;
                    default: break;
                }
            }
        }
        SDL_Delay(16);
    }
    return selected;
}

"""

if FIRE_ANCHOR not in src:
    print("ERROR: CHANGE 2 anchor not found"); sys.exit(1)
src = src.replace(FIRE_ANCHOR, CHAR_SELECT_FN + FIRE_ANCHOR, 1)
print("CHANGE 2 applied")

# ------------------------------------------------------------------
# CHANGE 3: wire char select after ren is ready, before main loop
# The brief says after parse_room_table, but ren doesn't exist yet.
# Place it after SDL_RenderSetLogicalSize (ren is guaranteed ready).
# ------------------------------------------------------------------
OLD3 = "    SDL_RenderSetLogicalSize(ren, WIN_W, WIN_H);\n\n    SDL_Texture"
NEW3 = ("    SDL_RenderSetLogicalSize(ren, WIN_W, WIN_H);\n\n"
        "    /* Character select (skip in headless mode) */\n"
        "    if (!headless) {\n"
        "        gs.char_type = (uint8_t)show_char_select(ren);\n"
        "    }\n\n"
        "    SDL_Texture")

if OLD3 not in src:
    print("ERROR: CHANGE 3 anchor not found"); sys.exit(1)
src = src.replace(OLD3, NEW3, 1)
print("CHANGE 3 applied")

with open(path, "w") as f:
    f.write(src)

print("All changes written successfully.")
