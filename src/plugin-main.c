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
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* OBS 31 build dependencies do not declare the OBS 32 Canvas API. */
struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR("Ian Arango")

typedef void (*canvas_enum_all_fn)(bool (*enum_proc)(void *, obs_canvas_t *), void *param);
typedef const char *(*canvas_get_name_fn)(const obs_canvas_t *canvas);
typedef const char *(*canvas_get_uuid_fn)(const obs_canvas_t *canvas);
typedef obs_source_t *(*canvas_get_channel_fn)(obs_canvas_t *canvas, uint32_t channel);
typedef obs_canvas_t *(*source_get_canvas_fn)(const obs_source_t *source);
typedef void (*canvas_release_fn)(obs_canvas_t *canvas);
typedef void (*enum_all_sources_fn)(bool (*enum_proc)(void *, obs_source_t *), void *param);
typedef bool (*obj_is_private_fn)(void *obj);

struct bridge_api {
	canvas_enum_all_fn enum_canvases;
	canvas_get_name_fn get_canvas_name;
	canvas_get_uuid_fn get_canvas_uuid;
	canvas_get_channel_fn get_canvas_channel;
	source_get_canvas_fn source_get_canvas;
	canvas_release_fn release_canvas;
	enum_all_sources_fn enum_all_sources;
	obj_is_private_fn obj_is_private;
};

static struct bridge_api g_api;
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

