#include <libdragon.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

/*
 * AFTER: WORLD ZERO - N64 prototype v0.2
 * Top-down open world (128x128 tiles, 16px each) with:
 *  - overgrown ruined city (west), river (middle), dense forest (east)
 *  - drink water, fishing, hunting rabbits, gathering, cooking
 *  - building walls / floors / campfires
 *  - hunger / thirst / health survival loop
 */

#define SW 320
#define SH 240
#define TS 16
#define MW 128
#define MH 128
#define DEADZONE 18
#define NRABBIT 24

enum {
    T_GRASS, T_ROAD, T_TREE, T_STUMP, T_WATER, T_RWALL, T_RFLOOR,
    T_BUSH, T_BUSH_E, T_BWALL, T_BFLOOR, T_FIRE
};

enum { B_WALL, B_FLOOR, B_FIRE, B_COUNT };
static const char *build_name[B_COUNT] = { "WALL", "FLOOR", "FIRE" };
static const int build_cost[B_COUNT] = { 2, 1, 3 };

typedef struct { int16_t x, y; int8_t dx, dy; uint8_t alive; uint16_t t; } Rabbit;

static uint8_t map[MH][MW];
static Rabbit rabbits[NRABBIT];

static int px, py, face = 2;               /* face: 0 up 1 right 2 down 3 left */
static int hunger = 100, thirst = 100, health = 100;
static int wood, scrap, fish, meat, berry, cfish, cmeat;
static int build_mode, build_sel;
static int fish_state, fish_timer, fish_tx, fish_ty;
static int hit_tx = -1, hit_ty = -1, hit_count;
static int frame_no, msg_timer, drink_cd, dead;
static char message[48] = "Find water. Survive.";
static uint32_t rng_state = 12345;

