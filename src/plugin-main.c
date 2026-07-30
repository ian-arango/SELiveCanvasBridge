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

/*
 * The OBS 31 SDK used by the template does not declare the Canvas API.
 * Resolve OBS 32 Canvas functions at runtime so GitHub Actions can keep
 * using the template's prebuilt OBS 31 dependencies.
 */
struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR("Ian Arango")

typedef void (*canvas_enum_all_fn)(bool (*enum_proc)(void *, obs_canvas_t *), void *param);
typedef const char *(*canvas_get_name_fn)(const obs_canvas_t *canvas);
typedef const char *(*canvas_get_uuid_fn)(const obs_canvas_t *canvas);
typedef void (*canvas_enum_scenes_fn)(obs_canvas_t *canvas, bool (*enum_proc)(void *, obs_source_t *), void *param);
typedef void (*canvas_set_channel_fn)(obs_canvas_t *canvas, uint32_t channel, obs_source_t *source);
typedef obs_source_t *(*canvas_get_channel_fn)(obs_canvas_t *canvas, uint32_t channel);
typedef obs_canvas_t *(*source_get_canvas_fn)(const obs_source_t *source);
typedef void (*canvas_release_fn)(obs_canvas_t *canvas);

struct canvas_api {
	canvas_enum_all_fn enum_all;
	canvas_get_name_fn get_name;
	canvas_get_uuid_fn get_uuid;
	canvas_enum_scenes_fn enum_scenes;
	canvas_set_channel_fn set_channel;
	canvas_get_channel_fn get_channel;
	source_get_canvas_fn source_get_canvas;
	canvas_release_fn release;
};

struct vertical_graph {
	obs_source_t *shared_root;
	obs_source_t *control_transition;
	obs_source_t *current_scene_source;
	obs_canvas_t *private_canvas;
	char shared_canvas_name[256];
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
	g_canvas_api.enum_all = (canvas_enum_all_fn)lookup_symbol("obs_enum_canvases");
	g_canvas_api.get_name = (canvas_get_name_fn)lookup_symbol("obs_canvas_get_name");
	g_canvas_api.get_uuid = (canvas_get_uuid_fn)lookup_symbol("obs_canvas_get_uuid");
	g_canvas_api.enum_scenes = (canvas_enum_scenes_fn)lookup_symbol("obs_canvas_enum_scenes");
	g_canvas_api.set_channel = (canvas_set_channel_fn)lookup_symbol("obs_canvas_set_channel");
	g_canvas_api.get_channel = (canvas_get_channel_fn)lookup_symbol("obs_canvas_get_channel");
	g_canvas_api.source_get_canvas = (source_get_canvas_fn)lookup_symbol("obs_source_get_canvas");
	g_canvas_api.release = (canvas_release_fn)lookup_symbol("obs_canvas_release");

	if (!g_canvas_api.enum_all || !g_canvas_api.get_name || !g_canvas_api.enum_scenes ||
	    !g_canvas_api.set_channel || !g_canvas_api.get_channel ||
	    !g_canvas_api.source_get_canvas || !g_canvas_api.release) {
		obs_log(LOG_ERROR,
			"[SELiveCanvasBridge] OBS 32 Canvas API is incomplete or unavailable.");
		return false;
	}

	obs_log(LOG_INFO, "[SELiveCanvasBridge] OBS Canvas and private-canvas discovery API loaded.");
	return true;
}

