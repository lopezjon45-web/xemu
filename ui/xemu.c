/*
 * xemu SDL display driver
 *
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * Based on sdl2.c, sdl2-gl.c
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/* Ported SDL 1.2 code to 2.0 by Dave Airlie. */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu-version.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
#include "qobject/qdict.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/kbd-state.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "block/block.h"
#include "system/system.h"
#include "xui/xemu-hud.h"
#include "xemu-input.h"
#include "xemu-settings.h"
#include "xemu-snapshots.h"
#include "xemu-notifications.h"
#include "xemu-version.h"
#include "xemu-os-utils.h"

#include "data/xemu_64x64.png.h"

#include "hw/xbox/smbus.h" // For eject, drive tray
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/debug.h"
#ifdef XEMU_UWP
#include "xemu-uwp-audio.h"
#endif
#include "qemu/xemu-uwp.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "xemu-uwp-dash.h"
#include <signal.h>
#include "ui/xemu-notifications.h"

#include <stb_image.h>
#include <locale.h>
#include <math.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
#include "xui/win32-dxgi-present.h"
#endif


#ifndef DEBUG_XEMU_C
#define DEBUG_XEMU_C 0
#endif

#if DEBUG_XEMU_C
#define DPRINTF(...) fprintf(stderr, __VA_ARGS__)
#else
#define DPRINTF(...)
#endif

uint64_t vblank_interval_ns = 16666666LL;
bool use_vblank_timer_thread = true;

struct xemu_console {
    DisplayChangeListener dcl;
    DisplaySurface *surface;
    DisplayOptions *opts;
    SDL_Window *real_window;
    int idx;
    int hidden;
    int ignore_hotkeys;
    SDL_GLContext winctx;
    QKbdState *kbd;
};

#ifdef _WIN32
#include "nvapi.h"
// Provide hint to prefer high-performance graphics for hybrid systems
// https://gpuopen.com/learn/amdpowerxpressrequesthighperformance/
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
// https://docs.nvidia.com/gameworks/content/technologies/desktop/optimus.htm
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
#endif

static int num_outputs;
static struct xemu_console *scon_list;
static SDL_Surface *guest_sprite_surface;
static int gui_grab; /* if true, all keyboard/mouse events are grabbed */
static bool alt_grab;
static bool ctrl_grab;
static int gui_saved_grab;
static int gui_fullscreen;
static int gui_grab_code = SDL_KMOD_LALT | SDL_KMOD_LCTRL;
static SDL_Cursor *sdl_cursor_normal;
static SDL_Cursor *sdl_cursor_hidden;
static int absolute_enabled;
static int guest_cursor;
static int guest_x, guest_y;
static SDL_Cursor *guest_sprite;
static Notifier mouse_mode_notifier;
static SDL_Window *m_window;
static SDL_GLContext m_context;
static QemuSemaphore display_init_sem;
static QemuSemaphore display_shutdown_sem;
static QEMUTimer *vblank_timer;
static QemuThread vblank_thread;
static bool qemu_exiting;
static int exit_status;

void tcg_register_init_ctx(void); // tcg.c

#if DEBUG_XEMU_C
static uint64_t lock_held_acc;
static uint64_t lock_start;
#endif

void xemu_main_loop_lock(void)
{
    qemu_mutex_lock_main_loop();
    bql_lock();
#if DEBUG_XEMU_C
    lock_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
#endif
}

void xemu_main_loop_unlock(void)
{
#if DEBUG_XEMU_C
    lock_held_acc += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - lock_start;
#endif
    bql_unlock();
    qemu_mutex_unlock_main_loop();
}

SDL_Window *xemu_get_window(void)
{
    return m_window;
}

static struct xemu_console *get_scon_from_window(uint32_t window_id)
{
    int i;
    for (i = 0; i < num_outputs; i++) {
        if (scon_list[i].real_window == SDL_GetWindowFromID(window_id)) {
            return &scon_list[i];
        }
    }
    return NULL;
}

static void window_resize(struct xemu_console *scon)
{
    if (!scon->real_window) {
        return;
    }

    SDL_SetWindowSize(scon->real_window,
                      surface_width(scon->surface),
                      surface_height(scon->surface));
}

static void hide_cursor(struct xemu_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    SDL_HideCursor();
    SDL_SetCursor(sdl_cursor_hidden);

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetWindowRelativeMouseMode(scon->real_window, true);
    }
}

static void show_cursor(struct xemu_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetWindowRelativeMouseMode(scon->real_window, false);
    }

    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    } else {
        SDL_SetCursor(sdl_cursor_normal);
    }

    SDL_ShowCursor();
}

static void grab_start(struct xemu_console *scon)
{
}

static void grab_end(struct xemu_console *scon)
{
    SDL_SetWindowKeyboardGrab(scon->real_window, false);
    SDL_SetWindowMouseGrab(scon->real_window, false);
    gui_grab = 0;
    show_cursor(scon);
}

static void absolute_mouse_grab(struct xemu_console *scon)
{
    float mouse_x, mouse_y;
    int scr_w, scr_h;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
    if (mouse_x > 0 && mouse_x < scr_w - 1 &&
        mouse_y > 0 && mouse_y < scr_h - 1) {
        grab_start(scon);
    }
}

static void mouse_mode_change(Notifier *notify, void *data)
{
    if (qemu_input_is_absolute(scon_list[0].dcl.con)) {
        if (!absolute_enabled) {
            absolute_enabled = 1;
            SDL_SetWindowRelativeMouseMode(scon_list[0].real_window, false);
            absolute_mouse_grab(&scon_list[0]);
        }
    } else if (absolute_enabled) {
        if (!gui_fullscreen) {
            grab_end(&scon_list[0]);
        }
        absolute_enabled = 0;
    }
}

static void send_mouse_event(struct xemu_console *scon, int dx, int dy,
                                 int x, int y, int state)
{
    static uint32_t bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT]       = SDL_BUTTON_MASK(SDL_BUTTON_LEFT),
        [INPUT_BUTTON_MIDDLE]     = SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE),
        [INPUT_BUTTON_RIGHT]      = SDL_BUTTON_MASK(SDL_BUTTON_RIGHT),
    };
    static uint32_t prev_state;

    if (prev_state != state) {
        qemu_input_update_buttons(scon->dcl.con, bmap, prev_state, state);
        prev_state = state;
    }

    if (qemu_input_is_absolute(scon->dcl.con)) {
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_X,
                             x, 0, surface_width(scon->surface));
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_Y,
                             y, 0, surface_height(scon->surface));
    } else {
        if (guest_cursor) {
            x -= guest_x;
            y -= guest_y;
            guest_x += x;
            guest_y += y;
            dx = x;
            dy = y;
        }
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_X, dx);
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_Y, dy);
    }
    qemu_input_event_sync();
}

static void set_full_screen(struct xemu_console *scon, bool set)
{
    gui_fullscreen = set;

    if (gui_fullscreen) {
        const SDL_DisplayMode *mode = NULL;
        SDL_DisplayMode **modes = NULL;
        if (g_config.display.window.fullscreen_exclusive) {
            SDL_DisplayID display = SDL_GetDisplayForWindow(scon->real_window);
            if (display) {
                int num_modes = 0;
                modes = SDL_GetFullscreenDisplayModes(display, &num_modes);
                if (modes && num_modes > 0) {
                    // First mode is the highest resolution, typically the native resolution
                    mode = modes[0];
                }
            }
            if (mode) {
                fprintf(stderr, "Selected exclusive fullscreen mode: %dx%d pixel_density=%f refresh_rate=%f\n", mode->w, mode->h, mode->pixel_density, mode->refresh_rate);
            } else {
                fprintf(stderr, "Failed to get fullscreen display mode: %s\n", SDL_GetError());
            }
        }
        SDL_SetWindowFullscreenMode(scon->real_window, mode);
        SDL_free(modes);
        SDL_SetWindowFullscreen(scon->real_window, true);
        gui_saved_grab = gui_grab;
        grab_start(scon);
    } else {
        if (!gui_saved_grab) {
            grab_end(scon);
        }
        SDL_SetWindowFullscreen(scon->real_window, false);
    }
}

static void toggle_full_screen(struct xemu_console *scon)
{
    set_full_screen(scon, !gui_fullscreen);
}

void xemu_toggle_fullscreen(void)
{
    toggle_full_screen(&scon_list[0]);
}

int xemu_is_fullscreen(void)
{
    return gui_fullscreen;
}

static int get_mod_state(void)
{
    SDL_Keymod mod = SDL_GetModState();

    if (alt_grab) {
        return (mod & (gui_grab_code | SDL_KMOD_LSHIFT)) ==
            (gui_grab_code | SDL_KMOD_LSHIFT);
    } else if (ctrl_grab) {
        return (mod & SDL_KMOD_RCTRL) == SDL_KMOD_RCTRL;
    } else {
        return (mod & gui_grab_code) == gui_grab_code;
    }
}

static void process_key(struct xemu_console *scon, SDL_KeyboardEvent *ev)
{
    int qcode;

    if (ev->scancode >= qemu_input_map_usb_to_qcode_len) {
        return;
    }
    qcode = qemu_input_map_usb_to_qcode[ev->scancode];
    qkbd_state_key_event(scon->kbd, qcode, ev->type == SDL_EVENT_KEY_DOWN);
}

static void handle_keydown(SDL_Event *ev)
{
    int win;
    struct xemu_console *scon = get_scon_from_window(ev->key.windowID);
    if (scon == NULL) return;
    int gui_key_modifier_pressed = get_mod_state();
    int gui_keysym = 0;

    if (!scon->ignore_hotkeys && gui_key_modifier_pressed && !ev->key.repeat) {
        switch (ev->key.scancode) {
        case SDL_SCANCODE_2:
        case SDL_SCANCODE_3:
        case SDL_SCANCODE_4:
        case SDL_SCANCODE_5:
        case SDL_SCANCODE_6:
        case SDL_SCANCODE_7:
        case SDL_SCANCODE_8:
        case SDL_SCANCODE_9:
            if (gui_grab) {
                grab_end(scon);
            }

            win = ev->key.scancode - SDL_SCANCODE_1;
            if (win < num_outputs) {
                scon_list[win].hidden = !scon_list[win].hidden;
                if (scon_list[win].real_window) {
                    if (scon_list[win].hidden) {
                        SDL_HideWindow(scon_list[win].real_window);
                    } else {
                        SDL_ShowWindow(scon_list[win].real_window);
                    }
                }
                gui_keysym = 1;
            }
            break;
        case SDL_SCANCODE_F:
            toggle_full_screen(scon);
            gui_keysym = 1;
            break;
        case SDL_SCANCODE_G:
            gui_keysym = 1;
            if (!gui_grab) {
                grab_start(scon);
            } else if (!gui_fullscreen) {
                grab_end(scon);
            }
            break;
        case SDL_SCANCODE_U:
            window_resize(scon);
            gui_keysym = 1;
            break;
        default:
            break;
        }
    }
    if (!gui_keysym) {
        process_key(scon, &ev->key);
    }
}

static void handle_keyup(SDL_Event *ev)
{
    struct xemu_console *scon = get_scon_from_window(ev->key.windowID);
    if (!scon) return;

    scon->ignore_hotkeys = false;
    process_key(scon, &ev->key);
}

static void handle_mousemotion(SDL_Event *ev)
{
    int max_x, max_y;
    struct xemu_console *scon = get_scon_from_window(ev->motion.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        int scr_w, scr_h;
        SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
        max_x = scr_w - 1;
        max_y = scr_h - 1;
        if (gui_grab && !gui_fullscreen
            && (ev->motion.x == 0 || ev->motion.y == 0 ||
                ev->motion.x == max_x || ev->motion.y == max_y)) {
            grab_end(scon);
        }
        if (!gui_grab &&
            (ev->motion.x > 0 && ev->motion.x < max_x &&
             ev->motion.y > 0 && ev->motion.y < max_y)) {
            grab_start(scon);
        }
    }
    if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        send_mouse_event(scon, ev->motion.xrel, ev->motion.yrel,
                             ev->motion.x, ev->motion.y, ev->motion.state);
    }
}

