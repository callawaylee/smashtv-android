#include "touch.h"
#include "libretro.h"
#include <math.h>

#define BIT(n) (1u << (n))
#define MAXF 10

static bool enabled;
static struct { SDL_FingerID id; int role; float x, y; } fingers[MAXF];   /* role: 1 move, 2 fire, 3 coin, 4 start */
static float out_w, out_h;
SDL_FRect touch_last_game;

void touch_set_enabled(bool on) { enabled = on; }
bool touch_enabled(void) { return enabled; }
void touch_release_all(void) { SDL_memset(fingers, 0, sizeof fingers); }

/* ---------------------------------------------------------------- layout */
typedef struct { float x, y, r; } circle;
static void layout(SDL_FRect game, circle *move, circle *fire, circle btn[3])
{
    float side = game.x;                     /* empty space left and right of the game */
    bool overlay = side < out_h * 0.30f;     /* tablets / 4:3 screens: sticks sit over the game's corners */
    float r = overlay ? out_h * 0.12f : SDL_min(side * 0.36f, out_h * 0.17f);
    float cx = overlay ? r * 1.35f : side * 0.5f;
    float cy = out_h * 0.61f;
    if (overlay) cy = out_h - r * 1.5f;
    *move = (circle){cx, cy, r};
    *fire = (circle){out_w - cx, cy, r};
    float br = SDL_max(out_h * 0.042f, 22.0f), by = br * 1.9f;
    btn[0] = (circle){br * 2.4f, by, br};                 /* pause */
    btn[1] = (circle){br * 2.4f + br * 2.8f, by, br};     /* coin */
    btn[2] = (circle){out_w - br * 2.4f, by, br};         /* start */
}

static bool inside(circle c, float x, float y, float extra)
{
    float dx = x - c.x, dy = y - c.y, r = c.r * extra;
    return dx * dx + dy * dy <= r * r;
}

/* ---------------------------------------------------------------- events */
int touch_event(const SDL_Event *e, SDL_Renderer *ren, SDL_FRect game)
{
    if (!enabled) return TOUCH_NONE;
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION &&
        e->type != SDL_EVENT_FINGER_UP && e->type != SDL_EVENT_FINGER_CANCELED) return TOUCH_NONE;
    int w, h;
    SDL_GetRenderOutputSize(ren, &w, &h);
    out_w = (float)w; out_h = (float)h;
    float x = e->tfinger.x * out_w, y = e->tfinger.y * out_h;
    circle mv, fr, btn[3];
    layout(game, &mv, &fr, btn);
    int slot = -1, free_slot = -1;
    for (int i = 0; i < MAXF; i++) {
        if (fingers[i].role && fingers[i].id == e->tfinger.fingerID) slot = i;
        if (!fingers[i].role && free_slot < 0) free_slot = i;
    }
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (inside(btn[0], x, y, 1.5f)) return TOUCH_PAUSE;
        if (free_slot < 0) return TOUCH_NONE;
        int role;
        if (inside(btn[1], x, y, 1.5f)) role = 3;
        else if (inside(btn[2], x, y, 1.5f)) role = 4;
        else role = x < out_w * 0.5f ? 1 : 2;    /* anywhere on a half drives that half's stick */
        fingers[free_slot].id = e->tfinger.fingerID;
        fingers[free_slot].role = role;
        fingers[free_slot].x = x; fingers[free_slot].y = y;
    } else if (slot >= 0) {
        if (e->type == SDL_EVENT_FINGER_MOTION) { fingers[slot].x = x; fingers[slot].y = y; }
        else fingers[slot].role = 0;
    }
    return TOUCH_NONE;
}

