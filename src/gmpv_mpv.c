/*
 * Copyright (c) 2014-2017 gnome-mpv
 *
 * This file is part of GNOME MPV.
 *
 * GNOME MPV is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GNOME MPV is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNOME MPV.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <unistd.h>
#include <glib-object.h>
#include <glib/gi18n.h>
#include <gdk/gdk.h>
#include <stdlib.h>
#include <string.h>

#include <epoxy/gl.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <epoxy/glx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#include <epoxy/egl.h>
#endif
#ifdef GDK_WINDOWING_WIN32
#include <gdk/gdkwin32.h>
#include <epoxy/wgl.h>
#endif

#include "gmpv_mpv.h"
#include "gmpv_mpv_private.h"
#include "gmpv_mpv_wrapper.h"
#include "gmpv_mpv_options.h"
#include "gmpv_common.h"
#include "gmpv_def.h"
#include "gmpv_marshal.h"

static void *GLAPIENTRY glMPGetNativeDisplay(const gchar *name);
static void *get_proc_address(void *fn_ctx, const gchar *name);
static void set_property(	GObject *object,
				guint property_id,
				const GValue *value,
				GParamSpec *pspec );
static void get_property(	GObject *object,
				guint property_id,
				GValue *value,
				GParamSpec *pspec );
static void dispose(GObject *object);
static void finalize(GObject *object);
static void apply_extra_options(GmpvMpv *mpv);
static void observe_properties(GmpvMpv *mpv);
static void wakeup_callback(void *data);
static void mpv_property_changed_handler(	GmpvMpv *mpv,
						const gchar *name,
						gpointer value );
static void mpv_event_handler(GmpvMpv *mpv, gint event_id, gpointer event_data);
static gboolean process_mpv_events(gpointer data);
static void initialize(GmpvMpv *mpv);
static void load_file(GmpvMpv *mpv, const gchar *uri, gboolean append);
static void reset(GmpvMpv *mpv);
static gint apply_args(mpv_handle *mpv_ctx, gchar *args);
static void log_handler(GmpvMpv *mpv, mpv_event_log_message* message);

G_DEFINE_TYPE_WITH_PRIVATE(GmpvMpv, gmpv_mpv, G_TYPE_OBJECT)

static void *GLAPIENTRY glMPGetNativeDisplay(const gchar *name)
{
       GdkDisplay *display = gdk_display_get_default();

#ifdef GDK_WINDOWING_WAYLAND
       if(GDK_IS_WAYLAND_DISPLAY(display) && g_strcmp0(name, "wl") == 0)
               return gdk_wayland_display_get_wl_display(display);
#endif
#ifdef GDK_WINDOWING_X11
       if(GDK_IS_X11_DISPLAY(display) && g_strcmp0(name, "x11") == 0)
               return gdk_x11_display_get_xdisplay(display);
#endif

       return NULL;
}

static void *get_proc_address(void *fn_ctx, const gchar *name)
{
	GdkDisplay *display = gdk_display_get_default();

	if(g_strcmp0(name, "glMPGetNativeDisplay") == 0)
		return glMPGetNativeDisplay;

#ifdef GDK_WINDOWING_WAYLAND
	if (GDK_IS_WAYLAND_DISPLAY(display))
		return eglGetProcAddress(name);
#endif
#ifdef GDK_WINDOWING_X11
	if (GDK_IS_X11_DISPLAY(display))
		return	(void *)(intptr_t)
			glXGetProcAddressARB((const GLubyte *)name);
#endif
#ifdef GDK_WINDOWING_WIN32
	if (GDK_IS_WIN32_DISPLAY(display))
		return wglGetProcAddress(name);
#endif
	g_assert_not_reached();
}

static void set_property(	GObject *object,
				guint property_id,
				const GValue *value,
				GParamSpec *pspec )
{
	GmpvMpvPrivate *priv = get_private(GMPV_MPV(object));

	if(property_id == PROP_WID)
	{
		priv->wid = g_value_get_int64(value);
	}
	else if(property_id == PROP_READY)
	{
		priv->ready = g_value_get_boolean(value);
	}
	else
	{
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void get_property(	GObject *object,
				guint property_id,
				GValue *value,
				GParamSpec *pspec )
{
	GmpvMpvPrivate *priv = get_private(GMPV_MPV(object));

	if(property_id == PROP_WID)
	{
		g_value_set_int64(value, priv->wid);
	}
	else if(property_id == PROP_READY)
	{
		g_value_set_boolean(value, priv->ready);
	}
	else
	{
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void dispose(GObject *object)
{
	GmpvMpv *mpv = GMPV_MPV(object);

	if(get_private(mpv)->mpv_ctx)
	{
		gmpv_mpv_quit(mpv);
		while(g_source_remove_by_user_data(object));
	}

	G_OBJECT_CLASS(gmpv_mpv_parent_class)->dispose(object);
}

static void finalize(GObject *object)
{
	GmpvMpvPrivate *priv = get_private(GMPV_MPV(object));

	g_slist_free_full(	priv->log_level_list,
				(GDestroyNotify)module_log_level_free);

	G_OBJECT_CLASS(gmpv_mpv_parent_class)->finalize(object);
}

static void apply_extra_options(GmpvMpv *mpv)
{
	GSettings *settings = g_settings_new(CONFIG_ROOT);
	gchar *extra_options = g_settings_get_string(settings, "mpv-options");

	g_debug("Applying extra mpv options: %s", extra_options);

	/* Apply extra options */
	if(apply_args(get_private(mpv)->mpv_ctx, extra_options) < 0)
	{
		const gchar *msg = _("Failed to apply one or more MPV options.");
		g_signal_emit_by_name(mpv, "error", msg);
	}

	g_free(extra_options);
	g_object_unref(settings);
}

