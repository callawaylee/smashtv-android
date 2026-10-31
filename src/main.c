/*
 * Smash T.V. Native — a standalone SDL3 player for the Smash T.V. arcade game.
 *
 * Runs the game through a cut-down MAME libretro core (only the Williams
 * Y-unit driver) that ships next to this executable. You supply the ROMs;
 * on first launch drag smashtv.zip (or the folder of ROM files) onto the
 * window, or press Enter to browse.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "libretro.h"
#include "romset.h"
#include "touch.h"

#define APP_NAME    "Smash T.V."
#define APP_VERSION "1.2.0"
#define GAME_NAME   "smashtv"

#if defined(SDL_PLATFORM_ANDROID)
#define CORE_FILE "libmame_libretro.so"   /* packed in the APK's lib folder */
#elif defined(_WIN32)
#define CORE_FILE "mame_libretro.dll"
#elif defined(__APPLE__)
#define CORE_FILE "mame_libretro.dylib"
#else
#define CORE_FILE "mame_libretro.so"
#endif

/* ------------------------------------------------------------------ core API */
static struct {
    SDL_SharedObject *lib;
    void (*init)(void);
    void (*deinit)(void);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*reset)(void);
    void (*run)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
} core;

/* ------------------------------------------------------------------ state */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tex_w, tex_h;
static SDL_PixelFormat tex_fmt = SDL_PIXELFORMAT_XRGB1555;
static enum retro_pixel_format core_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static float aspect = 4.0f / 3.0f;
static double core_fps = 60.0;
static double sample_rate = 48000.0;
static SDL_AudioStream *audio;
static bool have_frame;

static char pref_dir[1024], rom_dir[1024], sys_dir[1024], save_dir[1024], rom_zip[1200];
static char log_path[1200], settings_path[1200], state_path[1200];
static FILE *log_file;

static struct {
    bool free_play;      /* unlimited continues */
    bool smooth;
    bool scanlines;
    bool fullscreen;
    bool skip_boot;      /* always on: the ~20 s arcade power-on self-test is fast-forwarded */
} cfg = {true, false, true, false, true};

#define BOOT_FRAMES 1150
static int boot_left;
static bool frame_is_selftest, frame_is_blank;
static int selftest_extra;

typedef struct { char key[64]; char value[64]; } core_var;
static core_var vars[64];
static int nvars;

/* input */
static SDL_Gamepad *pads[2];
static uint16_t joy_mask[2];
static int16_t joy_axis[2][4]; /* lx ly rx ry */
static int start_seq[2];       /* free-play coin+start sequence frame counter */
static bool start_prev[2];

/* OSD message */
static char osd_msg[256];
static Uint64 osd_until;

/* menu */
enum { M_RESUME, M_FREEPLAY, M_SAVE, M_LOAD, M_SMOOTH, M_SCAN, M_FULL, M_HELP, M_RESET, M_QUIT, M_COUNT };
static bool menu_open, help_open;
static int menu_sel;
static bool quit_requested;

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    SDL_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stderr);
    if (log_file) { fputs(buf, log_file); fflush(log_file); }
}

static void osd(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(osd_msg, sizeof osd_msg, fmt, ap);
    va_end(ap);
    osd_until = SDL_GetTicks() + 2500;
    logf_("[osd] %s\n", osd_msg);
}

/* ------------------------------------------------------------------ settings */
static void load_settings(void) {
    size_t n = 0;
    char *t = SDL_LoadFile(settings_path, &n);
    if (!t) return;
    char *save = NULL;
    for (char *line = SDL_strtok_r(t, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        bool v = SDL_atoi(eq + 1) != 0;
        if (!strcmp(line, "free_play")) cfg.free_play = v;
        else if (!strcmp(line, "smooth")) cfg.smooth = v;
        else if (!strcmp(line, "scanlines")) cfg.scanlines = v;
        else if (!strcmp(line, "fullscreen")) cfg.fullscreen = v;
    }
    SDL_free(t);
}

static void save_settings(void) {
    char buf[256];
    int n = SDL_snprintf(buf, sizeof buf, "free_play=%d\nsmooth=%d\nscanlines=%d\nfullscreen=%d\nskip_boot=%d\n",
                         cfg.free_play, cfg.smooth, cfg.scanlines, cfg.fullscreen, cfg.skip_boot);
    SDL_SaveFile(settings_path, buf, (size_t)n);
}

/* ------------------------------------------------------------------ libretro callbacks */
static void RETRO_CALLCONV core_log(enum retro_log_level level, const char *fmt, ...) {
    static const char *lv[] = {"debug", "info", "warn", "error"};
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (level == RETRO_LOG_DEBUG) return;
    logf_("[core %s] %s", lv[level & 3], buf);
    size_t l = strlen(buf);
    if (l && buf[l - 1] != '\n') logf_("\n");
}

static const char *var_get(const char *key) {
    for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].key, key)) return vars[i].value;
    return NULL;
}

static void var_set(const char *key, const char *value) {
    for (int i = 0; i < nvars; i++)
        if (!strcmp(vars[i].key, key)) { SDL_strlcpy(vars[i].value, value, sizeof vars[i].value); return; }
    if (nvars < (int)SDL_arraysize(vars)) {
        SDL_strlcpy(vars[nvars].key, key, sizeof vars[nvars].key);
        SDL_strlcpy(vars[nvars].value, value, sizeof vars[nvars].value);
        nvars++;
    }
}

/* Our fixed overrides of the core's defaults. */
static void apply_core_overrides(void) {
    var_set("mame_saves", "game");
    var_set("mame_auto_save", "disabled");
    var_set("mame_boot_to_osd", "disabled");
    var_set("mame_boot_to_bios", "disabled");
    var_set("mame_softlists_enable", "disabled");
    var_set("mame_cheats_enable", "disabled");
    var_set("mame_read_config", "disabled");
    var_set("mame_write_config", "disabled");
    var_set("mame_mame_paths_enable", "disabled");
    var_set("mame_throttle", "disabled");
    var_set("mame_alternate_renderer", "disabled");
    var_set("mame_mame_4way_enable", "disabled");
}

static void set_geometry(const struct retro_game_geometry *g) {
    if (g->aspect_ratio > 0.1f) aspect = g->aspect_ratio;
    else if (g->base_height) aspect = (float)g->base_width / (float)g->base_height;
}