static bool contains_ci(const char *text, const char *needle)
{
	if (!text || !needle || !*needle)
		return false;

	for (; *text; text++) {
		const char *a = text;
		const char *b = needle;

		while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
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

static void log_source(const char *label, obs_source_t *source)
{
	if (!source) {
		obs_log(LOG_INFO, "[SELiveCanvasBridge]   %s: (null)", label);
		return;
	}

	obs_log(LOG_INFO,
		"[SELiveCanvasBridge]   %s: name='%s', id='%s', type=%s, uuid='%s'",
		label, obs_source_get_name(source) ? obs_source_get_name(source) : "(unnamed)",
		obs_source_get_id(source) ? obs_source_get_id(source) : "(no id)",
		source_type_name(source),
		obs_source_get_uuid(source) ? obs_source_get_uuid(source) : "(no uuid)");
}

static obs_source_t *get_transition_child(obs_source_t *transition)
{
	obs_source_t *child;

	if (!transition || obs_source_get_type(transition) != OBS_SOURCE_TYPE_TRANSITION)
		return NULL;

	child = obs_transition_get_active_source(transition);
	if (child)
		return child;

	child = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B);
	if (child)
		return child;

	return obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_A);
}

static void release_vertical_graph(struct vertical_graph *graph)
{
	if (!graph)
		return;

	if (graph->private_canvas)
		g_canvas_api.release(graph->private_canvas);
	if (graph->current_scene_source)
		obs_source_release(graph->current_scene_source);
	if (graph->control_transition)
		obs_source_release(graph->control_transition);
	if (graph->shared_root)
		obs_source_release(graph->shared_root);

	memset(graph, 0, sizeof(*graph));
}

struct graph_find_context {
	struct vertical_graph *graph;
	bool require_streamelements_name;
	bool found_shared_canvas;
};

static bool graph_find_canvas_callback(void *data, obs_canvas_t *canvas)
{
	struct graph_find_context *context = data;
	struct vertical_graph *graph = context->graph;
	const char *canvas_name = g_canvas_api.get_name(canvas);
	const bool is_vertical = contains_ci(canvas_name, "vertical");
	const bool is_streamelements =
		contains_ci(canvas_name, "se.live") || contains_ci(canvas_name, "streamelements");
	obs_source_t *first_child = NULL;
	obs_source_t *second_child = NULL;

	if (!is_vertical)
		return true;
	if (context->require_streamelements_name && !is_streamelements)
		return true;

	context->found_shared_canvas = true;
	graph->shared_root = g_canvas_api.get_channel(canvas, 0);
	if (!graph->shared_root) {
		obs_log(LOG_WARNING,
			"[SELiveCanvasBridge] Shared vertical canvas '%s' has no source on channel 0.",
			canvas_name ? canvas_name : "(unnamed)");
		return false;
	}

	if (canvas_name) {
		strncpy(graph->shared_canvas_name, canvas_name,
			sizeof(graph->shared_canvas_name) - 1);
		graph->shared_canvas_name[sizeof(graph->shared_canvas_name) - 1] = '\0';
	}

	/*
	 * SE.Live places a private root transition on the public/shared canvas.
	 * That root wraps the composition transition, which wraps the current
	 * private scene. Follow those references to recover the private canvas.
	 */
	first_child = get_transition_child(graph->shared_root);
	if (!first_child) {
		obs_log(LOG_WARNING,
			"[SELiveCanvasBridge] Could not traverse shared vertical root transition.");
		return false;
	}

	if (obs_source_get_type(first_child) == OBS_SOURCE_TYPE_TRANSITION) {
		graph->control_transition = first_child;
		first_child = NULL;
		second_child = get_transition_child(graph->control_transition);
		graph->current_scene_source = second_child;
		second_child = NULL;
	} else {
		/* Fallback for a composition with only one transition layer. */
		graph->control_transition = obs_source_get_ref(graph->shared_root);
		graph->current_scene_source = first_child;
		first_child = NULL;
	}

	if (!graph->current_scene_source) {
		obs_log(LOG_WARNING,
			"[SELiveCanvasBridge] Could not resolve the current private vertical scene.");
		return false;
	}

	graph->private_canvas = g_canvas_api.source_get_canvas(graph->current_scene_source);
	if (!graph->private_canvas) {
		obs_log(LOG_WARNING,
			"[SELiveCanvasBridge] Current vertical scene did not expose its private canvas.");
		return false;
	}

	return false;
}