static void observe_properties(GmpvMpv *mpv)
{
	mpv_handle *mpv_ctx = get_private(mpv)->mpv_ctx;

	mpv_observe_property(mpv_ctx, 0, "aid", MPV_FORMAT_STRING);
	mpv_observe_property(mpv_ctx, 0, "vid", MPV_FORMAT_STRING);
	mpv_observe_property(mpv_ctx, 0, "sid", MPV_FORMAT_STRING);
	mpv_observe_property(mpv_ctx, 0, "chapters", MPV_FORMAT_INT64);
	mpv_observe_property(mpv_ctx, 0, "core-idle", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv_ctx, 0, "idle-active", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv_ctx, 0, "fullscreen", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv_ctx, 0, "pause", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv_ctx, 0, "loop", MPV_FORMAT_STRING);
	mpv_observe_property(mpv_ctx, 0, "duration", MPV_FORMAT_DOUBLE);
	mpv_observe_property(mpv_ctx, 0, "media-title", MPV_FORMAT_STRING);
	mpv_observe_property(mpv_ctx, 0, "metadata", MPV_FORMAT_NODE);
	mpv_observe_property(mpv_ctx, 0, "playlist", MPV_FORMAT_NODE);
	mpv_observe_property(mpv_ctx, 0, "playlist-count", MPV_FORMAT_INT64);
	mpv_observe_property(mpv_ctx, 0, "playlist-pos", MPV_FORMAT_INT64);
	mpv_observe_property(mpv_ctx, 0, "speed", MPV_FORMAT_DOUBLE);
	mpv_observe_property(mpv_ctx, 0, "track-list", MPV_FORMAT_NODE);
	mpv_observe_property(mpv_ctx, 0, "vo-configured", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv_ctx, 0, "volume", MPV_FORMAT_DOUBLE);
}

static void wakeup_callback(void *data)
{
	g_idle_add_full(G_PRIORITY_HIGH_IDLE, process_mpv_events, data, NULL);
}

static void mpv_property_changed_handler(	GmpvMpv *mpv,
						const gchar *name,
						gpointer value )
{
	g_debug("Received mpv property change event for \"%s\"", name);
}