static void handle_mousebutton(SDL_Event *ev)
{
    int buttonstate = SDL_GetMouseState(NULL, NULL);
    SDL_MouseButtonEvent *bev;
    struct xemu_console *scon = get_scon_from_window(ev->button.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    bev = &ev->button;
    if (!gui_grab && !qemu_input_is_absolute(scon->dcl.con)) {
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP && bev->button == SDL_BUTTON_LEFT) {
            /* start grabbing all events */
            grab_start(scon);
        }
    } else {
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            buttonstate |= SDL_BUTTON_MASK(bev->button);
        } else {
            buttonstate &= ~SDL_BUTTON_MASK(bev->button);
        }
        send_mouse_event(scon, 0, 0, bev->x, bev->y, buttonstate);
    }
}

static void handle_mousewheel(SDL_Event *ev)
{
    struct xemu_console *scon = get_scon_from_window(ev->wheel.windowID);
    SDL_MouseWheelEvent *wev = &ev->wheel;
    InputButton btn;

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (wev->y > 0) {
        btn = INPUT_BUTTON_WHEEL_UP;
    } else if (wev->y < 0) {
        btn = INPUT_BUTTON_WHEEL_DOWN;
    } else {
        return;
    }

    qemu_input_queue_btn(scon->dcl.con, btn, true);
    qemu_input_event_sync();
    qemu_input_queue_btn(scon->dcl.con, btn, false);
    qemu_input_event_sync();
}

static void handle_windowevent(SDL_Event *ev)
{
    struct xemu_console *scon = get_scon_from_window(ev->window.windowID);
    bool allow_close = true;

    if (!scon) {
        return;
    }

    switch (ev->type) {
    case SDL_EVENT_WINDOW_RESIZED:
        {
            QemuUIInfo info;
            memset(&info, 0, sizeof(info));
            info.width = ev->window.data1;
            info.height = ev->window.data2;
            dpy_set_ui_info(scon->dcl.con, &info, true);

            if (!gui_fullscreen) {
                g_config.display.window.last_width = ev->window.data1;
                g_config.display.window.last_height = ev->window.data2;
            }

#ifdef _WIN32
            if (win32_dxgi_present_is_active()) {
                int width;
                int height;
                if (!SDL_GetWindowSizeInPixels(scon->real_window, &width,
                                               &height)) {
                    fprintf(stderr, "SDL_GetWindowSizeInPixels failed "
                                    "responding to resize event.\n");
                } else {
                    win32_dxgi_present_resize(width, height);
                }
            }
#endif
        }
        break;
#ifdef _WIN32
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (win32_dxgi_present_is_active()) {
            win32_dxgi_present_resize(ev->window.data1, ev->window.data2);
        }
        break;
#endif

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        if (!gui_grab && (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
            absolute_mouse_grab(scon);
        }
        /* If a new console window opened using a hotkey receives the
         * focus, SDL sends another KEYDOWN event to the new window,
         * closing the console window immediately after.
         *
         * Work around this by ignoring further hotkey events until a
         * key is released.
         */
        scon->ignore_hotkeys = get_mod_state();
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (gui_grab && !gui_fullscreen) {
            grab_end(scon);
        }
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (qemu_console_is_graphic(scon->dcl.con)) {
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
        } else {
            SDL_HideWindow(scon->real_window);
            scon->hidden = true;
        }
        break;
    case SDL_EVENT_WINDOW_SHOWN:
        scon->hidden = false;
        break;
    case SDL_EVENT_WINDOW_HIDDEN:
        scon->hidden = true;
        break;
    }
}

static void mouse_warp(DisplayChangeListener *dcl,
                       int x, int y, bool on)
{
    struct xemu_console *scon = container_of(dcl, struct xemu_console, dcl);

    if (!qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (on) {
        if (!guest_cursor) {
            show_cursor(scon);
        }
        if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
            SDL_SetCursor(guest_sprite);
            if (!qemu_input_is_absolute(scon->dcl.con) && !absolute_enabled) {
                SDL_WarpMouseInWindow(scon->real_window, x, y);
            }
        }
    } else if (gui_grab) {
        hide_cursor(scon);
    }
    guest_cursor = on;
    guest_x = x, guest_y = y;
}

static void mouse_define(DisplayChangeListener *dcl,
                             QEMUCursor *c)
{

    if (guest_sprite) {
        SDL_DestroyCursor(guest_sprite);
    }

    if (guest_sprite_surface) {
        SDL_DestroySurface(guest_sprite_surface);
    }

    guest_sprite_surface =
        SDL_CreateSurfaceFrom(c->width, c->height, SDL_PIXELFORMAT_ARGB8888, c->data, c->width * 4);

    if (!guest_sprite_surface) {
        fprintf(stderr, "Failed to make rgb surface from %p\n", c);
        return;
    }
    guest_sprite = SDL_CreateColorCursor(guest_sprite_surface,
                                         c->hot_x, c->hot_y);
    if (!guest_sprite) {
        fprintf(stderr, "Failed to make color cursor from %p\n", c);
        return;
    }
    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(dcl->con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    }
}

static void xb_surface_gl_create_texture(DisplaySurface *surface)
{
    assert(QEMU_IS_ALIGNED(surface_stride(surface), surface_bytes_per_pixel(surface)));

    switch (surface_format(surface)) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
        surface->glformat = GL_BGRA_EXT;
        surface->gltype = GL_UNSIGNED_BYTE;
        break;
    case PIXMAN_BE_x8r8g8b8:
    case PIXMAN_BE_a8r8g8b8:
        surface->glformat = GL_RGBA;
        surface->gltype = GL_UNSIGNED_BYTE;
        break;
    case PIXMAN_r5g6b5:
        surface->glformat = GL_RGB;
        surface->gltype = GL_UNSIGNED_SHORT_5_6_5;
        break;
    default:
        g_assert_not_reached();
    }

    if (!surface->texture) {
        glGenTextures(1, &surface->texture);
    }
    glBindTexture(GL_TEXTURE_2D, surface->texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT,
                  surface_stride(surface) / surface_bytes_per_pixel(surface));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                 surface_width(surface),
                 surface_height(surface),
                 0, surface->glformat, surface->gltype,
                 surface_data(surface));
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

static void xb_surface_gl_destroy_texture(DisplaySurface *surface)
{
    if (!surface || !surface->texture) {
        return;
    }
    glDeleteTextures(1, &surface->texture);
    surface->texture = 0;
}

static bool xb_console_gl_check_format(DisplayChangeListener *dcl,
                                       pixman_format_code_t format)
{
    switch (format) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
    case PIXMAN_r5g6b5:
        return true;
    default:
        return false;
    }
}

static void gl_switch(DisplayChangeListener *dcl,
                      DisplaySurface *new_surface)
{
    struct xemu_console *scon = container_of(dcl, struct xemu_console, dcl);
    scon->surface = new_surface;
}

static float update_avg(float avg, float ms, float r) {
    if (fabs(avg-ms) > 0.25*avg) avg = ms;
    else avg = avg*(1.0-r)+ms*r;
    return avg;
}

static float fps = 1.0;

static void update_fps(void)
{
    static float avg = 1.0;
    static int64_t last_update = 0;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (!last_update) {
        last_update = now;
        return;
    }
    float ms = ((float)(now-last_update)/1000000.0);
    last_update = now;
    avg = update_avg(avg, ms, 0.5);
    fps = 1000.0/avg;
}

static void process_vblank(struct xemu_console *scon)
{
    assert(bql_locked());

    update_fps();

#if 0
    static uint64_t last_ns = 0;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    uint64_t delta_ns = last_ns ? now_ns - last_ns : 0;
    fprintf(stderr, "%s delta_ns=%"PRId64"\n", __func__, delta_ns);
    last_ns = now_ns;
#endif

    graphic_hw_update(scon->dcl.con);
}

static void vblank_timer_callback(void *opaque)
{
    struct xemu_console *scon = (struct xemu_console *)opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    process_vblank(scon);
    timer_mod_ns(vblank_timer, now + vblank_interval_ns);
}

static void *vblank_timer_thread(void *opaque)
{
    struct xemu_console *scon = (struct xemu_console *)opaque;
    int64_t next_vblank = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    while (!qatomic_read(&qemu_exiting)) {
        // Schedule next vblank at fixed interval (absolute deadline)
        next_vblank += vblank_interval_ns;

        // Wait until deadline
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (now < next_vblank) {
            SDL_DelayPrecise(next_vblank - now);
        } else if (now > next_vblank + vblank_interval_ns) {
            // We've fallen behind by more than one frame, reset to avoid
            // rapid-fire catch-up
            next_vblank = now;
        }

        if (!qatomic_read(&qemu_exiting)) {
            xemu_main_loop_lock();
            process_vblank(scon);
            xemu_main_loop_unlock();
        }
    }

    return NULL;
}

#if DEBUG_XEMU_C
static void report_stats(void)
{
    uint64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    static uint64_t last_reported = 0;
    static int num_frames = 0;
    uint64_t delta_ms = now - last_reported;
    num_frames += 1;
    if (delta_ms >= 1000) {
        DPRINTF("[[ ");
        DPRINTF("vblank @%fHz avg", fps);
        DPRINTF(" - bql %"PRId64"ns/iter, %g%% time avg", lock_held_acc/num_frames, (double)lock_held_acc/(double)(delta_ms * 10000.0));
        DPRINTF(" ]]\n");
        lock_held_acc = 0;
        last_reported = now;
        num_frames = 0;
    }
}
#endif

/**
 * Renders the main interface. Usually called from the main thread,
 * but may sometimes be called from another thread.
 */
static void gl_render_frame(struct xemu_console *scon)
{
    static bool rendering;
    if (qatomic_xchg(&rendering, true) || qatomic_read(&qemu_exiting)) {
        return;
    }

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    bool flip_required = false;
    bool release_surface_texture = false;

    /* XXX: Note that this bypasses the usual VGA path in order to quickly
     * get the surface. This is simple and fast, at the cost of accuracy.
     * Ideally, this should go through the VGA code and opportunistically pull
     * the surface like this, but handle the VGA logic as well. For now, just
     * use this fast path to handle the common case.
     *
     * In the event the surface is not found in the surface cache, e.g. when
     * the guest code isn't using HW accelerated rendering, but just blitting
     * to the framebuffer, fall back to the VGA path.
     */
    UWP_PROF_BEGIN(t_getfb);
    GLuint tex = nv2a_get_framebuffer_surface();
    UWP_PROF_END(UI_GETFB, t_getfb);

    assert(glGetError() == GL_NO_ERROR);

    if (tex == 0) {
        xemu_main_loop_lock();
        // FIXME: Don't upload if notdirty
        xb_surface_gl_create_texture(scon->surface);
        tex = scon->surface->texture;
        flip_required = true;
        release_surface_texture = true;
        xemu_main_loop_unlock();
    }

#ifdef _WIN32
    if (win32_dxgi_present_is_active()) {
        win32_dxgi_present_begin_frame();
    }
#endif

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    xemu_snapshots_set_framebuffer_texture(tex, flip_required);
    xemu_hud_set_framebuffer_texture(tex, flip_required);

    /* FIXME: Finer locking. Event handlers in segments of the code expect
     * to be running on the main thread with the BQL. For now, acquire the
     * lock and perform rendering, but release before swap to avoid
     * possible lengthy blocking (for vsync).
     */
    xemu_main_loop_lock();
    xemu_hud_update();
    xemu_main_loop_unlock();

    UWP_PROF_BEGIN(t_hud);
    xemu_hud_render();
    UWP_PROF_END(UI_HUD, t_hud);
    UWP_PROF_BEGIN(t_finish);
    glFinish();
    UWP_PROF_END(UI_FINISH, t_finish);

    if (release_surface_texture) {
        xemu_main_loop_lock();
        xb_surface_gl_destroy_texture(scon->surface);
        xemu_main_loop_unlock();
    }

    nv2a_release_framebuffer_surface();

#ifdef _WIN32
    if (win32_dxgi_present_is_active()) {
        win32_dxgi_present_end_frame(g_config.display.window.vsync);
    } else {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr,
                    "win32_dxgi present failed or unavailable, falling back to "
                    "SDL_GL_SwapWindow\n");
            warned = true;
        }
        UWP_PROF_BEGIN(t_swap);
        SDL_GL_SwapWindow(scon->real_window);
        UWP_PROF_END(UI_SWAP, t_swap);
    }
