/* Copyright (C) 2006 Buzztrax team <buzztrax-devel@buzztrax.org>
 * Copyright (C) Gabor Karsay 2016 <gabor.karsay@gmx.at>
 *
 * Taken from wave.c (bt_wave_load_from_uri) from Buzztrax and modified.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#define _POSIX_SOURCE

#include "config.h"
#include <stdio.h>	/* sscanf */
#include <gio/gio.h>
#include <glib/gi18n.h>	
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <sys/stat.h>	/* fstat */
#include "pt-waveloader.h"

struct _PtWaveloaderPrivate
{
	GstElement *pipeline;
	GstElement *fmt;

	gchar      *uri;
	gint64	    duration;
	gint	    channels;
	gint	    rate;
	guint64	    length;

	gint	    fd;
	FILE	   *tf;
};

enum
{
	PROP_0,
	PROP_URI,
	N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

G_DEFINE_TYPE_WITH_PRIVATE (PtWaveloader, pt_waveloader, G_TYPE_OBJECT)


/**
 * SECTION: pt-waveloader
 * @short_description: Loads the waveform for a given file.
 *
 * Here is the long description.
 *
 */


static void
on_wave_loader_new_pad (GstElement *bin,
			GstPad	   *pad,
			gpointer    user_data)
{
	// TODO(ensonic): if we pass the pad in user_data we can use gst_pad_link()
	if (!gst_element_link (bin, GST_ELEMENT (user_data))) {
		GST_WARNING ("Can't link output of wave decoder to converter.");
	}
}

static gboolean
setup_pipeline (PtWaveloader *wl)
{
	gboolean result = TRUE;
	GstElement *src, *dec, *conv, *sink;
	GstCaps *caps;

	// create loader pipeline
	wl->priv->pipeline = gst_pipeline_new ("wave-loader");
	src 		   = gst_element_make_from_uri (GST_URI_SRC, wl->priv->uri, NULL, NULL);
	dec 		   = gst_element_factory_make ("decodebin", NULL);
	conv 		   = gst_element_factory_make ("audioconvert", NULL);
	wl->priv->fmt 	   = gst_element_factory_make ("capsfilter", NULL);
	sink 		   = gst_element_factory_make ("fdsink", NULL);

	// configure elements
	caps = gst_caps_new_simple ("audio/x-raw",
				    "format", G_TYPE_STRING, GST_AUDIO_NE (S16),
				    "layout", G_TYPE_STRING, "interleaved",
				    "rate", GST_TYPE_INT_RANGE, 1, G_MAXINT,
				    "channels", GST_TYPE_INT_RANGE, 1, 2, NULL);
	g_object_set (wl->priv->fmt, "caps", caps, NULL);
	gst_caps_unref (caps);

	g_object_set (sink, "fd", wl->priv->fd, "sync", FALSE, NULL);

	// add and link
	gst_bin_add_many (GST_BIN (wl->priv->pipeline), src, dec, conv, wl->priv->fmt, sink, NULL);
	result = gst_element_link (src, dec);
	if (!result) {
		GST_WARNING_OBJECT (wl->priv->pipeline,
			"Can't link wave loader pipeline (src ! dec ! conv ! fmt ! sink).");
		return result;
	}

	result = gst_element_link_many (conv, wl->priv->fmt, sink, NULL);
	if (!result) {
		GST_WARNING_OBJECT (wl->priv->pipeline,
			"Can't link wave loader pipeline (conf ! fmt ! sink).");
		return result;
	}

	g_signal_connect (dec, "pad-added", G_CALLBACK (on_wave_loader_new_pad),
			(gpointer) conv);

	return result;
}

static gboolean
run_pipeline (PtWaveloader *wl)
{
	gboolean res = TRUE, done = FALSE;
	GstBus *bus = NULL;
	GstMessage *msg;

	bus = gst_element_get_bus (wl->priv->pipeline);

	// play and wait for EOS
	if (gst_element_set_state (wl->priv->pipeline,
			GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		GST_WARNING_OBJECT (wl->priv->pipeline,
			"Can't set wave loader pipeline for %s to playing", wl->priv->uri);
		gst_element_set_state (wl->priv->pipeline, GST_STATE_NULL);
		gst_object_unref (bus);
		return FALSE;

	} else {
		GST_INFO_OBJECT (wl->priv->pipeline, "loading sample ...");
	}

	/* load wave in sync mode, loading them async causes troubles in the 
	 * persistence code and makes testing complicated */
	while (!done) {
		msg = gst_bus_poll (bus,
			GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_TAG,
			GST_CLOCK_TIME_NONE);
		if (!msg)
			break;

		switch (msg->type) {
			case GST_MESSAGE_EOS:
				res = done = TRUE;
				break;
			case GST_MESSAGE_ERROR:{
				GError *err;
				gchar *desc, *dbg = NULL;

				gst_message_parse_error (msg, &err, &dbg);
				desc = gst_error_get_message (err->domain, err->code);
				GST_WARNING_OBJECT (GST_MESSAGE_SRC (msg), "ERROR: %s (%s) (%s)",
					err->message, desc, (dbg ? dbg : "no debug"));
				g_error_free (err);
				g_free (dbg);
				g_free (desc);
				res = FALSE;
				done = TRUE;
				break;
			}
			default:
				break;
			}
		gst_message_unref (msg);
	}

	gst_object_unref (bus);
	return res;
}

/* 
 * pt_waveloader_load:
 * @self: the wave to load
 * @uri: the location to load from
 *
 * Load the wavedata from the @uri.
 *
 * Returns: %TRUE if the wavedata could be loaded
 */
gboolean
pt_waveloader_load (PtWaveloader *wl,
		    GError      **error)
{
	gboolean res = TRUE;
	GstPad *pad;

	/* setup file descriptor */
	if (!(wl->priv->tf = tmpfile ())) {
		GST_WARNING ("Can't create tempfile.");
		return FALSE;
	}
	wl->priv->fd = fileno (wl->priv->tf);
	
	/* setup and run pipeline */
	if (!setup_pipeline (wl))
		return FALSE;

	if (!run_pipeline (wl))
		return FALSE;

	// query length and convert to samples
	if (!gst_element_query_duration (wl->priv->pipeline, GST_FORMAT_TIME, &wl->priv->duration)) {
		GST_WARNING ("getting sample duration failed");
	}
	// get caps for sample rate and channels
	if ((pad = gst_element_get_static_pad (wl->priv->fmt, "src"))) {
		GstCaps *caps = gst_pad_get_current_caps (pad);
		if (caps && GST_CAPS_IS_SIMPLE (caps)) {
			GstStructure *structure = gst_caps_get_structure (caps, 0);

			gst_structure_get_int (structure, "channels", &wl->priv->channels);
			gst_structure_get_int (structure, "rate", &wl->priv->rate);

		} else {
			GST_WARNING ("No caps or format has not been fixed.");
			wl->priv->channels = 1;
			wl->priv->rate = GST_AUDIO_DEF_RATE;
		}
		if (caps)
			gst_caps_unref (caps);
		gst_object_unref (pad);
	}

	g_debug ("sample decoded: channels=%d, rate=%d, length=%" GST_TIME_FORMAT,
		wl->priv->channels, wl->priv->rate, GST_TIME_ARGS (wl->priv->duration));

	return res;
}

void
pt_waveloader_cancel (PtWaveloader *wl)
{
	if (wl->priv->pipeline) {
		gst_element_set_state (wl->priv->pipeline, GST_STATE_NULL);
		gst_object_unref (wl->priv->pipeline);
	}
}

gchar *
pt_waveloader_get_uri (PtWaveloader *wl)
{
	return wl->priv->uri;
}

gint64
pt_waveloader_get_duration (PtWaveloader *wl)
{
	return wl->priv->duration;
}

gint
pt_waveloader_get_channels (PtWaveloader *wl)
{
	return wl->priv->channels;
}

gint
pt_waveloader_get_rate (PtWaveloader *wl)
{
	return wl->priv->rate;
}

gint16 *
pt_waveloader_get_data (PtWaveloader *wl)
{
	struct stat buf;
	gint16 *data = NULL;

	if (!(fstat (wl->priv->fd, &buf))) {
		if ((data = g_try_malloc (buf.st_size))) {
			if (lseek (wl->priv->fd, 0, SEEK_SET) == 0) {
				ssize_t bytes = read (wl->priv->fd, data, buf.st_size);

				g_debug ("sample loaded (%" G_GSSIZE_FORMAT "/%ld bytes)", bytes,
					buf.st_size);
				return data;
			} else {
				GST_WARNING ("can't seek to start of sample data");
			}
		} else {
			GST_WARNING
				("sample is too long or empty (%ld bytes), not trying to load",
				buf.st_size);
		}
	} else {
		GST_WARNING ("can't stat() sample");
	}
}


/* --------------------- Init and GObject management ------------------------ */

static void
pt_waveloader_init (PtWaveloader *wl)
{
	wl->priv = pt_waveloader_get_instance_private (wl);

	wl->priv->pipeline = NULL;
	wl->priv->fd = -1;
}

static void
pt_waveloader_dispose (GObject *object)
{
	PtWaveloader *wl;
	wl = PT_WAVELOADER (object);

	g_free (wl->priv->uri);	

	if (wl->priv->tf) {
		// fd is fileno() of a tf
		fclose (wl->priv->tf);
		wl->priv->tf = NULL;
		wl->priv->fd = -1;
	} else if (wl->priv->fd != -1) {
		close (wl->priv->fd);
		wl->priv->fd = -1;
	}

	if (wl->priv->pipeline) {
		
		gst_element_set_state (wl->priv->pipeline, GST_STATE_NULL);
		gst_object_unref (GST_OBJECT (wl->priv->pipeline));
		wl->priv->pipeline = NULL;
		//remove_message_bus (player);
	}

	G_OBJECT_CLASS (pt_waveloader_parent_class)->dispose (object);
}

static void
pt_waveloader_set_property (GObject      *object,
			    guint         property_id,
			    const GValue *value,
			    GParamSpec   *pspec)
{
	PtWaveloader *wl;
	wl = PT_WAVELOADER (object);

	switch (property_id) {
	case PROP_URI:
		g_free (wl->priv->uri);
		wl->priv->uri = g_value_dup_string (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void
pt_waveloader_get_property (GObject    *object,
			    guint       property_id,
			    GValue     *value,
			    GParamSpec *pspec)
{
	PtWaveloader *wl;
	wl = PT_WAVELOADER (object);
	gdouble tmp;

	switch (property_id) {
	case PROP_URI:
		g_value_set_string (value, wl->priv->uri);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void
pt_waveloader_class_init (PtWaveloaderClass *klass)
{
	G_OBJECT_CLASS (klass)->set_property = pt_waveloader_set_property;
	G_OBJECT_CLASS (klass)->get_property = pt_waveloader_get_property;
	G_OBJECT_CLASS (klass)->dispose = pt_waveloader_dispose;

	/**
	* PtPlayer::player-state-changed:
	* @player: the player emitting the signal
	* @state: the new state, TRUE is ready, FALSE is not ready
	*
	* The ::player-state-changed signal is emitted when the @player changes
	* its state to ready to play (a file was opened) or not ready to play
	* (an error occured). If the player is ready, a duration of the stream
	* is available.
	*/
	g_signal_new ("progress",
		      G_TYPE_OBJECT,
		      G_SIGNAL_RUN_FIRST,
		      0,
		      NULL,
		      NULL,
		      g_cclosure_marshal_VOID__INT,
		      G_TYPE_NONE,
		      1, G_TYPE_INT);


	/**
	* PtWaveloader:uri:
	*
	* URI to load from.
	*/
	obj_properties[PROP_URI] =
	g_param_spec_string (
			"uri",
			"URI to load from",
			"URI to load from",
			"",
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

	g_object_class_install_properties (
			G_OBJECT_CLASS (klass),
			N_PROPERTIES,
			obj_properties);
}

/**
 * pt_waveloader_new:
 * @uri: 
 *
 * Some description
 *
 * After use g_object_unref() it.
 *
 * Return value: (transfer full): a new PtWaveloader
 */
PtWaveloader *
pt_waveloader_new (gchar *uri)
{
	return g_object_new (PT_WAVELOADER_TYPE,
			     "uri", uri,
			     NULL);
}
