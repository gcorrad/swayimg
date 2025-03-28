// SPDX-License-Identifier: MIT
// Image viewer application: main loop and event handler.
// Copyright (C) 2024 Artem Senichev <artemsen@gmail.com>

#include "application.h"

#include "array.h"
#include "buildcfg.h"
#include "font.h"
#include "gallery.h"
#include "imagelist.h"
#include "info.h"
#include "loader.h"
#include "shellcmd.h"
#include "sway.h"
#include "ui.h"
#include "viewer.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Special ids for windows size and position
#define SIZE_FULLSCREEN  SIZE_MAX
#define SIZE_FROM_IMAGE  (SIZE_MAX - 1)
#define SIZE_FROM_PARENT (SIZE_MAX - 2)
#define POS_FROM_PARENT  SSIZE_MAX

/** Main loop state */
enum loop_state {
    loop_run,
    loop_stop,
    loop_error,
};

/** File descriptor and its handler. */
struct watchfd {
    int fd;
    void* data;
    fd_callback callback;
};

/* Application event queue (list). */
struct event_queue {
    struct list list;
    struct event event;
};

/** Application context */
struct application {
    enum loop_state state; ///< Main loop state

    struct watchfd* wfds; ///< FD polling descriptors
    size_t wfds_num;      ///< Number of polling FD

    struct event_queue* events;  ///< Event queue
    pthread_mutex_t events_lock; ///< Event queue lock
    int event_signal;            ///< Queue change notification

    struct action_seq sigusr1; ///< Actions applied by USR1 signal
    struct action_seq sigusr2; ///< Actions applied by USR2 signal

    event_handler ehandler; ///< Event handler for the current mode
    struct wndrect window;  ///< Preferable window position and size
    bool wnd_decor;         //< Window decoration: borders and title
    char* app_id;           ///< Application id (app_id name)
};

/** Global application context. */
static struct application ctx;

/**
 * Setup window position via Sway IPC.
 * @param cfg config instance
 */
static void sway_setup(const struct config* cfg)
{
    struct wndrect parent;
    bool fullscreen;
    int border;
    int ipc;

    ipc = sway_connect();
    if (ipc == INVALID_SWAY_IPC) {
        return; // sway not available
    }
    if (!sway_current(ipc, &parent, &border, &fullscreen)) {
        sway_disconnect(ipc);
        return;
    }

    if (fullscreen) {
        ctx.window.width = SIZE_FULLSCREEN;
        ctx.window.height = SIZE_FULLSCREEN;
        sway_disconnect(ipc);
        return;
    }

    if (ctx.window.width == SIZE_FROM_PARENT) {
        ctx.window.width = parent.width;
        ctx.window.height = parent.height;
        if (config_get_bool(cfg, CFG_GENERAL, CFG_GNRL_DECOR)) {
            ctx.window.width -= border * 2;
            ctx.window.height -= border * 2;
        }
    }
    if (ctx.window.x == POS_FROM_PARENT) {
        ctx.window.x = parent.x;
        ctx.window.y = parent.y;
    }

    // set window position via sway rules
    sway_add_rules(ipc, ctx.window.x, ctx.window.y);

    sway_disconnect(ipc);
}

/**
 * Apply common action.
 * @param action pointer to the action being performed
 * @return true if action handled, false if it's not a common action
 */
static bool apply_common_action(const struct action* action)
{
    bool handled = true;

    switch (action->type) {
        case action_info:
            info_switch(action->params);
            app_redraw();
            break;
        case action_status:
            info_update(info_status, "%s", action->params);
            app_redraw();
            break;
        case action_fullscreen:
            ui_toggle_fullscreen();
            break;
        case action_help:
            info_switch_help();
            app_redraw();
            break;
        case action_exit:
            if (info_help_active()) {
                info_switch_help(); // remove help overlay
                app_redraw();
            } else {
                app_exit(0);
            }
            break;
        default:
            handled = false;
            break;
    }

    return handled;
}