static bool RETRO_CALLCONV environment(unsigned cmd, void *data) {
    cmd &= ~RETRO_ENVIRONMENT_EXPERIMENTAL;
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(enum retro_pixel_format *)data;
        if (f == RETRO_PIXEL_FORMAT_XRGB8888) { core_fmt = f; tex_fmt = SDL_PIXELFORMAT_XRGB8888; return true; }
        if (f == RETRO_PIXEL_FORMAT_RGB565) { core_fmt = f; tex_fmt = SDL_PIXELFORMAT_RGB565; return true; }
        if (f == RETRO_PIXEL_FORMAT_0RGB1555) { core_fmt = f; tex_fmt = SDL_PIXELFORMAT_XRGB1555; return true; }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = sys_dir; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = save_dir; return true;
    case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY: *(const char **)data = rom_dir; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = core_log; return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        for (const struct retro_variable *v = data; v && v->key; v++) {
            /* "Description; default|other|..." */
            const char *semi = strchr(v->value, ';');
            const char *s = semi ? semi + 1 : v->value;
            while (*s == ' ') s++;
            char def[64];
            size_t i = 0;
            while (s[i] && s[i] != '|' && i < sizeof def - 1) { def[i] = s[i]; i++; }
            def[i] = 0;
            if (!var_get(v->key)) var_set(v->key, def);
        }
        apply_core_overrides();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        v->value = var_get(v->key);
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: set_geometry(data); return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        const struct retro_system_av_info *av = data;
        set_geometry(&av->geometry);
        if (av->timing.fps > 1) core_fps = av->timing.fps;
        if (av->timing.sample_rate > 1000 && av->timing.sample_rate != sample_rate) {
            sample_rate = av->timing.sample_rate;
            if (audio) {
                SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)sample_rate};
                SDL_SetAudioStreamFormat(audio, &spec, NULL);
            }
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING: *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE: *(unsigned *)data = RETRO_LANGUAGE_ENGLISH; return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: osd("%s", ((const struct retro_message *)data)->msg); return true;
    case RETRO_ENVIRONMENT_SET_ROTATION:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        return true;
    default:
        return false;
    }
}

static void RETRO_CALLCONV video_refresh(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data || !w || !h) return;
    if (!tex || (int)w != tex_w || (int)h != tex_h || tex->format != tex_fmt) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, tex_fmt, SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
        tex_w = (int)w; tex_h = (int)h;
        SDL_SetTextureScaleMode(tex, cfg.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        logf_("video: %ux%u\n", w, h);
    }
    SDL_UpdateTexture(tex, NULL, data, (int)pitch);
    have_frame = true;
    /* Spot the arcade's ROM/RAM self-test screen (green chip boxes on black) so it is never shown */
    if (core_fmt != RETRO_PIXEL_FORMAT_XRGB8888) {
        int green = 0, black = 0, n = 0;
        for (unsigned y = 4; y < h; y += 8)
            for (unsigned x = 4; x < w; x += 8, n++) {
                uint16_t v = ((const uint16_t *)((const uint8_t *)data + y * pitch))[x];
                int r, g, b;
                if (core_fmt == RETRO_PIXEL_FORMAT_RGB565) { r = v >> 11; g = (v >> 6) & 31; b = v & 31; }
                else { r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31; }
                if (g > 18 && r < 12 && b < 12) green++;
                else if (r < 3 && g < 3 && b < 3) black++;
            }
        frame_is_selftest = n && green * 100 > n * 6 && black * 100 > n * 55;
        /* nearly empty screen: the boot messages (e.g. "default adjustments restored") between self test and title */
        frame_is_blank = n && black * 100 > n * 88;
    }
}

static size_t RETRO_CALLCONV audio_batch(const int16_t *data, size_t frames) {
    if (audio) SDL_PutAudioStreamData(audio, data, (int)(frames * 4));
    return frames;
}

static void RETRO_CALLCONV audio_sample(int16_t l, int16_t r) {
    int16_t s[2] = {l, r};
    audio_batch(s, 1);
}

static void RETRO_CALLCONV input_poll(void) {}

static int16_t RETRO_CALLCONV input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port > 1) return 0;
    switch (device) {
    case RETRO_DEVICE_JOYPAD:
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)joy_mask[port];
        return (joy_mask[port] >> id) & 1;
    case RETRO_DEVICE_ANALOG:
        if (index > 1 || id > 1) return 0;
        return joy_axis[port][index * 2 + id];
    default:
        return 0; /* keyboard/mouse/lightgun deliberately not exposed */
    }
}

static void run_frame(void) { core.run(); }

/* ------------------------------------------------------------------ core loading */
#define LOADSYM(field, name)                                                      \
    do {                                                                          \
        *(void **)&core.field = (void *)SDL_LoadFunction(core.lib, name);         \
        if (!core.field) { SDL_snprintf(err, errlen, "Core is missing %s", name); return false; } \
    } while (0)

static bool load_core(char *err, size_t errlen) {
    const char *base = SDL_GetBasePath();
    const char *tries[4];
    char p1[1200], p2[1200], p3[1200];
    SDL_snprintf(p1, sizeof p1, "%s%s", base ? base : "", CORE_FILE);
    SDL_snprintf(p2, sizeof p2, "%s../Frameworks/%s", base ? base : "", CORE_FILE);
    SDL_snprintf(p3, sizeof p3, "%s../lib/smashtv-native/%s", base ? base : "", CORE_FILE);
    tries[0] = p1; tries[1] = p2; tries[2] = p3; tries[3] = NULL;
#ifdef SDL_PLATFORM_ANDROID
    core.lib = SDL_LoadObject(CORE_FILE);
#endif
    for (int i = 0; tries[i] && !core.lib; i++) {
        core.lib = SDL_LoadObject(tries[i]);
        if (core.lib) logf_("core: %s\n", tries[i]);
    }
    if (!core.lib) {
        SDL_snprintf(err, errlen, "Can't find the emulator core (%s).\nIt must be in the same folder as this program:\n%s", CORE_FILE, base ? base : "?");
        return false;
    }
    LOADSYM(init, "retro_init");
    LOADSYM(deinit, "retro_deinit");
    LOADSYM(set_environment, "retro_set_environment");
    LOADSYM(set_video_refresh, "retro_set_video_refresh");
    LOADSYM(set_audio_sample, "retro_set_audio_sample");
    LOADSYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
    LOADSYM(set_input_poll, "retro_set_input_poll");
    LOADSYM(set_input_state, "retro_set_input_state");
    LOADSYM(get_system_info, "retro_get_system_info");
    LOADSYM(get_system_av_info, "retro_get_system_av_info");
    LOADSYM(set_controller_port_device, "retro_set_controller_port_device");
    LOADSYM(reset, "retro_reset");
    LOADSYM(run, "retro_run");
    LOADSYM(serialize_size, "retro_serialize_size");
    LOADSYM(serialize, "retro_serialize");
    LOADSYM(unserialize, "retro_unserialize");
    LOADSYM(load_game, "retro_load_game");
    LOADSYM(unload_game, "retro_unload_game");
    return true;
}