static bool load_api(void)
{
	g_api.enum_canvases = (canvas_enum_all_fn)lookup_symbol("obs_enum_canvases");
	g_api.get_canvas_name = (canvas_get_name_fn)lookup_symbol("obs_canvas_get_name");
	g_api.get_canvas_uuid = (canvas_get_uuid_fn)lookup_symbol("obs_canvas_get_uuid");
	g_api.get_canvas_channel = (canvas_get_channel_fn)lookup_symbol("obs_canvas_get_channel");
	g_api.source_get_canvas = (source_get_canvas_fn)lookup_symbol("obs_source_get_canvas");
	g_api.release_canvas = (canvas_release_fn)lookup_symbol("obs_canvas_release");
	g_api.enum_all_sources = (enum_all_sources_fn)lookup_symbol("obs_enum_all_sources");
	g_api.obj_is_private = (obj_is_private_fn)lookup_symbol("obs_obj_is_private");

	if (!g_api.enum_canvases || !g_api.get_canvas_name ||
	    !g_api.get_canvas_channel || !g_api.release_canvas ||
	    !g_api.enum_all_sources || !g_api.obj_is_private) {
		obs_log(LOG_ERROR,
			"[SELiveCanvasBridge] Required OBS 32 source-graph APIs are unavailable.");
		return false;
	}

	obs_log(LOG_INFO,
		"[SELiveCanvasBridge] OBS private-source bridge API loaded successfully.");
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

/* Compare names while ignoring case, spaces, dashes, and punctuation. */
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

static bool source_has_public_canvas(obs_source_t *source)
{
	obs_canvas_t *canvas;

	if (!source || !g_api.source_get_canvas)
		return false;

	canvas = g_api.source_get_canvas(source);
	if (!canvas)
		return false;

	g_api.release_canvas(canvas);
	return true;
}

struct vertical_transition_context {
	obs_source_t *transition;
	char canvas_name[256];
};

static bool vertical_canvas_callback(void *data, obs_canvas_t *canvas)
{
	struct vertical_transition_context *context = data;
	const char *canvas_name = g_api.get_canvas_name(canvas);
	obs_source_t *root = NULL;
	obs_source_t *active = NULL;

	if (!contains_ci(canvas_name, "vertical"))
		return true;

	root = g_api.get_canvas_channel(canvas, 0);
	if (!root)
		return true;

	if (obs_source_get_type(root) == OBS_SOURCE_TYPE_TRANSITION)
		active = obs_transition_get_active_source(root);

	/* Current SE.Live graph: Vertical root transition -> inner transition -> scene. */
	if (active && obs_source_get_type(active) == OBS_SOURCE_TYPE_TRANSITION) {
		context->transition = active;
		active = NULL;
	} else if (obs_source_get_type(root) == OBS_SOURCE_TYPE_TRANSITION &&
		   contains_ci(obs_source_get_name(root), "vertical: transition")) {
		context->transition = obs_source_get_ref(root);
	}

	if (canvas_name) {
		strncpy(context->canvas_name, canvas_name,
			sizeof(context->canvas_name) - 1);
		context->canvas_name[sizeof(context->canvas_name) - 1] = '\0';
	}

	if (active)
		obs_source_release(active);
	obs_source_release(root);

	return context->transition == NULL;
}

static obs_source_t *get_vertical_transition(char *canvas_name, size_t canvas_name_size)
{
	struct vertical_transition_context context;

	memset(&context, 0, sizeof(context));
	g_api.enum_canvases(vertical_canvas_callback, &context);

	if (canvas_name && canvas_name_size) {
		strncpy(canvas_name, context.canvas_name, canvas_name_size - 1);
		canvas_name[canvas_name_size - 1] = '\0';
	}

	return context.transition;
}

struct private_scene_match_context {
	const char *target_name;
	obs_source_t *match;
	unsigned int matches;
};

static bool private_scene_match_callback(void *data, obs_source_t *source)
{
	struct private_scene_match_context *context = data;
	const char *name;

	if (!source || obs_source_get_type(source) != OBS_SOURCE_TYPE_SCENE)
		return true;
	if (!g_api.obj_is_private(source))
		return true;
	if (source_has_public_canvas(source))
		return true;

	name = obs_source_get_name(source);
	if (!scene_names_equal(name, context->target_name))
		return true;

	context->matches++;
	if (!context->match)
		context->match = obs_source_get_ref(source);
	return true;
}

static obs_source_t *find_private_scene(const char *target_name, unsigned int *matches)
{
	struct private_scene_match_context context;

	memset(&context, 0, sizeof(context));
	context.target_name = target_name;
	g_api.enum_all_sources(private_scene_match_callback, &context);

	if (matches)
		*matches = context.matches;
	return context.match;
}

struct private_scene_list_context {
	unsigned int count;
};

static bool private_scene_list_callback(void *data, obs_source_t *source)
{
	struct private_scene_list_context *context = data;
	const char *name;
	const char *uuid;

	if (!source || obs_source_get_type(source) != OBS_SOURCE_TYPE_SCENE)
		return true;
	if (!g_api.obj_is_private(source))
		return true;
	if (source_has_public_canvas(source))
		return true;

	name = obs_source_get_name(source);
	uuid = obs_source_get_uuid(source);
	context->count++;
	obs_log(LOG_INFO,
		"[SELiveCanvasBridge]   Private scene %u: '%s' [uuid=%s]",
		context->count, name ? name : "(unnamed)", uuid ? uuid : "(no-uuid)");
	return true;
}

static void dump_vertical_mapping(void)
{
	struct private_scene_list_context scenes;
	obs_source_t *transition;
	obs_source_t *active;
	char canvas_name[256];

	memset(&scenes, 0, sizeof(scenes));
	memset(canvas_name, 0, sizeof(canvas_name));

	obs_log(LOG_INFO,
		"[SELiveCanvasBridge] ===== Vertical mapping diagnostic v0.6 start =====");

	transition = get_vertical_transition(canvas_name, sizeof(canvas_name));
	if (!transition) {
		obs_log(LOG_WARNING,
			"[SELiveCanvasBridge] Vertical inner transition was not found.");
	} else {
		obs_log(LOG_INFO,
			"[SELiveCanvasBridge] Canvas='%s', transition='%s' [uuid=%s]",
			canvas_name[0] ? canvas_name : "(unknown)",
			obs_source_get_name(transition) ? obs_source_get_name(transition) : "(unnamed)",
			obs_source_get_uuid(transition) ? obs_source_get_uuid(transition) : "(no-uuid)");
		active = obs_transition_get_active_source(transition);
		if (active) {
			obs_log(LOG_INFO,
				"[SELiveCanvasBridge] Active vertical scene='%s' [uuid=%s]",
				obs_source_get_name(active) ? obs_source_get_name(active) : "(unnamed)",
				obs_source_get_uuid(active) ? obs_source_get_uuid(active) : "(no-uuid)");
			obs_source_release(active);
		}
		obs_source_release(transition);
	}

	g_api.enum_all_sources(private_scene_list_callback, &scenes);
	obs_log(LOG_INFO, "[SELiveCanvasBridge] Private vertical candidates total: %u",
		scenes.count);
	obs_log(LOG_INFO,
		"[SELiveCanvasBridge] ===== Vertical mapping diagnostic v0.6 end =====");
}

static bool switch_vertical_to(const char *horizontal_name, const char *reason)
{
	obs_source_t *transition = NULL;
	obs_source_t *target = NULL;
	obs_source_t *active = NULL;
	unsigned int matches = 0;
	char canvas_name[256];
	bool changed = false;

	memset(canvas_name, 0, sizeof(canvas_name));
	transition = get_vertical_transition(canvas_name, sizeof(canvas_name));
	if (!transition) {
		obs_log(LOG_WARNING,
			"[SELiveCanvasBridge] Sync failed (%s): vertical inner transition not found.",
			reason ? reason : "unknown");
		return false;
	}

	target = find_private_scene(horizontal_name, &matches);
	if (!target) {
		obs_log(LOG_WARNING,
			"[SELiveCanvasBridge] No private vertical scene matches horizontal scene '%s'.",
			horizontal_name ? horizontal_name : "(unnamed)");
		obs_source_release(transition);
		return false;
	}

	if (matches > 1) {
		obs_log(LOG_WARNING,
			"[SELiveCanvasBridge] %u private scenes match '%s'; using the first one.",
			matches, horizontal_name);
	}

	active = obs_transition_get_active_source(transition);
	if (active && active == target) {
		obs_log(LOG_INFO,
			"[SELiveCanvasBridge] Vertical scene already matches '%s'.",
			obs_source_get_name(target));
	} else {
		obs_transition_set(transition, target);
		changed = true;
		obs_log(LOG_INFO,
			"[SELiveCanvasBridge] Vertical canvas '%s' switched to private scene '%s' (%s).",
			canvas_name[0] ? canvas_name : "(unknown)",
			obs_source_get_name(target) ? obs_source_get_name(target) : "(unnamed)",
			reason ? reason : "unknown");
	}

	if (active)
		obs_source_release(active);
	obs_source_release(target);
	obs_source_release(transition);
	return changed;
}

static void sync_current_scene(const char *reason)
{
	obs_source_t *current_scene;
	const char *scene_name;

	if (!g_frontend_loaded)
		return;

	current_scene = obs_frontend_get_current_scene();
	if (!current_scene) {
		obs_log(LOG_WARNING,
			"[SELiveCanvasBridge] Cannot read the current horizontal scene.");
		return;
	}

	scene_name = obs_source_get_name(current_scene);
	obs_log(LOG_INFO,
		"[SELiveCanvasBridge] Sync requested (%s): horizontal='%s'.",
		reason ? reason : "unknown", scene_name ? scene_name : "(unnamed)");

	if (scene_name)
		switch_vertical_to(scene_name, reason);

	obs_source_release(current_scene);
}

static void queued_sync(void *unused)
{
	(void)unused;
	sync_current_scene("delayed retry");
}

static void schedule_retries(void)
{
	g_retries_left = 3;
	g_retry_elapsed = 0.0f;
}

static void tick_callback(void *unused, float seconds)
{
	(void)unused;

	if (!g_frontend_loaded || g_retries_left <= 0)
		return;

	g_retry_elapsed += seconds;
	if (g_retry_elapsed < 0.30f)
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
		dump_vertical_mapping();
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
	g_frontend_loaded = true;
	sync_current_scene("manual tools menu");
	schedule_retries();
}

static void tools_dump_callback(void *unused)
{
	(void)unused;
	dump_vertical_mapping();
}

const char *obs_module_description(void)
{
	return "Synchronizes horizontal OBS scenes with matching private SE.Live vertical scenes.";
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "[SELiveCanvasBridge] Loading experimental version %s on OBS %s.",
		PLUGIN_VERSION, obs_get_version_string());

	if (!load_api())
		return true;

	obs_frontend_add_event_callback(frontend_event_callback, NULL);
	obs_frontend_add_tools_menu_item(
		"SE.Live Bridge: Sync private vertical now (v0.6)",
		tools_sync_callback, NULL);
	obs_frontend_add_tools_menu_item(
		"SE.Live Bridge: Dump vertical mapping (v0.6)",
		tools_dump_callback, NULL);
	obs_add_tick_callback(tick_callback, NULL);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event_callback, NULL);
	obs_remove_tick_callback(tick_callback, NULL);
	obs_log(LOG_INFO, "[SELiveCanvasBridge] Experimental v0.6 plugin unloaded.");
}