/** Notification callback: handle event queue. */
static void handle_event_queue(__attribute__((unused)) void* data)
{
    notification_reset(ctx.event_signal);

    while (ctx.events && ctx.state == loop_run) {
        struct event_queue* entry = NULL;
        pthread_mutex_lock(&ctx.events_lock);
        if (ctx.events) {
            entry = ctx.events;
            ctx.events = list_remove(entry);
        }
        pthread_mutex_unlock(&ctx.events_lock);
        if (entry) {
            if (entry->event.type != event_action ||
                !apply_common_action(entry->event.param.action)) {
                ctx.ehandler(&entry->event);
            }
            free(entry);
        }
    }
}

/**
 * Append event to queue.
 * @param event pointer to the event
 */
static void append_event(const struct event* event)
{
    struct event_queue* entry;

    // create new entry
    entry = malloc(sizeof(*entry));
    if (!entry) {
        return;
    }
    memcpy(&entry->event, event, sizeof(entry->event));

    // add to queue tail
    pthread_mutex_lock(&ctx.events_lock);
    ctx.events = list_append(ctx.events, entry);
    pthread_mutex_unlock(&ctx.events_lock);

    notification_raise(ctx.event_signal);
}

/**
 * POSIX Signal handler.
 * @param signum signal number
 */
static void on_signal(int signum)
{
    const struct action_seq* sigact;

    switch (signum) {
        case SIGUSR1:
            sigact = &ctx.sigusr1;
            break;
        case SIGUSR2:
            sigact = &ctx.sigusr2;
            break;
        default:
            return;
    }

    for (size_t i = 0; i < sigact->num; ++i) {
        const struct event event = {
            .type = event_action,
            .param.action = &sigact->sequence[i],
        };
        append_event(&event);
    }
}

/**
 * Load first (initial) image.
 * @param index initial index of image in the image list
 * @param force mandatory image index flag
 * @return image instance or NULL on errors
 */
static struct image* load_first_file(size_t index, bool force)
{
    struct image* img = NULL;
    enum loader_status status = ldr_ioerror;

    if (index == IMGLIST_INVALID) {
        index = image_list_first();
        force = false;
    }

    while (index != IMGLIST_INVALID) {
        status = loader_from_index(index, &img);
        if (force || status == ldr_success) {
            break;
        }
        index = image_list_skip(index);
    }

    if (status != ldr_success) {
        // print error message
        if (!force) {
            fprintf(stderr, "No image files was loaded, exit\n");
        } else {
            const char* reason = "Unknown error";
            switch (status) {
                case ldr_success:
                    break;
                case ldr_unsupported:
                    reason = "Unsupported format";
                    break;
                case ldr_fmterror:
                    reason = "Invalid format";
                    break;
                case ldr_ioerror:
                    reason = "I/O error";
                    break;
            }
            fprintf(stderr, "%s: %s\n", image_list_get(index), reason);
        }
    }

    return img;
}

/**
 * Load config.
 * @param cfg config instance
 */