/* ------------------------------------------------------------------ text drawing */
static float ui_scale(void) {
    int w, h;
    SDL_GetRenderOutputSize(ren, &w, &h);
    float s = (float)h / 240.0f;
    return s < 1 ? 1 : SDL_floorf(s);
}

static void text_at(float x, float y, float scale, const char *s, Uint8 r, Uint8 g, Uint8 b) {
    SDL_SetRenderScale(ren, scale, scale);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderDebugText(ren, x / scale + 1, y / scale + 1, s);
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    SDL_RenderDebugText(ren, x / scale, y / scale, s);
    SDL_SetRenderScale(ren, 1, 1);
}

static void text_center(float y, float scale, const char *s, Uint8 r, Uint8 g, Uint8 b) {
    int w, h;
    SDL_GetRenderOutputSize(ren, &w, &h);
    float tw = (float)strlen(s) * 8 * scale;
    text_at(((float)w - tw) / 2, y, scale, s, r, g, b);
}

static void dim_screen(Uint8 alpha) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, alpha);
    SDL_RenderFillRect(ren, NULL);
}

/* ------------------------------------------------------------------ rendering */
static SDL_FRect game_rect(void) {
    int w, h;
    SDL_GetRenderOutputSize(ren, &w, &h);
    float dw = (float)w, dh = dw / aspect;
    if (dh > h) { dh = (float)h; dw = dh * aspect; }
    SDL_FRect r = {((float)w - dw) / 2, ((float)h - dh) / 2, dw, dh};
    return r;
}

static void draw_scanlines(SDL_FRect r) {
    if (!tex_h) return;
    float line = r.h / (float)tex_h;
    if (line < 2.5f) return; /* too small to look good */
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 90);
    static SDL_FRect rects[1024];
    int n = 0;
    float th = SDL_floorf(line * 0.35f);
    if (th < 1) th = 1;
    for (int y = 0; y < tex_h && n < 1024; y++)
        rects[n++] = (SDL_FRect){r.x, r.y + (y + 1) * line - th, r.w, th};
    SDL_RenderFillRects(ren, rects, n);
}

static const char *HELP[] = {
    "CONTROLS",
    "",
    "GAMEPAD   Left stick / D-pad  move",
    "          Right stick         fire",
    "          A B X Y             fire (4 ways)",
    "          Start               start / continue",
    "          Back / Select       insert coin",
    "          Guide or Back+Start this menu",
    "",
    "KEYBOARD  W A S D             move",
    "          Arrow keys / IJKL   fire",
    "          1 or Enter          start / continue",
    "          5                   insert coin",
    "          Esc                 this menu",
    "          F5 / F9             save / load state",
    "          F11 or Alt+Enter    fullscreen",
    "",
    "Player 2 uses the second gamepad.",
};

static void render_menu(void) {
    dim_screen(170);
    float s = ui_scale();
    int w, h;
    SDL_GetRenderOutputSize(ren, &w, &h);
    if (help_open) {
        float y = (float)h / 2 - SDL_arraysize(HELP) * 10 * s / 2;
        float x = ((float)w - 40 * 8 * s) / 2;
        for (size_t i = 0; i < SDL_arraysize(HELP); i++, y += 10 * s)
            text_at(x < 0 ? 0 : x, y, s, HELP[i], 255, i == 0 ? 220 : 255, i == 0 ? 60 : 255);
        text_center((float)h - 14 * s, s, "Press any button to go back", 160, 160, 160);
        return;
    }
    char items[M_COUNT][64];
    SDL_snprintf(items[M_RESUME], 64, "Resume");
    SDL_snprintf(items[M_FREEPLAY], 64, "Unlimited continues: %s", cfg.free_play ? "ON" : "OFF");
    SDL_snprintf(items[M_SAVE], 64, "Save state");
    SDL_snprintf(items[M_LOAD], 64, "Load state");
    SDL_snprintf(items[M_SMOOTH], 64, "Picture: %s", cfg.smooth ? "smooth" : "sharp");
    SDL_snprintf(items[M_SCAN], 64, "Scanlines: %s", cfg.scanlines ? "ON" : "OFF");
    SDL_snprintf(items[M_FULL], 64, "Fullscreen: %s", cfg.fullscreen ? "ON" : "OFF");
    SDL_snprintf(items[M_HELP], 64, "Controls");
    SDL_snprintf(items[M_RESET], 64, "Restart game");
    SDL_snprintf(items[M_QUIT], 64, "Quit");
    float row = touch_enabled() ? 18 * s : 12 * s;
    float big = s * 2, y = (float)h / 2 - (M_COUNT * row + 30 * s) / 2;
    text_center(y, big, "PAUSED", 255, 220, 60);
    y += 30 * s;
    for (int i = 0; i < M_COUNT; i++, y += row) {
        char line[80];
        SDL_snprintf(line, sizeof line, i == menu_sel ? "> %s <" : "  %s  ", items[i]);
        if (i == menu_sel) text_center(y, s, line, 255, 255, 255);
        else text_center(y, s, line, 150, 150, 150);
    }
}

static void render_frame(void) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (boot_left > 0) {
        int w, h;
        SDL_GetRenderOutputSize(ren, &w, &h);
        float s = ui_scale();
        text_center((float)h / 2 - 20 * s, s * 2, "SMASH T.V.", 255, 210, 40);
        float pw = (float)w * 0.4f, done = 1.0f - (float)SDL_min(boot_left, BOOT_FRAMES) / BOOT_FRAMES;
        SDL_FRect bar = {((float)w - pw) / 2, (float)h / 2 + 10 * s, pw, 6 * s};
        SDL_SetRenderDrawColor(ren, 60, 60, 70, 255);
        SDL_RenderFillRect(ren, &bar);
        bar.w *= done;
        SDL_SetRenderDrawColor(ren, 255, 210, 40, 255);
        SDL_RenderFillRect(ren, &bar);
    } else if (tex && have_frame) {
        SDL_FRect r = game_rect();
        SDL_RenderTexture(ren, tex, NULL, &r);
        if (cfg.scanlines) draw_scanlines(r);
    }
    if (tex && have_frame && boot_left <= 0 && !menu_open) touch_render(ren, game_rect());
    if (menu_open) render_menu();
    if (osd_msg[0] && SDL_GetTicks() < osd_until) {
        float s = ui_scale();
        text_at(8 * s, 8 * s, s, osd_msg, 255, 255, 120);
    }
    SDL_RenderPresent(ren);
}

/* ------------------------------------------------------------------ actions */
static void set_fullscreen(bool on) {
    cfg.fullscreen = on;
    SDL_SetWindowFullscreen(win, on);
    SDL_HideCursor();
    if (!on) SDL_ShowCursor();
    save_settings();
}

static void save_state(void) {
    size_t n = core.serialize_size();
    void *buf = n ? SDL_malloc(n) : NULL;
    if (buf && core.serialize(buf, n) && SDL_SaveFile(state_path, buf, n)) osd("State saved");
    else osd("Couldn't save state");
    SDL_free(buf);
}