static void mpv_event_handler(GmpvMpv *mpv, gint event_id, gpointer event_data)
{
	GmpvMpvPrivate *priv = get_private(mpv);

	if(event_id == MPV_EVENT_PROPERTY_CHANGE)
	{
		mpv_event_property *prop = event_data;

		g_signal_emit_by_name(	mpv,
					"mpv-prop-change",
					prop->name,
					prop->data );
	}
	else if(event_id == MPV_EVENT_IDLE)
	{
		priv->loaded = FALSE;
		gmpv_mpv_set_property_flag(mpv, "pause", TRUE);
	}
	else if(event_id == MPV_EVENT_FILE_LOADED)
	{
		priv->loaded = TRUE;
	}
	else if(event_id == MPV_EVENT_END_FILE)
	{
		mpv_event_end_file *ef_event = event_data;

		if(priv->loaded)
		{
			priv->new_file = FALSE;
		}

		if(ef_event->reason == MPV_END_FILE_REASON_ERROR)
		{
			const gchar *err;
			gchar *msg;

			err = mpv_error_string(ef_event->error);
			msg = g_strdup_printf
				(	_("Playback was terminated "
					"abnormally. Reason: %s."),
					err );

			gmpv_mpv_set_property_flag(mpv, "pause", TRUE);
			g_signal_emit_by_name(mpv, "error", msg);

			g_free(msg);
		}
	}
	else if(event_id == MPV_EVENT_VIDEO_RECONFIG)
	{
		g_signal_emit_by_name(mpv, "mpv-video-reconfig");
	}
	else if(event_id == MPV_EVENT_PLAYBACK_RESTART)
	{
		g_signal_emit_by_name(mpv, "mpv-playback-restart");
	}
	else if(event_id == MPV_EVENT_LOG_MESSAGE)
	{
		log_handler(mpv, event_data);
	}
	else if(event_id == MPV_EVENT_CLIENT_MESSAGE)
	{
		mpv_event_client_message *event_cmsg = event_data;
		gchar* msg = strnjoinv(	" ",
					event_cmsg->args,
					(gsize)event_cmsg->num_args );

		g_signal_emit_by_name(mpv, "message", msg);
		g_free(msg);
	}
	else if(event_id == MPV_EVENT_SHUTDOWN)
	{
		g_signal_emit_by_name(mpv, "shutdown");
	}
}

static gboolean process_mpv_events(gpointer data)
{
	GmpvMpv *mpv = data;
	GmpvMpvPrivate *priv = get_private(mpv);
	gboolean done = !mpv;

	while(!done)
	{
		mpv_event *event =	priv->mpv_ctx?
					mpv_wait_event(priv->mpv_ctx, 0):
					NULL;

		if(event)
		{
			if(	!priv->mpv_ctx ||
				event->event_id == MPV_EVENT_SHUTDOWN ||
				event->event_id == MPV_EVENT_NONE )
			{
				done = TRUE;
			}

			GMPV_MPV_GET_CLASS(mpv)
				->mpv_event(mpv, event->event_id, event->data);
		}
		else
		{
			done = TRUE;
		}
	}

	return FALSE;
}

static void initialize(GmpvMpv *mpv)
{
	GmpvMpvPrivate *priv = get_private(mpv);
	GSettings *settings = g_settings_new(CONFIG_ROOT);
	gchar *current_vo = NULL;
	gchar *mpv_version = NULL;

	apply_extra_options(mpv);

	if(priv->force_opengl || priv->wid <= 0)
	{
		g_info("Forcing --vo=opengl-cb");
		mpv_set_option_string(priv->mpv_ctx, "vo", "opengl-cb");
	}
	else
	{
		g_debug("Attaching mpv window to wid %#x", (guint)priv->wid);
		mpv_set_option(priv->mpv_ctx, "wid", MPV_FORMAT_INT64, &priv->wid);
	}

	observe_properties(mpv);
	mpv_set_wakeup_callback(priv->mpv_ctx, wakeup_callback, mpv);
	mpv_initialize(priv->mpv_ctx);

	mpv_version = gmpv_mpv_get_property_string(mpv, "mpv-version");
	current_vo = gmpv_mpv_get_property_string(mpv, "current-vo");
	priv->use_opengl = !current_vo;

	g_info("Using %s", mpv_version);

	if(!priv->use_opengl && !GDK_IS_X11_DISPLAY(gdk_display_get_default()))
	{
		g_info(	"The chosen vo is %s but the display is not X11; "
			"forcing --vo=opengl-cb and resetting",
			current_vo );

		priv->force_opengl = TRUE;

		gmpv_mpv_reset(mpv);
	}
	else
	{
		GSettings *win_settings;
		gdouble volume;

		win_settings = g_settings_new(CONFIG_WIN_STATE);
		volume = g_settings_get_double(win_settings, "volume")*100;

		g_debug("Setting volume to %f", volume);
		mpv_set_property(	priv->mpv_ctx,
					"volume",
					MPV_FORMAT_DOUBLE,
					&volume );

		if(priv->use_opengl)
		{
			priv->opengl_ctx =	mpv_get_sub_api
						(	priv->mpv_ctx,
							MPV_SUB_API_OPENGL_CB );
		}

		gmpv_mpv_options_init(mpv);

		priv->force_opengl = FALSE;
		priv->ready = TRUE;
		g_object_notify(G_OBJECT(mpv), "ready");

		g_object_unref(win_settings);
	}

	g_object_unref(settings);
	mpv_free(current_vo);
	mpv_free(mpv_version);
}