static void load_config(const struct config* cfg)
{
    const char* value;

    // startup mode
    static const char* modes[] = { CFG_MODE_VIEWER, CFG_MODE_GALLERY };
    if (config_get_oneof(cfg, CFG_GENERAL, CFG_GNRL_MODE, modes,
                         ARRAY_SIZE(modes)) == 1) {
        ctx.ehandler = gallery_handle;
    } else {
        ctx.ehandler = viewer_handle;
    }

    // initial window position
    ctx.window.x = POS_FROM_PARENT;
    ctx.window.y = POS_FROM_PARENT;
    value = config_get(cfg, CFG_GENERAL, CFG_GNRL_POSITION);
    if (strcmp(value, CFG_FROM_PARENT) != 0) {
        struct str_slice slices[2];
        ssize_t x, y;
        if (str_split(value, ',', slices, 2) == 2 &&
            str_to_num(slices[0].value, slices[0].len, &x, 0) &&
            str_to_num(slices[1].value, slices[1].len, &y, 0)) {
            ctx.window.x = (ssize_t)x;
            ctx.window.y = (ssize_t)y;
        } else {
            config_error_val(CFG_GENERAL, CFG_GNRL_POSITION);
        }
    }

    // initial window size
    value = config_get(cfg, CFG_GENERAL, CFG_GNRL_SIZE);
    if (strcmp(value, CFG_FROM_PARENT) == 0) {
        ctx.window.width = SIZE_FROM_PARENT;
        ctx.window.height = SIZE_FROM_PARENT;
    } else if (strcmp(value, CFG_FROM_IMAGE) == 0) {
        ctx.window.width = SIZE_FROM_IMAGE;
        ctx.window.height = SIZE_FROM_IMAGE;
    } else if (strcmp(value, CFG_FULLSCREEN) == 0) {
        ctx.window.width = SIZE_FULLSCREEN;
        ctx.window.height = SIZE_FULLSCREEN;
    } else {
        ssize_t width, height;
        struct str_slice slices[2];
        if (str_split(value, ',', slices, 2) == 2 &&
            str_to_num(slices[0].value, slices[0].len, &width, 0) &&
            str_to_num(slices[1].value, slices[1].len, &height, 0) &&
            width > 0 && width < 100000 && height > 0 && height < 100000) {
            ctx.window.width = width;
            ctx.window.height = height;
        } else {
            ctx.window.width = SIZE_FROM_PARENT;
            ctx.window.height = SIZE_FROM_PARENT;
            config_error_val(CFG_GENERAL, CFG_GNRL_SIZE);
        }
    }

    ctx.wnd_decor = config_get_bool(cfg, CFG_GENERAL, CFG_GNRL_DECOR);

    // signal actions
    value = config_get(cfg, CFG_GENERAL, CFG_GNRL_SIGUSR1);
    if (!action_create(value, &ctx.sigusr1)) {
        config_error_val(CFG_GENERAL, CFG_GNRL_SIGUSR1);
        value = config_get_default(CFG_GENERAL, CFG_GNRL_SIGUSR1);
        action_create(value, &ctx.sigusr1);
    }
    value = config_get(cfg, CFG_GENERAL, CFG_GNRL_SIGUSR2);
    if (!action_create(value, &ctx.sigusr2)) {
        config_error_val(CFG_GENERAL, CFG_GNRL_SIGUSR2);
        value = config_get_default(CFG_GENERAL, CFG_GNRL_SIGUSR2);
        action_create(value, &ctx.sigusr2);
    }

    // app id
    value = config_get(cfg, CFG_GENERAL, CFG_GNRL_APP_ID);
    if (!*value) {
        config_error_val(CFG_GENERAL, CFG_GNRL_APP_ID);
        value = config_get_default(CFG_GENERAL, CFG_GNRL_APP_ID);
    }
    str_dup(value, &ctx.app_id);
}