static void load_state(void) {
    size_t n = 0;
    void *buf = SDL_LoadFile(state_path, &n);
    if (!buf) { osd("No saved state yet (F5 saves)"); return; }
    if (core.unserialize(buf, n)) osd("State loaded");
    else osd("Couldn't load state");
    SDL_free(buf);
    if (audio) SDL_ClearAudioStream(audio);
}

static void apply_smooth(void) {
    if (tex) SDL_SetTextureScaleMode(tex, cfg.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
}

static void menu_activate(int item, int dir) {
    switch (item) {
    case M_RESUME: menu_open = false; break;
    case M_FREEPLAY: cfg.free_play = !cfg.free_play; save_settings(); break;
    case M_SAVE: save_state(); menu_open = false; break;
    case M_LOAD: load_state(); menu_open = false; break;
    case M_SMOOTH: cfg.smooth = !cfg.smooth; apply_smooth(); save_settings(); break;
    case M_SCAN: cfg.scanlines = !cfg.scanlines; save_settings(); break;
    case M_FULL: set_fullscreen(!cfg.fullscreen); break;
    case M_HELP: if (dir == 0) help_open = true; break;
    case M_RESET:
        if (dir == 0) {
            core.reset();
            menu_open = false;
            boot_left = BOOT_FRAMES; selftest_extra = 0;
            osd("Game restarted");
        }
        break;
    case M_QUIT: if (dir == 0) quit_requested = true; break;
    }
}

static void open_menu(void) {
    menu_open = true;
    help_open = false;
    menu_sel = 0;
    if (audio) SDL_ClearAudioStream(audio);
}

/* a tap on the pause menu: pick the row under the finger */
static void menu_tap(float fx, float fy) {
    if (help_open) { help_open = false; return; }
    int w, h;
    SDL_GetRenderOutputSize(ren, &w, &h);
    float s = ui_scale(), row = touch_enabled() ? 18 * s : 12 * s;
    float y0 = (float)h / 2 - (M_COUNT * row + 30 * s) / 2 + 30 * s - row * 0.3f;
    int i = (int)SDL_floorf((fy * (float)h - y0) / row);
    (void)fx;
    if (i < 0 || i >= M_COUNT) return;
    menu_sel = i;
    menu_activate(i, 0);
}

/* menu navigation: 0=up 1=down 2=left 3=right 4=ok 5=back */
static void menu_input(int a) {
    if (help_open) { help_open = false; return; }
    switch (a) {
    case 0: menu_sel = (menu_sel + M_COUNT - 1) % M_COUNT; break;
    case 1: menu_sel = (menu_sel + 1) % M_COUNT; break;
    case 2: case 3: menu_activate(menu_sel, a == 2 ? -1 : 1); break;
    case 4: menu_activate(menu_sel, 0); break;
    case 5: menu_open = false; break;
    }
}

/* ------------------------------------------------------------------ input */
static void pad_added(SDL_JoystickID id) {
    for (int i = 0; i < 2; i++) if (pads[i] && SDL_GetGamepadID(pads[i]) == id) return;
    for (int i = 0; i < 2; i++) {
        if (!pads[i]) {
            pads[i] = SDL_OpenGamepad(id);
            if (pads[i]) osd("Player %d: %s", i + 1, SDL_GetGamepadName(pads[i]));
            return;
        }
    }
}

static void pad_removed(SDL_JoystickID id) {
    for (int i = 0; i < 2; i++) {
        if (pads[i] && SDL_GetGamepadID(pads[i]) == id) {
            SDL_CloseGamepad(pads[i]);
            pads[i] = NULL;
            osd("Player %d controller disconnected", i + 1);
            if (!menu_open) open_menu();
        }
    }
}

static int pad_player(SDL_JoystickID id) {
    for (int i = 0; i < 2; i++) if (pads[i] && SDL_GetGamepadID(pads[i]) == id) return i;
    return -1;
}

#define BIT(id) (1u << (id))

static void build_input(void) {
    const bool *k = SDL_GetKeyboardState(NULL);
    bool kb_active = !menu_open && SDL_GetKeyboardFocus() == win;
    for (int p = 0; p < 2; p++) {
        uint16_t m = 0;
        int16_t ax[4] = {0, 0, 0, 0};
        SDL_Gamepad *g = pads[p];
        if (g && !menu_open) {
            static const struct { SDL_GamepadButton b; unsigned id; } map[] = {
                {SDL_GAMEPAD_BUTTON_SOUTH, RETRO_DEVICE_ID_JOYPAD_B},
                {SDL_GAMEPAD_BUTTON_EAST, RETRO_DEVICE_ID_JOYPAD_A},
                {SDL_GAMEPAD_BUTTON_WEST, RETRO_DEVICE_ID_JOYPAD_Y},
                {SDL_GAMEPAD_BUTTON_NORTH, RETRO_DEVICE_ID_JOYPAD_X},
                {SDL_GAMEPAD_BUTTON_BACK, RETRO_DEVICE_ID_JOYPAD_SELECT},
                {SDL_GAMEPAD_BUTTON_START, RETRO_DEVICE_ID_JOYPAD_START},
                {SDL_GAMEPAD_BUTTON_DPAD_UP, RETRO_DEVICE_ID_JOYPAD_UP},
                {SDL_GAMEPAD_BUTTON_DPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN},
                {SDL_GAMEPAD_BUTTON_DPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT},
                {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT},
            };
            for (size_t i = 0; i < SDL_arraysize(map); i++)
                if (SDL_GetGamepadButton(g, map[i].b)) m |= BIT(map[i].id);
            ax[0] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
            ax[1] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
            ax[2] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
            ax[3] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY);
            /* Back held + Start = menu, so don't pass that combo to the game */
            if ((m & BIT(RETRO_DEVICE_ID_JOYPAD_SELECT)) && (m & BIT(RETRO_DEVICE_ID_JOYPAD_START)))
                m &= ~(BIT(RETRO_DEVICE_ID_JOYPAD_SELECT) | BIT(RETRO_DEVICE_ID_JOYPAD_START));
        }
        if (p == 0 && kb_active) {
            if (k[SDL_SCANCODE_W]) m |= BIT(RETRO_DEVICE_ID_JOYPAD_UP);
            if (k[SDL_SCANCODE_S]) m |= BIT(RETRO_DEVICE_ID_JOYPAD_DOWN);
            if (k[SDL_SCANCODE_A]) m |= BIT(RETRO_DEVICE_ID_JOYPAD_LEFT);
            if (k[SDL_SCANCODE_D]) m |= BIT(RETRO_DEVICE_ID_JOYPAD_RIGHT);
            if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_I]) m |= BIT(RETRO_DEVICE_ID_JOYPAD_X);
            if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_K]) m |= BIT(RETRO_DEVICE_ID_JOYPAD_B);
            if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_J]) m |= BIT(RETRO_DEVICE_ID_JOYPAD_Y);
            if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_L]) m |= BIT(RETRO_DEVICE_ID_JOYPAD_A);
            if (k[SDL_SCANCODE_1] || k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]) m |= BIT(RETRO_DEVICE_ID_JOYPAD_START);
            if (k[SDL_SCANCODE_5]) m |= BIT(RETRO_DEVICE_ID_JOYPAD_SELECT);
        }
        if (p == 0 && !menu_open) m |= touch_mask();
        /* Unlimited continues: each Start press drops a coin first, then presses Start. */
        bool start = (m & BIT(RETRO_DEVICE_ID_JOYPAD_START)) != 0;
        if (cfg.free_play) {
            m &= ~BIT(RETRO_DEVICE_ID_JOYPAD_START);
            if (start && !start_prev[p] && start_seq[p] == 0) start_seq[p] = 1;
            if (start_seq[p]) {
                int f = start_seq[p]++;
                if (f <= 4) m |= BIT(RETRO_DEVICE_ID_JOYPAD_SELECT);
                else if (f >= 14 && f <= 20) m |= BIT(RETRO_DEVICE_ID_JOYPAD_START);
                else if (f > 20) start_seq[p] = 0;
            }
        }
        start_prev[p] = start;
        joy_mask[p] = m;
        memcpy(joy_axis[p], ax, sizeof ax);
    }
}

