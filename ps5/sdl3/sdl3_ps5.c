/* bbport PS5: the part of SDL3 the port uses, on the console's own services: the controller
 * (scePad), sound (sceAudioOut, one 48 kHz stereo port mixed from every stream), a pseudo window
 * whose video driver is "ps5" (gpu/shim/window.cpp then creates a VK_KHR_display surface),
 * timing, and an event queue fed from the controller (the in-game menu's L3+R3 and its
 * navigation arrive as SDL gamepad events, as on a PC).
 * The real SDL3 headers (ps5/.deps/sdl3/include) give the types and constants.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <SDL3/SDL.h>

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- console services (no SDK header declares them) ---- */
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void *params);
int scePadReadState(int32_t handle, void *data);
int scePadSetVibration(int32_t handle, const void *vibration);
int scePadSetLightBar(int32_t handle, const void *color);
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate, uint32_t format);
int sceAudioOutOutput(int32_t port, const void *samples);

/* One controller reading: OrbisPadData, as PS4 and PS5 lay it out (120 bytes). */
typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;
    uint8_t pad0[2];
    float quat[4], ang_vel[3], accel[3];
    uint8_t touch_count, touch_reserved[3];
    uint32_t touch_reserved1;
    struct { uint16_t x, y; uint8_t id, reserved[3]; } touches[2];
    uint8_t connected, pad1[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t remaining[15];
} PadState;
_Static_assert(sizeof(PadState) == 120, "pad reading layout");
_Static_assert(__builtin_offsetof(PadState, touches) == 0x3c, "touches at 0x3c");
_Static_assert(__builtin_offsetof(PadState, connected) == 0x4c, "connection at 0x4c");

enum {
    PB_L3 = 0x2, PB_R3 = 0x4, PB_OPTIONS = 0x8, PB_UP = 0x10, PB_RIGHT = 0x20, PB_DOWN = 0x40,
    PB_LEFT = 0x80, PB_L2 = 0x100, PB_R2 = 0x200, PB_L1 = 0x400, PB_R1 = 0x800,
    PB_TRIANGLE = 0x1000, PB_CIRCLE = 0x2000, PB_CROSS = 0x4000, PB_SQUARE = 0x8000,
    PB_TOUCHPAD = 0x100000,
};

/* ---- errors, time, memory ---- */
static _Thread_local char error_text[256] = "";
const char *SDL_GetError(void) { return error_text; }
bool SDL_SetError(SDL_PRINTF_FORMAT_STRING const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(error_text, sizeof(error_text), fmt, args);
    va_end(args);
    return false;
}
bool SDL_ClearError(void) { error_text[0] = 0; return true; }
void SDL_free(void *mem) { free(mem); }
void *SDL_malloc(size_t size) { return malloc(size); }
void *SDL_calloc(size_t n, size_t size) { return calloc(n, size); }
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static uint64_t start_ns;
Uint64 SDL_GetTicks(void) { if (!start_ns) start_ns = now_ns(); return (now_ns() - start_ns) / 1000000u; }
Uint64 SDL_GetTicksNS(void) { if (!start_ns) start_ns = now_ns(); return now_ns() - start_ns; }
Uint64 SDL_GetPerformanceCounter(void) { return now_ns(); }
Uint64 SDL_GetPerformanceFrequency(void) { return 1000000000u; }
void SDL_Delay(Uint32 ms) { usleep((useconds_t)ms * 1000u); }
void SDL_DelayNS(Uint64 ns) { usleep((useconds_t)(ns / 1000u)); }
bool SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return true; }
void SDL_Log(SDL_PRINTF_FORMAT_STRING const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
}

/* ---- the controller ---- */
static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;
static int pad_handle = -1, pad_tried;
static PadState pad_now, pad_before;
static uint64_t pad_read_ns, pad_retry_ns;
struct SDL_Gamepad { int unused; };
static struct SDL_Gamepad the_gamepad;
static atomic_bool gamepad_announced;

/* Events: a small queue, filled by SDL_PushEvent and from controller changes. */
#define EVENTS 256
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_Event events[EVENTS];
static unsigned event_head, event_count;
static void queue_event(const SDL_Event *e) {
    pthread_mutex_lock(&event_lock);
    if (event_count < EVENTS) { events[(event_head + event_count) % EVENTS] = *e; ++event_count; }
    pthread_mutex_unlock(&event_lock);
}