static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static uint32_t hash2(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void say(const char *s) {
    snprintf(message, sizeof(message), "%s", s);
    msg_timer = 120;
}

static int river_x(int y) { return 44 + (int)(5.0f * sinf((float)y * 0.07f)); }

static int tile_at(int tx, int ty) {
    if (tx < 0 || ty < 0 || tx >= MW || ty >= MH) return T_TREE;
    return map[ty][tx];
}

static int solid_tile(int t) {
    return t == T_TREE || t == T_WATER || t == T_RWALL || t == T_BWALL || t == T_FIRE;
}

static int solid_px(int x, int y) { return solid_tile(tile_at(x / TS, y / TS)); }

static int blocked(int x, int y) {
    const int h = 5;
    return solid_px(x - h, y - h) || solid_px(x + h, y - h) ||
           solid_px(x - h, y + h) || solid_px(x + h, y + h);
}

static void gen_world(void) {
    for (int y = 0; y < MH; y++) {
        for (int x = 0; x < MW; x++) {
            uint32_t h = hash2(x, y);
            int t = T_GRASS;
            int rx = river_x(y);
            if (x >= rx - 1 && x <= rx + 1) {
                t = T_WATER;
            } else if (x < 40) {
                /* ruined city */
                int lx = x % 16, ly = y % 16;
                int lot = (int)(hash2(x / 16, y / 16) % 4);
                if (lx == 0 || ly == 0) {
                    t = T_ROAD;
                } else if (lot != 0 && lx >= 3 && lx <= 8 && ly >= 3 && ly <= 7) {
                    int edge = (lx == 3 || lx == 8 || ly == 3 || ly == 7);
                    if (edge) t = (h % 6 == 0) ? T_RFLOOR : T_RWALL;
                    else t = T_RFLOOR;
                    if (lx == 5 && ly == 7) t = T_RFLOOR;      /* door */
                } else {
                    if (h % 9 == 0) t = T_TREE;
                    else if (h % 23 == 0) t = T_BUSH;
                }
            } else {
                /* forest */
                if (h % 4 == 0) t = T_TREE;
                else if (h % 31 == 0) t = T_BUSH;
            }
            map[y][x] = (uint8_t)t;
        }
    }
    /* clear spawn */
    px = 16 * TS + 8; py = 16 * TS + 8;
    for (int y = 14; y <= 18; y++)
        for (int x = 14; x <= 18; x++) map[y][x] = T_ROAD;

    for (int i = 0; i < NRABBIT; i++) {
        Rabbit *r = &rabbits[i];
        for (int tries = 0; tries < 50; tries++) {
            int tx = 30 + (int)(rnd() % (MW - 32));
            int ty = 2 + (int)(rnd() % (MH - 4));
            if (map[ty][tx] == T_GRASS) {
                r->x = (int16_t)(tx * TS + 8); r->y = (int16_t)(ty * TS + 8);
                break;
            }
        }
        r->alive = 1; r->t = 0; r->dx = r->dy = 0;
    }
}

static void reset_game(void) {
    hunger = thirst = health = 100;
    wood = scrap = fish = meat = berry = cfish = cmeat = 0;
    build_mode = 0; build_sel = 0; fish_state = 0; hit_count = 0;
    dead = 0; frame_no = 0;
    gen_world();
    say("Find water. Survive.");
}

static void target_tile(int *tx, int *ty) {
    static const int ddx[4] = { 0, 1, 0, -1 };
    static const int ddy[4] = { -1, 0, 1, 0 };
    *tx = (px + ddx[face] * 12) / TS;
    *ty = (py + ddy[face] * 12) / TS;
}

static void update_rabbits(void) {
    for (int i = 0; i < NRABBIT; i++) {
        Rabbit *r = &rabbits[i];
        if (!r->alive) {
            if (++r->t > 3600) {            /* respawn later */
                r->alive = 1; r->t = 0;
            }
            continue;
        }
        int ddx = r->x - px, ddy = r->y - py;
        int near = (ddx > -70 && ddx < 70 && ddy > -70 && ddy < 70);
        int mx = 0, my = 0;
        if (near) {
            mx = ddx > 0 ? 1 : (ddx < 0 ? -1 : 0);
            my = ddy > 0 ? 1 : (ddy < 0 ? -1 : 0);
        } else {
            if (r->t == 0) {
                r->dx = (int8_t)((int)(rnd() % 3) - 1);
                r->dy = (int8_t)((int)(rnd() % 3) - 1);
                r->t = 40 + rnd() % 80;
            }
            r->t--;
            if (frame_no & 1) { mx = r->dx; my = r->dy; }
        }
        int nx = r->x + mx, ny = r->y + my;
        if (!solid_px(nx, r->y)) r->x = (int16_t)nx;
        if (!solid_px(r->x, ny)) r->y = (int16_t)ny;
    }
}

static int try_hunt(void) {
    static const int ddx[4] = { 0, 1, 0, -1 };
    static const int ddy[4] = { -1, 0, 1, 0 };
    int cx = px + ddx[face] * 14, cy = py + ddy[face] * 14;
    for (int i = 0; i < NRABBIT; i++) {
        Rabbit *r = &rabbits[i];
        if (!r->alive) continue;
        if (abs(r->x - cx) < 14 && abs(r->y - cy) < 14) {
            r->alive = 0; r->t = 0;
            meat++;
            say("Caught a rabbit! +1 raw meat");
            return 1;
        }
    }
    return 0;
}

static void do_action_a(void) {
    int tx, ty;
    target_tile(&tx, &ty);
    int t = tile_at(tx, ty);

    if (try_hunt()) return;

    if (hit_tx != tx || hit_ty != ty) { hit_tx = tx; hit_ty = ty; hit_count = 0; }

    switch (t) {
    case T_WATER:
        if (drink_cd == 0) {
            thirst = clampi(thirst + 15, 0, 100);
            drink_cd = 20;
            say("Drank river water");
        }
        break;
    case T_TREE:
        if (++hit_count >= 3) {
            map[ty][tx] = T_STUMP; wood += 2; hit_count = 0;
            say("Chopped tree: +2 wood");
        } else say("Chopping...");
        break;
    case T_RWALL:
        if (++hit_count >= 4) {
            map[ty][tx] = T_RFLOOR; scrap += 2; hit_count = 0;
            say("Salvaged wall: +2 scrap");
        } else say("Salvaging...");
        break;
    case T_BUSH:
        map[ty][tx] = T_BUSH_E; berry += 2;
        say("Picked berries: +2");
        break;
    case T_FIRE:
        if (fish > 0) { fish--; cfish++; say("Cooked fish"); }
        else if (meat > 0) { meat--; cmeat++; say("Cooked meat"); }
        else say("Nothing to cook");
        break;
    default:
        break;
    }
}

static void do_fishing(void) {
    int tx, ty;
    target_tile(&tx, &ty);
    if (fish_state == 0) {
        /* water in front, or 2 tiles ahead */
        int wx = tx, wy = ty;
        if (tile_at(tx, ty) != T_WATER) {
            static const int ddx[4] = { 0, 1, 0, -1 };
            static const int ddy[4] = { -1, 0, 1, 0 };
            wx = tx + ddx[face]; wy = ty + ddy[face];
        }
        if (tile_at(wx, wy) == T_WATER) {
            fish_state = 1; fish_timer = 90 + (int)(rnd() % 210);
            fish_tx = wx; fish_ty = wy;
            say("Line cast. Wait for bite...");
        } else say("Face the water to fish");
    } else if (fish_state == 1) {
        fish_state = 0; say("Too early!");
    } else if (fish_state == 2) {
        fish_state = 0; fish++; say("Caught a fish! +1");
    }
}

static void do_eat(void) {
    if (cfish > 0)      { cfish--; hunger = clampi(hunger + 35, 0, 100); say("Ate cooked fish"); }
    else if (cmeat > 0) { cmeat--; hunger = clampi(hunger + 40, 0, 100); say("Ate cooked meat"); }
    else if (berry > 0) { berry--; hunger = clampi(hunger + 8, 0, 100);  say("Ate berries"); }
    else if (fish > 0)  { fish--;  hunger = clampi(hunger + 12, 0, 100); health = clampi(health - 5, 0, 100); say("Raw fish... ugh"); }
    else if (meat > 0)  { meat--;  hunger = clampi(hunger + 12, 0, 100); health = clampi(health - 5, 0, 100); say("Raw meat... ugh"); }
    else say("No food");
}

static void do_build(int place) {
    int tx, ty;
    target_tile(&tx, &ty);
    int t = tile_at(tx, ty);
    if (place) {
        if (wood < build_cost[build_sel]) { say("Not enough wood"); return; }
        int ok = (t == T_GRASS || t == T_ROAD || t == T_STUMP || t == T_RFLOOR);
        if (build_sel == B_FLOOR && t == T_RFLOOR) ok = 0;
        if (!ok) { say("Can't build here"); return; }
        wood -= build_cost[build_sel];
        map[ty][tx] = (build_sel == B_WALL) ? T_BWALL : (build_sel == B_FLOOR ? T_BFLOOR : T_FIRE);
        say("Built");
    } else {
        if (t == T_BWALL || t == T_BFLOOR || t == T_FIRE) {
            map[ty][tx] = T_GRASS; wood += 1; say("Demolished: +1 wood");
        }
    }
}

static void update_survival(void) {
    if (frame_no % 700 == 0) hunger = clampi(hunger - 1, 0, 100);
    if (frame_no % 500 == 0) thirst = clampi(thirst - 1, 0, 100);
    if (hunger == 0 || thirst == 0) {
        if (frame_no % 120 == 0) health = clampi(health - 1, 0, 100);
    } else if (hunger > 30 && thirst > 30 && frame_no % 240 == 0) {
        health = clampi(health + 1, 0, 100);
    }
    if (health <= 0) dead = 1;

    /* slow regrowth */
    if (frame_no % 300 == 0) {
        for (int i = 0; i < 8; i++) {
            int x = (int)(rnd() % MW), y = (int)(rnd() % MH);
            if (map[y][x] == T_BUSH_E) map[y][x] = T_BUSH;
            else if (map[y][x] == T_STUMP && (rnd() & 3) == 0) map[y][x] = T_TREE;
        }
    }
}

/* ------------------------------ drawing ------------------------------ */

static void fillc(color_t c) { rdpq_set_fill_color(c); }
static void rect(int x0, int y0, int x1, int y1) { rdpq_fill_rectangle(x0, y0, x1, y1); }
static void rectc(int x0, int y0, int x1, int y1, color_t c) { fillc(c); rect(x0, y0, x1, y1); }

static void draw_tile(int tx, int ty, int sx, int sy) {
    int t = map[ty][tx];
    uint32_t h = hash2(tx, ty);
    switch (t) {
    case T_GRASS:
        if ((h & 7) == 0) rectc(sx + 4, sy + 5, sx + 6, sy + 10, RGBA32(44, 122, 54, 255));
        break;
    case T_ROAD:
        rectc(sx, sy, sx + TS, sy + TS, RGBA32(92, 91, 78, 255));
        if ((h & 3) == 0) rectc(sx + 3, sy + 7, sx + 9, sy + 9, RGBA32(139, 130, 105, 255));
        if ((h & 7) == 1) rectc(sx + 6, sy + 3, sx + 9, sy + 8, RGBA32(44, 122, 54, 255)); /* weeds */
        break;
    case T_WATER:
        rectc(sx, sy, sx + TS, sy + TS, RGBA32(40, 90, 140, 255));
        if (((frame_no / 20 + (int)(h & 3)) & 3) == 0) rectc(sx + 3, sy + 6, sx + 11, sy + 7, RGBA32(120, 170, 210, 255));
        break;
    case T_TREE:
        rectc(sx + 6, sy + 8, sx + 10, sy + 16, RGBA32(92, 61, 37, 255));
        rectc(sx + 1, sy + 1, sx + 15, sy + 10, RGBA32(39, 105, 55, 255));
        rectc(sx + 4, sy, sx + 12, sy + 6, RGBA32(53, 130, 66, 255));
        break;
    case T_STUMP:
        rectc(sx + 5, sy + 9, sx + 11, sy + 14, RGBA32(92, 61, 37, 255));
        break;
    case T_BUSH:
        rectc(sx + 2, sy + 5, sx + 14, sy + 14, RGBA32(40, 100, 50, 255));
        rectc(sx + 4, sy + 7, sx + 6, sy + 9, RGBA32(190, 40, 60, 255));
        rectc(sx + 9, sy + 9, sx + 11, sy + 11, RGBA32(190, 40, 60, 255));
        break;
    case T_BUSH_E:
        rectc(sx + 2, sy + 5, sx + 14, sy + 14, RGBA32(40, 100, 50, 255));
        break;
    case T_RWALL:
        rectc(sx, sy, sx + TS, sy + TS, RGBA32(98, 100, 92, 255));
        rectc(sx, sy, sx + TS, sy + 3, RGBA32(65, 69, 65, 255));
        if ((h & 3) == 0) rectc(sx + 4, sy + 6, sx + 8, sy + 11, RGBA32(43, 61, 59, 255));
        if ((h & 3) != 3) rectc(sx + (int)(h >> 4 & 7), sy + 3, sx + (int)(h >> 4 & 7) + 3, sy + 14, RGBA32(50, 116, 56, 255)); /* vines */
        break;
    case T_RFLOOR:
        rectc(sx, sy, sx + TS, sy + TS, RGBA32(84, 80, 70, 255));
        if ((h & 3) < 2) rectc(sx + 2, sy + 3, sx + 12, sy + 10, RGBA32(52, 112, 58, 255)); /* moss */
        break;
    case T_BWALL:
        rectc(sx, sy, sx + TS, sy + TS, RGBA32(130, 92, 52, 255));
        rectc(sx, sy + 5, sx + TS, sy + 6, RGBA32(90, 60, 32, 255));
        rectc(sx, sy + 11, sx + TS, sy + 12, RGBA32(90, 60, 32, 255));
        break;
    case T_BFLOOR:
        rectc(sx, sy, sx + TS, sy + TS, RGBA32(150, 112, 70, 255));
        rectc(sx + 7, sy, sx + 8, sy + TS, RGBA32(110, 78, 46, 255));
        break;
    case T_FIRE:
        rectc(sx + 3, sy + 10, sx + 13, sy + 14, RGBA32(90, 60, 32, 255));
        rectc(sx + 5, sy + 3, sx + 11, sy + 11, ((frame_no >> 2) & 1) ? RGBA32(255, 140, 30, 255) : RGBA32(255, 200, 60, 255));
        break;
    }
}

static void draw_hud_rects(int camx, int camy) {
    (void)camx; (void)camy;
    rectc(0, 0, SW, 28, RGBA32(16, 24, 25, 255));
    rectc(0, 224, SW, SH, RGBA32(16, 24, 25, 255));
    /* bars */
    rectc(4, 4, 4 + 100, 9, RGBA32(40, 40, 40, 255));
    rectc(4, 4, 4 + hunger, 9, RGBA32(200, 140, 40, 255));
    rectc(4, 11, 4 + 100, 16, RGBA32(40, 40, 40, 255));
    rectc(4, 11, 4 + thirst, 16, RGBA32(60, 140, 220, 255));
    rectc(4, 18, 4 + 100, 23, RGBA32(40, 40, 40, 255));
    rectc(4, 18, 4 + health, 23, RGBA32(200, 50, 50, 255));
    if (msg_timer > 0) rectc(40, 196, 280, 212, RGBA32(20, 25, 22, 255));
}

static void draw_world_and_entities(int camx, int camy) {
    rdpq_set_mode_fill(RGBA32(70, 98, 62, 255));
    rect(0, 0, SW, SH);

    int tx0 = camx / TS, ty0 = camy / TS;
    for (int ty = ty0; ty <= ty0 + SH / TS + 1; ty++) {
        for (int tx = tx0; tx <= tx0 + SW / TS + 1; tx++) {
            if (tx < 0 || ty < 0 || tx >= MW || ty >= MH) continue;
            draw_tile(tx, ty, tx * TS - camx, ty * TS - camy);
        }
    }

    /* rabbits */
    for (int i = 0; i < NRABBIT; i++) {
        Rabbit *r = &rabbits[i];
        if (!r->alive) continue;
        int sx = r->x - camx, sy = r->y - camy;
        if (sx < -8 || sx > SW + 8 || sy < -8 || sy > SH + 8) continue;
        rectc(sx - 4, sy - 2, sx + 4, sy + 3, RGBA32(200, 190, 170, 255));
        rectc(sx + 2, sy - 6, sx + 4, sy - 2, RGBA32(200, 190, 170, 255));
    }

    /* fishing bobber */
    if (fish_state) {
        int bx = fish_tx * TS - camx + 8, by = fish_ty * TS - camy + 8;
        rectc(bx - 2, by - 2, bx + 2, by + 2, fish_state == 2 ? RGBA32(255, 60, 60, 255) : RGBA32(240, 240, 240, 255));
    }

    /* player */
    int sx = px - camx, sy = py - camy;
    rectc(sx - 5, sy - 6, sx + 5, sy + 7, RGBA32(39, 48, 72, 255));
    rectc(sx - 3, sy - 11, sx + 3, sy - 5, RGBA32(198, 160, 119, 255));
    static const int fx[4] = { 0, 6, 0, -6 };
    static const int fy[4] = { -8, 0, 8, 0 };
    rectc(sx + fx[face] - 1, sy + fy[face] - 1, sx + fx[face] + 1, sy + fy[face] + 1, RGBA32(255, 255, 255, 255));

    /* build ghost */
    if (build_mode) {
        int tx, ty;
        target_tile(&tx, &ty);
        int gx = tx * TS - camx, gy = ty * TS - camy;
        rectc(gx, gy, gx + TS, gy + 1, RGBA32(255, 255, 0, 255));
        rectc(gx, gy + TS - 1, gx + TS, gy + TS, RGBA32(255, 255, 0, 255));
        rectc(gx, gy, gx + 1, gy + TS, RGBA32(255, 255, 0, 255));
        rectc(gx + TS - 1, gy, gx + TS, gy + TS, RGBA32(255, 255, 0, 255));
    }
}

static void draw_text(const char *s, int x, int y) {
    rdpq_text_printf(NULL, FONT_BUILTIN_DEBUG_MONO, x, y, "%s", s);
}

static void draw_hud_text(void) {
    char line[64];
    draw_text("HUNGER", 108, 3);
    draw_text("WATER", 108, 10);
    draw_text("HEALTH", 108, 17);
    snprintf(line, sizeof(line), "WOOD:%d SCRAP:%d", wood, scrap);
    draw_text(line, 170, 8);
    snprintf(line, sizeof(line), "FISH:%d/%d MEAT:%d/%d BERRY:%d", fish, cfish, meat, cmeat, berry);
    draw_text(line, 170, 19);
    if (build_mode) {
        snprintf(line, sizeof(line), "BUILD:%s (%d wood)  A place Z remove R next L exit", build_name[build_sel], build_cost[build_sel]);
        draw_text(line, 4, 229);
    } else {
        draw_text("A:act Z:fish B:run Cdown:eat L:build", 4, 229);
    }
    if (msg_timer > 0) draw_text(message, 46, 201);
    if (fish_state == 2) draw_text("BITE! PRESS Z!", 120, 120);
}

int main(void) {
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
    rdpq_init();
    joypad_init();
    rdpq_text_register_font(FONT_BUILTIN_DEBUG_MONO, rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));

    reset_game();

    while (1) {
        joypad_poll();
        joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);

        if (dead) {
            if (pressed.start) reset_game();
        } else {
            int sx = (in.stick_x > DEADZONE || in.stick_x < -DEADZONE) ? in.stick_x : 0;
            int sy = (in.stick_y > DEADZONE || in.stick_y < -DEADZONE) ? in.stick_y : 0;
            int speed = held.b ? 4 : 2;
            int dx = (sx * speed) / 64;
            int dy = (-sy * speed) / 64;   /* stick up = screen up */

            if (dx || dy) {
                if (abs(dx) >= abs(dy)) face = dx > 0 ? 1 : 3;
                else face = dy > 0 ? 2 : 0;
                if (fish_state && (abs(sx) > 30 || abs(sy) > 30)) { fish_state = 0; say("Fishing canceled"); }
            }
            if (!blocked(px + dx, py)) px = clampi(px + dx, 8, MW * TS - 8);
            if (!blocked(px, py + dy)) py = clampi(py + dy, 8, MH * TS - 8);

            if (pressed.l) { build_mode = !build_mode; fish_state = 0; }
            if (pressed.r && build_mode) build_sel = (build_sel + 1) % B_COUNT;
            if (pressed.c_down) do_eat();

            if (build_mode) {
                if (pressed.a) do_build(1);
                if (pressed.z) do_build(0);
            } else {
                if (pressed.a) do_action_a();
                if (pressed.z) do_fishing();
            }

            if (fish_state == 1 && --fish_timer <= 0) { fish_state = 2; fish_timer = 45; }
            else if (fish_state == 2 && --fish_timer <= 0) { fish_state = 0; say("The fish got away"); }
            if (drink_cd > 0) drink_cd--;
            if (msg_timer > 0) msg_timer--;

            update_rabbits();
            update_survival();
            frame_no++;
        }

        int camx = clampi(px - SW / 2, 0, MW * TS - SW);
        int camy = clampi(py - SH / 2, 0, MH * TS - SH);

        surface_t *disp = display_get();
        rdpq_attach_clear(disp, NULL);
        draw_world_and_entities(camx, camy);
        draw_hud_rects(camx, camy);
        if (dead) {
            rdpq_set_mode_fill(RGBA32(120, 0, 0, 255));
            rect(80, 100, 240, 140);
        }
        draw_hud_text();
        if (dead) {
            draw_text("YOU DIED - PRESS START", 94, 116);
        }
        rdpq_detach_show();
    }
    return 0;
}