static void load_file(GmpvMpv *mpv, const gchar *uri, gboolean append)
{
	GmpvMpvPrivate *priv = get_private(mpv);
	gchar *path = get_path_from_uri(uri);
	const gchar *load_cmd[] = {"loadfile", path, NULL, NULL};
	gint64 playlist_count = 0;

	g_assert(uri);
	g_info(	"Loading file (append=%s): %s", append?"TRUE":"FALSE", uri);

	mpv_get_property(	priv->mpv_ctx,
				"playlist-count",
				MPV_FORMAT_INT64,
				&playlist_count );

	load_cmd[2] = (append && playlist_count > 0)?"append":"replace";

	if(!append)
	{
		priv->new_file = TRUE;
		priv->loaded = FALSE;

		gmpv_mpv_set_property_flag(mpv, "pause", FALSE);
	}

	g_assert(priv->mpv_ctx);
	mpv_request_event(priv->mpv_ctx, MPV_EVENT_END_FILE, 0);
	mpv_command(priv->mpv_ctx, load_cmd);
	mpv_request_event(priv->mpv_ctx, MPV_EVENT_END_FILE, 1);

	g_free(path);
}

static void reset(GmpvMpv *mpv)
{
	GmpvMpvPrivate *priv = get_private(mpv);
	const gchar *quit_cmd[] = {"quit_watch_later", NULL};
	gchar *loop_str;
	gboolean loop;
	gboolean pause;

	loop_str = gmpv_mpv_get_property_string(mpv, "loop");
	loop = (g_strcmp0(loop_str, "inf") == 0);

	mpv_free(loop_str);

	/* Reset priv->mpv_ctx */
	priv->ready = FALSE;
	g_object_notify(G_OBJECT(mpv), "ready");

	gmpv_mpv_command(mpv, quit_cmd);
	gmpv_mpv_quit(mpv);

	priv->mpv_ctx = mpv_create();
	gmpv_mpv_initialize(mpv);

	gmpv_mpv_set_opengl_cb_callback
		(	mpv,
			priv->opengl_cb_callback,
			priv->opengl_cb_callback_data );

	gmpv_mpv_set_property_string(mpv, "loop", loop?"inf":"no");
	gmpv_mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &pause);
}

static gint apply_args(mpv_handle *mpv_ctx, gchar *args)
{
	gint fail_count = 0;
	gchar **tokens = g_regex_split_simple(	"(^|\\s+)--",
						args,
						G_REGEX_NO_AUTO_CAPTURE,
						0 );

	/* Skip the first token if it's non-NULL, since it is just going to be
	 * empty string for any valid args.
	 */
	for(gint i = tokens[0]?1:0; tokens[i]; i++)
	{
		gchar **parts = g_strsplit(g_strchomp(tokens[i]), "=", 2);
		const gchar *option = parts[0];
		const gchar *value = (option?parts[1]:NULL)?:"";

		g_debug("Applying option: --%s", tokens[i]);

		if(mpv_set_option_string(mpv_ctx, option, value) < 0)
		{
			fail_count++;

			g_warning("Failed to apply option: --%s\n", tokens[i]);
		}

		g_strfreev(parts);
	}

	g_strfreev(tokens);

	return fail_count*(-1);
}