bool app_init(const struct config* cfg, const char** sources, size_t num)
{
    bool force_load = false;
    struct image* first_image;
    struct sigaction sigact;

    load_config(cfg);

    // compose image list
    if (num == 0) {
        // no input files specified, use all from the current directory
        static const char* current_dir = ".";
        sources = &current_dir;
        num = 1;
    } else if (num == 1) {
        force_load = true;
        if (strcmp(sources[0], "-") == 0) {
            // load from stdin
            static const char* stdin_name = LDRSRC_STDIN;
            sources = &stdin_name;
        }
    }
    image_list_init(cfg);
    for (size_t i = 0; i < num; ++i) {
        image_list_add(sources[i]);
    }
    if (image_list_size() == 0) {
        if (force_load) {
            fprintf(stderr, "%s: Unable to open\n", sources[0]);
        } else {
            fprintf(stderr, "No image files found to view, exit\n");
        }
        return false;
    }
    image_list_reorder();

    // load the first image
    first_image = load_first_file(image_list_find(sources[0]), force_load);
    if (!first_image) {
        return false;
    }

    // setup window position and size
    if (ctx.window.width != SIZE_FULLSCREEN) {
        sway_setup(cfg); // try Sway integration
    }
    if (ctx.window.width == SIZE_FULLSCREEN) {
        ui_toggle_fullscreen();
    } else if (ctx.window.width == SIZE_FROM_IMAGE ||
               ctx.window.width == SIZE_FROM_PARENT) {
        // determine window size from the first image
        const struct pixmap* pm = &first_image->frames[0].pm;
        ctx.window.width = pm->width;
        ctx.window.height = pm->height;
    }

    // connect to wayland
    if (!ui_init(ctx.app_id, ctx.window.width, ctx.window.height,
                 ctx.wnd_decor)) {
        return false;
    }

    // create event queue notification
    ctx.event_signal = notification_create();
    if (ctx.event_signal != -1) {
        app_watch(ctx.event_signal, handle_event_queue, NULL);
    } else {
        perror("Unable to create eventfd");
        return false;
    }
    pthread_mutex_init(&ctx.events_lock, NULL);

    // initialize other subsystems
    font_init(cfg);
    keybind_init(cfg);
    info_init(cfg);
    loader_init();
    viewer_init(cfg, ctx.ehandler == viewer_handle ? first_image : NULL);
    gallery_init(cfg, ctx.ehandler == gallery_handle ? first_image : NULL);

    // set mode for info
    if (info_enabled()) {
        info_switch(ctx.ehandler == viewer_handle ? CFG_MODE_VIEWER
                                                  : CFG_MODE_GALLERY);
    }

    // set signal handler
    sigact.sa_handler = on_signal;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGUSR1, &sigact, NULL);
    sigaction(SIGUSR2, &sigact, NULL);

    return true;
}

void app_destroy(void)
{
    loader_destroy();
    gallery_destroy();
    viewer_destroy();
    ui_destroy();
    image_list_destroy();
    info_destroy();
    keybind_destroy();
    font_destroy();

    for (size_t i = 0; i < ctx.wfds_num; ++i) {
        close(ctx.wfds[i].fd);
    }
    free(ctx.wfds);

    list_for_each(ctx.events, struct event_queue, it) {
        if (it->event.type == event_load) {
            image_free(it->event.param.load.image);
        }
        free(it);
    }
    if (ctx.event_signal != -1) {
        notification_free(ctx.event_signal);
    }
    pthread_mutex_destroy(&ctx.events_lock);

    action_free(&ctx.sigusr1);
    action_free(&ctx.sigusr2);
}

void app_watch(int fd, fd_callback cb, void* data)
{
    const size_t sz = (ctx.wfds_num + 1) * sizeof(*ctx.wfds);
    struct watchfd* handlers = realloc(ctx.wfds, sz);
    if (handlers) {
        ctx.wfds = handlers;
        ctx.wfds[ctx.wfds_num].fd = fd;
        ctx.wfds[ctx.wfds_num].data = data;
        ctx.wfds[ctx.wfds_num].callback = cb;
        ++ctx.wfds_num;
    }
}

bool app_run(void)
{
    struct pollfd* fds;

    // file descriptors to poll
    fds = calloc(1, ctx.wfds_num * sizeof(struct pollfd));
    if (!fds) {
        perror("Failed to allocate memory");
        return false;
    }
    for (size_t i = 0; i < ctx.wfds_num; ++i) {
        fds[i].fd = ctx.wfds[i].fd;
        fds[i].events = POLLIN;
    }

    // main event loop
    ctx.state = loop_run;
    while (ctx.state == loop_run) {
        ui_event_prepare();

        // poll events
        if (poll(fds, ctx.wfds_num, -1) < 0) {
            if (errno != EINTR) {
                perror("Error polling events");
                ctx.state = loop_error;
                break;
            }
        }

        // call handlers for each active event
        for (size_t i = 0; ctx.state == loop_run && i < ctx.wfds_num; ++i) {
            if (fds[i].revents & POLLIN) {
                ctx.wfds[i].callback(ctx.wfds[i].data);
            }
        }

        ui_event_done();
    }

    free(fds);

    return ctx.state != loop_error;
}