static bool find_vertical_graph(struct vertical_graph *graph)
{
	struct graph_find_context preferred;

	memset(graph, 0, sizeof(*graph));
	memset(&preferred, 0, sizeof(preferred));
	preferred.graph = graph;
	preferred.require_streamelements_name = true;
	g_canvas_api.enum_all(graph_find_canvas_callback, &preferred);

	if (graph->private_canvas)
		return true;

	release_vertical_graph(graph);

	if (!preferred.found_shared_canvas) {
		struct graph_find_context fallback;

		memset(&fallback, 0, sizeof(fallback));
		fallback.graph = graph;
		fallback.require_streamelements_name = false;
		g_canvas_api.enum_all(graph_find_canvas_callback, &fallback);
	}

	return graph->private_canvas != NULL;
}

struct dump_scene_context {
	unsigned int count;
	const char *prefix;
};

static bool dump_scene_callback(void *data, obs_source_t *source)
{
	struct dump_scene_context *context = data;

	context->count++;
	obs_log(LOG_INFO, "[SELiveCanvasBridge]   %s scene %u: %s [uuid=%s]",
		context->prefix ? context->prefix : "Canvas", context->count,
		obs_source_get_name(source) ? obs_source_get_name(source) : "(unnamed)",
		obs_source_get_uuid(source) ? obs_source_get_uuid(source) : "(no uuid)");
	return true;
}

static bool dump_canvas_callback(void *data, obs_canvas_t *canvas)
{
	unsigned int *canvas_count = data;
	const char *name = g_canvas_api.get_name(canvas);
	const char *uuid = g_canvas_api.get_uuid ? g_canvas_api.get_uuid(canvas) : NULL;
	struct dump_scene_context context;

	memset(&context, 0, sizeof(context));
	context.prefix = "Public";
	(*canvas_count)++;

	obs_log(LOG_INFO, "[SELiveCanvasBridge] Public canvas %u: %s [uuid=%s]", *canvas_count,
		name ? name : "(unnamed)", uuid ? uuid : "(unavailable)");
	g_canvas_api.enum_scenes(canvas, dump_scene_callback, &context);
	obs_log(LOG_INFO, "[SELiveCanvasBridge]   Public scenes total: %u", context.count);
	return true;
}

static void dump_canvases(void)
{
	unsigned int count = 0;
	struct vertical_graph graph;
	struct dump_scene_context private_context;

	if (!g_canvas_api.enum_all)
		return;

	obs_log(LOG_INFO, "[SELiveCanvasBridge] ===== Canvas diagnostic v0.4 start =====");
	g_canvas_api.enum_all(dump_canvas_callback, &count);

	memset(&graph, 0, sizeof(graph));
	if (find_vertical_graph(&graph)) {
		const char *private_name = g_canvas_api.get_name(graph.private_canvas);
		const char *private_uuid =
			g_canvas_api.get_uuid ? g_canvas_api.get_uuid(graph.private_canvas) : NULL;

		obs_log(LOG_INFO, "[SELiveCanvasBridge] SE.Live private composition discovered:");
		obs_log(LOG_INFO, "[SELiveCanvasBridge]   Shared canvas: %s",
			graph.shared_canvas_name[0] ? graph.shared_canvas_name : "(unnamed)");
		log_source("Shared root", graph.shared_root);
		log_source("Control transition", graph.control_transition);
		log_source("Current private scene", graph.current_scene_source);
		obs_log(LOG_INFO, "[SELiveCanvasBridge]   Private canvas: %s [uuid=%s]",
			private_name ? private_name : "(unnamed)",
			private_uuid ? private_uuid : "(unavailable)");

		memset(&private_context, 0, sizeof(private_context));
		private_context.prefix = "Private vertical";
		g_canvas_api.enum_scenes(graph.private_canvas, dump_scene_callback,
			&private_context);
		obs_log(LOG_INFO, "[SELiveCanvasBridge]   Private vertical scenes total: %u",
			private_context.count);
	} else {
		obs_log(LOG_WARNING,
			"[SELiveCanvasBridge] The SE.Live private composition could not be discovered.");
	}

	release_vertical_graph(&graph);
	obs_log(LOG_INFO, "[SELiveCanvasBridge] ===== Canvas diagnostic v0.4 end =====");
}