#else
    SDL_GL_SwapWindow(scon->real_window);
#endif
    assert(glGetError() == GL_NO_ERROR);

    qatomic_set(&rendering, false);

#if DEBUG_XEMU_C
    report_stats();
#endif
}

static bool event_watch_callback(void *userdata, SDL_Event *event)
{
    struct xemu_console *scon = (struct xemu_console *)userdata;

    if (event->type == SDL_EVENT_WINDOW_RESIZED) {
#ifdef _WIN32
        if (win32_dxgi_present_is_active()) {
            int width;
            int height;
            if (!SDL_GetWindowSizeInPixels(scon->real_window, &width,
                                           &height)) {
                fprintf(stderr, "SDL_GetWindowSizeInPixels failed "
                                "responding to resize event.\n");
            } else {
                win32_dxgi_present_resize(width, height);
            }
        }
#endif
        gl_render_frame(scon);
    } else if (event->type == SDL_EVENT_WINDOW_EXPOSED) {
        gl_render_frame(scon);
    }

    return true; // Ignored
}

static void poll_events(struct xemu_console *scon)
{
    SDL_Event ev1, *ev = &ev1;
    bool allow_close = true;

    int kbd = 0, mouse = 0;
    xemu_hud_should_capture_kbd_mouse(&kbd, &mouse);

    while (SDL_PollEvent(ev)) {
        xemu_main_loop_lock();

        // HUD must process events first so that if a controller is detached,
        // a latent rebind request can cancel before the state is freed
        xemu_hud_process_sdl_events(ev);
        xemu_input_process_sdl_events(ev);

        switch (ev->type) {
        case SDL_EVENT_KEY_DOWN:
            if (kbd) break;
            handle_keydown(ev);
            break;
        case SDL_EVENT_KEY_UP:
            if (kbd) break;
            handle_keyup(ev);
            break;
        case SDL_EVENT_QUIT:
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (mouse) break;
            handle_mousemotion(ev);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (mouse) break;
            handle_mousebutton(ev);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (mouse) break;
            handle_mousewheel(ev);
            break;
        case SDL_EVENT_WINDOW_FIRST ... SDL_EVENT_WINDOW_LAST:
            handle_windowevent(ev);
            break;
        default:
            break;
        }

        xemu_main_loop_unlock();
    }

    xemu_main_loop_lock();
    xemu_input_update_controllers();
    xemu_main_loop_unlock();
}

static void display_very_early_init(DisplayOptions *o)
{
#ifdef __linux__
    /* SDL3 falls back to XWayland on compositors without fifo-v1 [1],
     * breaking HiDPI. Swap may block when occluded, but the BQL is released
     * before swap so emulation is unaffected.
     *
     * [1] https://github.com/libsdl-org/SDL/pull/9383
     */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
#endif

#ifdef XEMU_UWP
    {
        /*
         * Give Mesa a folder for its shader cache. Without one it has no disk
         * cache, reports no program binary formats, and xemu's own shader
         * cache stays empty too: every start compiled every shader again.
         * (rodrigoandrigo/xemu's UWP port sets the same variable.)
         */
        char *base = SDL_GetPrefPath("xemu", "xemu");
        char *dir = g_strdup_printf("%smesa-shader-cache", base ? base : "");
        wchar_t *wdir = g_utf8_to_utf16(dir, -1, NULL, NULL, NULL);
        g_mkdir_with_parents(dir, 0700);
        if (wdir) {
            _wputenv_s(L"MESA_SHADER_CACHE_DIR", wdir);
        }
        g_free(wdir);
        g_free(dir);
        SDL_free(base);
    }
#endif

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Failed to initialize SDL video subsystem: %s\n",
                SDL_GetError());
        exit(1);
    }

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR /* only available since SDL 2.0.8 */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    // Initialize rendering context
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    char *title = g_strdup_printf("xemu | v%s"
#ifdef XEMU_DEBUG_BUILD
                                  " Debug"
#endif
                                  , xemu_version);

    // Decide window size
    int min_window_width = 640;
    int min_window_height = 480;
    int window_width = min_window_width;
    int window_height = min_window_height;

    const int res_table[][2] = {
        {640,  480},
        {720,  480},
        {1280, 720},
        {1280, 800},
        {1280, 960},
        {1920, 1080},
        {2560, 1440},
        {2560, 1600},
        {2560, 1920},
        {3840, 2160}
    };

    if (g_config.display.window.startup_size == CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_LAST_USED) {
        window_width  = g_config.display.window.last_width;
        window_height = g_config.display.window.last_height;
    } else {
        window_width  = res_table[g_config.display.window.startup_size-1][0];
        window_height = res_table[g_config.display.window.startup_size-1][1];
    }

    if (window_width < min_window_width) {
        window_width = min_window_width;
    }
    if (window_height < min_window_height) {
        window_height = min_window_height;
    }

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    // Create main window
    m_window = SDL_CreateWindow(
        title, window_width, window_height,
        window_flags);
    if (m_window == NULL) {
        fprintf(stderr, "Failed to create main window: %s\n", SDL_GetError());
        SDL_Quit();
        exit(1);
    }
    g_free(title);
    SDL_SetWindowMinimumSize(m_window, min_window_width, min_window_height);

    const SDL_DisplayMode *disp_mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_window));
    if (disp_mode && (disp_mode->w < window_width || disp_mode->h < window_height)) {
        SDL_SetWindowSize(m_window, min_window_width, min_window_height);
        SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }

    m_context = SDL_GL_CreateContext(m_window);

    if (m_context != NULL && epoxy_gl_version() < 40) {
        SDL_GL_MakeCurrent(NULL, NULL);
        SDL_GL_DestroyContext(m_context);
        m_context = NULL;
    }

    if (m_context == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Unable to create OpenGL context",
            "Unable to create OpenGL context. This usually means the\r\n"
            "graphics device on this system does not support OpenGL 4.0.\r\n"
            "\r\n"
            "xemu cannot continue and will now exit.",
            m_window);
        SDL_DestroyWindow(m_window);
        SDL_Quit();
        exit(1);
    }

    int width, height, channels = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char *icon_data = stbi_load_from_memory(xemu_64x64_data, xemu_64x64_size, &width, &height, &channels, 4);
    if (icon_data) {
        SDL_Surface *icon = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, icon_data, width*4);
        if (icon) {
            SDL_SetWindowIcon(m_window, icon);
        }
        // Note: Retaining the memory allocated by stbi_load. It's used in place
        // by the SDL surface.
    }

    fprintf(stderr, "CPU: %s\n", xemu_get_cpu_info());
    fprintf(stderr, "OS_Version: %s\n", xemu_get_os_info());
    fprintf(stderr, "GL_VENDOR: %s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "GL_VERSION: %s\n", glGetString(GL_VERSION));
    fprintf(stderr, "GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
#ifdef XEMU_UWP
    {
        GLint formats = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
        uwp_log("GL program binary formats: %d (0 = shaders cannot be cached)", formats);
    }
#endif

    // Initialize offscreen rendering context now
    nv2a_context_init();
    SDL_GL_MakeCurrent(NULL, NULL);
}

static void display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_XEMU);
    display_opengl = 1;

    SDL_GL_MakeCurrent(m_window, m_context);
    SDL_GL_SetSwapInterval(g_config.display.window.vsync ? 1 : 0);
#ifdef _WIN32
    win32_dxgi_present_init(m_window);
#endif
    xemu_hud_init(m_window, m_context);
}

static const DisplayChangeListenerOps dcl_gl_ops = {
    .dpy_name                = "xemu-gl",
    .dpy_gfx_switch          = gl_switch,
    .dpy_gfx_check_format    = xb_console_gl_check_format,
    .dpy_mouse_set           = mouse_warp,
    .dpy_cursor_define       = mouse_define,
};

static void display_init(DisplayState *ds, DisplayOptions *o)
{
    uint8_t data = 0;
    int i;

    assert(o->type == DISPLAY_TYPE_XEMU);
    SDL_GL_MakeCurrent(m_window, m_context);

    gui_fullscreen = o->has_full_screen && o->full_screen;
    gui_fullscreen |= g_config.display.window.fullscreen_on_startup;

    num_outputs = 1;
    scon_list = g_new0(struct xemu_console, num_outputs);
    for (i = 0; i < num_outputs; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        assert(con != NULL);
        if (!qemu_console_is_graphic(con) &&
            qemu_console_get_index(con) != 0) {
            scon_list[i].hidden = true;
        }
        scon_list[i].idx = i;
        scon_list[i].opts = o;
        scon_list[i].dcl.ops = &dcl_gl_ops;
        scon_list[i].dcl.con = con;
        scon_list[i].kbd = qkbd_state_init(con);
        register_displaychangelistener(&scon_list[i].dcl);

#if defined(SDL_VIDEO_DRIVER_WINDOWS)
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(scon_list[i].real_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd) {
            qemu_console_set_window_id(con, (uintptr_t)hwnd);
        }
#elif defined(SDL_VIDEO_DRIVER_X11)
        Window xwindow = (Window)SDL_GetNumberProperty(SDL_GetWindowProperties(scon_list[i].real_window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (xwindow) {
            qemu_console_set_window_id(con, xwindow);
        }
#endif
    }

    scon_list[0].real_window = m_window;
    scon_list[0].winctx = m_context;

    mouse_mode_notifier.notify = mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&mouse_mode_notifier);

    sdl_cursor_hidden = SDL_CreateCursor(&data, &data, 8, 1, 0, 0);
    sdl_cursor_normal = SDL_GetCursor();

    // SDL_PollEvent may block during main window resize or drag operations.
    // Register event watch to handle rendering during these operations.
    SDL_AddEventWatch(event_watch_callback, &scon_list[0]);

    if (use_vblank_timer_thread) {
        qemu_thread_create(&vblank_thread, "vblank-timer", vblank_timer_thread,
                           &scon_list[0], QEMU_THREAD_JOINABLE);
    } else {
        vblank_timer = timer_new_ns(QEMU_CLOCK_REALTIME, vblank_timer_callback, &scon_list[0]);
        timer_mod_ns(vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + vblank_interval_ns);
    }

    /* Tell main thread to go ahead and create the app and enter the run loop */
    SDL_GL_MakeCurrent(NULL, NULL);
    qemu_sem_post(&display_init_sem);
}

static void display_finalize(void)
{
    if (use_vblank_timer_thread) {
        qemu_thread_join(&vblank_thread);
    }

    SDL_RemoveEventWatch(event_watch_callback, &scon_list[0]);
#ifdef _WIN32
    win32_dxgi_present_cleanup();
#endif
    SDL_GL_MakeCurrent(NULL, NULL);
    SDL_GL_DestroyContext(m_context);
    SDL_DestroyWindow(m_window);
    SDL_Quit();
}

static QemuDisplay qemu_display_xemu = {
    .type       = DISPLAY_TYPE_XEMU,
    .early_init = display_early_init,
    .init       = display_init,
};

static void register_xemu_display(void)
{
    qemu_display_register(&qemu_display_xemu);
}

type_init(register_xemu_display);

int gArgc;
char **gArgv;

#ifdef XEMU_UWP
/* Progress log for the Xbox port, in its own file (uwp_debug.log in LocalState\xemu\xemu). */
SDL_AudioStream *xemu_uwp_audio_stream; /* opened on the UI thread, used by the APU */
void uwp_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void uwp_log(const char *fmt, ...)
{
    static FILE *f;
    if (!f) {
        char *base = SDL_GetPrefPath("xemu", "xemu");
        char *path = g_strdup_printf("%suwp_debug.log", base ? base : "");
        f = fopen(path, "w");
        g_free(path);
        SDL_free(base);
        if (!f) {
            return;
        }
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "[%8.3f] ", SDL_GetTicks() / 1000.0);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
    va_end(ap);
}

/* Profiler state, see include/qemu/xemu-uwp.h. */
int64_t uwp_prof_ticks[UWP_PROF__COUNT];
int uwp_prof_calls[UWP_PROF__COUNT];
int uwp_dl_site;
int uwp_dl_counts[UWP_DL__COUNT];
int uwp_trace_budget;
int uwp_gpu_blits;
extern int64_t uwp_prof_nv2a_totals[NV2A_PROF__COUNT];
extern int64_t uwp_prof_nv2a_frames;

#define UWP_PROF_MAX_THREADS 48
static struct {
    char name[32];
    DWORD tid;
    HANDLE handle;
    uint64_t last_cpu;
} uwp_prof_threads[UWP_PROF_MAX_THREADS];
static int uwp_prof_num_threads;

void uwp_prof_register_thread(const char *name)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    HANDLE h = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &h, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        return;
    }
    AcquireSRWLockExclusive(&lock);
    /* a thread that registers twice (QEMU's name, then ours) is renamed */
    int slot = 0;
    while (slot < uwp_prof_num_threads &&
           uwp_prof_threads[slot].tid != GetCurrentThreadId()) {
        slot++;
    }
    if (slot < UWP_PROF_MAX_THREADS) {
        char *out = uwp_prof_threads[slot].name;
        for (int i = 0; name[i] && i < 31; i++) {
            *out++ = g_ascii_isalnum(name[i]) ? name[i] : '_';
        }
        *out = '\0';
    }
    if (slot == uwp_prof_num_threads && slot < UWP_PROF_MAX_THREADS) {
        uwp_prof_threads[slot].tid = GetCurrentThreadId();
        uwp_prof_threads[slot].handle = h;
        uwp_prof_num_threads++;
    } else {
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&lock);
}

