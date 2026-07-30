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
typedef void (*canvas_enum_scenes_fn)(obs_canvas_t *canvas,
	bool (*enum_proc)(void *, obs_source_t *), void *param);
typedef obs_source_t *(*canvas_get_channel_fn)(obs_canvas_t *canvas, uint32_t channel);
typedef obs_canvas_t *(*source_get_canvas_fn)(const obs_source_t *source);
typedef void (*canvas_release_fn)(obs_canvas_t *canvas);
typedef void (*enum_all_sources_fn)(bool (*enum_proc)(void *, obs_source_t *), void *param);
typedef bool (*obj_is_private_fn)(void *obj);

struct canvas_api {
	canvas_enum_all_fn enum_canvases;
	canvas_get_name_fn get_name;
	canvas_get_uuid_fn get_uuid;
	canvas_enum_scenes_fn enum_scenes;
	canvas_get_channel_fn get_channel;
	source_get_canvas_fn source_get_canvas;
	canvas_release_fn release;
	enum_all_sources_fn enum_all_sources;
	obj_is_private_fn obj_is_private;
};

static struct canvas_api g_api;

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
	g_api.get_name = (canvas_get_name_fn)lookup_symbol("obs_canvas_get_name");
	g_api.get_uuid = (canvas_get_uuid_fn)lookup_symbol("obs_canvas_get_uuid");
	g_api.enum_scenes = (canvas_enum_scenes_fn)lookup_symbol("obs_canvas_enum_scenes");
	g_api.get_channel = (canvas_get_channel_fn)lookup_symbol("obs_canvas_get_channel");
	g_api.source_get_canvas = (source_get_canvas_fn)lookup_symbol("obs_source_get_canvas");
	g_api.release = (canvas_release_fn)lookup_symbol("obs_canvas_release");
	g_api.enum_all_sources = (enum_all_sources_fn)lookup_symbol("obs_enum_all_sources");
	g_api.obj_is_private = (obj_is_private_fn)lookup_symbol("obs_obj_is_private");

	if (!g_api.enum_canvases || !g_api.get_name || !g_api.get_uuid ||
	    !g_api.enum_scenes || !g_api.get_channel || !g_api.release ||
	    !g_api.enum_all_sources) {
		obs_log(LOG_ERROR,
			"[SELiveCanvasBridge] Required OBS diagnostic APIs are unavailable.");
		return false;
	}

	obs_log(LOG_INFO,
		"[SELiveCanvasBridge] OBS source-graph diagnostic API loaded (source_get_canvas=%s, private_flag=%s).",
		g_api.source_get_canvas ? "yes" : "no",
		g_api.obj_is_private ? "yes" : "no");
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

static const char *source_type_name(obs_source_t *source)
{
	if (!source)
		return "null";

	switch (obs_source_get_type(source)) {
	case OBS_SOURCE_TYPE_INPUT:
		return "input";
	case OBS_SOURCE_TYPE_FILTER:
		return "filter";
	case OBS_SOURCE_TYPE_TRANSITION:
		return "transition";
	case OBS_SOURCE_TYPE_SCENE:
		return "scene";
	default:
		return "unknown";
	}
}

static void get_source_canvas_text(obs_source_t *source, char *buffer, size_t size)
{
	obs_canvas_t *canvas;
	const char *name;
	const char *uuid;

	if (!buffer || !size)
		return;
	buffer[0] = '\0';

	if (!source || !g_api.source_get_canvas) {
		strncpy(buffer, "unavailable", size - 1);
		buffer[size - 1] = '\0';
		return;
	}

	canvas = g_api.source_get_canvas(source);
	if (!canvas) {
		strncpy(buffer, "none", size - 1);
		buffer[size - 1] = '\0';
		return;
	}

	name = g_api.get_name(canvas);
	uuid = g_api.get_uuid(canvas);
	snprintf(buffer, size, "%s [%s]", name ? name : "(unnamed)",
		uuid ? uuid : "no-uuid");
	g_api.release(canvas);
}

static void log_source_line(const char *prefix, obs_source_t *source, unsigned int depth)
{
	char canvas_text[512];
	const char *name;
	const char *id;
	const char *uuid;
	bool is_private = false;
	char indent[32];
	unsigned int spaces = depth * 2;

	if (spaces >= sizeof(indent))
		spaces = sizeof(indent) - 1;
	memset(indent, ' ', spaces);
	indent[spaces] = '\0';

	if (!source) {
		obs_log(LOG_INFO, "[SELiveCanvasBridge] %s%s: (null)", indent,
			prefix ? prefix : "source");
		return;
	}

	name = obs_source_get_name(source);
	id = obs_source_get_id(source);
	uuid = obs_source_get_uuid(source);
	if (g_api.obj_is_private)
		is_private = g_api.obj_is_private(source);
	get_source_canvas_text(source, canvas_text, sizeof(canvas_text));

	obs_log(LOG_INFO,
		"[SELiveCanvasBridge] %s%s: name='%s', id='%s', type=%s, uuid='%s', private=%s, canvas=%s",
		indent, prefix ? prefix : "source", name ? name : "(unnamed)",
		id ? id : "(no-id)", source_type_name(source),
		uuid ? uuid : "(no-uuid)", is_private ? "yes" : "no", canvas_text);
}

static bool same_source(obs_source_t *a, obs_source_t *b)
{
	const char *ua;
	const char *ub;
	if (!a || !b)
		return false;
	if (a == b)
		return true;
	ua = obs_source_get_uuid(a);
	ub = obs_source_get_uuid(b);
	return ua && ub && strcmp(ua, ub) == 0;
}