static void pad_open_locked(void) {
    if (pad_handle >= 0) return;
    const uint64_t now = now_ns();
    if (pad_tried && now < pad_retry_ns) return;
    pad_retry_ns = now + 1000000000u;
    if (!pad_tried) { sceUserServiceInitialize(NULL); scePadInit(); }
    pad_tried = 1;
    int32_t user = -1;
    if (sceUserServiceGetInitialUser(&user) < 0 || user < 0) return;
    int handle = scePadOpen(user, 0, 0, NULL);
    if (handle < 0) { fprintf(stderr, "PS5: scePadOpen(user %d) -> 0x%08x\n", user, (unsigned)handle); return; }
    pad_handle = handle;
    printf("PS5: controller of user %d open (handle %d)\n", user, handle);
    struct { uint8_t r, g, b, a; } red = {120, 0, 0, 255};
    scePadSetLightBar(pad_handle, &red);
}
static const struct { uint32_t bit; SDL_GamepadButton button; } button_map[] = {
    {PB_CROSS, SDL_GAMEPAD_BUTTON_SOUTH}, {PB_CIRCLE, SDL_GAMEPAD_BUTTON_EAST},
    {PB_SQUARE, SDL_GAMEPAD_BUTTON_WEST}, {PB_TRIANGLE, SDL_GAMEPAD_BUTTON_NORTH},
    {PB_L1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {PB_R1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {PB_L3, SDL_GAMEPAD_BUTTON_LEFT_STICK}, {PB_R3, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {PB_OPTIONS, SDL_GAMEPAD_BUTTON_START}, {PB_UP, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {PB_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {PB_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {PB_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {PB_TOUCHPAD, SDL_GAMEPAD_BUTTON_TOUCHPAD},
};
/* Reads the controller at most every 2 ms, and queues button and axis events for changes. */
static void pad_update_locked(void) {
    const uint64_t now = now_ns();
    if (pad_read_ns && now - pad_read_ns < 2000000u) return;
    pad_read_ns = now;
    pad_open_locked();
    if (pad_handle < 0) return;
    PadState state;
    memset(&state, 0, sizeof(state));
    if (scePadReadState(pad_handle, &state) < 0) return;
    pad_before = pad_now;
    pad_now = state;
    if (!atomic_exchange(&gamepad_announced, true)) {
        SDL_Event e = {0};
        e.gdevice.type = SDL_EVENT_GAMEPAD_ADDED;
        e.gdevice.which = 1;
        queue_event(&e);
    }
    const uint32_t changed = pad_now.buttons ^ pad_before.buttons;
    for (size_t i = 0; changed && i < sizeof(button_map) / sizeof(*button_map); ++i) {
        if (!(changed & button_map[i].bit)) continue;
        SDL_Event e = {0};
        const bool down = (pad_now.buttons & button_map[i].bit) != 0;
        e.gbutton.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = 1;
        e.gbutton.button = (Uint8)button_map[i].button;
        e.gbutton.down = down;
        e.gbutton.timestamp = SDL_GetTicksNS();
        queue_event(&e);
    }
    const uint8_t now_axes[6] = {pad_now.lx, pad_now.ly, pad_now.rx, pad_now.ry, pad_now.l2, pad_now.r2};
    const uint8_t old_axes[6] = {pad_before.lx, pad_before.ly, pad_before.rx, pad_before.ry, pad_before.l2, pad_before.r2};
    for (int a = 0; a < 6; ++a) {
        if (abs((int)now_axes[a] - (int)old_axes[a]) < 24) continue;
        SDL_Event e = {0};
        e.gaxis.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        e.gaxis.which = 1;
        e.gaxis.axis = (Uint8)a; /* SDL's order: LEFTX, LEFTY, RIGHTX, RIGHTY, LEFT_TRIGGER, RIGHT_TRIGGER */
        e.gaxis.value = a < 4 ? (Sint16)(((int)now_axes[a] - 128) * 256) : (Sint16)(now_axes[a] * 128);
        queue_event(&e);
    }
}
static void pad_update(void) {
    pthread_mutex_lock(&pad_lock);
    pad_update_locked();
    pthread_mutex_unlock(&pad_lock);
}

bool SDL_Init(SDL_InitFlags flags) { (void)flags; if (!start_ns) start_ns = now_ns(); return true; }
bool SDL_InitSubSystem(SDL_InitFlags flags) { return SDL_Init(flags); }
SDL_InitFlags SDL_WasInit(SDL_InitFlags flags) { return flags; }
void SDL_QuitSubSystem(SDL_InitFlags flags) { (void)flags; }
void SDL_Quit(void) {}

SDL_JoystickID *SDL_GetGamepads(int *count) {
    pad_update();
    pthread_mutex_lock(&pad_lock);
    const int have = pad_handle >= 0;
    pthread_mutex_unlock(&pad_lock);
    SDL_JoystickID *ids = calloc(2, sizeof(*ids));
    if (ids && have) ids[0] = 1;
    if (count) *count = ids && have ? 1 : 0;
    return ids;
}
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) { (void)id; return &the_gamepad; }
void SDL_CloseGamepad(SDL_Gamepad *gamepad) { (void)gamepad; }
bool SDL_GamepadConnected(SDL_Gamepad *gamepad) {
    (void)gamepad;
    pad_update();
    pthread_mutex_lock(&pad_lock);
    const bool connected = pad_handle >= 0;
    pthread_mutex_unlock(&pad_lock);
    return connected;
}
const char *SDL_GetGamepadName(SDL_Gamepad *gamepad) { (void)gamepad; return "DualSense (console)"; }
const char *SDL_GetGamepadNameForID(SDL_JoystickID id) { (void)id; return "DualSense (console)"; }
SDL_GamepadType SDL_GetGamepadTypeForID(SDL_JoystickID id) { (void)id; return SDL_GAMEPAD_TYPE_PS5; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad) { (void)gamepad; return SDL_GAMEPAD_TYPE_PS5; }
const char *SDL_GetGamepadStringForType(SDL_GamepadType type) { (void)type; return "ps5"; }
bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button) {
    (void)gamepad;
    pad_update();
    pthread_mutex_lock(&pad_lock);
    const uint32_t buttons = pad_now.buttons;
    pthread_mutex_unlock(&pad_lock);
    for (size_t i = 0; i < sizeof(button_map) / sizeof(*button_map); ++i)
        if (button_map[i].button == button) return (buttons & button_map[i].bit) != 0;
    return false;
}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis) {
    (void)gamepad;
    pad_update();
    pthread_mutex_lock(&pad_lock);
    const PadState s = pad_now;
    pthread_mutex_unlock(&pad_lock);
    int v;
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: v = s.lx; break;
    case SDL_GAMEPAD_AXIS_LEFTY: v = s.ly; break;
    case SDL_GAMEPAD_AXIS_RIGHTX: v = s.rx; break;
    case SDL_GAMEPAD_AXIS_RIGHTY: v = s.ry; break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return (Sint16)(s.l2 * 32767 / 255);
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return (Sint16)(s.r2 * 32767 / 255);
    default: return 0;
    }
    v = (v - 128) * 256;
    return (Sint16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
int SDL_GetNumGamepadTouchpads(SDL_Gamepad *gamepad) { (void)gamepad; return 1; }
int SDL_GetNumGamepadTouchpadFingers(SDL_Gamepad *gamepad, int touchpad) { (void)gamepad; (void)touchpad; return 2; }
bool SDL_GetGamepadTouchpadFinger(SDL_Gamepad *gamepad, int touchpad, int finger, bool *down, float *x, float *y,
                                  float *pressure) {
    (void)gamepad; (void)touchpad;
    pad_update();
    pthread_mutex_lock(&pad_lock);
    const PadState s = pad_now;
    pthread_mutex_unlock(&pad_lock);
    const bool on = finger >= 0 && finger < 2 && finger < s.touch_count;
    /* The DualSense's touch surface reports 0..1919 x 0..1079. */
    if (down) *down = on;
    if (x) *x = on ? s.touches[finger].x / 1919.0f : 0.0f;
    if (y) *y = on ? s.touches[finger].y / 1079.0f : 0.0f;
    if (pressure) *pressure = on ? 1.0f : 0.0f;
    return true;
}
bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, Uint16 low, Uint16 high, Uint32 duration_ms) {
    (void)gamepad; (void)duration_ms;
    pthread_mutex_lock(&pad_lock);
    if (pad_handle >= 0) {
        struct { uint8_t large, small; } motors = {(uint8_t)(low >> 8), (uint8_t)(high >> 8)};
        scePadSetVibration(pad_handle, &motors);
    }
    pthread_mutex_unlock(&pad_lock);
    return true;
}

/* ---- keyboard and mouse: none on the console ---- */
static bool no_keys[SDL_SCANCODE_COUNT];
const bool *SDL_GetKeyboardState(int *numkeys) { if (numkeys) *numkeys = SDL_SCANCODE_COUNT; return no_keys; }
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, SDL_Keymod *modstate) {
    (void)modstate;
    /* SDL3's keycodes for scancode keys are the scancode with SDLK_SCANCODE_MASK. */
    if (key & SDLK_SCANCODE_MASK) return (SDL_Scancode)(key & ~SDLK_SCANCODE_MASK);
    if (key >= 'a' && key <= 'z') return (SDL_Scancode)(SDL_SCANCODE_A + (key - 'a'));
    if (key >= '1' && key <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (key - '1'));
    switch (key) {
    case '0': return SDL_SCANCODE_0;
    case SDLK_RETURN: return SDL_SCANCODE_RETURN;
    case SDLK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case SDLK_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
    case SDLK_TAB: return SDL_SCANCODE_TAB;
    case SDLK_SPACE: return SDL_SCANCODE_SPACE;
    case SDLK_MINUS: return SDL_SCANCODE_MINUS;
    case SDLK_EQUALS: return SDL_SCANCODE_EQUALS;
    case SDLK_LEFTBRACKET: return SDL_SCANCODE_LEFTBRACKET;
    case SDLK_RIGHTBRACKET: return SDL_SCANCODE_RIGHTBRACKET;
    case SDLK_BACKSLASH: return SDL_SCANCODE_BACKSLASH;
    case SDLK_SEMICOLON: return SDL_SCANCODE_SEMICOLON;
    case SDLK_APOSTROPHE: return SDL_SCANCODE_APOSTROPHE;
    case SDLK_GRAVE: return SDL_SCANCODE_GRAVE;
    case SDLK_COMMA: return SDL_SCANCODE_COMMA;
    case SDLK_PERIOD: return SDL_SCANCODE_PERIOD;
    case SDLK_SLASH: return SDL_SCANCODE_SLASH;
    case SDLK_DELETE: return SDL_SCANCODE_DELETE;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}
const char *SDL_GetScancodeName(SDL_Scancode scancode) { (void)scancode; return ""; }
const char *SDL_GetKeyName(SDL_Keycode key) { (void)key; return ""; }
SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y) { if (x) *x = 0; if (y) *y = 0; return 0; }
SDL_MouseButtonFlags SDL_GetRelativeMouseState(float *x, float *y) { if (x) *x = 0; if (y) *y = 0; return 0; }
SDL_MouseButtonFlags SDL_GetGlobalMouseState(float *x, float *y) { if (x) *x = 0; if (y) *y = 0; return 0; }
bool SDL_ShowCursor(void) { return true; }
bool SDL_HideCursor(void) { return true; }
bool SDL_CaptureMouse(bool enabled) { (void)enabled; return true; }

/* ---- the window: one, the display's size ---- */
struct SDL_Window { char title[256]; int w, h; };
static struct SDL_Window the_window = {"Bloodborne", 3840, 2160};
static int window_made;
/* Properties: a handful of numbers, strings and pointers per property set. */
typedef struct { char name[64]; Sint64 number; void *pointer; char text[256]; bool flag; } Prop;
typedef struct { Prop items[24]; int count; } PropSet;
static PropSet prop_sets[8];
static Prop *prop(SDL_PropertiesID id, const char *name, bool create) {
    if (id < 1 || id > 8) return NULL;
    PropSet *set = &prop_sets[id - 1];
    for (int i = 0; i < set->count; ++i) if (!strcmp(set->items[i].name, name)) return &set->items[i];
    if (!create || set->count == 24) return NULL;
    Prop *p = &set->items[set->count++];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    return p;
}
SDL_PropertiesID SDL_CreateProperties(void) {
    for (int i = 1; i < 8; ++i) if (!prop_sets[i].count) { prop_sets[i].count = 0; return (SDL_PropertiesID)(i + 1); }
    return 0;
}
void SDL_DestroyProperties(SDL_PropertiesID id) { if (id >= 2 && id <= 8) prop_sets[id - 1].count = 0; }
bool SDL_SetStringProperty(SDL_PropertiesID id, const char *name, const char *value) {
    Prop *p = prop(id, name, true);
    if (p) snprintf(p->text, sizeof(p->text), "%s", value ? value : "");
    return p != NULL;
}
bool SDL_SetNumberProperty(SDL_PropertiesID id, const char *name, Sint64 value) {
    Prop *p = prop(id, name, true);
    if (p) p->number = value;
    return p != NULL;
}
bool SDL_SetBooleanProperty(SDL_PropertiesID id, const char *name, bool value) {
    Prop *p = prop(id, name, true);
    if (p) p->flag = value;
    return p != NULL;
}
bool SDL_SetPointerProperty(SDL_PropertiesID id, const char *name, void *value) {
    Prop *p = prop(id, name, true);
    if (p) p->pointer = value;
    return p != NULL;
}
Sint64 SDL_GetNumberProperty(SDL_PropertiesID id, const char *name, Sint64 fallback) {
    Prop *p = prop(id, name, false);
    return p ? p->number : fallback;
}
void *SDL_GetPointerProperty(SDL_PropertiesID id, const char *name, void *fallback) {
    Prop *p = prop(id, name, false);
    return p ? p->pointer : fallback;
}
static int display_w = 3840, display_h = 2160, display_hz = 60;
/* gpu/shim: the display's real mode comes from VK_KHR_display; this is what SDL reports
 * before the swapchain exists (BB_DISPLAY_RES overrides, e.g. 1920x1080). */
static void display_defaults(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *res = getenv("BB_DISPLAY_RES");
    if (res) sscanf(res, "%dx%d", &display_w, &display_h);
    const char *hz = getenv("BB_VBLANK_HZ");
    if (hz && atoi(hz) >= 100) display_hz = 120;
}
SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID props) {
    display_defaults();
    Prop *title = prop(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, false);
    if (title) snprintf(the_window.title, sizeof(the_window.title), "%s", title->text);
    the_window.w = display_w;
    the_window.h = display_h;
    window_made = 1;
    printf("PS5: display %dx%d @ %d Hz requested\n", display_w, display_h, display_hz);
    return &the_window;
}
SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags) {
    (void)w; (void)h; (void)flags;
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetStringProperty(p, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_Window *window = SDL_CreateWindowWithProperties(p);
    SDL_DestroyProperties(p);
    return window;
}
void SDL_DestroyWindow(SDL_Window *window) { (void)window; window_made = 0; }
const char *SDL_GetCurrentVideoDriver(void) { return "ps5"; }
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *window) { (void)window; return 1; }
SDL_WindowID SDL_GetWindowID(SDL_Window *window) { (void)window; return 1; }
SDL_Window *SDL_GetWindowFromID(SDL_WindowID id) { return id == 1 && window_made ? &the_window : NULL; }
bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h) {
    if (w) *w = window ? window->w : display_w;
    if (h) *h = window ? window->h : display_h;
    return true;
}
bool SDL_GetWindowSize(SDL_Window *window, int *w, int *h) { return SDL_GetWindowSizeInPixels(window, w, h); }
float SDL_GetWindowPixelDensity(SDL_Window *window) { (void)window; return 1.0f; }
float SDL_GetWindowDisplayScale(SDL_Window *window) { (void)window; return 1.0f; }
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window) { (void)window; return SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN | SDL_WINDOW_INPUT_FOCUS; }
bool SDL_SetWindowTitle(SDL_Window *window, const char *title) { if (window) snprintf(window->title, sizeof(window->title), "%s", title); return true; }
bool SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled) { (void)window; (void)enabled; return true; }
bool SDL_StartTextInput(SDL_Window *window) { (void)window; return true; }
bool SDL_StopTextInput(SDL_Window *window) { (void)window; return true; }
bool SDL_TextInputActive(SDL_Window *window) { (void)window; return false; }
bool SDL_SetTextInputArea(SDL_Window *window, const SDL_Rect *rect, int cursor) { (void)window; (void)rect; (void)cursor; return true; }
SDL_DisplayID SDL_GetPrimaryDisplay(void) { return 1; }
static SDL_DisplayMode the_mode;
const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID id) {
    (void)id;
    display_defaults();
    the_mode.displayID = 1;
    the_mode.format = SDL_PIXELFORMAT_ARGB8888;
    the_mode.w = display_w;
    the_mode.h = display_h;
    the_mode.pixel_density = 1.0f;
    the_mode.refresh_rate = (float)display_hz;
    the_mode.refresh_rate_numerator = display_hz == 120 ? 120000 : 60000;
    the_mode.refresh_rate_denominator = 1001;
    return &the_mode;
}
const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID id) { return SDL_GetCurrentDisplayMode(id); }
char *SDL_GetClipboardText(void) { return calloc(1, 1); }
bool SDL_SetClipboardText(const char *text) { (void)text; return true; }
bool SDL_HasClipboardText(void) { return false; }
bool SDL_OpenURL(const char *url) { (void)url; return false; }