/* One log line: CPU use per thread, time per probe, nv2a counters per frame. */
static void uwp_prof_report(double seconds)
{
    static int64_t last_ticks[UWP_PROF__COUNT];
    static int last_calls[UWP_PROF__COUNT];
    static int64_t last_nv2a[NV2A_PROF__COUNT], last_frames;
    static const char *const names[UWP_PROF__COUNT] = {
#define _X(x) #x,
        UWP_PROF_TIMERS_XMAC
#undef _X
    };
    char line[2048];
    int n = 0;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    n += snprintf(line + n, sizeof(line) - n, "prof cpu%%:");
    for (int i = 0; i < uwp_prof_num_threads; i++) {
        FILETIME c, e, k, u;
        if (!GetThreadTimes(uwp_prof_threads[i].handle, &c, &e, &k, &u)) {
            continue;
        }
        uint64_t cpu = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                       (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
        double pct = (cpu - uwp_prof_threads[i].last_cpu) / 1e5 / seconds;
        if (pct >= 1.0 || i < 3) {
            n += snprintf(line + n, sizeof(line) - n, " %s %.0f",
                          uwp_prof_threads[i].name, pct);
        }
        uwp_prof_threads[i].last_cpu = cpu;
    }
    {
        /* the whole process, which also covers threads that are not ours
         * (graphics driver, audio) */
        static uint64_t last_total;
        FILETIME c, e, k, u;
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
            uint64_t total =
                (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
            n += snprintf(line + n, sizeof(line) - n, " ALL %.0f",
                          (total - last_total) / 1e5 / seconds);
            last_total = total;
        }
    }
    n += snprintf(line + n, sizeof(line) - n, " | wall%% (calls):");
    for (int i = 0; i < UWP_PROF__COUNT; i++) {
        int64_t t = uwp_prof_ticks[i];
        int c = uwp_prof_calls[i];
        if (c != last_calls[i]) {
            n += snprintf(line + n, sizeof(line) - n, " %s %.1f (%d)", names[i],
                          (t - last_ticks[i]) * 100.0 / freq.QuadPart / seconds,
                          c - last_calls[i]);
        }
        last_ticks[i] = t;
        last_calls[i] = c;
    }
    int64_t frames = uwp_prof_nv2a_frames - last_frames;
    n += snprintf(line + n, sizeof(line) - n, " | per frame (%d frames):",
                  (int)frames);
    for (int i = 0; i < NV2A_PROF__COUNT; i++) {
        int64_t v = uwp_prof_nv2a_totals[i];
        if (v != last_nv2a[i] && frames > 0 && n < (int)sizeof(line) - 64) {
            n += snprintf(line + n, sizeof(line) - n, " %s %.1f",
                          nv2a_profile_get_counter_name(i),
                          (double)(v - last_nv2a[i]) / frames);
        }
        last_nv2a[i] = v;
    }
    last_frames = uwp_prof_nv2a_frames;
    static int last_dl[UWP_DL__COUNT];
    n += snprintf(line + n, sizeof(line) - n, " | readbacks by site:");
    for (int i = 0; i < UWP_DL__COUNT; i++) {
        n += snprintf(line + n, sizeof(line) - n, " %d",
                      uwp_dl_counts[i] - last_dl[i]);
        last_dl[i] = uwp_dl_counts[i];
    }
    static int last_gpu_blits;
    n += snprintf(line + n, sizeof(line) - n, " | gpu blits %d",
                  uwp_gpu_blits - last_gpu_blits);
    last_gpu_blits = uwp_gpu_blits;
    uwp_log("%s", line);

    static int reports;
    if (++reports % 7 == 0) {
        uwp_trace_budget = 70; /* a frame or two of events, every ~20 s */
    }
}

/*
 * Sampling profiler for one thread (the emulated CPU, the renderer): about
 * 500 times a second it is stopped for a moment and the address it was
 * executing is noted. Every
 * 30 s the busiest places go to the log, as offsets into xemu.exe (to be
 * looked up in the build's symbols on the PC), as other modules by name, or
 * as "generated code" (the guest's own code, translated).
 */
#define UWP_SAMPLES 16384
#define UWP_STACK_FRAMES 10

/*
 * The name of the export a system DLL address belongs to (the nearest export
 * at or below it). win32u.dll and ntdll.dll export every system call stub by
 * name, so this says which kernel service a thread was in.
 */
static const char *uwp_export_name(HMODULE mod, uint64_t addr, uint32_t *off)
{
    const uint8_t *base = (const uint8_t *)mod;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)
        (base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dd =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd->VirtualAddress) {
        return NULL;
    }
    const IMAGE_EXPORT_DIRECTORY *ed =
        (const IMAGE_EXPORT_DIRECTORY *)(base + dd->VirtualAddress);
    const uint32_t *names = (const uint32_t *)(base + ed->AddressOfNames);
    const uint16_t *ords = (const uint16_t *)(base + ed->AddressOfNameOrdinals);
    const uint32_t *funcs = (const uint32_t *)(base + ed->AddressOfFunctions);
    uint32_t rva = (uint32_t)(addr - (uint64_t)(uintptr_t)base), best = 0;
    const char *name = NULL;
    for (uint32_t i = 0; i < ed->NumberOfNames; i++) {
        uint32_t f = funcs[ords[i]];
        if (f <= rva && f >= best) {
            best = f;
            name = (const char *)(base + names[i]);
        }
    }
    if (off) {
        *off = rva - best;
    }
    return name;
}

/* Return addresses of a suspended thread, innermost first. Allocates nothing. */
static int uwp_unwind(CONTEXT *ctx, uint64_t *out, int max)
{
    int n = 0;
    while (n < max && ctx->Rip) {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx->Rip, &image, NULL);
        if (fn) {
            void *handler_data = NULL;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx->Rip, fn, ctx,
                             &handler_data, &establisher, NULL);
        } else {
            /* a leaf function: the return address is on top of the stack */
            if (IsBadReadPtr((void *)ctx->Rsp, 8)) {
                break;
            }
            ctx->Rip = *(DWORD64 *)ctx->Rsp;
            ctx->Rsp += 8;
        }
        if (!ctx->Rip) {
            break;
        }
        out[n++] = ctx->Rip;
    }
    return n;
}

struct uwp_count {
    char name[72];
    int count;
};

static void uwp_count_add(struct uwp_count *tab, int max, const char *name)
{
    for (int i = 0; i < max; i++) {
        if (!tab[i].name[0]) {
            g_strlcpy(tab[i].name, name, sizeof(tab[i].name));
        }
        if (!strcmp(tab[i].name, name)) {
            tab[i].count++;
            return;
        }
    }
}