static void dump_transition_tree(obs_source_t *source, unsigned int depth, const char *edge)
{
	obs_source_t *active = NULL;
	obs_source_t *a = NULL;
	obs_source_t *b = NULL;

	log_source_line(edge ? edge : "node", source, depth);
	if (!source || depth >= 6 ||
	    obs_source_get_type(source) != OBS_SOURCE_TYPE_TRANSITION)
		return;

	active = obs_transition_get_active_source(source);
	a = obs_transition_get_source(source, OBS_TRANSITION_SOURCE_A);
	b = obs_transition_get_source(source, OBS_TRANSITION_SOURCE_B);

	if (active)
		dump_transition_tree(active, depth + 1, "active");
	if (a && !same_source(a, active))
		dump_transition_tree(a, depth + 1, "A");
	if (b && !same_source(b, active) && !same_source(b, a))
		dump_transition_tree(b, depth + 1, "B");

	if (active)
		obs_source_release(active);
	if (a)
		obs_source_release(a);
	if (b)
		obs_source_release(b);
}

struct canvas_scene_context {
	unsigned int count;
};

static bool canvas_scene_callback(void *data, obs_source_t *source)
{
	struct canvas_scene_context *context = data;
	context->count++;
	log_source_line("canvas scene", source, 1);
	return true;
}

struct canvas_dump_context {
	unsigned int count;
};

static bool canvas_dump_callback(void *data, obs_canvas_t *canvas)
{
	struct canvas_dump_context *context = data;
	struct canvas_scene_context scenes;
	const char *name = g_api.get_name(canvas);
	const char *uuid = g_api.get_uuid(canvas);
	uint32_t channel;

	context->count++;
	memset(&scenes, 0, sizeof(scenes));
	obs_log(LOG_INFO, "[SELiveCanvasBridge] Public canvas %u: %s [uuid=%s]",
		context->count, name ? name : "(unnamed)", uuid ? uuid : "(no-uuid)");

	g_api.enum_scenes(canvas, canvas_scene_callback, &scenes);
	obs_log(LOG_INFO, "[SELiveCanvasBridge]   Enumerated scenes: %u", scenes.count);

	for (channel = 0; channel < 8; channel++) {
		obs_source_t *root = g_api.get_channel(canvas, channel);
		if (!root)
			continue;
		obs_log(LOG_INFO, "[SELiveCanvasBridge]   Channel %u transition graph:", channel);
		dump_transition_tree(root, 2, "root");
		obs_source_release(root);
	}
	return true;
}

struct source_dump_context {
	unsigned int total;
	unsigned int logged;
	unsigned int scenes;
	unsigned int transitions;
	unsigned int private_sources;
};

static bool all_source_callback(void *data, obs_source_t *source)
{
	struct source_dump_context *context = data;
	const char *name = obs_source_get_name(source);
	const char *id = obs_source_get_id(source);
	const enum obs_source_type type = obs_source_get_type(source);
	bool is_private = g_api.obj_is_private ? g_api.obj_is_private(source) : false;
	bool interesting;

	context->total++;
	if (type == OBS_SOURCE_TYPE_SCENE)
		context->scenes++;
	if (type == OBS_SOURCE_TYPE_TRANSITION)
		context->transitions++;
	if (is_private)
		context->private_sources++;

	interesting = type == OBS_SOURCE_TYPE_SCENE ||
		type == OBS_SOURCE_TYPE_TRANSITION || is_private ||
		contains_ci(name, "se.live") || contains_ci(name, "vertical") ||
		contains_ci(id, "streamelements");

	if (!interesting)
		return true;

	context->logged++;
	log_source_line("global source", source, 1);
	if (type == OBS_SOURCE_TYPE_TRANSITION)
		dump_transition_tree(source, 2, "transition graph");
	return true;
}

static void dump_source_graph(void)
{
	struct canvas_dump_context canvases;
	struct source_dump_context sources;

	memset(&canvases, 0, sizeof(canvases));
	memset(&sources, 0, sizeof(sources));

	obs_log(LOG_INFO,
		"[SELiveCanvasBridge] ===== Source graph diagnostic v0.5 start =====");
	obs_log(LOG_INFO, "[SELiveCanvasBridge] --- Public canvases and channels ---");
	g_api.enum_canvases(canvas_dump_callback, &canvases);
	obs_log(LOG_INFO, "[SELiveCanvasBridge] Public canvases total: %u", canvases.count);

	obs_log(LOG_INFO,
		"[SELiveCanvasBridge] --- Global/private scenes and transitions ---");
	g_api.enum_all_sources(all_source_callback, &sources);
	obs_log(LOG_INFO,
		"[SELiveCanvasBridge] All-source totals: total=%u, logged=%u, scenes=%u, transitions=%u, private=%u",
		sources.total, sources.logged, sources.scenes, sources.transitions,
		sources.private_sources);
	obs_log(LOG_INFO,
		"[SELiveCanvasBridge] ===== Source graph diagnostic v0.5 end =====");
}

static void tools_dump_callback(void *unused)
{
	(void)unused;
	dump_source_graph();
}

const char *obs_module_description(void)
{
	return "Read-only diagnostic bridge for SE.Live vertical source graphs.";
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "[SELiveCanvasBridge] Loading diagnostic version %s on OBS %s.",
		PLUGIN_VERSION, obs_get_version_string());

	if (!load_api())
		return true;

	obs_frontend_add_tools_menu_item("SE.Live Bridge: Dump source graph (v0.5)",
		tools_dump_callback, NULL);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "[SELiveCanvasBridge] Diagnostic plugin unloaded.");
}