/* ------------------------------------------------------------------ first-run ROM screen */
static char pending_path[2048];
static SDL_Mutex *pending_lock;
static bool dialog_open;

static void SDLCALL dialog_cb(void *ud, const char *const *files, int filter) {
    (void)ud; (void)filter;
    SDL_LockMutex(pending_lock);
    if (files && files[0]) SDL_strlcpy(pending_path, files[0], sizeof pending_path);
    dialog_open = false;
    SDL_UnlockMutex(pending_lock);
}

static bool try_import(const char *src, char *msg, size_t msglen) {
    logf_("import: %s\n", src);
    return romset_import(src, rom_zip, msg, msglen);
}

#ifdef SMASHTV_EMBEDDED_ROMS
extern const unsigned char smashtv_embedded_zip[];
extern const size_t smashtv_embedded_zip_size;
#endif

static bool find_roms_automatically(char *msg, size_t msglen) {
    if (romset_check_zip(rom_zip, msg, msglen) == ROMSET_COUNT) return true;
#ifdef SMASHTV_EMBEDDED_ROMS
    /* ROMs are compiled into this build: unpack them to the data folder for the core */
    if (SDL_SaveFile(rom_zip, smashtv_embedded_zip, smashtv_embedded_zip_size)
        && romset_check_zip(rom_zip, msg, msglen) == ROMSET_COUNT) {
        logf_("Using ROMs built into the app\n");
        return true;
    }
    logf_("Built-in ROMs couldn't be unpacked: %s\n", msg);
#endif
    const char *base = SDL_GetBasePath();
    char *cwd = SDL_GetCurrentDirectory();
    const char *dirs[] = {base, cwd};
    for (int i = 0; i < 2; i++) {
        if (!dirs[i]) continue;
        char p[1200], tmp[512];
        const char *sub[] = {"smashtv.zip", "roms/smashtv.zip", "roms", ""};
        for (int s = 0; s < 4; s++) {
            SDL_snprintf(p, sizeof p, "%s%s%s", dirs[i], (dirs[i][strlen(dirs[i]) - 1] == '/' || dirs[i][strlen(dirs[i]) - 1] == '\\') ? "" : "/", sub[s]);
            SDL_PathInfo info;
            if (!SDL_GetPathInfo(p, &info)) continue;
            if (try_import(p, tmp, sizeof tmp)) { SDL_free(cwd); return true; }
        }
    }
    SDL_free(cwd);
    msg[0] = 0;
    return false;
}

/* Returns false if the user quit. */
static bool rom_setup_screen(const char *initial_msg) {
    char msg[512] = "";
    if (find_roms_automatically(msg, sizeof msg)) return true;
    SDL_strlcpy(msg, initial_msg ? initial_msg : "", sizeof msg);
    static const SDL_DialogFileFilter filters[] = {{"ROM zip", "zip"}, {"All files", "*"}};
    bool ok_flash = false;
    Uint64 ok_time = 0;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: return false;
            case SDL_EVENT_DROP_FILE:
                if (try_import(e.drop.data, msg, sizeof msg)) { ok_flash = true; ok_time = SDL_GetTicks(); }
                break;
            case SDL_EVENT_GAMEPAD_ADDED: pad_added(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: pad_removed(e.gdevice.which); break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_AC_BACK) return false;
                if (dialog_open) break;
                if (e.key.key == SDLK_RETURN || e.key.key == SDLK_SPACE || e.key.key == SDLK_O) {
                    dialog_open = true;
                    SDL_ShowOpenFileDialog(dialog_cb, NULL, win, filters, 2, NULL, false);
                } else if (e.key.key == SDLK_F) {
                    dialog_open = true;
                    SDL_ShowOpenFolderDialog(dialog_cb, NULL, win, NULL, false);
                }
                break;
            case SDL_EVENT_FINGER_UP:
                if (!dialog_open) {
                    dialog_open = true;
                    SDL_ShowOpenFileDialog(dialog_cb, NULL, win, filters, 2, NULL, false);
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (!dialog_open && (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_START)) {
                    dialog_open = true;
                    SDL_ShowOpenFileDialog(dialog_cb, NULL, win, filters, 2, NULL, false);
                }
                break;
            }
        }
        SDL_LockMutex(pending_lock);
        char path[2048];
        SDL_strlcpy(path, pending_path, sizeof path);
        pending_path[0] = 0;
        SDL_UnlockMutex(pending_lock);
        if (path[0] && try_import(path, msg, sizeof msg)) { ok_flash = true; ok_time = SDL_GetTicks(); }
        if (ok_flash && SDL_GetTicks() - ok_time > 900) return true;

        SDL_SetRenderDrawColor(ren, 12, 10, 24, 255);
        SDL_RenderClear(ren);
        int w, h;
        SDL_GetRenderOutputSize(ren, &w, &h);
        float s = ui_scale(), y = (float)h * 0.22f;
        text_center(y, s * 3, "SMASH T.V.", 255, 210, 40); y += 50 * s;
        text_center(y, s, "First, add your arcade ROM files.", 230, 230, 230); y += 20 * s;
        if (touch_enabled()) {
            text_center(y, s, "Tap the screen, then pick smashtv.zip", 180, 200, 255); y += 11 * s;
            text_center(y, s, "(the MAME \"smashtv\" ROM set).", 180, 200, 255); y += 24 * s;
        } else {
            text_center(y, s, "Drag smashtv.zip (or the folder with the", 230, 230, 230); y += 11 * s;
            text_center(y, s, "14 ROM files) onto this window,", 230, 230, 230); y += 20 * s;
            text_center(y, s, "or press ENTER / A to choose the zip,", 180, 200, 255); y += 11 * s;
            text_center(y, s, "or F to choose a folder.", 180, 200, 255); y += 24 * s;
        }
        if (ok_flash) text_center(y, s, "ROMs verified! Starting...", 120, 255, 120);
        else if (msg[0]) {
            /* wrap long messages */
            size_t maxc = (size_t)((float)w / (8 * s)) - 4, len = strlen(msg), pos = 0;
            while (pos < len) {
                size_t take = len - pos > maxc ? maxc : len - pos;
                if (pos + take < len) { size_t b = take; while (b > 0 && msg[pos + b] != ' ') b--; if (b > 0) take = b; }
                char line[256];
                SDL_strlcpy(line, msg + pos, take + 1 < sizeof line ? take + 1 : sizeof line);
                text_center(y, s, line, 255, 120, 120);
                y += 11 * s;
                pos += take;
                while (msg[pos] == ' ') pos++;
            }
        }
        if (!touch_enabled()) text_center((float)h - 14 * s, s, "Esc to quit", 120, 120, 120);
        if (SDL_getenv("SMASHTV_TEST_SETUP_SHOT")) { /* automated UI check */
            SDL_Surface *shot = SDL_RenderReadPixels(ren, NULL);
            if (shot) { SDL_SaveBMP(shot, SDL_getenv("SMASHTV_TEST_SETUP_SHOT")); SDL_DestroySurface(shot); }
            if (msg[0] || !SDL_getenv("SMASHTV_TEST_BAD")) return false;
        }
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }
}