static void log_handler(GmpvMpv *mpv, mpv_event_log_message* message)
{
	GSList *iter = get_private(mpv)->log_level_list;
	module_log_level *level = iter?iter->data:NULL;
	gsize event_prefix_len = strlen(message->prefix);
	gboolean found = FALSE;

	while(iter && !found)
	{
		gsize prefix_len = strlen(level->prefix);
		gint cmp = strncmp(	level->prefix,
					message->prefix,
					(event_prefix_len < prefix_len)?
					event_prefix_len:prefix_len );

		/* Allow both exact match and prefix match */
		if(cmp == 0
		&& (prefix_len == event_prefix_len
		|| (prefix_len < event_prefix_len
		&& message->prefix[prefix_len] == '/')))
		{
			found = TRUE;
		}
		else
		{
			iter = g_slist_next(iter);
			level = iter?iter->data:NULL;
		}
	}

	if(!iter || (message->log_level <= level->level))
	{
		gchar *buf = g_strdup(message->text);
		gsize len = strlen(buf);

		if(len > 1)
		{
			/* g_message() automatically adds a newline
			 * character when using the default log handler,
			 * but log messages from mpv already come
			 * terminated with a newline character so we
			 * need to take it out.
			 */
			if(buf[len-1] == '\n')
			{
				buf[len-1] = '\0';
			}

			g_message("[%s] %s", message->prefix, buf);
		}

		g_free(buf);
	}
}

static void gmpv_mpv_class_init(GmpvMpvClass* klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);
	GParamSpec *pspec = NULL;

	klass->mpv_event = mpv_event_handler;
	klass->mpv_property_changed = mpv_property_changed_handler;
	klass->initialize = initialize;
	klass->load_file = load_file;
	klass->reset = reset;
	obj_class->set_property = set_property;
	obj_class->get_property = get_property;
	obj_class->dispose = dispose;
	obj_class->finalize = finalize;

	pspec = g_param_spec_int64
		(	"wid",
			"WID",
			"The ID of the window to attach to",
			G_MININT64,
			G_MAXINT64,
			-1,
			G_PARAM_CONSTRUCT_ONLY|G_PARAM_READWRITE );
	g_object_class_install_property(obj_class, PROP_WID, pspec);

	pspec = g_param_spec_boolean
		(	"ready",
			"Ready",
			"Whether mpv is initialized and ready to recieve commands",
			FALSE,
			G_PARAM_READABLE );
	g_object_class_install_property(obj_class, PROP_READY, pspec);

	g_signal_new(	"error",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_marshal_VOID__STRING,
			G_TYPE_NONE,
			1,
			G_TYPE_STRING );
	g_signal_new(	"mpv-video-reconfig",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_marshal_VOID__VOID,
			G_TYPE_NONE,
			0 );
	g_signal_new(	"mpv-playback-restart",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_marshal_VOID__VOID,
			G_TYPE_NONE,
			0 );
	g_signal_new(	"mpv-prop-change",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			G_STRUCT_OFFSET(GmpvMpvClass, mpv_property_changed),
			NULL,
			NULL,
			g_cclosure_gen_marshal_VOID__STRING_POINTER,
			G_TYPE_NONE,
			2,
			G_TYPE_STRING,
			G_TYPE_POINTER );

	g_signal_new(	"message",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_marshal_VOID__STRING,
			G_TYPE_NONE,
			1,
			G_TYPE_STRING );
	g_signal_new(	"window-resize",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_gen_marshal_VOID__INT64_INT64,
			G_TYPE_NONE,
			2,
			G_TYPE_INT64,
			G_TYPE_INT64 );
	g_signal_new(	"window-move",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_gen_marshal_VOID__BOOLEAN_BOOLEAN_POINTER_POINTER,
			G_TYPE_NONE,
			4,
			G_TYPE_BOOLEAN,
			G_TYPE_BOOLEAN,
			G_TYPE_POINTER,
			G_TYPE_POINTER );
	g_signal_new(	"shutdown",
			G_TYPE_FROM_CLASS(klass),
			G_SIGNAL_RUN_FIRST,
			0,
			NULL,
			NULL,
			g_cclosure_marshal_VOID__VOID,
			G_TYPE_NONE,
			0 );
}