/* ---- events ---- */
bool SDL_PushEvent(SDL_Event *event) { if (event) queue_event(event); return true; }
bool SDL_PollEvent(SDL_Event *event) {
    pad_update();
    pthread_mutex_lock(&event_lock);
    const bool have = event_count > 0;
    if (have) {
        if (event) *event = events[event_head];
        event_head = (event_head + 1) % EVENTS;
        --event_count;
    }
    pthread_mutex_unlock(&event_lock);
    return have;
}
void SDL_PumpEvents(void) { pad_update(); }

/* ---- sound ----
 * Every stream (one per game audio port) queues 48 kHz frames; one thread mixes a 256-frame
 * grain of all of them into the console's stereo 16-bit output, whose sceAudioOutOutput paces it. */
#define GRAIN 256
#define RING (48000 / 2) /* frames each stream can hold: 0.5 s */
struct SDL_AudioStream {
    SDL_AudioSpec spec;
    float *ring; /* stereo frames */
    unsigned head, count;
    float gain;
    bool used, paused;
};
#define STREAMS 32
static SDL_AudioStream streams[STREAMS];
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static int audio_port = -1;
static pthread_t audio_thread;
static void *audio_main(void *unused) {
    (void)unused;
    static int16_t out[GRAIN * 2];
    float mix[GRAIN * 2];
    for (;;) {
        memset(mix, 0, sizeof(mix));
        pthread_mutex_lock(&audio_lock);
        for (int s = 0; s < STREAMS; ++s) {
            SDL_AudioStream *st = &streams[s];
            if (!st->used || st->paused || !st->count) continue;
            const unsigned n = st->count < GRAIN ? st->count : GRAIN;
            for (unsigned i = 0; i < n; ++i) {
                const unsigned at = (st->head + i) % RING;
                mix[i * 2] += st->ring[at * 2] * st->gain;
                mix[i * 2 + 1] += st->ring[at * 2 + 1] * st->gain;
            }
            st->head = (st->head + n) % RING;
            st->count -= n;
        }
        pthread_mutex_unlock(&audio_lock);
        for (int i = 0; i < GRAIN * 2; ++i) {
            float v = mix[i] * 32767.0f;
            out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)lrintf(v));
        }
        if (sceAudioOutOutput(audio_port, out) < 0) usleep(5000);
    }
    return NULL;
}
static bool audio_open_locked(void) {
    if (audio_port >= 0) return true;
    sceAudioOutInit();
    /* user 0xff (system), type main, 256 frames, 48 kHz, format 1: stereo 16-bit */
    audio_port = sceAudioOutOpen(0xff, 0, 0, GRAIN, 48000, 1);
    if (audio_port < 0) {
        SDL_SetError("sceAudioOutOpen 0x%08x", (unsigned)audio_port);
        audio_port = -1;
        return false;
    }
    if (pthread_create(&audio_thread, NULL, audio_main, NULL)) { SDL_SetError("no audio thread"); return false; }
    printf("PS5: sound output open (port %d, 48 kHz stereo)\n", audio_port);
    return true;
}
const char *SDL_GetCurrentAudioDriver(void) { return "ps5-audioout"; }
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec *spec, int *sample_frames) {
    (void)devid;
    if (spec) { spec->format = SDL_AUDIO_S16; spec->channels = 2; spec->freq = 48000; }
    if (sample_frames) *sample_frames = GRAIN;
    return true;
}
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec,
                                           SDL_AudioStreamCallback callback, void *userdata) {
    (void)devid; (void)callback; (void)userdata;
    if (!spec || spec->channels < 1 || spec->channels > 8) { SDL_SetError("bad spec"); return NULL; }
    pthread_mutex_lock(&audio_lock);
    if (!audio_open_locked()) { pthread_mutex_unlock(&audio_lock); return NULL; }
    SDL_AudioStream *st = NULL;
    for (int s = 0; s < STREAMS; ++s) if (!streams[s].used) { st = &streams[s]; break; }
    if (st) {
        if (!st->ring) st->ring = calloc((size_t)RING * 2, sizeof(float));
        st->spec = *spec;
        st->head = st->count = 0;
        st->gain = 1.0f;
        st->paused = true;
        st->used = st->ring != NULL;
        if (!st->used) st = NULL;
    }
    pthread_mutex_unlock(&audio_lock);
    if (!st) SDL_SetError("no free stream");
    return st;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream) { if (stream) stream->paused = false; return true; }
