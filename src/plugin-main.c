/*
 * SE.Live Canvas Bridge
 * Copyright (C) 2026 Ian Arango
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR("Ian Arango")

/*
 * The template builds against OBS 31.1.1, while the canvas functions used
 * here are resolved at runtime from OBS 32. This keeps GitHub Actions simple
 * and prevents link errors against the older template SDK.
 */
typedef const char *(*canvas_get_name_fn)(const obs_canvas_t *canvas);
typedef void (*canvas_enum_scenes_fn)(obs_canvas_t *canvas,
                                      bool (*enum_proc)(void *, obs_source_t *),
                                      void *param);
typedef void (*canvas_set_channel_fn)(obs_canvas_t *canvas, uint32_t channel,
                                      obs_source_t *source);

struct canvas_api {
    canvas_get_name_fn get_name;
    canvas_enum_scenes_fn enum_scenes;
    canvas_set_channel_fn set_channel;
};

static struct canvas_api g_canvas_api;
static bool g_frontend_loaded;
static int g_retries_left;
static float g_retry_elapsed;

static void *lookup_symbol(const char *name)
{
#ifdef _WIN32
    HMODULE module = GetModuleHandleW(L"obs.dll");
    if (!module)
        module = GetModuleHandleW(L"libobs.dll");
    return module ? (void *)GetProcAddress(module, name) : NULL;
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
}

static bool load_canvas_api(void)
{
    g_canvas_api.get_name =
        (canvas_get_name_fn)lookup_symbol("obs_canvas_get_name");
    g_canvas_api.enum_scenes =
        (canvas_enum_scenes_fn)lookup_symbol("obs_canvas_enum_scenes");
    g_canvas_api.set_channel =
        (canvas_set_channel_fn)lookup_symbol("obs_canvas_set_channel");

    if (!g_canvas_api.get_name || !g_canvas_api.enum_scenes ||
        !g_canvas_api.set_channel) {
        obs_log(LOG_ERROR,
                "[SELiveCanvasBridge] OBS canvas API unavailable. "
                "This build requires OBS 32.x.");
        return false;
    }

    obs_log(LOG_INFO,
            "[SELiveCanvasBridge] OBS canvas API loaded successfully.");
    return true;
}

static bool contains_ci(const char *text, const char *needle)
{
    if (!text || !needle || !*needle)
        return false;

    for (; *text; text++) {
        const char *a = text;
        const char *b = needle;
        while (*a && *b &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b)
            return true;
    }
    return false;
}

/* Compare scene names ignoring case, spaces, dashes and punctuation. */
static bool scene_names_equal(const char *a, const char *b)
{
    if (!a || !b)
        return false;

    for (;;) {
        while (*a && !isalnum((unsigned char)*a))
            a++;
        while (*b && !isalnum((unsigned char)*b))
            b++;

        if (!*a || !*b)
            break;

        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;

        a++;
        b++;
    }

    while (*a && !isalnum((unsigned char)*a))
        a++;
    while (*b && !isalnum((unsigned char)*b))
        b++;

    return *a == '\0' && *b == '\0';
}

struct dump_scene_context {
    const char *canvas_name;
    unsigned int count;
};

static bool dump_scene_callback(void *data, obs_source_t *source)
{
    struct dump_scene_context *context = data;
    const char *name = obs_source_get_name(source);
    context->count++;
    obs_log(LOG_INFO, "[SELiveCanvasBridge]   Scene %u: %s",
            context->count, name ? name : "(unnamed)");
    return true;
}

static bool dump_canvas_callback(void *data, obs_canvas_t *canvas)
{
    unsigned int *canvas_count = data;
    const char *name = g_canvas_api.get_name(canvas);
    struct dump_scene_context context = {
        .canvas_name = name,
        .count = 0,
    };

    (*canvas_count)++;
    obs_log(LOG_INFO, "[SELiveCanvasBridge] Canvas %u: %s", *canvas_count,
            name ? name : "(unnamed)");
    g_canvas_api.enum_scenes(canvas, dump_scene_callback, &context);
    obs_log(LOG_INFO, "[SELiveCanvasBridge]   Total scenes: %u",
            context.count);
    return true;
}

static void dump_canvases(void)
{
    unsigned int count = 0;

    if (!g_canvas_api.get_name)
        return;

    obs_log(LOG_INFO,
            "[SELiveCanvasBridge] ===== Canvas diagnostic start =====");
    obs_enum_canvases(dump_canvas_callback, &count);
    obs_log(LOG_INFO,
            "[SELiveCanvasBridge] ===== Canvas diagnostic end (%u canvases) =====",
            count);
}

struct scene_match_context {
    obs_canvas_t *canvas;
    const char *canvas_name;
    const char *target_scene;
    bool matched;
};

static bool scene_match_callback(void *data, obs_source_t *source)
{
    struct scene_match_context *context = data;
    const char *scene_name = obs_source_get_name(source);

    if (!scene_names_equal(scene_name, context->target_scene))
        return true;

    /* Channel 0 is the program/output channel for the canvas. */
    g_canvas_api.set_channel(context->canvas, 0, source);
    context->matched = true;

    obs_log(LOG_INFO,
            "[SELiveCanvasBridge] Vertical canvas '%s' switched to '%s'.",
            context->canvas_name ? context->canvas_name : "(unnamed)",
            scene_name ? scene_name : "(unnamed)");
    return false;
}