static void gmpv_mpv_init(GmpvMpv *mpv)
{
	GmpvMpvPrivate *priv = get_private(mpv);

	priv->mpv_ctx = mpv_create();
	priv->opengl_ctx = NULL;
	priv->log_level_list = NULL;

	priv->ready = FALSE;
	priv->loaded = FALSE;
	priv->new_file = TRUE;

	priv->init_vo_config = TRUE;
	priv->force_opengl = FALSE;
	priv->use_opengl = FALSE;
	priv->wid = -1;
	priv->opengl_cb_callback_data = NULL;
	priv->opengl_cb_callback = NULL;
}

GmpvMpv *gmpv_mpv_new(gint64 wid)
{
	return GMPV_MPV(g_object_new(gmpv_mpv_get_type(), "wid", wid, NULL));
}

inline mpv_opengl_cb_context *gmpv_mpv_get_opengl_cb_context(GmpvMpv *mpv)
{
	return get_private(mpv)->opengl_ctx;
}

inline gboolean gmpv_mpv_get_use_opengl_cb(GmpvMpv *mpv)
{
	return get_private(mpv)->use_opengl;
}

void gmpv_mpv_initialize(GmpvMpv *mpv)
{
	GMPV_MPV_GET_CLASS(mpv)->initialize(mpv);
}

void gmpv_mpv_init_gl(GmpvMpv *mpv)
{
	mpv_opengl_cb_context *opengl_ctx = gmpv_mpv_get_opengl_cb_context(mpv);
	gint rc = mpv_opengl_cb_init_gl(	opengl_ctx,
						"GL_MP_MPGetNativeDisplay",
						get_proc_address,
						NULL );

	if(rc >= 0)
	{
		g_debug("Initialized opengl-cb");
	}
	else
	{
		g_critical("Failed to initialize opengl-cb");
	}
}

void gmpv_mpv_reset(GmpvMpv *mpv)
{
	GMPV_MPV_GET_CLASS(mpv)->reset(mpv);
}

void gmpv_mpv_quit(GmpvMpv *mpv)
{
	GmpvMpvPrivate *priv = get_private(mpv);

	g_info("Terminating mpv");

	if(priv->opengl_ctx)
	{
		g_debug("Uninitializing opengl-cb");
		mpv_opengl_cb_uninit_gl(priv->opengl_ctx);

		priv->opengl_ctx = NULL;
	}

	g_assert(priv->mpv_ctx);
	mpv_terminate_destroy(priv->mpv_ctx);

	priv->mpv_ctx = NULL;
}

void gmpv_mpv_load_track(GmpvMpv *mpv, const gchar *uri, TrackType type)
{
	const gchar *cmd[3] = {NULL};
	gchar *path = g_filename_from_uri(uri, NULL, NULL);

	if(type == TRACK_TYPE_AUDIO)
	{
		cmd[0] = "audio-add";
	}
	else if(type == TRACK_TYPE_SUBTITLE)
	{
		cmd[0] = "sub-add";
	}
	else
	{
		g_assert_not_reached();
	}

	cmd[1] = path?:uri;

	g_debug("Loading external track %s with type %d", cmd[1], type);
	gmpv_mpv_command(mpv, cmd);

	g_free(path);
}

void gmpv_mpv_load_file(GmpvMpv *mpv, const gchar *uri, gboolean append)
{
	GMPV_MPV_GET_CLASS(mpv)->load_file(mpv, uri, append);
}

void gmpv_mpv_load(GmpvMpv *mpv, const gchar *uri, gboolean append)
{
	const gchar *subtitle_exts[] = SUBTITLE_EXTS;

	if(extension_matches(uri, subtitle_exts))
	{
		gmpv_mpv_load_track(mpv, uri, TRACK_TYPE_SUBTITLE);
	}
	else
	{
		gmpv_mpv_load_file(mpv, uri, append);
	}
}