bool SDL_PauseAudioStreamDevice(SDL_AudioStream *stream) { if (stream) stream->paused = true; return true; }
void SDL_DestroyAudioStream(SDL_AudioStream *stream) {
    if (!stream) return;
    pthread_mutex_lock(&audio_lock);
    stream->used = false;
    stream->count = 0;
    pthread_mutex_unlock(&audio_lock);
}
static int frame_bytes(const SDL_AudioStream *st) {
    return st->spec.channels * (st->spec.format == SDL_AUDIO_F32 ? 4 : 2);
}
int SDL_GetAudioStreamQueued(SDL_AudioStream *stream) {
    if (!stream) return -1;
    pthread_mutex_lock(&audio_lock);
    const int bytes = (int)stream->count * frame_bytes(stream);
    pthread_mutex_unlock(&audio_lock);
    return bytes;
}
bool SDL_SetAudioStreamGain(SDL_AudioStream *stream, float gain) { if (stream) stream->gain = gain; return true; }
bool SDL_ClearAudioStream(SDL_AudioStream *stream) {
    if (!stream) return false;
    pthread_mutex_lock(&audio_lock);
    stream->count = 0;
    pthread_mutex_unlock(&audio_lock);
    return true;
}
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buf, int len) {
    if (!stream || !buf || len < 0) return false;
    const int ch = stream->spec.channels, fb = frame_bytes(stream);
    const int frames = len / fb;
    const bool is_float = stream->spec.format == SDL_AUDIO_F32;
    pthread_mutex_lock(&audio_lock);
    for (int f = 0; f < frames; ++f) {
        float l, r;
        if (is_float) {
            const float *p = (const float *)buf + (size_t)f * ch;
            l = p[0]; r = ch > 1 ? p[1] : p[0];
        } else {
            const int16_t *p = (const int16_t *)buf + (size_t)f * ch;
            l = p[0] / 32768.0f; r = ch > 1 ? p[1] / 32768.0f : l;
        }
        if (stream->count == RING) { stream->head = (stream->head + 1) % RING; --stream->count; } /* drop the oldest */
        const unsigned at = (stream->head + stream->count) % RING;
        stream->ring[at * 2] = l;
        stream->ring[at * 2 + 1] = r;
        ++stream->count;
    }
    pthread_mutex_unlock(&audio_lock);
    return true;
}