static void uwp_count_log(const char *title, struct uwp_count *tab, int max, int rows)
{
    char line[1500];
    int n = snprintf(line, sizeof(line), "%s:", title);
    for (int rank = 0; rank < rows && n < (int)sizeof(line) - 100; rank++) {
        int best = -1;
        for (int i = 0; i < max && tab[i].name[0]; i++) {
            if (tab[i].count > 0 && (best < 0 || tab[i].count > tab[best].count)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        n += snprintf(line + n, sizeof(line) - n, " %s %d,", tab[best].name, tab[best].count);
        tab[best].count = 0;
    }
    uwp_log("%s", line);
}

static void uwp_module_name(HMODULE mod, char *out, int size)
{
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(mod, path, sizeof(path));
    const char *base = strrchr(path, '\\');
    g_strlcpy(out, base ? base + 1 : path, size);
}

static DWORD WINAPI uwp_sampler_thread(void *arg)
{
    const char *target = arg; /* which thread to watch: "vcpu" or "pfifo" */
    uint64_t *samples = g_malloc(UWP_SAMPLES * sizeof(uint64_t));
    struct {
        uint64_t key;
        int count;
    } *top = g_malloc(4096 * sizeof(*top));
    HMODULE exe = GetModuleHandleW(NULL);
    uint64_t exe_base = (uint64_t)(uintptr_t)exe;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)
        ((const uint8_t *)exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
    uint64_t exe_size = nt->OptionalHeader.SizeOfImage;
    int num = 0;
    HANDLE vcpu = NULL;
    DWORD next_dump = GetTickCount() + 30000;
    /* for the renderer thread: who called, to tell what the graphics library asks the kernel for */
    bool want_stacks = !strcmp(target, "pfifo");
    uint64_t (*stacks)[UWP_STACK_FRAMES] =
        want_stacks ? g_malloc0(UWP_SAMPLES * sizeof(*stacks)) : NULL;

    rcu_register_thread(); /* for uwp_read_guest */

    for (;;) {
        Sleep(10); /* 500 a second cost the watched thread about a fifth */
        if (!vcpu) {
            for (int i = 0; i < uwp_prof_num_threads; i++) {
                if (!strcmp(uwp_prof_threads[i].name, target)) {
                    vcpu = uwp_prof_threads[i].handle;
                }
            }
            continue;
        }
        if (num < UWP_SAMPLES && SuspendThread(vcpu) != (DWORD)-1) {
            CONTEXT ctx;
            ctx.ContextFlags = want_stacks ? CONTEXT_FULL : CONTEXT_CONTROL;
            if (GetThreadContext(vcpu, &ctx)) {
                samples[num] = ctx.Rip;
                if (want_stacks) {
                    memset(stacks[num], 0, sizeof(stacks[num]));
                    if (ctx.Rip - exe_base >= exe_size) {
                        uwp_unwind(&ctx, stacks[num], UWP_STACK_FRAMES);
                    }
                }
                num++;
            }
            ResumeThread(vcpu);
        }
        if ((int)(GetTickCount() - next_dump) < 0) {
            continue;
        }
        next_dump = GetTickCount() + 30000;

        /* group: xemu.exe in 64-byte pieces, anything else per module */
        int groups = 0, jit = 0;
        for (int i = 0; i < num; i++) {
            uint64_t key;
            if (samples[i] - exe_base < exe_size) {
                key = (samples[i] - exe_base) >> 6;
            } else {
                HMODULE mod = NULL;
                if (!GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCWSTR)(uintptr_t)samples[i], &mod) || !mod) {
                    jit++;
                    continue;
                }
                key = (uint64_t)(uintptr_t)mod | (1ull << 63);
            }
            int g = 0;
            while (g < groups && top[g].key != key) {
                g++;
            }
            if (g == groups) {
                if (groups == 4096) {
                    continue;
                }
                top[groups].key = key;
                top[groups++].count = 0;
            }
            top[g].count++;
        }
        char line[1800];
        int n = snprintf(line, sizeof(line),
                         "%s samples %d: generated code %d", target, num, jit);
        for (int rank = 0; rank < 40 && n < (int)sizeof(line) - 80; rank++) {
            int best = -1;
            for (int g = 0; g < groups; g++) {
                if (top[g].count > 0 &&
                    (best < 0 || top[g].count > top[best].count)) {
                    best = g;
                }
            }
            if (best < 0) {
                break;
            }
            if (top[best].key >> 63) {
                wchar_t path[260];
                DWORD len = GetModuleFileNameW(
                    (HMODULE)(uintptr_t)(top[best].key & ~(1ull << 63)), path,
                    ARRAY_SIZE(path));
                const wchar_t *base = path + len;
                while (base > path && base[-1] != L'\\') {
                    base--;
                }
                n += snprintf(line + n, sizeof(line) - n, ", %ls %d", base,
                              top[best].count);
            } else {
                n += snprintf(line + n, sizeof(line) - n, ", +%llx %d",
                              (unsigned long long)(top[best].key << 6),
                              top[best].count);
            }
            top[best].count = 0;
        }
        uwp_log("%s", line);

        if (want_stacks) {
            /*
             * Samples outside xemu.exe: the export they are in (a system call
             * stub in win32u / ntdll says which kernel service), the first
             * caller inside the GL driver, and the first caller in xemu.exe.
             */
            static struct uwp_count where[160], gallium[160], ours[160];
            memset(where, 0, sizeof(where));
            memset(gallium, 0, sizeof(gallium));
            memset(ours, 0, sizeof(ours));
            for (int i = 0; i < num; i++) {
                HMODULE mod = NULL;
                if (samples[i] - exe_base < exe_size ||
                    !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        (LPCWSTR)(uintptr_t)samples[i], &mod) || !mod) {
                    continue;
                }
                char name[72], modname[40];
                uwp_module_name(mod, modname, sizeof(modname));
                uint32_t off = 0;
                const char *exp = uwp_export_name(mod, samples[i], &off);
                if (exp && off < 0x400) {
                    snprintf(name, sizeof(name), "%.12s!%.50s", modname, exp);
                } else {
                    snprintf(name, sizeof(name), "%.20s+%llx", modname,
                             (unsigned long long)((samples[i] - (uint64_t)(uintptr_t)mod) & ~0xffull));
                }
                uwp_count_add(where, 160, name);
                bool got_gallium = false, got_ours = false;
                for (int f = 0; f < UWP_STACK_FRAMES && stacks[i][f]; f++) {
                    uint64_t a = stacks[i][f];
                    if (a - exe_base < exe_size) {
                        if (!got_ours) {
                            snprintf(name, sizeof(name), "+%llx", (unsigned long long)((a - exe_base) & ~0x3full));
                            uwp_count_add(ours, 160, name);
                            got_ours = true;
                        }
                        break;
                    }
                    HMODULE m2 = NULL;
                    if (!got_gallium &&
                        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCWSTR)(uintptr_t)a, &m2) && m2) {
                        uwp_module_name(m2, modname, sizeof(modname));
                        if (!g_ascii_strcasecmp(modname, "libgallium_wgl.dll")) {
                            snprintf(name, sizeof(name), "+%llx",
                                     (unsigned long long)((a - (uint64_t)(uintptr_t)m2) & ~0x7full));
                            uwp_count_add(gallium, 160, name);
                            got_gallium = true;
                        }
                    }
                }
            }
            uwp_count_log("pfifo outside xemu, by export", where, 160, 24);
            uwp_count_log("pfifo first caller in libgallium_wgl", gallium, 160, 24);
            uwp_count_log("pfifo first caller in xemu.exe", ours, 160, 16);
        }

        /* the same samples by guest address (start of the translated block) */
        if (jit) {
            extern uint32_t uwp_guest_pc_of(uintptr_t host_pc);
            extern int uwp_read_guest(uint32_t addr, uint8_t *buf, int len);
            int found = 0;
            groups = 0;
            for (int i = 0; i < num; i++) {
                uint32_t pc;
                if (samples[i] - exe_base < exe_size ||
                    !(pc = uwp_guest_pc_of((uintptr_t)samples[i]))) {
                    continue;
                }
                found++;
                int g = 0;
                while (g < groups && top[g].key != pc) {
                    g++;
                }
                if (g == groups) {
                    if (groups == 4096) {
                        continue;
                    }
                    top[groups].key = pc;
                    top[groups++].count = 0;
                }
                top[g].count++;
            }
            n = snprintf(line, sizeof(line), "%s guest blocks %d of %d (%d distinct)",
                         target, found, jit, groups);
            for (int rank = 0; rank < 48 && n < (int)sizeof(line) - 40; rank++) {
                int best = -1;
                for (int g = 0; g < groups; g++) {
                    if (top[g].count > 0 &&
                        (best < 0 || top[g].count > top[best].count)) {
                        best = g;
                    }
                }
                if (best < 0) {
                    break;
                }
                n += snprintf(line + n, sizeof(line) - n, ", %08x %d",
                              (unsigned)top[best].key, top[best].count);
                if (rank < 12) {
                    uint8_t code[64];
                    char hex[2 * sizeof(code) + 1];
                    if (uwp_read_guest((uint32_t)top[best].key, code,
                                       sizeof(code)) == 0) {
                        for (int k = 0; k < (int)sizeof(code); k++) {
                            snprintf(hex + 2 * k, 3, "%02x", code[k]);
                        }
                        uwp_log("%s guest code %08x x%d: %s", target,
                                (unsigned)top[best].key, top[best].count, hex);
                    }
                }
                top[best].count = 0;
            }
            uwp_log("%s", line);
        }
        num = 0;
    }
    return 0;
}

/*
 * Starting a game from a launcher (Game Vault): the app is opened through the
 * link xemu:?disc=<path of the disc image>. SDL (local patch) publishes each
 * activation as the hint SDL_WINRT_ACTIVATION_URI; this takes the disc path
 * out of it, or returns NULL when there is none. Free with g_free().
 */
#define UWP_ACTIVATION_HINT "SDL_WINRT_ACTIVATION_URI"

/*
 * Leave the emulator the same way the Exit menu item does. When xemu's main
 * returns, SDL opens the launcher's "launchOnExit" link, so this is also the
 * way back to Game Vault.
 */
static void uwp_quit(const char *why)
{
    uwp_log("quitting: %s", why);
    shutdown_action = SHUTDOWN_ACTION_POWEROFF;
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

/*
 * Quick save states. One per disc, kept as a snapshot inside the hard disk
 * image like xemu's own snapshots (they show up in its Snapshots window too).
 *   View + RB: save      View + LB: load
 * and the launch link may say "state=1" to load the disc's state as soon as
 * the machine runs, which skips logos and menus.
 */
static bool uwp_load_state_wanted, uwp_save_state_wanted;

static char *uwp_state_name(void)
{
    const char *disc = g_config.sys.files.dvd_path;
    const char *base = disc ? disc : "";
    for (const char *c = base; *c; c++) {
        if (*c == '/' || *c == '\\') {
            base = c + 1;
        }
    }
    char *name = g_strdup_printf("quick %s", base[0] ? base : "no disc");
    if (strlen(name) > 120) {
        name[120] = '\0';
    }
    return name;
}

static void uwp_state(bool save)
{
    char *name = uwp_state_name();
    Error *err = NULL;
    xemu_main_loop_lock();
    bool was_running = runstate_is_running();
    if (save) {
        xemu_snapshots_save(name, &err);
    } else {
        xemu_snapshots_load(name, &err);
    }
    /*
     * xemu_snapshots_load stops the machine first and starts it again only
     * when the load worked: asking for a state that does not exist left the
     * game frozen. Nothing has been changed in that case, so carry on.
     */
    if (err && was_running && !runstate_is_running()) {
        vm_start();
    }
    xemu_main_loop_unlock();
    if (err) {
        uwp_log("state %s failed (%s): %s", save ? "save" : "load", name,
                error_get_pretty(err));
        xemu_queue_error_message(save ? "The state could not be saved"
                                      : "There is no saved state for this game yet (View + RB saves one)");
        error_free(err);
    } else {
        uwp_log("state %s: %s", save ? "saved" : "loaded", name);
        xemu_queue_notification(save ? "State saved" : "State loaded");
    }
    g_free(name);
}

/*
 * Pressing the emulated controller's buttons from outside, for testing and
 * for setup screens that wait for a button when nobody is at the console:
 * the launch link takes press=down,down,a (names below; "wait" is a pause of
 * half a second, a number is a pause of that many milliseconds). Each press
 * is held for 150 ms and followed by 350 ms of nothing.
 */
extern uint16_t xemu_uwp_injected_buttons;
static uint16_t uwp_press_queue[64];
static uint16_t uwp_press_pause[64];
static int uwp_press_count, uwp_press_next;
static uint64_t uwp_press_until, uwp_press_release_at;

static void uwp_press(const char *list)
{
    static const struct {
        const char *name;
        uint16_t mask;
    } names[] = {
        { "a", CONTROLLER_BUTTON_A }, { "b", CONTROLLER_BUTTON_B },
        { "x", CONTROLLER_BUTTON_X }, { "y", CONTROLLER_BUTTON_Y },
        { "left", CONTROLLER_BUTTON_DPAD_LEFT }, { "up", CONTROLLER_BUTTON_DPAD_UP },
        { "right", CONTROLLER_BUTTON_DPAD_RIGHT }, { "down", CONTROLLER_BUTTON_DPAD_DOWN },
        { "back", CONTROLLER_BUTTON_BACK }, { "start", CONTROLLER_BUTTON_START },
        { "white", CONTROLLER_BUTTON_WHITE }, { "black", CONTROLLER_BUTTON_BLACK },
        { "ls", CONTROLLER_BUTTON_LSTICK }, { "rs", CONTROLLER_BUTTON_RSTICK },
    };
    char **parts = g_strsplit(list, ",", -1);
    if (uwp_press_next >= uwp_press_count) {
        uwp_press_count = uwp_press_next = 0;
    }
    for (int i = 0; parts[i] && uwp_press_count < ARRAY_SIZE(uwp_press_queue); i++) {
        uint16_t mask = 0, pause = 0;
        for (int n = 0; n < ARRAY_SIZE(names); n++) {
            if (!g_ascii_strcasecmp(parts[i], names[n].name)) {
                mask = names[n].mask;
            }
        }
        if (!mask) {
            pause = !g_ascii_strcasecmp(parts[i], "wait") ? 500 : (uint16_t)atoi(parts[i]);
            if (!pause) {
                continue;
            }
        }
        uwp_press_queue[uwp_press_count] = mask;
        uwp_press_pause[uwp_press_count++] = pause;
    }
    g_strfreev(parts);
    uwp_log("remote buttons: %s", list);
}

static void uwp_press_poll(void)
{
    uint64_t now = SDL_GetTicks();
    if (xemu_uwp_injected_buttons && now >= uwp_press_release_at) {
        xemu_uwp_injected_buttons = 0;
    }
    if (uwp_press_next >= uwp_press_count || now < uwp_press_until) {
        return;
    }
    uint16_t mask = uwp_press_queue[uwp_press_next];
    uint16_t pause = uwp_press_pause[uwp_press_next++];
    if (mask) {
        xemu_uwp_injected_buttons = mask;
        uwp_press_release_at = now + 150;
        uwp_press_until = now + 500;
    } else {
        uwp_press_until = now + pause;
    }
}

/* View + Menu (Back + Start) held together for two seconds quits. */
static void uwp_poll_quit_combo(void)
{
    static uint64_t held_since;
    static bool fired;
    static bool state_combo_was_down;
    bool held = false, want_save = false, want_load = false;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; i++) {
        SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
        if (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)) {
            held = true;
        }
        if (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
            !SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)) {
            want_save |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
            want_load |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        }
    }
    SDL_free(ids);
    if ((want_save || want_load) && !state_combo_was_down) {
        uwp_state(want_save);
    }
    state_combo_was_down = want_save || want_load;
    if (!held) {
        held_since = 0;
        fired = false;
    } else if (!held_since) {
        held_since = SDL_GetTicks();
    } else if (!fired && SDL_GetTicks() - held_since >= 2000) {
        fired = true;
        uwp_quit("View + Menu held");
    }
}