/* ------------------------------------------------------------------ icon */
/* A small original icon: a retro TV set with a star on screen. */
static void set_icon(void) {
    const int N = 64;
    SDL_Surface *s = SDL_CreateSurface(N, N, SDL_PIXELFORMAT_RGBA32);
    if (!s) return;
    Uint32 *px = s->pixels;
    const SDL_PixelFormatDetails *f = SDL_GetPixelFormatDetails(s->format);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            Uint32 c = SDL_MapRGBA(f, NULL, 0, 0, 0, 0);
            bool body = x >= 4 && x < 60 && y >= 14 && y < 58;
            bool corner = (x < 9 || x > 54) && (y < 19 || y > 52);
            int ax = x - 32, ay = y - 14;
            bool antenna = y < 14 && (SDL_abs(ax + ay) <= 1 || SDL_abs(ax - ay) <= 1) && y > 2;
            if (antenna) c = SDL_MapRGBA(f, NULL, 200, 200, 210, 255);
            if (body && !corner) {
                c = SDL_MapRGBA(f, NULL, 150, 30, 40, 255);
                bool screen = x >= 9 && x < 47 && y >= 19 && y < 53;
                if (screen) {
                    c = SDL_MapRGBA(f, NULL, 20, 40, 90, 255);
                    /* five-point star centred on the screen */
                    float cx = x - 28 + 0.5f, cy = y - 36 + 0.5f;
                    float r = SDL_sqrtf(cx * cx + cy * cy), a = SDL_atan2f(cy, cx) + SDL_PI_F / 2;
                    float k = SDL_cosf(SDL_PI_F / 5) / SDL_cosf(SDL_fmodf(a + 20 * SDL_PI_F, 2 * SDL_PI_F / 5) - SDL_PI_F / 5);
                    if (r < 13 * (0.45f + 0.55f * (k - 0.81f) / 0.19f)) c = SDL_MapRGBA(f, NULL, 255, 210, 40, 255);
                }
                if (x >= 51 && x < 56 && (y == 24 || y == 25 || y == 32 || y == 33)) c = SDL_MapRGBA(f, NULL, 230, 230, 230, 255);
            }
            px[y * (s->pitch / 4) + x] = c;
        }
    }
    SDL_SetWindowIcon(win, s);
    SDL_DestroySurface(s);
}