struct canvas_match_context {
    const char *target_scene;
    bool require_streamelements_name;
    bool found_canvas;
    bool matched_scene;
};

static bool canvas_match_callback(void *data, obs_canvas_t *canvas)
{
    struct canvas_match_context *context = data;
    const char *canvas_name = g_canvas_api.get_name(canvas);
    const bool is_vertical = contains_ci(canvas_name, "vertical");
    const bool is_streamelements = contains_ci(canvas_name, "se.live") ||
                                   contains_ci(canvas_name, "streamelements");

    if (!is_vertical)
        return true;
    if (context->require_streamelements_name && !is_streamelements)
        return true;

    context->found_canvas = true;

    struct scene_match_context scene_context = {
        .canvas = canvas,
        .canvas_name = canvas_name,
        .target_scene = context->target_scene,
        .matched = false,
    };

    g_canvas_api.enum_scenes(canvas, scene_match_callback, &scene_context);
    context->matched_scene = scene_context.matched;

    if (!scene_context.matched) {
        obs_log(LOG_WARNING,
                "[SELiveCanvasBridge] Canvas '%s' found, but no vertical "
                "scene matched horizontal scene '%s'.",
                canvas_name ? canvas_name : "(unnamed)",
                context->target_scene);
    }

    return false;
}

static bool switch_vertical_to(const char *scene_name)
{
    struct canvas_match_context preferred = {
        .target_scene = scene_name,
        .require_streamelements_name = true,
        .found_canvas = false,
        .matched_scene = false,
    };

    obs_enum_canvases(canvas_match_callback, &preferred);
    if (preferred.matched_scene)
        return true;

    /* Fallback for builds where SE.Live exposes a simpler canvas name. */
    if (!preferred.found_canvas) {
        struct canvas_match_context fallback = {
            .target_scene = scene_name,
            .require_streamelements_name = false,
            .found_canvas = false,
            .matched_scene = false,
        };
        obs_enum_canvases(canvas_match_callback, &fallback);
        if (fallback.matched_scene)
            return true;
        if (!fallback.found_canvas)
            obs_log(LOG_WARNING,
                    "[SELiveCanvasBridge] No vertical canvas was found.");
    }

    return false;
}

static void sync_current_scene(const char *reason)
{
    obs_source_t *current_scene;
    const char *scene_name;

    if (!g_frontend_loaded || !g_canvas_api.set_channel)
        return;

    current_scene = obs_frontend_get_current_scene();
    if (!current_scene) {
        obs_log(LOG_WARNING,
                "[SELiveCanvasBridge] Cannot read current horizontal scene.");
        return;
    }

    scene_name = obs_source_get_name(current_scene);
    obs_log(LOG_INFO,
            "[SELiveCanvasBridge] Sync requested (%s): horizontal='%s'.",
            reason ? reason : "unknown", scene_name ? scene_name : "(unnamed)");

    if (scene_name)
        switch_vertical_to(scene_name);

    obs_source_release(current_scene);
}

static void queued_sync(void *unused)
{
    (void)unused;
    sync_current_scene("delayed retry");
}

static void schedule_retries(void)
{
    g_retries_left = 4;
    g_retry_elapsed = 0.0f;
}

static void tick_callback(void *unused, float seconds)
{
    (void)unused;

    if (!g_frontend_loaded || g_retries_left <= 0)
        return;

    g_retry_elapsed += seconds;
    if (g_retry_elapsed < 0.25f)
        return;

    g_retry_elapsed = 0.0f;
    g_retries_left--;
    obs_queue_task(OBS_TASK_UI, queued_sync, NULL, false);
}

static void frontend_event_callback(enum obs_frontend_event event, void *unused)
{
    (void)unused;

    switch (event) {
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
        g_frontend_loaded = true;
        dump_canvases();
        sync_current_scene("OBS finished loading");
        schedule_retries();
        break;

    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
        sync_current_scene("horizontal scene changed");
        schedule_retries();
        break;

    default:
        break;
    }
}

static void tools_sync_callback(void *unused)
{
    (void)unused;
    sync_current_scene("manual tools menu");
    schedule_retries();
}

static void tools_dump_callback(void *unused)
{
    (void)unused;
    dump_canvases();
}

const char *obs_module_description(void)
{
    return "Synchronizes the main OBS scene with a matching SE.Live vertical canvas scene.";
}

bool obs_module_load(void)
{
    obs_log(LOG_INFO,
            "[SELiveCanvasBridge] Loading version %s on OBS %s.",
            PLUGIN_VERSION, obs_get_version_string());

    if (!load_canvas_api())
        return true;

    obs_frontend_add_event_callback(frontend_event_callback, NULL);
    obs_frontend_add_tools_menu_item("SE.Live Bridge: Sync vertical now",
                                     tools_sync_callback, NULL);
    obs_frontend_add_tools_menu_item("SE.Live Bridge: Dump canvases to log",
                                     tools_dump_callback, NULL);
    obs_add_tick_callback(tick_callback, NULL);

    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(frontend_event_callback, NULL);
    obs_remove_tick_callback(tick_callback, NULL);
    obs_log(LOG_INFO, "[SELiveCanvasBridge] Plugin unloaded.");
}