/*
 * The console suspends an app that leaves the screen and may end it without
 * another word. Write out what would be lost: the virtual hard disk's cached
 * blocks (save games) and the settings.
 */
static bool uwp_machine_running;

/*
 * Why the process ended, when it ends by itself: an abort() (failed assert)
 * or a plain exit() leaves no crash dump on the console, and the last lines
 * of a buffered stderr are lost with it.
 */
static void uwp_on_abort(int sig)
{
    /*
     * The assert text goes to a message box that an Xbox app cannot show, so
     * log where it happened instead: module + offset for every caller, to be
     * looked up in the unstripped exe (addr2line -e xemu_<ver>_unstripped.exe).
     */
    void *frames[40];
    USHORT n = RtlCaptureStackBackTrace(0, 40, frames, NULL);
    uwp_log("ABORT (signal %d): a check inside the emulator failed, thread %lu",
            sig, GetCurrentThreadId());
    for (USHORT i = 0; i < n; i++) {
        HMODULE mod = NULL;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &mod) && mod) {
            GetModuleFileNameA(mod, name, sizeof(name));
        }
        const char *base = strrchr(name, '\\');
        uwp_log("  #%u %s+0x%llx", i, base ? base + 1 : name,
                (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)mod));
    }
    fflush(NULL);
}

static void uwp_on_exit(void)
{
    uwp_log("exit() reached");
    fflush(NULL);
}

static void uwp_glib_print(const gchar *text)
{
    uwp_log("glib: %s", text);
}

/* How much memory the system still grants this app (the console ends an app that asks for more). */
unsigned int xemu_uwp_counters[UWP_CNT_MAX];

/*
 * The dashboard from the user's own disc. Xbox Live sign-up (and with it
 * online play through a replacement service) needs Microsoft's dashboard on
 * the hard disk, and the free hard disk image has none. Many Xbox Live games
 * carry its installer; when the game being started has one and the hard disk
 * has no dashboard, or an older one, it is unpacked onto C: before the
 * console boots (ui/xemu-uwp-dash.c). The link parameter dash=0 turns this off.
 */
static bool uwp_no_dash;

static int uwp_dash_blk_read(void *opaque, uint64_t offset, void *buf, size_t len)
{
    return blk_pread(opaque, offset, len, buf, 0) < 0;
}

static int uwp_dash_blk_write(void *opaque, uint64_t offset, const void *buf, size_t len)
{
    return blk_pwrite(opaque, offset, len, buf, 0) < 0;
}

void xemu_uwp_dashboard_from_disc(void)
{
    DriveInfo *hd = drive_get(IF_IDE, 0, 0), *cd = drive_get(IF_IDE, 0, 1);
    if (uwp_no_dash || !hd || !cd) {
        return;
    }
    BlockBackend *hdd_blk = blk_by_legacy_dinfo(hd), *disc_blk = blk_by_legacy_dinfo(cd);
    if (!hdd_blk || !disc_blk || !blk_is_inserted(disc_blk)) {
        return;
    }
    UwpDashIo hdd = { uwp_dash_blk_read, uwp_dash_blk_write, hdd_blk };
    UwpDashIo disc = { uwp_dash_blk_read, NULL, disc_blk };
    uint32_t have = 0, offer = 0;
    int installed = uwp_dash_installed(&hdd, &have);
    if (installed < 0) {
        uwp_log("dashboard: the hard disk has no Xbox C: partition, left alone");
        return;
    }
    if (!uwp_dash_on_disc(&disc, &offer)) {
        uwp_log("dashboard: %s on the hard disk; this disc carries no installer",
                installed ? "present" : "none");
        return;
    }
    if (installed && have >= offer) {
        uwp_log("dashboard: %08x on the hard disk, the disc offers %08x: nothing to do", have, offer);
        return;
    }
    uwp_log("dashboard: %s on the hard disk, installing %08x from the disc",
            installed ? "an older one" : "none", offer);
    if (uwp_dash_install(&disc, &hdd, uwp_log) > 0) {
        blk_flush(hdd_blk);
        xemu_queue_notification("Xbox dashboard installed from this disc");
    }
}

static void uwp_log_memory(void)
{
    typedef struct {
        ULONG64 AvailableCommit, PrivateCommitUsage, PeakPrivateCommitUsage, TotalCommitUsage;
    } UwpAppMemory;
    typedef BOOL (WINAPI *GetProcessInformation_fn)(HANDLE, int, LPVOID, DWORD);
    GetProcessInformation_fn fn = (GetProcessInformation_fn)
        xemu_uwp_kernelbase_proc("GetProcessInformation");
    UwpAppMemory m = { 0 };
    if (fn && fn(GetCurrentProcess(), 2 /* ProcessAppMemoryInfo */, &m, sizeof(m))) {
        uwp_log("memory: %llu MB in use, %llu MB more allowed, peak %llu MB",
                (unsigned long long)(m.PrivateCommitUsage >> 20),
                (unsigned long long)(m.AvailableCommit >> 20),
                (unsigned long long)(m.PeakPrivateCommitUsage >> 20));
    }
    {
        static unsigned int last[UWP_CNT_MAX];
        unsigned int *c = xemu_uwp_counters;
        uwp_log("gpu objects: queries waiting %u (read early %u), textures +%u -%u, surfaces +%u -%u, "
                "stream buffers %u, GL errors %u",
                c[UWP_CNT_QUERIES_PENDING], c[UWP_CNT_QUERIES_FOLDED] - last[UWP_CNT_QUERIES_FOLDED],
                c[UWP_CNT_TEX_NEW] - last[UWP_CNT_TEX_NEW], c[UWP_CNT_TEX_DEL] - last[UWP_CNT_TEX_DEL],
                c[UWP_CNT_SURF_NEW] - last[UWP_CNT_SURF_NEW], c[UWP_CNT_SURF_DEL] - last[UWP_CNT_SURF_DEL],
                c[UWP_CNT_STREAM_WRAP] - last[UWP_CNT_STREAM_WRAP], c[UWP_CNT_GL_ERRORS]);
        memcpy(last, c, sizeof(last));
    }
}

/*
 * The three system files as xemu.toml gave them (empty when there is no xemu.toml). What xemu
 * picks by itself at startup (Game Vault's folder, the free BIOS, a new blank disk) is never
 * written back: otherwise a path chosen once would stick, and a BIOS found later on the
 * player's USB drive, or a new package version with another install path, would be ignored.
 */
static char *uwp_toml_bootrom, *uwp_toml_bios, *uwp_toml_hdd;
static bool uwp_toml_remembered;

static void uwp_save_settings(void)
{
    if (!uwp_toml_remembered) {
        xemu_settings_save();
        return;
    }
    const char **slot[] = { &g_config.sys.files.bootrom_path,
                            &g_config.sys.files.flashrom_path,
                            &g_config.sys.files.hdd_path };
    char *toml[] = { uwp_toml_bootrom, uwp_toml_bios, uwp_toml_hdd };
    char *now[3];
    for (int i = 0; i < 3; i++) {
        now[i] = g_strdup(*slot[i] ? *slot[i] : "");
        xemu_settings_set_string(slot[i], toml[i]);
    }
    xemu_settings_save();
    for (int i = 0; i < 3; i++) {
        xemu_settings_set_string(slot[i], now[i]);
        g_free(now[i]);
    }
}

static void uwp_save_for_background(void)
{
    if (!uwp_machine_running) {
        return;
    }
    xemu_main_loop_lock();
    int ret = bdrv_flush_all();
    xemu_main_loop_unlock();
    uwp_save_settings();
    uwp_log("going to the background: disks flushed (%d), settings saved", ret);
}

/*
 * Runs inside SDL's event pump at the moment the system announces the
 * suspension (the polling loop would only see the event after the app has
 * been resumed, which may be never). No xemu lock is held here.
 */
static bool uwp_event_watch(void *userdata, SDL_Event *event)
{
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
        uwp_save_for_background();
    }
    return true;
}

static char *uwp_take_activation_disc(void)
{
    const char *hint = SDL_GetHint(UWP_ACTIVATION_HINT);
    if (!hint || !hint[0]) {
        return NULL;
    }
    char *uri = g_strdup(hint);
    SDL_SetHint(UWP_ACTIVATION_HINT, "");

    char *disc = NULL;
    char *query = strchr(uri, '?');
    for (char *p = query ? query + 1 : NULL; p && *p;) {
        char *end = strchr(p, '&');
        if (end) {
            *end = '\0';
        }
        if (!strcmp(p, "quit=1")) {
            uwp_quit("asked by link");
        }
        if (!strcmp(p, "state=1")) {
            uwp_load_state_wanted = true;
        }
        if (!strncmp(p, "press=", 6)) {
            uwp_press(p + 6);
        }
        if (!strcmp(p, "dash=0")) {
            uwp_no_dash = true;
        }
        if (!strcmp(p, "noflush=1")) {
            extern bool xemu_uwp_skip_cb_flush;
            xemu_uwp_skip_cb_flush = true;
            uwp_log("measuring: no TLB flush when a surface watch is added or removed");
        }
        if (!strcmp(p, "skipdraws=1")) {
            extern bool xemu_uwp_skip_draws;
            xemu_uwp_skip_draws = true;
            uwp_log("measuring: draws are skipped");
        }
        if (!strcmp(p, "state=save")) {
            uwp_save_state_wanted = true;
        }
        if (!strncmp(p, "disc=", 5)) {
            /* undo the %XX escaping; xemu.toml uses forward slashes */
            disc = g_malloc(strlen(p) + 1);
            char *out = disc;
            for (const char *in = p + 5; *in; in++) {
                if (in[0] == '%' && g_ascii_isxdigit(in[1]) &&
                    g_ascii_isxdigit(in[2])) {
                    *out = (char)(g_ascii_xdigit_value(in[1]) * 16 +
                                  g_ascii_xdigit_value(in[2]));
                    in += 2;
                } else {
                    *out = *in;
                }
                if (*out == '\\') {
                    *out = '/';
                }
                out++;
            }
            *out = '\0';
        }
        p = end ? end + 1 : NULL;
    }
    g_free(uri);
    if (disc && !disc[0]) {
        g_free(disc);
        disc = NULL;
    }
    return disc;
}

/* Before the machine is created: boot with the disc the launcher asked for. */
/*
 * Setting itself up. The launcher (Game Vault, same publisher) puts the boot
 * ROM, the BIOS and the hard disk image it found on the user's USB drive into
 * the publisher's shared folder "games\xbox-system". A path from xemu.toml
 * wins when its file exists (someone who set things up by hand); otherwise
 * the file from that folder, or one lying next to xemu.toml, is used, so no
 * xemu.toml has to be written or uploaded at all.
 */
static bool uwp_file_exists(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    wchar_t *w = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    HANDLE h = w ? xemu_uwp_create_file_w(w, GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          OPEN_EXISTING, 0)
                 : INVALID_HANDLE_VALUE;
    g_free(w);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
}

/* ...\Local\Packages\<name>_<publisher id>\LocalState\... -> ...\Local\Publishers\<publisher id>\games\xbox-system\ */
static char *uwp_system_folder(void)
{
    char *base = SDL_GetPrefPath("xemu", "xemu");
    char *result = NULL;
    for (char *c = base; c && *c; c++) {
        if (*c == '/') {
            *c = '\\';
        }
    }
    char *packages = base ? strstr(base, "\\Packages\\") : NULL;
    if (packages) {
        char *name = packages + strlen("\\Packages\\");
        char *end = strchr(name, '\\');
        char *id = NULL;
        for (char *c = name; end && c < end; c++) {
            if (*c == '_') {
                id = c + 1;
            }
        }
        if (id) {
            result = g_strdup_printf("%.*s\\Publishers\\%.*s\\games\\xbox-system\\",
                                     (int)(packages - base), base,
                                     (int)(end - id), id);
        }
    }
    SDL_free(base);
    return result;
}