/* ------------------------------------------------------------------ main */
static void fatal(const char *msg) {
    logf_("FATAL: %s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, msg, win);
}

static void make_dirs(void) {
    char *p = SDL_GetPrefPath("SmashTVNative", "smashtv");
    SDL_strlcpy(pref_dir, p ? p : "./", sizeof pref_dir);
    SDL_free(p);
    SDL_snprintf(rom_dir, sizeof rom_dir, "%sroms", pref_dir);
    SDL_snprintf(sys_dir, sizeof sys_dir, "%ssystem", pref_dir);
    SDL_snprintf(save_dir, sizeof save_dir, "%ssaves", pref_dir);
    SDL_snprintf(rom_zip, sizeof rom_zip, "%s/%s.zip", rom_dir, GAME_NAME);
    SDL_snprintf(log_path, sizeof log_path, "%slog.txt", pref_dir);
    SDL_snprintf(settings_path, sizeof settings_path, "%ssettings.ini", pref_dir);
    SDL_snprintf(state_path, sizeof state_path, "%s/%s.state", save_dir, GAME_NAME);
    SDL_CreateDirectory(rom_dir);
    SDL_CreateDirectory(sys_dir);
    SDL_CreateDirectory(save_dir);
    log_file = fopen(log_path, "w");
}

int main(int argc, char **argv) {
    const char *rom_arg = NULL;
    int force_full = -1;
    int test_frames = 0; /* --test-frames N: run N frames headless-friendly, dump a screenshot */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fullscreen")) force_full = 1;
        else if (!strcmp(argv[i], "--windowed")) force_full = 0;
        else if (!strcmp(argv[i], "--touch")) touch_set_enabled(true);
        else if (!strcmp(argv[i], "--test-frames") && i + 1 < argc) test_frames = SDL_atoi(argv[++i]);
        else if (!strcmp(argv[i], "--version")) { printf("%s %s\n", APP_NAME, APP_VERSION); return 0; }
        else if (!strcmp(argv[i], "--help")) {
            printf("Usage: %s [--fullscreen|--windowed] [path to smashtv.zip or ROM folder]\n", argv[0]);
            return 0;
        } else rom_arg = argv[i];
    }

    SDL_SetAppMetadata(APP_NAME, APP_VERSION, "io.github.smashtv_native");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
#ifdef SDL_PLATFORM_ANDROID
    touch_set_enabled(true);
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    force_full = 1;
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
    }
    make_dirs();
    logf_("%s %s, data folder: %s\n", APP_NAME, APP_VERSION, pref_dir);
    load_settings();
    if (force_full >= 0) cfg.fullscreen = force_full;
    pending_lock = SDL_CreateMutex();

    if (!SDL_CreateWindowAndRenderer(APP_NAME, 1024, 768, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &win, &ren)) {
        fprintf(stderr, "Window failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(win, 320, 240);
    if (SDL_getenv("SMASHTV_TEST_SIZE")) {   /* automated checks at phone resolutions, e.g. "2400x1080" */
        int tw = SDL_atoi(SDL_getenv("SMASHTV_TEST_SIZE")), th = SDL_atoi(strchr(SDL_getenv("SMASHTV_TEST_SIZE"), 'x') + 1);
        SDL_SetWindowSize(win, tw, th);
    }
    SDL_SetRenderVSync(ren, 1);
    set_icon();
    if (cfg.fullscreen) set_fullscreen(true);

    int npads = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&npads);
    for (int i = 0; i < npads; i++) pad_added(ids[i]);
    SDL_free(ids);

    char err[1024] = "";
    if (rom_arg && !try_import(rom_arg, err, sizeof err)) logf_("ROM import from argument failed: %s\n", err);
    else err[0] = 0;
    if (!rom_setup_screen(err)) goto out;

    if (!load_core(err, sizeof err)) { fatal(err); goto out; }
    core.set_environment(environment);
    core.set_video_refresh(video_refresh);
    core.set_audio_sample(audio_sample);
    core.set_audio_sample_batch(audio_batch);
    core.set_input_poll(input_poll);
    core.set_input_state(input_state);
    core.init();
    struct retro_system_info si = {0};
    core.get_system_info(&si);
    logf_("core: %s %s\n", si.library_name, si.library_version);

    struct retro_game_info gi = {rom_zip, NULL, 0, NULL};
    if (!core.load_game(&gi)) {
        SDL_snprintf(err, sizeof err, "The emulator couldn't start the game.\nDetails are in:\n%s", log_path);
        fatal(err);
        core.deinit();
        goto out;
    }
    core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    core.set_controller_port_device(1, RETRO_DEVICE_JOYPAD);

    struct retro_system_av_info av = {0};
    core.get_system_av_info(&av);
    set_geometry(&av.geometry);
    if (av.timing.fps > 1) core_fps = av.timing.fps;
    if (av.timing.sample_rate > 1000) sample_rate = av.timing.sample_rate;
    logf_("timing: %.3f fps, %.0f Hz audio, aspect %.3f\n", core_fps, sample_rate, aspect);

    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)sample_rate};
    if (!test_frames) {
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (audio) SDL_ResumeAudioStreamDevice(audio);
        else logf_("No audio: %s\n", SDL_GetError());
    }

    if (test_frames) {
        /* Automated test mode: optional scripted input via SMASHTV_TEST_INPUT="frame:mask,frame:mask" */
        const char *script = SDL_getenv("SMASHTV_TEST_INPUT");
        const char *shot_every = SDL_getenv("SMASHTV_TEST_SHOT_EVERY");
        int every = shot_every ? SDL_atoi(shot_every) : 0;
        for (int f = 1; f <= test_frames; f++) {
            joy_mask[0] = 0;
            if (script) {
                const char *p = script;
                while (*p) {
                    int a = SDL_atoi(p), b = 0;
                    const char *dash = strchr(p, '-'), *colon = strchr(p, ':');
                    b = (dash && colon && dash < colon) ? SDL_atoi(dash + 1) : a;
                    if (colon && f >= a && f <= b) joy_mask[0] |= (uint16_t)SDL_strtoul(colon + 1, NULL, 16);
                    const char *comma = strchr(p, ',');
                    if (!comma) break;
                    p = comma + 1;
                }
            }
            if (f == 1 && SDL_getenv("SMASHTV_TEST_TOUCH")) {
                /* "x,y;x,y" normalised finger positions held for the whole run */
                const char *t = SDL_getenv("SMASHTV_TEST_TOUCH");
                for (int id = 1; t && *t; id++) {
                    SDL_Event ev; SDL_zero(ev);
                    ev.type = SDL_EVENT_FINGER_DOWN; ev.tfinger.fingerID = (SDL_FingerID)id;
                    ev.tfinger.x = (float)SDL_atof(t);
                    const char *c = strchr(t, ','); if (!c) break;
                    ev.tfinger.y = (float)SDL_atof(c + 1);
                    render_frame();
                    touch_event(&ev, ren, game_rect());
                    t = strchr(c, ';'); if (t) t++;
                }
            }
            if (SDL_getenv("SMASHTV_TEST_TOUCH")) build_input();
            run_frame();
            if (f == test_frames || (every && f % every == 0)) {
                if (f == test_frames && SDL_getenv("SMASHTV_TEST_MENU")) { menu_open = true; menu_sel = 4; }
                render_frame();
                SDL_Surface *shot = SDL_RenderReadPixels(ren, NULL);
                char name[256];
                SDL_snprintf(name, sizeof name, "%sshot_%05d.bmp", pref_dir, f);
                if (shot) { SDL_SaveBMP(shot, name); SDL_DestroySurface(shot); }
            }
            if (f == test_frames / 2 && SDL_getenv("SMASHTV_TEST_STATES")) { save_state(); }
        }
        if (SDL_getenv("SMASHTV_TEST_STATES")) { load_state(); run_frame(); render_frame(); }
        logf_("test run finished\n");
        core.unload_game();
        core.deinit();
        goto out;
    }

    Uint64 freq = SDL_GetPerformanceFrequency(), last = SDL_GetPerformanceCounter();
    Uint64 t_start = SDL_GetTicks(), frames_total = 0, play_start = 0, play_frames = 0;
    double acc = 0;
    const int bytes_per_frame = (int)(sample_rate / core_fps) * 4;
    boot_left = BOOT_FRAMES;
    {   /* very first start (no NVRAM yet): the game also shows a "default adjustments restored" notice */
        char nv[1200]; SDL_PathInfo pi;
        SDL_snprintf(nv, sizeof nv, "%s/mame/nvram/%s/nvram", save_dir, GAME_NAME);
        if (!SDL_GetPathInfo(nv, &pi)) boot_left += 420;
    }
    while (!quit_requested) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: quit_requested = true; break;
            case SDL_EVENT_GAMEPAD_ADDED: pad_added(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: pad_removed(e.gdevice.which); break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: if (cfg.fullscreen == false && !menu_open) open_menu(); break;
            case SDL_EVENT_DROP_FILE: osd("ROMs are already installed"); break;
            case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_CANCELED:
                if (!touch_enabled()) break;
                if (menu_open) { if (e.type == SDL_EVENT_FINGER_DOWN) menu_tap(e.tfinger.x, e.tfinger.y); touch_release_all(); break; }
                if (touch_event(&e, ren, game_rect()) == TOUCH_PAUSE) { touch_release_all(); open_menu(); }
                break;
            case SDL_EVENT_DID_ENTER_BACKGROUND:
                touch_release_all();
                if (!menu_open) open_menu();
                break;
            case SDL_EVENT_KEY_DOWN: {
                SDL_Keycode key = e.key.key;
                if ((key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) || key == SDLK_F11) { set_fullscreen(!cfg.fullscreen); break; }
                if (key == SDLK_F5 && !e.key.repeat) { save_state(); break; }
                if (key == SDLK_F9 && !e.key.repeat) { load_state(); break; }
                if (key == SDLK_AC_BACK) { if (menu_open) menu_input(5); else open_menu(); break; }
                if (menu_open) {
                    if (key == SDLK_UP || key == SDLK_W) menu_input(0);
                    else if (key == SDLK_DOWN || key == SDLK_S) menu_input(1);
                    else if (key == SDLK_LEFT || key == SDLK_A) menu_input(2);
                    else if (key == SDLK_RIGHT || key == SDLK_D) menu_input(3);
                    else if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_KP_ENTER) menu_input(4);
                    else if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) menu_input(5);
                    else if (help_open) help_open = false;
                } else if (key == SDLK_ESCAPE || key == SDLK_P || key == SDLK_PAUSE) open_menu();
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                int p = pad_player(e.gbutton.which);
                if (p < 0) break;
                Uint8 b = e.gbutton.button;
                if (!menu_open) {
                    bool combo = (b == SDL_GAMEPAD_BUTTON_START && SDL_GetGamepadButton(pads[p], SDL_GAMEPAD_BUTTON_BACK)) ||
                                 (b == SDL_GAMEPAD_BUTTON_BACK && SDL_GetGamepadButton(pads[p], SDL_GAMEPAD_BUTTON_START));
                    if (b == SDL_GAMEPAD_BUTTON_GUIDE || combo) open_menu();
                } else {
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) menu_input(0);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) menu_input(1);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) menu_input(2);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) menu_input(3);
                    else if (b == SDL_GAMEPAD_BUTTON_SOUTH) menu_input(4);
                    else if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_GUIDE || b == SDL_GAMEPAD_BUTTON_START) menu_input(help_open ? 5 : (b == SDL_GAMEPAD_BUTTON_START ? 4 : 5));
                    else if (help_open) help_open = false;
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                /* left stick navigates the menu */
                static int held[2];
                int p = pad_player(e.gaxis.which);
                if (!menu_open || p < 0 || e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) break;
                int dir = e.gaxis.value < -20000 ? -1 : e.gaxis.value > 20000 ? 1 : 0;
                if (dir && dir != held[p]) menu_input(dir < 0 ? 0 : 1);
                held[p] = dir;
                break;
            }
            }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - last) / (double)freq;
        last = now;
        if (dt > 0.25) dt = 0.25;

        int ran = 0;
        if (boot_left > 0) {
            /* fast-forward the power-on self-test, silently, ~25 ms per iteration */
            Uint64 t0 = SDL_GetTicks();
            memset(joy_mask, 0, sizeof joy_mask);
            memset(joy_axis, 0, sizeof joy_axis);
            while (boot_left > 0 && SDL_GetTicks() - t0 < 50) {
                core.run(); boot_left--; ran++;
                /* still on the self-test screen at the end? keep going (up to ~30 s of game time more) */
                if (boot_left == 0 && (frame_is_selftest || frame_is_blank) && selftest_extra < 1800) { boot_left = 30; selftest_extra += 30; }
            }
            if (audio) SDL_ClearAudioStream(audio);
            if (boot_left == 0) acc = 0;
        } else if (!menu_open && frame_is_selftest && selftest_extra < 1800) {
            boot_left = 60; selftest_extra += 60;       /* the machine reset itself: skip its self test again */
        } else if (!menu_open) {
            if (!frame_is_selftest) selftest_extra = 0;
            build_input();
            /* The clock sets game speed; audio playback rate is nudged (+-0.5%)
               to keep about 3 frames of sound queued, so neither drifts. */
            acc += dt;
            while (acc >= 1.0 / core_fps && ran < 4) { acc -= 1.0 / core_fps; run_frame(); ran++; }
            if (acc > 1.0 / core_fps) acc = 0; /* too slow to keep up: don't spiral */
            if (audio && ran) {
                int q = SDL_GetAudioStreamQueued(audio), target = bytes_per_frame * 3;
                if (q > bytes_per_frame * 10) { SDL_ClearAudioStream(audio); q = 0; }
                float err = (float)(q - target) / (float)target;
                if (err > 1) err = 1;
                if (err < -1) err = -1;
                SDL_SetAudioStreamFrequencyRatio(audio, 1.0f + 0.005f * err);
            }
        } else {
            acc = 0;
        }
        frames_total += (Uint64)ran;
        if (boot_left == 0 && !play_start) play_start = SDL_GetTicks();
        else if (boot_left == 0 && !menu_open) play_frames += (Uint64)ran;
        render_frame();
        {   /* automated check of the real loop: SMASHTV_TEST_LOOP_SHOT=seconds saves a screenshot and quits */
            static const char *ls = NULL; static bool lsi = false;
            if (!lsi) { ls = SDL_getenv("SMASHTV_TEST_LOOP_SHOT"); lsi = true; }
            if (ls && SDL_GetTicks() - t_start > (Uint64)(SDL_atof(ls) * 1000)) {
                SDL_Surface *shot = SDL_RenderReadPixels(ren, NULL);
                char name[1200]; SDL_snprintf(name, sizeof name, "%sloop_shot.bmp", pref_dir);
                if (shot) { SDL_SaveBMP(shot, name); SDL_DestroySurface(shot); }
                logf_("loop shot: boot_left=%d selftest=%d frames=%llu\n", boot_left, frame_is_selftest, (unsigned long long)frames_total);
                quit_requested = true;
            }
        }
        if (!ran) SDL_Delay(1);
    }

    if (play_start && SDL_GetTicks() > play_start)
        logf_("ran %llu frames in %.1f s; after boot: %.2f fps (target %.2f)\n", (unsigned long long)frames_total,
              (SDL_GetTicks() - t_start) / 1000.0, play_frames * 1000.0 / (double)(SDL_GetTicks() - play_start), core_fps);
    core.unload_game(); /* writes NVRAM (high scores, settings) */
    core.deinit();
out:
    save_settings();
    if (audio) SDL_DestroyAudioStream(audio);
    for (int i = 0; i < 2; i++) if (pads[i]) SDL_CloseGamepad(pads[i]);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    if (core.lib) SDL_UnloadObject(core.lib);
    if (log_file) fclose(log_file);
    SDL_Quit();
    return 0;
}