/* direction of a stick as 8-way (dx, dy each -1..1), 0,0 inside the dead zone */
static void stick_dir(circle c, float fx, float fy, int *dx, int *dy)
{
    float vx = fx - c.x, vy = fy - c.y, len = SDL_sqrtf(vx * vx + vy * vy);
    *dx = *dy = 0;
    if (len < c.r * 0.22f) return;
    float a = SDL_atan2f(vy, vx);
    int sector = (int)SDL_lroundf(a / (SDL_PI_F / 4)) & 7;       /* 0 = right, counter-clockwise in screen y-down */
    static const int tx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, ty[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    *dx = tx[sector]; *dy = ty[sector];
}

uint16_t touch_mask(void)
{
    if (!enabled || out_w <= 0) return 0;
    uint16_t m = 0;
    for (int i = 0; i < MAXF; i++) {
        if (!fingers[i].role) continue;
        if (fingers[i].role == 3) { m |= BIT(RETRO_DEVICE_ID_JOYPAD_SELECT); continue; }
        if (fingers[i].role == 4) { m |= BIT(RETRO_DEVICE_ID_JOYPAD_START); continue; }
        /* stick centres need the current layout; it was stored on the last event */
        circle mv, fr, btn[3];
        layout(touch_last_game, &mv, &fr, btn);
        int dx, dy;
        if (fingers[i].role == 1) {
            stick_dir(mv, fingers[i].x, fingers[i].y, &dx, &dy);
            if (dx < 0) m |= BIT(RETRO_DEVICE_ID_JOYPAD_LEFT);
            if (dx > 0) m |= BIT(RETRO_DEVICE_ID_JOYPAD_RIGHT);
            if (dy < 0) m |= BIT(RETRO_DEVICE_ID_JOYPAD_UP);
            if (dy > 0) m |= BIT(RETRO_DEVICE_ID_JOYPAD_DOWN);
        } else {
            stick_dir(fr, fingers[i].x, fingers[i].y, &dx, &dy);
            if (dx < 0) m |= BIT(RETRO_DEVICE_ID_JOYPAD_Y);     /* fire left  */
            if (dx > 0) m |= BIT(RETRO_DEVICE_ID_JOYPAD_A);     /* fire right */
            if (dy < 0) m |= BIT(RETRO_DEVICE_ID_JOYPAD_X);     /* fire up    */
            if (dy > 0) m |= BIT(RETRO_DEVICE_ID_JOYPAD_B);     /* fire down  */
        }
    }
    return m;
}

/* ---------------------------------------------------------------- drawing */

static void disc(SDL_Renderer *ren, float cx, float cy, float r, SDL_FColor col)
{
    enum { N = 48 };
    SDL_Vertex v[N + 1];
    int idx[N * 3];
    v[0] = (SDL_Vertex){{cx, cy}, col, {0, 0}};
    for (int i = 0; i < N; i++) {
        float a = (float)i / N * 2 * SDL_PI_F;
        v[i + 1] = (SDL_Vertex){{cx + SDL_cosf(a) * r, cy + SDL_sinf(a) * r}, col, {0, 0}};
        idx[i * 3] = 0; idx[i * 3 + 1] = i + 1; idx[i * 3 + 2] = (i + 1) % N + 1;
    }
    SDL_RenderGeometry(ren, NULL, v, N + 1, idx, N * 3);
}

static void ring(SDL_Renderer *ren, float cx, float cy, float r, float t, SDL_FColor col)
{
    enum { N = 64 };
    SDL_Vertex v[N * 2];
    int idx[N * 6];
    for (int i = 0; i < N; i++) {
        float a = (float)i / N * 2 * SDL_PI_F, c = SDL_cosf(a), s = SDL_sinf(a);
        v[i * 2] = (SDL_Vertex){{cx + c * (r - t / 2), cy + s * (r - t / 2)}, col, {0, 0}};
        v[i * 2 + 1] = (SDL_Vertex){{cx + c * (r + t / 2), cy + s * (r + t / 2)}, col, {0, 0}};
        int j = (i + 1) % N;
        int *q = idx + i * 6;
        q[0] = i * 2; q[1] = i * 2 + 1; q[2] = j * 2; q[3] = j * 2; q[4] = i * 2 + 1; q[5] = j * 2 + 1;
    }
    SDL_RenderGeometry(ren, NULL, v, N * 2, idx, N * 6);
}

static void label(SDL_Renderer *ren, float cx, float cy, float scale, const char *s, SDL_FColor col)
{
    SDL_SetRenderScale(ren, scale, scale);
    SDL_SetRenderDrawColorFloat(ren, col.r, col.g, col.b, col.a);
    SDL_RenderDebugText(ren, (cx - SDL_strlen(s) * 4 * scale) / scale, (cy - 4 * scale) / scale, s);
    SDL_SetRenderScale(ren, 1, 1);
}

void touch_render(SDL_Renderer *ren, SDL_FRect game)
{
    touch_last_game = game;
    if (!enabled) return;
    int w, h;
    SDL_GetRenderOutputSize(ren, &w, &h);
    out_w = (float)w; out_h = (float)h;
    circle mv, fr, btn[3];
    layout(game, &mv, &fr, btn);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    float ts = SDL_max(1.0f, SDL_floorf(out_h / 360.0f));
    float lw = SDL_max(3.0f, out_h * 0.005f);

    /* movement stick (white) and fire stick (red) */
    const SDL_FColor white = {1, 1, 1, 1}, red = {1, 0.36f, 0.28f, 1};
    for (int s = 0; s < 2; s++) {
        circle c = s ? fr : mv;
        SDL_FColor col = s ? red : white;
        disc(ren, c.x, c.y, c.r, (SDL_FColor){col.r, col.g, col.b, 0.08f});
        ring(ren, c.x, c.y, c.r, lw, (SDL_FColor){col.r, col.g, col.b, s ? 0.55f : 0.32f});
        if (s) for (int i = 0; i < 8; i++) {      /* the 8 firing directions */
            float a = i * SDL_PI_F / 4;
            disc(ren, c.x + SDL_cosf(a) * c.r, c.y + SDL_sinf(a) * c.r, lw * 1.3f, (SDL_FColor){1, 0.47f, 0.4f, 0.7f});
        }
        float kx = c.x, ky = c.y;
        for (int i = 0; i < MAXF; i++)
            if (fingers[i].role == (s ? 2 : 1)) {
                float vx = fingers[i].x - c.x, vy = fingers[i].y - c.y, len = SDL_sqrtf(vx * vx + vy * vy), lim = c.r * 0.75f;
                if (len > lim) { vx *= lim / len; vy *= lim / len; }
                kx = c.x + vx; ky = c.y + vy;
            }
        disc(ren, kx, ky, c.r * 0.41f, (SDL_FColor){col.r, col.g, col.b, s ? 0.66f : 0.47f});
        if (c.y + c.r + 20 * ts < out_h)
            label(ren, c.x, c.y + c.r + 14 * ts, ts * 1.5f, s ? "FIRE" : "MOVE", (SDL_FColor){col.r, col.g, col.b, 0.6f});
    }

    /* pause / coin / start */
    static const char *names[3] = {"II", "COIN", "START"};
    for (int b = 0; b < 3; b++) {
        bool held = false;
        for (int i = 0; i < MAXF; i++) if (fingers[i].role == b + 2 && b > 0) held = true;
        disc(ren, btn[b].x, btn[b].y, btn[b].r, (SDL_FColor){1, 1, 1, held ? 0.35f : 0.11f});
        ring(ren, btn[b].x, btn[b].y, btn[b].r, lw * 0.7f, (SDL_FColor){1, 1, 1, 0.35f});
        float s = SDL_max(1.5f, btn[b].r * 1.45f / (SDL_strlen(names[b]) * 8));
        label(ren, btn[b].x, btn[b].y, s, names[b], (SDL_FColor){0.92f, 0.92f, 0.94f, 0.9f});
    }
}