static void uwp_set_path(const char **setting, const char *what, char *path)
{
    for (char *c = path; *c; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    uwp_log("%s: %s", what, path);
    xemu_settings_set_string(setting, path);
}

/* Keeps the setting when its file exists, else looks for the names in the two folders. */
static bool uwp_fill_one(const char **setting, const char *what,
                         const char *folder, const char *own,
                         const char *const *names)
{
    if (uwp_file_exists(*setting)) {
        return true;
    }
    const char *dirs[] = { folder, own };
    for (int d = 0; d < 2; d++) {
        for (int i = 0; dirs[d] && names[i]; i++) {
            char *path = g_strdup_printf("%s%s", dirs[d], names[i]);
            if (uwp_file_exists(path)) {
                uwp_set_path(setting, what, path);
                g_free(path);
                return true;
            }
            g_free(path);
        }
    }
    return false;
}

/*
 * A new, empty hard disk from the copy of the xemu project's copyright-free blank image
 * (github.com/xemu-project/xemu-hdd-image, release 1.0) that comes with the package. Made in
 * the publisher's folder when possible, so it survives reinstalling xemu.
 */
static bool uwp_new_hdd(const char *folder, const char *own, const char *name)
{
    char *blank = g_strdup_printf("%sxbox_hdd_blank.qcow2", SDL_GetBasePath());
    gchar *data = NULL;
    gsize size = 0;
    bool ok = false;
    if (!g_file_get_contents(blank, &data, &size, NULL) || size < 4 ||
        memcmp(data, "QFI\xfb", 4)) {
        uwp_log("hard disk: the blank image in the package is missing or damaged (%s)", blank);
    } else {
        const char *dirs[] = { folder, own };
        for (int d = 0; d < 2 && !ok; d++) {
            if (!dirs[d]) {
                continue;
            }
            g_mkdir_with_parents(dirs[d], 0755);
            char *path = g_strdup_printf("%s%s", dirs[d], name);
            GError *err = NULL;
            if (g_file_set_contents(path, data, size, &err)) {
                uwp_log("hard disk: none yet, made a new blank one");
                uwp_set_path(&g_config.sys.files.hdd_path, "hard disk", path);
                ok = true;
            } else {
                uwp_log("hard disk: could not make %s: %s", path, err->message);
                g_error_free(err);
            }
            g_free(path);
        }
    }
    g_free(data);
    g_free(blank);
    return ok;
}

/*
 * The free SpiritBoot BIOS shipped in the package (GPL-2.0, github.com/Mainkill1/SpiritBoot,
 * release v1.0.0, see spiritboot\README.txt next to xemu.exe). It boots without the MCPX boot
 * ROM. It cannot do Xbox Live / Insignia (no Microsoft keys), and its saves do not work with a
 * Microsoft BIOS and the other way round, which is why it gets a hard disk of its own.
 */
static bool uwp_use_free_bios(void)
{
    bool big = g_config.sys.mem_limit == CONFIG_SYS_MEM_LIMIT_128;
    char *path = g_strdup_printf("%sspiritboot\\SpiritBoot-v1.0.0-%s.bin",
                                 SDL_GetBasePath(), big ? "128MB" : "64MB");
    bool ok = uwp_file_exists(path);
    if (ok) {
        uwp_set_path(&g_config.sys.files.flashrom_path, "BIOS (the free SpiritBoot)", path);
        if (!uwp_file_exists(g_config.sys.files.bootrom_path)) {
            xemu_settings_set_string(&g_config.sys.files.bootrom_path, "");
        }
    } else {
        uwp_log("BIOS: the free SpiritBoot is missing from the package (%s)", path);
    }
    g_free(path);
    return ok;
}

/*
 * Which BIOS and hard disk to use, with nothing for the player to do:
 *  - the player's own files (from xemu.toml, or what Game Vault found on their USB drive and
 *    put in the publisher's folder): their BIOS with the boot ROM, their hard disk;
 *  - otherwise the free BIOS with its own hard disk, xbox_hdd_free.qcow2;
 *  - a hard disk that does not exist yet is made from the blank image.
 * xemu.toml can choose the free BIOS on purpose: flashrom_path = 'spiritboot'.
 */
static void uwp_fill_system_files(void)
{
    static const char *const mcpx[] = { "mcpx_1.0.bin", "mcpx.bin", NULL };
    static const char *const bios[] = { "bios.bin", "Complex_4627.bin", NULL };
    static const char *const own_hdd[] = { "xbox_hdd.qcow2", NULL };
    static const char *const free_hdd[] = { "xbox_hdd_free.qcow2", NULL };
    char *folder = uwp_system_folder();
    char *own = SDL_GetPrefPath("xemu", "xemu");

    uwp_toml_bootrom = g_strdup(g_config.sys.files.bootrom_path ? g_config.sys.files.bootrom_path : "");
    uwp_toml_bios = g_strdup(g_config.sys.files.flashrom_path ? g_config.sys.files.flashrom_path : "");
    uwp_toml_hdd = g_strdup(g_config.sys.files.hdd_path ? g_config.sys.files.hdd_path : "");
    uwp_toml_remembered = true;

    bool asked_free = !g_ascii_strcasecmp(uwp_toml_bios, "spiritboot");
    bool toml_bios = uwp_file_exists(uwp_toml_bios);
    bool have_mcpx = uwp_fill_one(&g_config.sys.files.bootrom_path, "boot ROM", folder, own, mcpx);
    bool own_bios = !asked_free &&
                    uwp_fill_one(&g_config.sys.files.flashrom_path, "BIOS", folder, own, bios);
    if (own_bios && !have_mcpx && !toml_bios) {
        /* a Microsoft BIOS does not start without the boot ROM */
        uwp_log("BIOS: found one, but no boot ROM (MCPX) to start it with");
        own_bios = false;
    }
    bool free_bios = !own_bios && uwp_use_free_bios();
    if (!own_bios && !free_bios) {
        uwp_log("BIOS: none (run the Xbox setup in Game Vault)");
    }
    uwp_log("Xbox BIOS in use: %s", own_bios ? "the player's own" : free_bios ? "free (offline only)" : "none");

    if (!uwp_file_exists(uwp_toml_hdd)) {
        const char *const *names = free_bios ? free_hdd : own_hdd;
        xemu_settings_set_string(&g_config.sys.files.hdd_path, "");
        if (!uwp_fill_one(&g_config.sys.files.hdd_path, "hard disk", folder, own, names)) {
            uwp_new_hdd(folder, own, names[0]);
        }
    }
    g_free(folder);
    SDL_free(own);

    /* what a console needs, whatever the settings file says (or if there is none) */
    g_config.general.show_welcome = false;
    g_config.general.updates.check = false;
    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_OPENGL;
    g_config.display.window.fullscreen_on_startup = true;
    /* Network on, through xemu's own NAT: what an online service such as Insignia needs, and the
     * only backend that works in an app container besides the UDP tunnel (no packet capture). */
    if (g_config.net.backend == CONFIG_NET_BACKEND_NAT) {
        g_config.net.enable = true;
    }
}

static void uwp_startup_disc(void)
{
    /* the activation arrives through the window's event queue */
    for (int i = 0; i < 200 && !SDL_GetHint(UWP_ACTIVATION_HINT); i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    char *disc = uwp_take_activation_disc();
    if (disc) {
        uwp_log("disc from launcher: %s", disc);
        xemu_settings_set_string(&g_config.sys.files.dvd_path, disc);
        g_free(disc);
    }
    uwp_fill_system_files();
}

/*
 * While running (the app was left open and the launcher starts a game): the
 * same disc just continues; another disc is swapped in and the console reset.
 */
static void uwp_poll_activation(void)
{
    char *disc = uwp_take_activation_disc();
    if (!disc) {
        return;
    }
    const char *current = g_config.sys.files.dvd_path;
    if (current && !g_ascii_strcasecmp(current, disc)) {
        uwp_log("launcher asked for the disc that is already running");
    } else {
        Error *err = NULL;
        xemu_main_loop_lock();
        xemu_load_disc(disc, &err);
        if (err) {
            uwp_log("could not load %s: %s", disc, error_get_pretty(err));
            error_free(err);
        } else {
            uwp_log("disc changed by launcher: %s", disc);
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        xemu_main_loop_unlock();
    }
    g_free(disc);
}
#endif

static void *qemu_main(void *opaque)
{
#ifdef XEMU_UWP
    uwp_log("qemu_init start");
#endif
    qemu_init(gArgc, gArgv);
#ifdef XEMU_UWP
    uwp_log("qemu_init done, entering main loop");
#endif
    exit_status = qemu_main_loop();
    qatomic_set(&qemu_exiting, true);
    bql_unlock();
    qemu_mutex_unlock_main_loop();

    qemu_sem_wait(&display_shutdown_sem);
    bql_lock();
    qemu_cleanup(exit_status);
    bql_unlock();

    return NULL;
}

#ifdef _WIN32
static const wchar_t *get_executable_name(void)
{
    static wchar_t exe_name[MAX_PATH] = { 0 };
    static bool initialized = false;

    if (!initialized) {
        wchar_t full_path[MAX_PATH];
        DWORD length = GetModuleFileNameW(NULL, full_path, MAX_PATH);
        if (length == 0 || length == MAX_PATH) {
            return NULL;
        }

        wchar_t *last_slash = wcsrchr(full_path, L'\\');
        if (last_slash) {
            wcsncpy_s(exe_name, MAX_PATH, last_slash + 1, _TRUNCATE);
        } else {
            wcsncpy_s(exe_name, MAX_PATH, full_path, _TRUNCATE);
        }

        initialized = true;
    }

    return exe_name;
}

static void setup_nvidia_profile(void)
{
    const wchar_t *exe_name = get_executable_name();
    if (exe_name == NULL) {
        fprintf(stderr, "Failed to get current executable name\n");
        return;
    }

    if (nvapi_init()) {
        nvapi_setup_profile((NvApiProfileOpts){
            .profile_name = L"xemu",
            .executable_name = exe_name,
            .threaded_optimization = false,
            .present_method = OGL_CPL_PREFER_DXPRESENT_PREFER_DISABLED,
        });
        nvapi_finalize();
    }
}
#endif

static void init_sdl_app_metadata(void)
{
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "xemu");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_VERSION_STRING,
                               xemu_version);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING,
                               "app.xemu.xemu");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_URL_STRING,
                               "https://xemu.app");
}

#ifdef XEMU_UWP
/*
 * Xbox (UWP) build: SDL's WinRT backend owns the app's CoreWindow, so the
 * program starts in WinMain and SDL_RunApp calls xemu's main on the UI thread.
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

static int xemu_uwp_main(int argc, char **argv);

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    static char *uwp_argv[] = { (char *)"xemu.exe", NULL };

    /* Windows.Gaming.Input is the only controller API that works in Xbox apps */
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    return SDL_RunApp(1, uwp_argv, xemu_uwp_main, NULL);
}

static int xemu_uwp_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    QemuThread thread;

    setlocale(LC_NUMERIC, "C");

#ifdef XEMU_UWP
    {
        /* The install folder is read-only: log next to xemu.toml in LocalState */
        init_sdl_app_metadata();
        char *base = SDL_GetPrefPath("xemu", "xemu");
        if (base) {
            char *log_path = g_strdup_printf("%sxemu.log", base);
            /* append mode for both streams so they can't overwrite each other */
            remove(log_path);
            freopen(log_path, "a", stdout);
            freopen(log_path, "a", stderr);
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
            g_free(log_path);
            SDL_free(base);
        }
    }