struct private_scene_match_context {
	const char *target_scene;
	obs_source_t *matched_source;
};

static bool private_scene_match_callback(void *data, obs_source_t *source)
{
	struct private_scene_match_context *context = data;
	const char *scene_name = obs_source_get_name(source);

	if (!scene_names_equal(scene_name, context->target_scene))
		return true;

	context->matched_source = obs_source_get_ref(source);
	return false;
}

static bool switch_vertical_to(const char *scene_name)
{
	struct vertical_graph graph;
	struct private_scene_match_context match;
	bool started = false;

	memset(&graph, 0, sizeof(graph));
	if (!find_vertical_graph(&graph)) {
		obs_log(LOG_WARNING,
			"[SELiveCanvasBridge] Unable to access the SE.Live private vertical composition.");
		return false;
	}

	memset(&match, 0, sizeof(match));
	match.target_scene = scene_name;
	g_canvas_api.enum_scenes(graph.private_canvas, private_scene_match_callback, &match);

	if (!match.matched_source) {
		obs_log(LOG_WARNING,
			"[SELiveCanvasBridge] Private vertical canvas found, but no scene matched '%s'.",
			scene_name ? scene_name : "(null)");
		release_vertical_graph(&graph);
		return false;
	}

	if (scene_names_equal(obs_source_get_name(graph.current_scene_source),
			      obs_source_get_name(match.matched_source))) {
		obs_log(LOG_INFO, "[SELiveCanvasBridge] Vertical scene is already '%s'.",
			obs_source_get_name(match.matched_source));
		started = true;
	} else {
		/*
		 * Experimental fallback: start SE.Live's own private composition
		 * transition directly. This changes the rendered vertical output even
		 * though SE.Live does not expose its private scene list publicly.
		 */
		started = obs_transition_start(graph.control_transition,
			OBS_TRANSITION_MODE_AUTO, 0, match.matched_source);

		obs_log(started ? LOG_INFO : LOG_WARNING,
			"[SELiveCanvasBridge] Experimental private transition to '%s': %s.",
			obs_source_get_name(match.matched_source),
			started ? "started" : "failed");
	}

	obs_source_release(match.matched_source);
	release_vertical_graph(&graph);
	return started;
}

static void sync_current_scene(const char *reason)
{
	obs_source_t *current_scene;
	const char *scene_name;

	if (!g_frontend_loaded || !g_canvas_api.get_channel)
		return;

	current_scene = obs_frontend_get_current_scene();
	if (!current_scene) {
		obs_log(LOG_WARNING, "[SELiveCanvasBridge] Cannot read current horizontal scene.");
		return;
	}

	scene_name = obs_source_get_name(current_scene);
	obs_log(LOG_INFO, "[SELiveCanvasBridge] Sync requested (%s): horizontal='%s'.",
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
	g_retries_left = 6;
	g_retry_elapsed = 0.0f;
}

static void tick_callback(void *unused, float seconds)
{
	(void)unused;

	if (!g_frontend_loaded || g_retries_left <= 0)
		return;

	g_retry_elapsed += seconds;
	if (g_retry_elapsed < 0.35f)
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
	return "Synchronizes OBS scenes with SE.Live's private vertical composition.";
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "[SELiveCanvasBridge] Loading version %s on OBS %s.", PLUGIN_VERSION,
		obs_get_version_string());

	if (!load_canvas_api())
		return true;

	obs_frontend_add_event_callback(frontend_event_callback, NULL);
	obs_frontend_add_tools_menu_item("SE.Live Bridge: Sync vertical now (experimental)",
		tools_sync_callback, NULL);
	obs_frontend_add_tools_menu_item("SE.Live Bridge: Dump public + private canvases",
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