void app_exit(int rc)
{
    ctx.state = rc ? loop_error : loop_stop;
}

void app_switch_mode(size_t index)
{
    const char* info_mode;
    const struct event event = {
        .type = event_activate,
        .param.activate.index = index,
    };

    if (ctx.ehandler == viewer_handle) {
        ctx.ehandler = gallery_handle;
        info_mode = CFG_MODE_GALLERY;
    } else {
        ctx.ehandler = viewer_handle;
        info_mode = CFG_MODE_VIEWER;
    }

    ctx.ehandler(&event);

    if (info_enabled()) {
        info_switch(info_mode);
    }
    if (info_help_active()) {
        info_switch_help();
    }

    app_redraw();
}

bool app_is_viewer(void)
{
    return ctx.ehandler == viewer_handle;
}

void app_reload(void)
{
    static const struct action action = { .type = action_reload };
    const struct event event = {
        .type = event_action,
        .param.action = &action,
    };
    append_event(&event);
}

void app_redraw(void)
{
    const struct event event = {
        .type = event_redraw,
    };

    // remove the same event to append new one to tail
    pthread_mutex_lock(&ctx.events_lock);
    list_for_each(ctx.events, struct event_queue, it) {
        if (it->event.type == event_redraw) {
            if (list_is_last(it)) {
                pthread_mutex_unlock(&ctx.events_lock);
                return;
            }
            ctx.events = list_remove(it);
            free(it);
            break;
        }
    }
    pthread_mutex_unlock(&ctx.events_lock);

    append_event(&event);
}

void app_on_resize(void)
{
    const struct event event = {
        .type = event_resize,
    };
    append_event(&event);
}

void app_on_keyboard(xkb_keysym_t key, uint8_t mods)
{
    const struct keybind* kb = keybind_find(key, mods);

    if (kb) {
        for (size_t i = 0; i < kb->actions.num; ++i) {
            const struct event event = {
                .type = event_action,
                .param.action = &kb->actions.sequence[i],
            };
            append_event(&event);
        }
    } else {
        char* name = keybind_name(key, mods);
        if (name) {
            info_update(info_status, "Key %s is not bound", name);
            free(name);
            app_redraw();
        }
    }
}

void app_on_drag(int dx, int dy)
{
    const struct event event = { .type = event_drag,
                                 .param.drag.dx = dx,
                                 .param.drag.dy = dy };

    // merge with existing event
    pthread_mutex_lock(&ctx.events_lock);
    list_for_each(ctx.events, struct event_queue, it) {
        if (it->event.type == event_drag) {
            it->event.param.drag.dx += dx;
            it->event.param.drag.dy += dy;
            pthread_mutex_unlock(&ctx.events_lock);
            return;
        }
    }
    pthread_mutex_unlock(&ctx.events_lock);

    append_event(&event);
}

void app_on_load(struct image* image, size_t index)
{
    const struct event event = {
        .type = event_load,
        .param.load.image = image,
        .param.load.index = index,
    };
    append_event(&event);
}

void app_execute(const char* expr, const char* path)
{
    int rc;
    char* out = NULL;

    rc = shellcmd_expr(expr, path, &out);

    if (out) {
        // duplicate output to stdout
        printf("%s", out);

        // trim long output text
        const size_t max_len = 60;
        if (strlen(out) > max_len) {
            const char* ellipsis = "...";
            const size_t ellipsis_len = strlen(ellipsis) + 1;
            memcpy(&out[max_len - ellipsis_len], ellipsis, ellipsis_len);
        }
    }

    // show execution status
    if (rc) {
        info_update(info_status, "Error %d: %s", rc, out ? out : strerror(rc));
    } else if (out) {
        info_update(info_status, "%s", out);
    } else {
        info_update(info_status, "Success: %s", expr);
    }

    free(out);

    app_redraw();
}