#elif defined(_WIN32)
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // Launched with a console. If stdout and stderr are not associated with
        // an output stream, redirect to parent console.
        if (_fileno(stdout) == -2) {
            freopen("CONOUT$", "w+", stdout);
        }
        if (_fileno(stderr) == -2) {
            freopen("CONOUT$", "w+", stderr);
        }
    } else {
        // Launched without a console. Redirect stdout and stderr to a log file.
        HANDLE logfile = CreateFileA("xemu.log",
            GENERIC_WRITE, FILE_SHARE_WRITE|FILE_SHARE_READ,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (logfile != INVALID_HANDLE_VALUE) {
            freopen("xemu.log", "a", stdout);
            freopen("xemu.log", "a", stderr);
        }
    }

    _set_error_mode(_OUT_TO_STDERR);
#endif

    fprintf(stderr, "xemu_version: %s\n", xemu_version);
    fprintf(stderr, "xemu_commit: %s\n", xemu_commit);
    fprintf(stderr, "xemu_date: %s\n", xemu_date);

    init_sdl_app_metadata();

    gArgc = argc;
    gArgv = argv;

    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "-config_path") == 0) {
            argv[i] = NULL;
            if (i < argc - 1 && argv[i+1]) {
                xemu_settings_set_path(argv[i+1]);
                argv[i+1] = NULL;
            }
            break;
        }
    }

    if (!xemu_settings_load()) {
        const char *err_msg = xemu_settings_get_error_message();
        fprintf(stderr, "%s", err_msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Failed to load xemu config file", err_msg,
            m_window);
        SDL_Quit();
        exit(1);
    }
#ifdef XEMU_UWP
    atexit(uwp_save_settings);
#else
    atexit(xemu_settings_save);
#endif

#ifdef _WIN32
    if (g_config.display.setup_nvidia_profile) {
        setup_nvidia_profile();
    }
#endif

    display_very_early_init(NULL);
#ifdef XEMU_UWP
    uwp_log("display_very_early_init done");
    uwp_startup_disc();
#endif

#ifdef XEMU_UWP
    /* List disc images on the drives (USB sticks show up as E:, F:, ...) */
    {
        DWORD mask = GetLogicalDrives();
        for (char d = 'D'; d <= 'Z'; d++) {
            if (!(mask & (1u << (d - 'A')))) {
                continue;
            }
            const char *patterns[] = { "%c:\\*.iso", "%c:\\*\\*.iso" };
            for (int p = 0; p < 2; p++) {
                char pat[32];
                snprintf(pat, sizeof(pat), patterns[p], d);
                if (p == 1) {
                    /* one folder level down */
                    char root[8];
                    snprintf(root, sizeof(root), "%c:\\*", d);
                    WIN32_FIND_DATAA dir;
                    HANDLE hd = FindFirstFileA(root, &dir);
                    if (hd == INVALID_HANDLE_VALUE) {
                        continue;
                    }
                    do {
                        if (!(dir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                            dir.cFileName[0] == '.') {
                            continue;
                        }
                        char sub[MAX_PATH];
                        snprintf(sub, sizeof(sub), "%c:\\%s\\*.iso", d, dir.cFileName);
                        WIN32_FIND_DATAA f;
                        HANDLE hf = FindFirstFileA(sub, &f);
                        if (hf != INVALID_HANDLE_VALUE) {
                            do {
                                uwp_log("iso: %c:\\%s\\%s (%llu MB)", d, dir.cFileName, f.cFileName,
                                        (((unsigned long long)f.nFileSizeHigh << 32) | f.nFileSizeLow) >> 20);
                            } while (FindNextFileA(hf, &f));
                            FindClose(hf);
                        }
                    } while (FindNextFileA(hd, &dir));
                    FindClose(hd);
                    continue;
                }
                WIN32_FIND_DATAA f;
                HANDLE hf = FindFirstFileA(pat, &f);
                if (hf != INVALID_HANDLE_VALUE) {
                    do {
                        uwp_log("iso: %c:\\%s (%llu MB)", d, f.cFileName,
                                (((unsigned long long)f.nFileSizeHigh << 32) | f.nFileSizeLow) >> 20);
                    } while (FindNextFileA(hf, &f));
                    FindClose(hf);
                }
            }
        }
    }

    /*
     * Audio: SDL3-uwp's WASAPI device open never completes on the Xbox, so
     * the APU's stream is played through XAudio2 instead (xemu-uwp-audio.c).
     * The QEMU thread picks the stream up in mcpx_apu_monitor_init().
     */
    xemu_uwp_audio_stream = xemu_uwp_audio_start();
#endif
    qemu_sem_init(&display_init_sem, 0);
    qemu_sem_init(&display_shutdown_sem, 0);
    qemu_thread_create(&thread, "qemu_main", qemu_main,
                       NULL, QEMU_THREAD_JOINABLE);
#ifdef XEMU_UWP
    while (qemu_sem_timedwait(&display_init_sem, 10) != 0) {
        SDL_PumpEvents();
    }
#else
    qemu_sem_wait(&display_init_sem);
#endif
#ifdef XEMU_UWP
    uwp_log("display_init done, UI thread continuing");
#endif

    gui_grab = 0;
    if (gui_fullscreen) {
        grab_start(0);
        set_full_screen(&scon_list[0], gui_fullscreen);
    }

    /*
     * FIXME: May want to create a callback mechanism for main QEMU thread
     * to just run functions to avoid TLS bugs and locking issues.
     */
    tcg_register_init_ctx();
    qemu_set_current_aio_context(qemu_get_aio_context());

    xemu_main_loop_lock();
    xemu_input_init();
    xemu_main_loop_unlock();

    struct xemu_console *scon = &scon_list[0];
#ifdef XEMU_UWP
    uwp_log("UI loop starting");
    uwp_machine_running = true;
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGABRT, uwp_on_abort);
    atexit(uwp_on_exit);
    g_set_printerr_handler(uwp_glib_print);
    uwp_log_memory();
    SDL_AddEventWatch(uwp_event_watch, NULL);
    UWP_PROF_THREAD("ui");
    CloseHandle(CreateThread(NULL, 0, uwp_sampler_thread, (void *)"vcpu", 0,
                             NULL));
    CloseHandle(CreateThread(NULL, 0, uwp_sampler_thread, (void *)"pfifo", 0,
                             NULL));
    uint64_t uwp_frames = 0, uwp_next_report = SDL_GetTicks() + 3000;
    uint64_t uwp_started_ms = SDL_GetTicks();
#endif
    while (!qatomic_read(&qemu_exiting)) {
        poll_events(scon);
        gl_render_frame(scon);
#ifdef XEMU_UWP
        uwp_frames++;
        uwp_poll_activation();
        if ((uwp_frames & 7) == 0) {
            uwp_poll_quit_combo();
        }
        uwp_press_poll();
        /*
         * Asked for by the launch link. Not before the game has had 45 seconds: a state
         * loaded while the console is still booting can leave the audio processor (DSP)
         * running a program that is not there (Halo 2: assertion in read_memory_p).
         */
        if (uwp_load_state_wanted && SDL_GetTicks() > uwp_started_ms + 45000 &&
            runstate_is_running()) {
            uwp_load_state_wanted = false;
            uwp_state(false);
        }
        if (uwp_save_state_wanted && runstate_is_running()) {
            uwp_save_state_wanted = false;
            uwp_state(true);
        }
        if (SDL_GetTicks() >= uwp_next_report) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(scon->real_window, &w, &h);
            int pads = 0;
            SDL_free(SDL_GetGamepads(&pads));
            ControllerState *p1 = xemu_input_get_bound(0);
            uwp_log("%llu UI frames, window %dx%d, runstate %s, game fps %d, "
                    "gamepads %d, port1 %s",
                    (unsigned long long)uwp_frames, w, h,
                    RunState_str(runstate_get()), g_nv2a_stats.increment_fps,
                    pads, p1 ? p1->name : "(none)");
            uwp_prof_report((SDL_GetTicks() - (uwp_next_report - 3000)) / 1000.0);
            if ((uwp_frames / 180) % 3 == 0) {
                uwp_log_memory();
            }
            {
                extern unsigned int xemu_uwp_prim_hist[16];
                static const char *const names[16] = { "points", "lines", "lineloop", "linestrip", "tris",
                    "tristrip", "trifan", "quads", "quadstrip", "polygon", "lines_adj", "linestrip_adj",
                    "tris_adj", "tristrip_adj", "patches", "other" };
                char line[400] = "";
                for (int i = 0; i < 16; i++) {
                    if (xemu_uwp_prim_hist[i]) {
                        g_snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s %u", names[i], xemu_uwp_prim_hist[i]);
                        xemu_uwp_prim_hist[i] = 0;
                    }
                }
                uwp_log("draw primitives:%s", line);
            }
            uwp_next_report = SDL_GetTicks() + 3000;
        }
#endif
    }
    qemu_sem_post(&display_shutdown_sem);
    qemu_thread_join(&thread);
    display_finalize();
    return exit_status;
}

void xemu_eject_disc(Error **errp)
{
    Error *error = NULL;

    xbox_smc_eject_button();
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");

    // Xbox software may request that the drive open, but do it now anyway
    qmp_eject("ide0-cd1", NULL, true, false, &error);
    if (error) {
        error_propagate(errp, error);
    }

    xbox_smc_update_tray_state();
}

#ifdef XEMU_UWP
/*
 * A full dump of an Xbox disc (7.3 GB) has the game partition 0x18300000
 * bytes in, after the DVD-Video part; the console's kernel wants the drive to
 * start at the game partition, which is what an XISO file is. Instead of
 * making the user convert dumps on a PC, look where the partition starts and
 * let the block layer skip to it. Returns the file name to open: the path
 * itself, or a json: description of the same file read from that offset.
 * Free with g_free().
 */
char *xemu_uwp_disc_filename(const char *path);
char *xemu_uwp_disc_filename(const char *path)
{
    static const char magic[] = "MICROSOFT*XBOX*MEDIA";
    static const uint64_t bases[] = { 0, 0x18300000ull, 0x0FD90000ull,
                                      0x02080000ull };
    uint64_t found = 0;

    if (!path || !path[0] || !strncmp(path, "json:", 5)) {
        return g_strdup(path ? path : "");
    }
    /* Opened like the block layer opens it, so a FAT32 / exFAT USB drive works. */
    wchar_t *wpath = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    HANDLE h = wpath ? xemu_uwp_create_file_w(wpath, GENERIC_READ,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                                              OPEN_EXISTING, 0)
                     : INVALID_HANDLE_VALUE;
    g_free(wpath);
    if (h != INVALID_HANDLE_VALUE) {
        for (int i = 0; i < ARRAY_SIZE(bases); i++) {
            char head[20];
            LARGE_INTEGER pos;
            DWORD got = 0;
            pos.QuadPart = bases[i] + 0x10000;
            if (SetFilePointerEx(h, pos, NULL, FILE_BEGIN) &&
                ReadFile(h, head, sizeof(head), &got, NULL) &&
                got == sizeof(head) && !memcmp(head, magic, sizeof(head))) {
                found = bases[i];
                break;
            }
        }
        CloseHandle(h);
    } else {
        uwp_log("disc: cannot open %s (error %lu)", path, GetLastError());
    }
    if (!found) {
        return g_strdup(path);
    }
    uwp_log("full disc dump: game partition at 0x%llx", (unsigned long long)found);
    GString *name = g_string_new(NULL);
    g_string_printf(name, "json:{\"driver\":\"raw\",\"offset\":%llu,\"file\":"
                    "{\"driver\":\"file\",\"filename\":\"",
                    (unsigned long long)found);
    for (const char *c = path; *c; c++) {
        if (*c == '"' || *c == '\\') {
            g_string_append_c(name, '\\');
        }
        g_string_append_c(name, *c);
    }
    g_string_append(name, "\"}}");
    return g_string_free(name, FALSE);
}
#endif

void xemu_load_disc(const char *path, Error **errp)
{
    Error *error = NULL;

    // Ensure an eject sequence is always triggered so Xbox software reloads
    xbox_smc_eject_button();
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");

#ifdef XEMU_UWP
    char *filename = xemu_uwp_disc_filename(path);
    qmp_blockdev_change_medium("ide0-cd1", NULL, filename, "raw", false, false,
                               false, 0, &error);
    g_free(filename);
#else
    qmp_blockdev_change_medium("ide0-cd1", NULL, path, "raw", false, false,
                               false, 0, &error);
#endif
    if (error) {
        error_propagate(errp, error);
    } else {
        xemu_settings_set_string(&g_config.sys.files.dvd_path, path);
    }

    xbox_smc_update_tray_state();
}
