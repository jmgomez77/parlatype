/* Copyright (C) Gabor Karsay 2016-2019 <gabor.karsay@gmx.at>
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


#include "config.h"
#include <gio/gio.h>
#define GETTEXT_PACKAGE "libparlatype"
#include <glib/gi18n-lib.h>
#include <gst/gst.h>
#include <gst/audio/streamvolume.h>
#include "pt-i18n.h"
#include "pt-waveviewer.h"
#include "pt-player.h"

struct _PtPlayerPrivate
{
	GstElement *play;
	GstElement *scaletempo;
	GstElement *play_bin;
	GstElement *sphinx_bin;
	GstElement *audio_bin;
	GstElement *tee;
	GstPad     *tee_playpad;
	GstPad     *tee_sphinxpad;
	guint	    bus_watch_id;

	gint64	    dur;
	gdouble	    speed;
	gdouble     volume;
	gint        pause;
	gint        back;
	gint        forward;
	gboolean    repeat_all;
	gboolean    repeat_selection;

	gint64      segstart;
	gint64      segend;

	GCancellable *c;

	PtPrecisionType timestamp_precision;
	gboolean        timestamp_fixed;
	gchar          *timestamp_left;
	gchar          *timestamp_right;
	gchar          *timestamp_sep;

	PtWaveviewer *wv;
};

enum
{
	PROP_0,
	PROP_SPEED,
	PROP_VOLUME,
	PROP_TIMESTAMP_PRECISION,
	PROP_TIMESTAMP_FIXED,
	PROP_TIMESTAMP_DELIMITER,
	PROP_TIMESTAMP_FRACTION_SEP,
	PROP_REWIND_ON_PAUSE,
	PROP_BACK,
	PROP_FORWARD,
	PROP_REPEAT_ALL,
	PROP_REPEAT_SELECTION,
	N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

#define METADATA_POSITION "metadata::parlatype::position"
#define ONE_HOUR 3600000
#define TEN_MINUTES 600000

static gboolean bus_call (GstBus *bus, GstMessage *msg, gpointer data);
static void remove_message_bus (PtPlayer *player);

G_DEFINE_TYPE_WITH_PRIVATE (PtPlayer, pt_player, G_TYPE_OBJECT)


/**
 * SECTION: pt-player
 * @short_description: The GStreamer backend for Parlatype.
 * @stability: Stable
 * @include: parlatype/pt-player.h
 *
 * PtPlayer is the GStreamer backend for Parlatype. Construct it with #pt_player_new().
 * Then you have to open a file with pt_player_open_uri().
 *
 * The internal time unit in PtPlayer are milliseconds and for scale widgets there
 * is a scale from 0 to 1000. Use it to jump to a position or to update your widget.
 */



/* -------------------------- static helpers -------------------------------- */

static gboolean
pt_player_query_position (PtPlayer *player,
                          gpointer  position)
{
	gboolean result;
	result = gst_element_query_position (player->priv->play, GST_FORMAT_TIME, position);
	return result;
}

static void
pt_player_clear (PtPlayer *player)
{
	remove_message_bus (player);
	gst_element_set_state (player->priv->play, GST_STATE_NULL);
}

static void
pt_player_seek (PtPlayer *player,
                gint64    position)
{
	/* Set the pipeline to @position.
	   The stop position (player->priv->segend) usually doesn’t has to be
	   set, but it’s important after a segment/selection change and after
	   a rewind (trickmode) has been completed. To simplify things, we
	   always set the stop position. */

	gst_element_seek (
		player->priv->play,
		player->priv->speed,
		GST_FORMAT_TIME,
		GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
		GST_SEEK_TYPE_SET,
		position,
		GST_SEEK_TYPE_SET,
		player->priv->segend);

	/* Block until state changed */
	gst_element_get_state (
		player->priv->play,
		NULL, NULL,
		GST_CLOCK_TIME_NONE);
}

static void
pt_player_set_state_blocking (PtPlayer *player,
                              GstState  state)

{
	g_assert (GST_IS_ELEMENT (player->priv->play));

	gst_element_set_state (player->priv->play, state);

	/* Block until state changed */
	gst_element_get_state (
		player->priv->play,
		NULL, NULL,
		GST_CLOCK_TIME_NONE);
}

static GFile*
pt_player_get_file (PtPlayer *player)
{
	gchar *uri = NULL;
	GFile *result = NULL;

	g_object_get (G_OBJECT (player->priv->play), "current_uri", &uri, NULL);

	if (uri) {
		result = g_file_new_for_uri (uri);
		g_free (uri);
	}

	return result;
}

static void
metadata_save_position (PtPlayer *player)
{
	/* Saves current position in milliseconds as metadata to file */

	GError	  *error = NULL;
	GFile	  *file = NULL;
	GFileInfo *info;
	gint64     pos;
	gchar	   value[64];

	if (!pt_player_query_position (player, &pos))
		return;

	file = pt_player_get_file (player);
	if (!file)
		return;

	pos = pos / GST_MSECOND;

	info = g_file_info_new ();
	g_snprintf (value, sizeof (value), "%" G_GINT64_FORMAT, pos);

	g_file_info_set_attribute_string (info, METADATA_POSITION, value);

	g_file_set_attributes_from_info (
			file,
			info,
			G_FILE_QUERY_INFO_NONE,
			NULL,
			&error);
	
	if (error) {
		/* There are valid cases were setting attributes is not
		 * possible, e.g. in sandboxed environments, containers etc.
		 * Use G_LOG_LEVEL_INFO because other log levels go to stderr
		 * and might result in failed tests. */
		g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_INFO,
			          "MESSAGE", "Position not saved: %s", error->message);
		g_error_free (error);
	} else {
		g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_INFO,
			          "MESSAGE", "Position saved");
	}

	g_object_unref (file);
	g_object_unref (info);
}

static void
metadata_get_position (PtPlayer *player)
{
	/* Queries position stored in metadata from file.
	   Sets position to that value or to 0 */

	GError	  *error = NULL;
	GFile	  *file = NULL;
	GFileInfo *info;
	gchar	  *value = NULL;
	gint64     pos = 0;

	file = pt_player_get_file (player);
	if (!file)
		return;

	info = g_file_query_info (file, METADATA_POSITION, G_FILE_QUERY_INFO_NONE, NULL, &error);
	if (error) {
		g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
			          "MESSAGE", "Metadata not retrieved: %s", error->message);
		g_error_free (error);
		g_object_unref (file);
		return;
	}

	value = g_file_info_get_attribute_as_string (info, METADATA_POSITION);
	if (value) {
		pos = g_ascii_strtoull (value, NULL, 0);
		g_free (value);

		if (pos > 0) {
			g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_INFO,
					  "MESSAGE", "Metadata: got position");
		}
	}
		
	/* Set either to position or 0 */
	pt_player_jump_to_position (player, pos);

	g_object_unref (file);
	g_object_unref (info);
}

static void
remove_message_bus (PtPlayer *player)
{
	if (player->priv->bus_watch_id > 0) {
		g_source_remove (player->priv->bus_watch_id);
		player->priv->bus_watch_id = 0;
	}
}

static void
add_message_bus (PtPlayer *player)
{
	GstBus *bus;

	remove_message_bus (player);
	bus = gst_pipeline_get_bus (GST_PIPELINE (player->priv->play));
	player->priv->bus_watch_id = gst_bus_add_watch (bus, bus_call, player);
	gst_object_unref (bus);
}

static gboolean
bus_call (GstBus     *bus,
          GstMessage *msg,
          gpointer    data)
{
	PtPlayer *player = (PtPlayer *) data;
	gint64    pos;

	switch (GST_MESSAGE_TYPE (msg)) {
	case GST_MESSAGE_SEGMENT_DONE:
		/* From GStreamer documentation:
		   When performing a segment seek: after the playback of the segment completes,
		   no EOS will be emitted by the element that performed the seek, but a
		   GST_MESSAGE_SEGMENT_DONE message will be posted on the bus by the element. */

	case GST_MESSAGE_EOS:
		/* We rely on that SEGMENT_DONE/EOS is exactly at the end of segment.
		   This works in Debian 8, but not Ubuntu 16.04 (because of newer GStreamer?)
		   with mp3s. Jump to the real end. */
		pt_player_query_position (player, &pos);
		if (pos != player->priv->segend) {
			g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,
					  "MESSAGE", "Correcting EOS position: %" G_GINT64_FORMAT " ms",
					  GST_TIME_AS_MSECONDS (player->priv->segend - pos));
			pt_player_seek (player, player->priv->segend);
		}
		g_signal_emit_by_name (player, "end-of-stream");
		break;

	case GST_MESSAGE_DURATION_CHANGED: {
		gint64 dur;
		gst_element_query_duration (player->priv->play, GST_FORMAT_TIME, &dur);
		g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "MESSAGE",
				  "New duration: %" G_GINT64_FORMAT, dur);
		if (player->priv->dur != player->priv->segend) {
			player->priv->dur = dur;
		} else {
			player->priv->dur = player->priv->segend = dur;
			pt_player_query_position (player, &pos);
			pt_player_seek (player, pos);
		}
		break;
		}
	case GST_MESSAGE_ERROR: {
		gchar  *debug;
		GError *error;

		gst_message_parse_error (msg, &error, &debug);

		/* Error is returned. Log the message here at level DEBUG,
		   as higher levels will abort tests. */

		g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,
				  "MESSAGE", "Error from element %s: %s", GST_OBJECT_NAME (msg->src), error->message);
		g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,
				  "MESSAGE", "Debugging info: %s", (debug) ? debug : "none");
		g_free (debug);

		g_signal_emit_by_name (player, "error", error);
		g_error_free (error);
		pt_player_clear (player);
		break;
		}

	case GST_MESSAGE_ELEMENT:
		if (g_strcmp0 (GST_MESSAGE_SRC_NAME (msg), "sphinx") != 0)
			break;
		const GstStructure *st = gst_message_get_structure (msg);
		if (g_value_get_boolean (gst_structure_get_value (st, "final"))) {
			g_signal_emit_by_name (player, "asr-final",
				g_value_get_string (
					gst_structure_get_value (st, "hypothesis")));
		} else {
			g_signal_emit_by_name (player, "asr-hypothesis",
				g_value_get_string (
					gst_structure_get_value (st, "hypothesis")));
		}
		break;

	default:
		break;
	}

	return TRUE;
}


/* -------------------------- opening files --------------------------------- */

/**
 * pt_player_open_uri:
 * @player: a #PtPlayer
 * @uri: the URI of the file
 *
 * Opens a local audio file for playback. It doesn’t work with videos or streams.
 * Only one file can be open at a time, playlists are not supported by the
 * backend. Opening a new file will close the previous one.
 *
 * When closing a file or on object destruction PtPlayer tries to write the
 * last position into the file’s metadata. On opening a file it reads the
 * metadata and jumps to the last known position if found.
 *
 * The player is set to the paused state and ready for playback. To start
 * playback use @pt_player_play().
 *
 * This operation blocks until it is finished. It returns TRUE on success or
 * FALSE on error. Errors are emitted async via #PtPlayer::error signal.
 *
 * Return value: TRUE if successful, otherwise FALSE
 *
 * Since: 2.0
 */
gboolean
pt_player_open_uri (PtPlayer *player,
                    gchar    *uri)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), FALSE);
	g_return_val_if_fail (uri != NULL, FALSE);

	/* If we had an open file before, remember its position */
	metadata_save_position (player);

	/* Reset any open streams */
	pt_player_clear (player);
	player->priv->dur = -1;

	g_object_set (G_OBJECT (player->priv->play), "uri", uri, NULL);

	/* setup message handler */
	add_message_bus (player);

	pt_player_pause (player);

	/* Block until state changed, return on failure */
	if (gst_element_get_state (player->priv->play,
				   NULL, NULL,
				   GST_CLOCK_TIME_NONE) == GST_STATE_CHANGE_FAILURE)
		return FALSE;

	gint64 dur = 0;
	gst_element_query_duration (player->priv->play, GST_FORMAT_TIME, &dur);
	player->priv->dur = player->priv->segend = dur;
	player->priv->segstart = 0;
	g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "MESSAGE",
			  "Initial duration: %" G_GINT64_FORMAT, dur);

	metadata_get_position (player);
	return TRUE;
}


/* ------------------------- Basic controls --------------------------------- */

/**
 * pt_player_pause:
 * @player: a #PtPlayer
 *
 * Sets the player to the paused state, meaning it stops playback and doesn’t
 * change position. To resume playback use @pt_player_play().
 *
 * Since: 1.4
 */
void
pt_player_pause (PtPlayer *player)
{
	g_return_if_fail (PT_IS_PLAYER (player));

	gst_element_set_state (player->priv->play, GST_STATE_PAUSED);
}

/**
 * pt_player_pause_and_rewind:
 * @player: a #PtPlayer
 *
 * Like @pt_player_pause(), additionally rewinds the value of
 * #PtPlayer:pause in milliseconds.
 *
 * Since: 1.6
 */
void
pt_player_pause_and_rewind (PtPlayer *player)
{
	pt_player_pause (player);
	pt_player_jump_relative (player, player->priv->pause * -1);
}

/**
 * pt_player_get_pause:
 * @player: a #PtPlayer
 *
 * Return value: time to rewind on pause in milliseconds
 *
 * Since: 1.6
 */
gint
pt_player_get_pause (PtPlayer *player)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), 0);

	return player->priv->pause;
}

/**
 * pt_player_play:
 * @player: a #PtPlayer
 *
 * Starts playback at the defined speed until it reaches the end of stream (or
 * the end of the selection). If the current position is at the end, playback
 * will start from the beginning of stream or selection.
 *
 * Since: 1.4
 */
void
pt_player_play (PtPlayer *player)
{
	g_return_if_fail (PT_IS_PLAYER (player));

	gint64 pos;
	gint64 start, end;
	gboolean selection;

	if (player->priv->wv) {
		/* If there is a selection, play it */
		g_object_get (player->priv->wv,
			      "selection-start", &start,
			      "selection-end", &end,
			      "has-selection", &selection,
			      NULL);

		if (selection) {
			/* Note: changes position if outside selection */
			pt_player_set_selection (player, start, end);
		}
	} else {
		selection = pt_player_selection_active (player);
	}

	if (!pt_player_query_position (player, &pos))
		return;

	if (pos == player->priv->segend) {
		if ((selection && player->priv->repeat_selection)
		    || (!selection && player->priv->repeat_all))
			pt_player_seek (player, player->priv->segstart);
		else
			pt_player_jump_relative (player, player->priv->pause * -1);
	}

	gst_element_set_state (player->priv->play, GST_STATE_PLAYING);
}

/**
 * pt_player_play_pause:
 * @player: a #PtPlayer
 *
 * Toggles between playback and pause, rewinds on pause.
 *
 * Since: 1.6
 */
void
pt_player_play_pause (PtPlayer *player)
{
	g_return_if_fail (PT_IS_PLAYER (player));

	GstState state;

	gst_element_get_state (
		player->priv->play,
		&state, NULL,
		GST_CLOCK_TIME_NONE);

	switch (state) {
	case GST_STATE_NULL:
		/* fall through */
	case GST_STATE_PAUSED:
		pt_player_play (player);
		break;
	case GST_STATE_PLAYING:
		pt_player_pause_and_rewind (player);
	case GST_STATE_VOID_PENDING:
		/* fall through */
	case GST_STATE_READY:
		/* don’t know what to do */
		;
	}

	g_signal_emit_by_name (player, "play-toggled");
}


/**
 * pt_player_set_selection:
 * @player: a #PtPlayer
 * @start: selection start time in milliseconds
 * @end: selection end time in milliseconds
 *
 * Set a selection. If the current position is outside the selection, it will
 * be set to the selection’s start position, otherwise the current position is
 * not changed. Playing will end at the stop position and it’s not possible to
 * jump out of the selection until it is cleared with #pt_player_clear_selection.
 *
 * Since: 1.5
 */
void
pt_player_set_selection (PtPlayer *player,
                         gint64    start,
                         gint64    end)
{
	g_return_if_fail (PT_IS_PLAYER (player));
	g_return_if_fail (start < end);

	player->priv->segstart = GST_MSECOND * start;
	player->priv->segend = GST_MSECOND * end;

	gint64 pos;

	if (!pt_player_query_position (player, &pos))
		return;

	if (pos < player->priv->segstart || pos > player->priv->segend)
		pos = player->priv->segstart;

	pt_player_seek (player, pos);
}

/**
 * pt_player_clear_selection:
 * @player: a #PtPlayer
 *
 * Clear and reset any selection.
 *
 * Since: 1.5
 */
void
pt_player_clear_selection (PtPlayer *player)
{
	g_return_if_fail (PT_IS_PLAYER (player));

	gint64 pos;

	if (!pt_player_query_position (player, &pos))
		return;

	player->priv->segstart = 0;
	player->priv->segend = player->priv->dur;

	pt_player_seek (player, pos);
}

/**
 * pt_player_selection_active:
 * @player: a #PtPlayer
 *
 * Return value: TRUE if there is a selection
 *
 * Since: 1.6
 */
gboolean
pt_player_selection_active (PtPlayer *player)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), FALSE);

	return !(player->priv->segstart == 0 && player->priv->segend == player->priv->dur);
}

/**
 * pt_player_rewind:
 * @player: a #PtPlayer
 * @speed: the speed
 *
 * Rewinds at the given speed. @speed accepts positive as well as negative
 * values and normalizes them to play backwards.
 *
 * <note><para>Note that depending on the file/stream format this works more
 * or less good.</para></note>
 *
 * Since: 1.5
 */
void
pt_player_rewind (PtPlayer *player,
                  gdouble   speed)
{
	g_return_if_fail (PT_IS_PLAYER (player));
	g_return_if_fail (speed != 0);

	gint64 pos;

	if (!pt_player_query_position (player, &pos))
		return;

	if (speed > 0)
		speed = speed * -1;

	gst_element_seek (
		player->priv->play,
		speed,
		GST_FORMAT_TIME,
		GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_TRICKMODE,
		GST_SEEK_TYPE_SET,
		player->priv->segstart,
		GST_SEEK_TYPE_SET,
		pos);

	/* Block until state changed */
	gst_element_get_state (
		player->priv->play,
		NULL, NULL,
		GST_CLOCK_TIME_NONE);

	pt_player_play (player);
}

/**
 * pt_player_fast_forward:
 * @player: a #PtPlayer
 * @speed: the speed
 *
 * Play fast forward at the given speed.
 *
 * Since: 1.5
 */
void
pt_player_fast_forward (PtPlayer *player,
                        gdouble   speed)
{
	g_return_if_fail (PT_IS_PLAYER (player));
	g_return_if_fail (speed > 0);

	gint64 pos;

	if (!pt_player_query_position (player, &pos))
		return;

	gst_element_seek (
		player->priv->play,
		speed,
		GST_FORMAT_TIME,
		GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_TRICKMODE,
		GST_SEEK_TYPE_SET,
		pos,
		GST_SEEK_TYPE_SET,
		player->priv->segend);

	/* Block until state changed */
	gst_element_get_state (
		player->priv->play,
		NULL, NULL,
		GST_CLOCK_TIME_NONE);

	pt_player_play (player);
}

/* -------------------- Positioning, speed, volume -------------------------- */

/**
 * pt_player_jump_relative:
 * @player: a #PtPlayer
 * @milliseconds: time in milliseconds to jump
 *
 * Skips @milliseconds in stream. A positive value means jumping ahead. If the
 * resulting position would be beyond the end of stream (or selection), it goes
 * to the end of stream (or selection). A negative value means jumping back.
 * If the resulting position would be negative (or before the selection), it
 * jumps to position 0:00 (or to the start of the selection).
 *
 * Since: 1.4
 */
void
pt_player_jump_relative (PtPlayer *player,
                         gint      milliseconds)
{
	g_return_if_fail (PT_IS_PLAYER (player));
	if (milliseconds == 0)
		return;

	gint64 pos, new;

	if (!pt_player_query_position (player, &pos))
		return;
	
	new = pos + GST_MSECOND * (gint64) milliseconds;
	g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,
		          "MESSAGE", "Jump relative: dur = %" G_GINT64_FORMAT, player->priv->dur);
	g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,
		          "MESSAGE", "Jump relative: new = %" G_GINT64_FORMAT, new);

	if (new > player->priv->segend)
		new = player->priv->segend;

	if (new < player->priv->segstart)
		new = player->priv->segstart;

	pt_player_seek (player, new);
}

/**
 * pt_player_jump_back:
 * @player: a #PtPlayer
 *
 * Jumps back the value of #PtPlayer:back.
 *
 * Since: 1.6
 */
void
pt_player_jump_back (PtPlayer *player)
{
	gint back;

	back = player->priv->back;
	if (back > 0)
		back = back * -1;
	pt_player_jump_relative (player, back);
	g_signal_emit_by_name (player, "jumped-back");
}

/**
 * pt_player_jump_forward:
 * @player: a #PtPlayer
 *
 * Jumps forward the value of #PtPlayer:forward.
 *
 * Since: 1.6
 */
void
pt_player_jump_forward (PtPlayer *player)
{
	pt_player_jump_relative (player, player->priv->forward);
	g_signal_emit_by_name (player, "jumped-forward");
}

/**
 * pt_player_get_back:
 * @player: a #PtPlayer
 *
 * Return value: time to jump back in milliseconds
 *
 * Since: 1.6
 */
gint
pt_player_get_back (PtPlayer *player)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), 0);

	return player->priv->back;
}

/**
 * pt_player_get_forward:
 * @player: a #PtPlayer
 *
 * Return value: time to jump forward in milliseconds
 *
 * Since: 1.6
 */
gint
pt_player_get_forward (PtPlayer *player)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), 0);

	return player->priv->forward;
}

/**
 * pt_player_jump_to_position:
 * @player: a #PtPlayer
 * @milliseconds: position in milliseconds
 *
 * Jumps to a given position in stream. The position is given in @milliseconds
 * starting from position 0:00. A position beyond the duration of stream (or
 * outside the selection) is ignored.
 *
 * Since: 1.4
 */
void
pt_player_jump_to_position (PtPlayer *player,
                            gint      milliseconds)
{
	g_return_if_fail (PT_IS_PLAYER (player));

	gint64 pos;

	pos = GST_MSECOND * (gint64) milliseconds;

	if (pos < 0) {
		g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,
				  "MESSAGE", "Jump to position failed: negative value");
		return;
	}

	/* TODO on opening a new file and jumping to the initial position,
	 * sometimes there is no duration yet and the jump is not done. */

	if (pos > player->priv->segend || pos < player->priv->segstart) {
		g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,
				  "MESSAGE", "Jump to position failed: start = %" G_GINT64_FORMAT, player->priv->segstart);
		g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,
				  "MESSAGE", "Jump to position failed: pos   = %" G_GINT64_FORMAT, pos);
		g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,
				  "MESSAGE", "Jump to position failed: end   = %" G_GINT64_FORMAT, player->priv->segend);
		return;
	}

	pt_player_seek (player, pos);
}

/**
 * pt_player_jump_to_permille:
 * @player: a #PtPlayer
 * @permille: scale position between 0 and 1000
 *
 * This is used for scale widgets. Start of stream is at 0, end of stream is
 * at 1000. This will jump to the given position. If your widget uses a different
 * scale, it’s up to you to convert it to 1/1000. Values beyond 1000 are not
 * allowed, values outside the selection are ignored.
 *
 * Since: 1.4
 */
void
pt_player_jump_to_permille (PtPlayer *player,
                            guint     permille)
{
	g_return_if_fail (PT_IS_PLAYER (player));
	g_return_if_fail (permille <= 1000);

	gint64 new;

	new = player->priv->dur * (gint64) permille / 1000;
	if (new > player->priv->segend || new < player->priv->segstart)
		return;

	pt_player_seek (player, new);
}

/**
 * pt_player_get_permille:
 * @player: a #PtPlayer
 *
 * This is used for scale widgets. If the scale has to synchronize with the
 * current position in stream, this gives the position on a scale between 0 and
 * 1000.
 *
 * Failure in querying the position returns -1.
 *
 * Return value: a scale position between 0 and 1000 or -1 on failure
 *
 * Since: 1.4
 */
gint
pt_player_get_permille (PtPlayer *player)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), -1);

	gint64 pos;

	if (!pt_player_query_position (player, &pos))
		return -1;

	return (gfloat) pos / (gfloat) player->priv->dur * 1000;
}

/**
 * pt_player_set_speed:
 * @player: a #PtPlayer
 * @speed: speed
 *
 * Sets the speed of playback in the paused state as well as during playback.
 * Normal speed is 1.0, everything above that is faster, everything below slower.
 * A speed of 0 is not allowed, use pt_player_pause() instead.
 * Recommended speed is starting from 0.5 as quality is rather poor below that.
 * Parlatype doesn’t change the pitch during slower or faster playback.
 *
 * Since: 1.4
 */
void
pt_player_set_speed (PtPlayer *player,
                     gdouble   speed)
{
	g_return_if_fail (PT_IS_PLAYER (player));
	g_return_if_fail (speed > 0);

	gint64 pos;

	player->priv->speed = speed;

	/* on object construction there is no pipeline yet */
	if (player->priv->play == NULL)
		return;

	if (pt_player_query_position (player, &pos))
		pt_player_seek (player, pos);

	g_object_notify_by_pspec (G_OBJECT (player),
				  obj_properties[PROP_SPEED]);
}

/**
 * pt_player_set_volume:
 * @player: a #PtPlayer
 * @volume: volume
 *
 * Sets the volume on a scale between 0 and 1. Instead of using this method
 * you could set the "volume" property.
 *
 * Since: 1.4
 */
void
pt_player_set_volume (PtPlayer *player,
                      gdouble   volume)
{
	g_return_if_fail (PT_IS_PLAYER (player));
	g_return_if_fail (volume >= 0 && volume <= 1);

	player->priv->volume = volume;

	if (player->priv->play)
		gst_stream_volume_set_volume (GST_STREAM_VOLUME (player->priv->play),
			                      GST_STREAM_VOLUME_FORMAT_CUBIC,
			                      volume);

	g_object_notify_by_pspec (G_OBJECT (player),
				  obj_properties[PROP_VOLUME]);
}

/**
 * pt_player_mute_volume:
 * @player: a #PtPlayer
 * @mute: a gboolean
 *
 * Mute the player (with TRUE) or set it back to normal volume (with FALSE).
 * This remembers the volume level, so you don’t have to keep track of the old value.
 *
 * Since: 1.4
 */
void
pt_player_mute_volume (PtPlayer *player,
                       gboolean  mute)
{
	g_return_if_fail (PT_IS_PLAYER (player));

	gst_stream_volume_set_mute (GST_STREAM_VOLUME (player->priv->play), mute);
}

/**
 * pt_player_get_position:
 * @player: a #PtPlayer
 *
 * Returns the current position in stream.
 *
 * Return value: position in milliseconds or -1 on failure
 *
 * Since: 1.5
 */
gint64
pt_player_get_position (PtPlayer *player)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), -1);

	gint64 time;

	if (!pt_player_query_position (player, &time))
		return -1;

	return GST_TIME_AS_MSECONDS (time);
}

/**
 * pt_player_get_duration:
 * @player: a #PtPlayer
 *
 * Returns the duration of stream.
 *
 * Return value: duration in milliseconds
 *
 * Since: 1.5
 */
gint64
pt_player_get_duration (PtPlayer *player)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), -1);

	return GST_TIME_AS_MSECONDS (player->priv->dur);
}


/* --------------------- Other widgets -------------------------------------- */

static void
wv_update_cursor (PtPlayer *player)
{
	g_object_set (player->priv->wv,
		      "playback-cursor",
		      pt_player_get_position (player),
		      NULL);
}

static void
wv_selection_changed_cb (GtkWidget *widget,
		         PtPlayer  *player)
{
	/* Selection changed in Waveviewer widget:
	   - if we are not playing a selection: ignore it
	   - if we are playing a selection and we are still in the selection:
	     update selection
	   - if we are playing a selection and the new one is somewhere else:
	     stop playing the selection */

	gint64 start, end, pos;
	if (pt_player_selection_active (player)) {
		if (!pt_player_query_position (player, &pos))
			return;
		g_object_get (player->priv->wv,
			      "selection-start", &start,
			      "selection-end", &end,
			      NULL);
		if (start <= pos && pos <= end) {
			pt_player_set_selection (player, start, end);
		} else {
			pt_player_clear_selection (player);
		}
	}
}

static void
wv_cursor_changed_cb (PtWaveviewer *wv,
                      gint64        pos,
                      PtPlayer     *player)
{
	/* user changed cursor position */

	pt_player_jump_to_position (player, pos);
	wv_update_cursor (player);
	pt_waveviewer_set_follow_cursor (wv, TRUE);
}

static void
wv_play_toggled_cb (GtkWidget *widget,
                    PtPlayer  *player)
{
	pt_player_play_pause (player);
}

/**
 * pt_player_connect_waveviewer:
 * @player: a #PtPlayer
 * @wv: a #PtWaveviewer
 *
 * Connect a #PtWaveviewer. The #PtPlayer will monitor selections made in the
 * #PtWaveviewer and act accordingly.
 *
 * Since: 1.6
 */
void
pt_player_connect_waveviewer (PtPlayer     *player,
                              PtWaveviewer *wv)
{
	player->priv->wv = wv;
	g_signal_connect (player->priv->wv,
			"selection-changed",
			G_CALLBACK (wv_selection_changed_cb),
			player);

	g_signal_connect (player->priv->wv,
			"cursor-changed",
			G_CALLBACK (wv_cursor_changed_cb),
			player);

	g_signal_connect (player->priv->wv,
			"play-toggled",
			G_CALLBACK (wv_play_toggled_cb),
			player);
}




/* --------------------- File utilities ------------------------------------- */

/**
 * pt_player_get_uri:
 * @player: a #PtPlayer
 *
 * Returns the URI of the currently open file or NULL if it can’t be determined.
 *
 * Return value: (transfer full): the uri
 *
 * Since: 1.4
 */
gchar*
pt_player_get_uri (PtPlayer *player)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), NULL);

	gchar *uri = NULL;
	g_object_get (G_OBJECT (player->priv->play), "current_uri", &uri, NULL);
	return uri;
}

/**
 * pt_player_get_filename:
 * @player: a #PtPlayer
 *
 * Returns the display name of the currently open file or NULL if it can’t be determined.
 *
 * Return value: (transfer full): the file name
 *
 * Since: 1.4
 */
gchar*
pt_player_get_filename (PtPlayer *player)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), NULL);

	GError	    *error = NULL;
	const gchar *filename = NULL;
	GFile       *file = NULL;
	GFileInfo   *info = NULL;
	gchar	      *result;

	file = pt_player_get_file (player);

	if (file)
		info = g_file_query_info (
				file,
				G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
				G_FILE_QUERY_INFO_NONE,
				NULL,
				&error);
	else
		return NULL;

	if (error) {
		g_log_structured (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
			          "MESSAGE", "File attributes not retrieved: %s", error->message);
		g_error_free (error);
		g_object_unref (file);
		return NULL;
	}		
	
	filename = g_file_info_get_attribute_string (
				info,
				G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);
	
	if (filename)
		result = g_strdup (filename);
	else
		result = NULL;

	if (info)
		g_object_unref (info);
	if (file)
		g_object_unref (file);

	return result;
}

/* --------------------- Time strings and timestamps ------------------------ */

/**
 * pt_player_get_time_string:
 * @time: time in milliseconds to converse
 * @duration: duration of stream in milliseconds (max time)
 * @precision: a #PtPrecisionType
 *
 * Returns the given time as a string for display to the user. Format type is
 * determined by @duration, e.g. if duration is long format, it returns a string
 * in long format, too.
 *
 * Return value: (transfer full): the time string
 *
 * Since: 1.4
 */
gchar*
pt_player_get_time_string (gint            time,
                           gint            duration,
                           PtPrecisionType precision)
{
	/* Don’t assert time <= duration because duration is not exact */

	g_return_val_if_fail (precision < PT_PRECISION_INVALID, NULL);

	gchar *result;
	gint   h, m, s, ms, mod;

	h = time / 3600000;
	mod = time % 3600000;
	m = mod / 60000;
	ms = time % 60000;
	s = ms / 1000;
	ms = ms % 1000;

	/* Short or long format depends on total duration */
	if (duration >= ONE_HOUR) {
		switch (precision) {
		case PT_PRECISION_SECOND:
		/* Translators: This is a time format, like "2:05:30" for 2
		   hours, 5 minutes, and 30 seconds. You may change ":" to
		   the separator that your locale uses or use "%Id" instead
		   of "%d" if your locale uses localized digits. */
			result = g_strdup_printf (C_("long time format", "%d:%02d:%02d"), h, m, s);
			break;
		case PT_PRECISION_SECOND_10TH:
		/* Translators: This is a time format, like "2:05:30.1" for 2
		   hours, 5 minutes, 30 seconds, and 1 tenthsecond. You may
		   change ":" or "." to the separator that your locale uses or
		   use "%Id" instead of "%d" if your locale uses localized digits. */
			result = g_strdup_printf (C_("long time format, 1 digit", "%d:%02d:%02d.%d"), h, m, s, ms / 100);
			break;
		case PT_PRECISION_SECOND_100TH:
		/* Translators: This is a time format, like "2:05:30.12" for 2
		   hours, 5 minutes, 30 seconds, and 12 hundrethseconds. You may
		   change ":" or "." to the separator that your locale uses or
		   use "%Id" instead of "%d" if your locale uses localized digits. */
			result = g_strdup_printf (C_("long time format, 2 digits", "%d:%02d:%02d.%02d"), h, m, s, ms / 10);
			break;
		default:
			g_return_val_if_reached (NULL);
			break;
		}

		return result;
	}

	if (duration >= TEN_MINUTES) {
		switch (precision) {
		case PT_PRECISION_SECOND:
		/* Translators: This is a time format, like "05:30" for
		   5 minutes, and 30 seconds. You may change ":" to
		   the separator that your locale uses or use "%I02d" instead
		   of "%02d" if your locale uses localized digits. */
			result = g_strdup_printf (C_("short time format", "%02d:%02d"), m, s);
			break;
		case PT_PRECISION_SECOND_10TH:
		/* Translators: This is a time format, like "05:30.1" for
		   5 minutes, 30 seconds, and 1 tenthsecond. You may change
		   ":" or "." to the separator that your locale uses or
		   use "%Id" instead of "%d" if your locale uses localized digits. */
			result = g_strdup_printf (C_("short time format, 1 digit", "%02d:%02d.%d"), m, s, ms / 100);
			break;
		case PT_PRECISION_SECOND_100TH:
		/* Translators: This is a time format, like "05:30.12" for
		   5 minutes, 30 seconds, and 12 hundrethseconds. You may change
		   ":" or "." to the separator that your locale uses or
		   use "%Id" instead of "%d" if your locale uses localized digits. */
			result = g_strdup_printf (C_("short time format, 2 digits", "%02d:%02d.%02d"), m, s, ms / 10);
			break;
		default:
			g_return_val_if_reached (NULL);
			break;
		}
	} else {
		switch (precision) {
		case PT_PRECISION_SECOND:
		/* Translators: This is a time format, like "5:30" for
		   5 minutes, and 30 seconds. You may change ":" to
		   the separator that your locale uses or use "%Id" instead
		   of "%d" if your locale uses localized digits. */
			result = g_strdup_printf (C_("shortest time format", "%d:%02d"), m, s);
			break;
		case PT_PRECISION_SECOND_10TH:
		/* Translators: This is a time format, like "05:30.1" for
		   5 minutes, 30 seconds, and 1 tenthsecond. You may change
		   ":" or "." to the separator that your locale uses or
		   use "%Id" instead of "%d" if your locale uses localized digits. */
			result = g_strdup_printf (C_("shortest time format, 1 digit", "%d:%02d.%d"), m, s, ms / 100);
			break;
		case PT_PRECISION_SECOND_100TH:
		/* Translators: This is a time format, like "05:30.12" for
		   5 minutes, 30 seconds, and 12 hundrethseconds. You may change
		   ":" or "." to the separator that your locale uses or
		   use "%Id" instead of "%d" if your locale uses localized digits. */
			result = g_strdup_printf (C_("shortest time format, 2 digits", "%d:%02d.%02d"), m, s, ms / 10);
			break;
		default:
			g_return_val_if_reached (NULL);
			break;
		}
	}

	return result;	
}

/**
 * pt_player_get_current_time_string:
 * @player: a #PtPlayer
 * @precision: a #PtPrecisionType
 *
 * Returns the current position of the stream as a string for display to the user.
 *
 * If the current position can not be determined, NULL is returned.
 *
 * Return value: (transfer full): the time string
 *
 * Since: 1.4
 */
gchar*
pt_player_get_current_time_string (PtPlayer        *player,
                                   PtPrecisionType  precision)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), NULL);
	g_return_val_if_fail (precision < PT_PRECISION_INVALID, NULL);

	gint64 time;

	if (!pt_player_query_position (player, &time))
		return NULL;

	return pt_player_get_time_string (
			GST_TIME_AS_MSECONDS (time),
			GST_TIME_AS_MSECONDS (player->priv->dur),
			precision);
}

/**
 * pt_player_get_duration_time_string:
 * @player: a #PtPlayer
 * @precision: a #PtPrecisionType
 *
 * Returns the duration of the stream as a string for display to the user.
 *
 * If the duration can not be determined, NULL is returned.
 *
 * Return value: (transfer full): the time string
 *
 * Since: 1.4
 */
gchar*
pt_player_get_duration_time_string (PtPlayer        *player,
                                    PtPrecisionType  precision)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), NULL);
	g_return_val_if_fail (precision < PT_PRECISION_INVALID, NULL);

	return pt_player_get_time_string (
			GST_TIME_AS_MSECONDS (player->priv->dur),
			GST_TIME_AS_MSECONDS (player->priv->dur),
			precision);
}

/**
 * pt_player_get_timestamp_for_time:
 * @player: a #PtPlayer
 * @time: the time in milliseconds
 * @duration: duration in milliseconds
 *
 * Returns the timestamp for the given time as a string. Duration is needed
 * for some short time formats, the resulting timestamp format depends on
 * whether duration is less than one hour or more than (including) an hour
 * (3600000 milliseconds).
 *
 * The format of the timestamp can be influenced with
 * #PtPlayer:timestamp-precision, #PtPlayer:timestamp-fixed,
 * #PtPlayer:timestamp-fraction-sep and #PtPlayer:timestamp-delimiter.
 *
 * Return value: (transfer full): the timestamp
 *
 * Since: 1.6
 */
gchar*
pt_player_get_timestamp_for_time (PtPlayer *player,
                                  gint      time,
                                  gint      duration)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), NULL);

	gint   h, m, s, ms, mod, fraction;
	gchar *timestamp = NULL;

	/* This is the same code as in pt_player_get_timestring() */
	h = time / 3600000;
	mod = time % 3600000;
	m = mod / 60000;
	ms = time % 60000;
	s = ms / 1000;
	ms = ms % 1000;
	switch (player->priv->timestamp_precision) {
	case PT_PRECISION_SECOND:
		fraction = -1;
		break;
	case PT_PRECISION_SECOND_10TH:
		fraction = ms / 100;
		break;
	case PT_PRECISION_SECOND_100TH:
		fraction = ms / 10;
		break;
	default:
		g_return_val_if_reached (NULL);
		break;
	}

	if (player->priv->timestamp_fixed) {
		if (fraction >= 0) {
			if (player->priv->timestamp_precision == PT_PRECISION_SECOND_10TH) {
				timestamp = g_strdup_printf ("%s%02d:%02d:%02d%s%d%s", player->priv->timestamp_left, h, m, s, player->priv->timestamp_sep, fraction, player->priv->timestamp_right);
			} else {
				timestamp = g_strdup_printf ("%s%02d:%02d:%02d%s%02d%s", player->priv->timestamp_left, h, m, s, player->priv->timestamp_sep, fraction, player->priv->timestamp_right);
			}
		} else {
			timestamp = g_strdup_printf ("%s%02d:%02d:%02d%s", player->priv->timestamp_left, h, m, s, player->priv->timestamp_right);
		}
	} else {
		if (fraction >= 0) {
			if (duration >= ONE_HOUR) {
				if (player->priv->timestamp_precision == PT_PRECISION_SECOND_10TH) {
					timestamp = g_strdup_printf ("%s%d:%02d:%02d%s%d%s", player->priv->timestamp_left, h, m, s, player->priv->timestamp_sep, fraction, player->priv->timestamp_right);
				} else {
					timestamp = g_strdup_printf ("%s%d:%02d:%02d%s%02d%s", player->priv->timestamp_left, h, m, s, player->priv->timestamp_sep, fraction, player->priv->timestamp_right);
				}
			} else {
				if (player->priv->timestamp_precision == PT_PRECISION_SECOND_10TH) {
					timestamp = g_strdup_printf ("%s%d:%02d%s%d%s", player->priv->timestamp_left, m, s, player->priv->timestamp_sep, fraction, player->priv->timestamp_right);
				} else {
					timestamp = g_strdup_printf ("%s%d:%02d%s%02d%s", player->priv->timestamp_left, m, s, player->priv->timestamp_sep, fraction, player->priv->timestamp_right);
				}
			}
		} else {
			if (duration >= ONE_HOUR) {
				timestamp = g_strdup_printf ("%s%d:%02d:%02d%s", player->priv->timestamp_left, h, m, s, player->priv->timestamp_right);
			} else {
				timestamp = g_strdup_printf ("%s%d:%02d%s", player->priv->timestamp_left, m, s, player->priv->timestamp_right);
			}
		}
	}

	return timestamp;
}

/**
 * pt_player_get_timestamp:
 * @player: a #PtPlayer
 *
 * Returns the current timestamp as a string. The format of the timestamp can
 * be influenced with #PtPlayer:timestamp-precision, #PtPlayer:timestamp-fixed,
 * #PtPlayer:timestamp-fraction-sep and #PtPlayer:timestamp-delimiter.
 *
 * If the current position can not be determined, NULL is returned.
 *
 * Return value: (transfer full): the timestamp
 *
 * Since: 1.4
 */
gchar*
pt_player_get_timestamp (PtPlayer *player)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), NULL);

	gint64 time;
	gint   duration;

	if (!pt_player_query_position (player, &time))
		return NULL;

	duration = GST_TIME_AS_MSECONDS (player->priv->dur);

	return pt_player_get_timestamp_for_time (player, GST_TIME_AS_MSECONDS (time), duration);
}

/**
 * pt_player_get_timestamp_position:
 * @player: a #PtPlayer
 * @timestamp: the timestamp
 * @check_duration: checking the timestamp’s validity also check duration
 *
 * Returns the time in milliseconds represented by the timestamp or -1 for
 * invalid timestamps.
 *
 * Return value: the time in milliseconds represented by the timestamp or -1
 * for invalid timestamps
 *
 * Since: 1.6
 */
gint
pt_player_get_timestamp_position (PtPlayer *player,
                                  gchar    *timestamp,
                                  gboolean  check_duration)
{
	gint       h, m, s, ms, result;
	gchar     *cmp; /* timestamp without delimiters */
	gboolean   long_format;
	gboolean   fraction;
	gchar    **split = NULL;
	guint      n_split;

	/* Check for formal validity */
	if (!g_regex_match_simple ("^[#|\\(|\\[]?[0-9][0-9]?:[0-9][0-9]:[0-9][0-9][\\.|\\-][0-9][0-9]?[#|\\)|\\]]?$", timestamp, 0, 0)
		&& !g_regex_match_simple ("^[#|\\(|\\[]?[0-9][0-9]?:[0-9][0-9][\\.|\\-][0-9][0-9]?[#|\\)|\\]]?$", timestamp, 0, 0)
		&& !g_regex_match_simple ("^[#|\\(|\\[]?[0-9][0-9]?:[0-9][0-9]:[0-9][0-9][#|\\)|\\]]?$", timestamp, 0, 0)
		&& !g_regex_match_simple ("^[#|\\(|\\[]?[0-9][0-9]?:[0-9][0-9][#|\\)|\\]]?$", timestamp, 0, 0)) {
		return -1;
	}

	/* Delimiters must match */
	if (g_str_has_prefix (timestamp, "#") && !g_str_has_suffix (timestamp, "#"))
		return -1;
	if (g_str_has_prefix (timestamp, "(") && !g_str_has_suffix (timestamp, ")"))
		return -1;
	if (g_str_has_prefix (timestamp, "[") && !g_str_has_suffix (timestamp, "]"))
		return -1;
	if (g_regex_match_simple ("^[0-9]", timestamp, 0, 0)) {
		if (!g_regex_match_simple ("[0-9]$", timestamp, 0, 0))
			return -1;
	}

	/* Remove delimiters */
	if (g_str_has_prefix (timestamp, "#")
			|| g_str_has_prefix (timestamp, "(")
			|| g_str_has_prefix (timestamp, "[")) {
		timestamp++;
		cmp = g_strdup_printf ("%.*s", (int)strlen (timestamp) -1, timestamp);
	} else {
		cmp = g_strdup (timestamp);
	}

	/* Determine format and split timestamp into h, m, s, ms */
	h = 0;
	ms = 0;

	long_format = g_regex_match_simple (":[0-9][0-9]:", cmp, 0, 0);
	fraction = !g_regex_match_simple (".*:[0-9][0-9]$", cmp, 0, 0);
	split = g_regex_split_simple ("[:|\\.|\\-]", cmp, 0, 0);
	g_free (cmp);
	if (!split)
		return -1;

	n_split = 2;
	if (long_format)
		n_split += 1;
	if (fraction)
		n_split += 1;
	if (n_split != g_strv_length (split)) {
		g_strfreev (split);
		return -1;
	}

	if (long_format) {
		h = (int)g_ascii_strtoull (split[0], NULL, 10);
		m = (int)g_ascii_strtoull (split[1], NULL, 10);
		s = (int)g_ascii_strtoull (split[2], NULL, 10);
		if (fraction) {
			ms = (int)g_ascii_strtoull (split[3], NULL, 10);
			if (strlen (split[3]) == 1)
				ms = ms * 100;
			else
				ms = ms * 10;
		}
	} else {
		m = (int)g_ascii_strtoull (split[0], NULL, 10);
		s = (int)g_ascii_strtoull (split[1], NULL, 10);
		if (fraction) {
			ms = (int)g_ascii_strtoull (split[2], NULL, 10);
			if (strlen (split[2]) == 1)
				ms = ms * 100;
			else
				ms = ms * 10;
		}
	}

	g_strfreev (split);
	
	/* Sanity check */
	if (s > 59 || m > 59)
		return -1;

	result = (h * 3600 + m * 60 + s) * 1000 + ms;

	if (check_duration) {
		if (GST_MSECOND * (gint64) result > player->priv->dur) {
			return -1;
		}
	}

	return result;
}

/**
 * pt_player_string_is_timestamp:
 * @player: a #PtPlayer
 * @timestamp: the string to be checked
 * @check_duration: whether timestamp’s time is less or equal stream’s duration
 *
 * Returns whether the given string is a valid timestamp. With @check_duration
 * FALSE it checks only for the formal validity of the timestamp. With
 * @check_duration TRUE the timestamp must be within the duration to be valid.
 *
 * See also pt_player_goto_timestamp() if you want to go to the timestamp’s
 * position immediately after.
 *
 * Return value: TRUE if the timestamp is valid, FALSE if not
 *
 * Since: 1.4
 */
gboolean
pt_player_string_is_timestamp (PtPlayer *player,
                               gchar    *timestamp,
                               gboolean  check_duration)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), FALSE);
	g_return_val_if_fail (timestamp != NULL, FALSE);

	return (pt_player_get_timestamp_position (player, timestamp, check_duration) != -1);
}

/**
 * pt_player_goto_timestamp:
 * @player: a #PtPlayer
 * @timestamp: the timestamp to go to
 *
 * Goes to the position of the timestamp. Returns false, if it’s not a
 * valid timestamp.
 *
 * Return value: TRUE on success, FALSE if the timestamp is not valid
 *
 * Since: 1.4
 */
gboolean
pt_player_goto_timestamp (PtPlayer *player,
                          gchar    *timestamp)
{
	g_return_val_if_fail (PT_IS_PLAYER (player), FALSE);
	g_return_val_if_fail (timestamp != NULL, FALSE);

	gint pos;

	pos = pt_player_get_timestamp_position (player, timestamp, TRUE);

	if (pos == -1)
		return FALSE;

	pt_player_jump_to_position (player, pos);
	return TRUE;
}

/* --------------------- Init and GObject management ------------------------ */

static void
pt_player_init (PtPlayer *player)
{
	gst_init (NULL, NULL);
	player->priv = pt_player_get_instance_private (player);
	player->priv->play = NULL;
	player->priv->timestamp_precision = PT_PRECISION_SECOND_10TH;
	player->priv->timestamp_fixed = FALSE;
	player->priv->wv = NULL;
	player->sphinx = NULL;
	player->priv->scaletempo = NULL;
	player->priv->play_bin = NULL;
	player->priv->sphinx_bin = NULL;
}

static gboolean
notify_volume_idle_cb (PtPlayer *player)
{
	gdouble vol;

	vol = gst_stream_volume_get_volume (GST_STREAM_VOLUME (player->priv->play),
	                                    GST_STREAM_VOLUME_FORMAT_CUBIC);
	player->priv->volume = vol;
	g_object_notify_by_pspec (G_OBJECT (player),
				  obj_properties[PROP_VOLUME]);
	return FALSE;
}

static void
vol_changed (GObject    *object,
             GParamSpec *pspec,
             PtPlayer   *player)
{
	/* This is taken from Totem’s bacon-video-widget.c
	   Changing the property immediately will crash, it has to be an idle source */

	guint id;
	id = g_idle_add ((GSourceFunc) notify_volume_idle_cb, player);
	g_source_set_name_by_id (id, "[parlatype] notify_volume_idle_cb");
}

static GstElement*
make_element (gchar   *factoryname,
              gchar   *name,
              GError **error)
{
	GstElement *result;

	result = gst_element_factory_make (factoryname, name);
	if (!result)
		g_set_error (error, GST_CORE_ERROR,
		             GST_CORE_ERROR_MISSING_PLUGIN,
			    _("Failed to load plugin “%s”."), factoryname);

	return result;
}

#define PROPAGATE_ERROR_NULL \
if (earlier_error != NULL) {\
	g_propagate_error (error, earlier_error);\
	return NULL;\
}

#define PROPAGATE_ERROR_FALSE \
if (earlier_error != NULL) {\
	g_propagate_error (error, earlier_error);\
	return FALSE;\
}

#ifdef HAVE_ASR
static GstElement*
create_sphinx_bin (PtPlayer  *player,
                   GError   **error)
{
	GError *earlier_error = NULL;

	/* Create gstreamer elements */
	GstElement *queue;
	GstElement *audioconvert;
	GstElement *audioresample;
	GstElement *fakesink;

	player->sphinx = G_OBJECT (make_element ("parlasphinx", "sphinx", &earlier_error));
	/* defined error propagation; skipping any cleanup */
	PROPAGATE_ERROR_NULL
	queue = make_element ("queue", "sphinx_queue", &earlier_error);
	PROPAGATE_ERROR_NULL
	audioconvert = make_element ("audioconvert", "audioconvert", &earlier_error);
	PROPAGATE_ERROR_NULL
	audioresample = make_element ("audioresample", "audioresample", &earlier_error);
	PROPAGATE_ERROR_NULL
	fakesink = make_element ("fakesink", "fakesink", &earlier_error);
	PROPAGATE_ERROR_NULL

	/* create audio output */
	GstElement *audio = gst_bin_new ("sphinx-audiobin");
	gst_bin_add_many (GST_BIN (audio), queue, audioconvert, audioresample, GST_ELEMENT (player->sphinx), fakesink, NULL);
	gst_element_link_many (queue, audioconvert, audioresample, GST_ELEMENT (player->sphinx), fakesink, NULL);

	/* create ghost pad for audiosink */
	GstPad *audiopad = gst_element_get_static_pad (queue, "sink");
	gst_element_add_pad (audio, gst_ghost_pad_new ("sink", audiopad));
	gst_object_unref (GST_OBJECT (audiopad));

	return audio;
}
#endif

static GstElement*
create_play_bin (PtPlayer  *player,
                 GError   **error)
{
	GError *earlier_error = NULL;

	/* Create gstreamer elements */
	GstElement *capsfilter;
	GstElement *audiosink;
	GstElement *queue;

	capsfilter = make_element ("capsfilter", "audiofilter", &earlier_error);
	/* defined error propagation; skipping any cleanup */
	PROPAGATE_ERROR_NULL
	audiosink = make_element ("autoaudiosink", "audiosink", &earlier_error);
	PROPAGATE_ERROR_NULL
	queue = make_element ("queue", "player_queue", &earlier_error);
	PROPAGATE_ERROR_NULL

	GstElement *audio = gst_bin_new ("player-audiobin");
	gst_bin_add_many (GST_BIN (audio), queue, capsfilter, audiosink, NULL);
	gst_element_link_many (queue, capsfilter, audiosink, NULL);

	/* create ghost pad for audiosink */
	GstPad *audiopad = gst_element_get_static_pad (queue, "sink");
	gst_element_add_pad (audio, gst_ghost_pad_new ("sink", audiopad));
	gst_object_unref (GST_OBJECT (audiopad));

	return audio;
}

/*

 .---------.    .-------------------------------------------------------------------------.
 | playbin |    | audio_bin                                                               |
 |         |    | .------.     .--------------------------------------------------------. |
 '---,-----'    | | tee  |     | play_bin                                               | |
     |          | |      |     | .--------.      .-------------.      .---------------. | |
     |          | |      |     | | queue  |      | capsfilter  |      | autoaudiosink | | |
     '------->  sink    src-> sink       src -> sink          src -> sink             | | |
    audio-sink  | |      |     | '--------'      '-------------'      '---------------' | |
    property    | |      |     '--------------------------------------------------------' |
                | |      |                                                                |
                | |      |     .--------------------------------------------------------. |
                | |      |     | sphinx_bin                                             | |
                | |      |     | .--------.                                .----------. | |
                | |      |     | | queue  |         (audioconvert          | fakesink | | |
                | |     src-> sink       src -> ...  audioresample ... -> sink        | | |
                | |      |     | '--------'          pocketsphinx)         '----------' | |
                | '------'     '--------------------------------------------------------' |
                |                                                                         |
                '-------------------------------------------------------------------------'

Note 1: audio_bin is needed. If the audio-sink property of playbin is set to the
        tee element, there is an internal data stream error.

Note 2: It doesn’t work if play_bin or sphinx_bin are added to audio_bin but are
        not linked! Either link it or remove it from bin!

Note 3: The original intent was to dynamically switch the tee element to either
        playback or ASR. Rethink this design, maybe tee element can be completely
        omitted. On the other hand, maybe they could be somehow synced to have
        audio and recognition in real time.

*/

static void
link_tee (GstPad     *tee_srcpad,
          GstElement *sink_bin)
{
	GstPad           *sinkpad;
	GstPadLinkReturn  r;

	sinkpad = gst_element_get_static_pad (sink_bin, "sink");
	g_assert_nonnull (sinkpad);
	r = gst_pad_link (tee_srcpad, sinkpad);
	g_assert (r == GST_PAD_LINK_OK);
	gst_object_unref (sinkpad);
}

static gboolean
pt_player_setup_pipeline (PtPlayer  *player,
                          GError   **error)
{
	GError *earlier_error = NULL;

	player->priv->play = make_element ("playbin", "play", &earlier_error);
	PROPAGATE_ERROR_FALSE
	player->priv->play_bin = create_play_bin (player, &earlier_error);
	PROPAGATE_ERROR_FALSE
#ifdef HAVE_ASR
	player->priv->sphinx_bin = create_sphinx_bin (player, &earlier_error);
	PROPAGATE_ERROR_FALSE
#endif
	player->priv->scaletempo = make_element ("scaletempo", "tempo", &earlier_error);
	PROPAGATE_ERROR_FALSE
	player->priv->tee = make_element ("tee", "tee", &earlier_error);
	PROPAGATE_ERROR_FALSE
	player->priv->tee_playpad = gst_element_get_request_pad (player->priv->tee, "src_%u");
	player->priv->tee_sphinxpad = gst_element_get_request_pad (player->priv->tee, "src_%u");

	player->priv->audio_bin = gst_bin_new ("audiobin");
	gst_bin_add (GST_BIN (player->priv->audio_bin), player->priv->tee);

	/* create ghost pad for audiosink */
	GstPad *audiopad = gst_element_get_static_pad (player->priv->tee, "sink");
	gst_element_add_pad (player->priv->audio_bin, gst_ghost_pad_new ("sink", audiopad));
	gst_object_unref (GST_OBJECT (audiopad));

	g_object_set (G_OBJECT (player->priv->play),
			"audio-sink", player->priv->audio_bin, NULL);

	/* This is responsible for syncing system volume with Parlatype volume.
	   Syncing is done only in Play state */
	g_signal_connect (G_OBJECT (player->priv->play),
			"notify::volume", G_CALLBACK (vol_changed), player);
	return TRUE;
}

static void
remove_element (GstBin *parent,
                gchar  *child_name)
{
	GstElement *child;
	child = gst_bin_get_by_name (parent, child_name);
	if (!child)
		return;

	/* removing dereferences removed element, we want to keep it */
	gst_object_ref (child);
	gst_bin_remove (parent, child);
}

static void
add_element (GstBin     *parent,
             GstElement *child,
             GstPad     *srcpad)
{
	GstElement *cmp;
	gchar      *child_name;

	child_name = gst_element_get_name (child);
	cmp = gst_bin_get_by_name (parent, child_name);
	g_free (child_name);
	if (cmp) {
		gst_object_unref (cmp);
		/* element is already in bin */
		return;
	}

	gst_bin_add (parent, child);
	link_tee (srcpad, child);
}

/**
 * pt_player_setup_sphinx:
 * @player: a #PtPlayer
 * @error: (nullable): return location for an error, or NULL
 *
 * Setup the GStreamer pipeline for automatic speech recognition using
 * CMU sphinx. This loads resources like language model and dictionary and
 * might take a few seconds.
 * There is no audio output in this mode. Connect to the
 * #PtPlayer::asr-hypothesis and/or #PtPlayer::asr-final signal to get
 * the results. Start recognition with pt_player_play().
 *
 * Return value: TRUE on success, FALSE if the pipeline could not be set up
 *
 * Since: 1.6
 */
gboolean
pt_player_setup_sphinx (PtPlayer  *player,
                        GError   **error)
{
	GError *earlier_error = NULL;

	if (!player->priv->play)
		pt_player_setup_pipeline (player, &earlier_error);
	PROPAGATE_ERROR_FALSE

	gint pos;
	pos = pt_player_get_position (player);

	pt_player_set_state_blocking (player, GST_STATE_NULL);

	remove_element (GST_BIN (player->priv->audio_bin), "player-audiobin");
	add_element (GST_BIN (player->priv->audio_bin),
			player->priv->sphinx_bin, player->priv->tee_sphinxpad);

	/* setting the "audio-filter" property unrefs the previous audio-filter! */
	gst_object_ref (player->priv->scaletempo);
	g_object_set (player->priv->play, "audio-filter", NULL, NULL);

	pt_player_set_state_blocking (player, GST_STATE_PAUSED);
	pt_player_jump_to_position (player, pos);

	return TRUE;
}

/**
 * pt_player_setup_player:
 * @player: a #PtPlayer
 * @error: (nullable): return location for an error, or NULL
 *
 * Setup the GStreamer pipeline for playback. This or pt_player_setup_sphinx()
 * must be called first on a new PtPlayer object. It’s a programmer’s error to
 * do anything with the #PtPlayer before calling the setup function.
 *
 * Return value: TRUE on success, FALSE if the pipeline could not be set up
 *
 * Since: 1.6
 */
gboolean
pt_player_setup_player (PtPlayer  *player,
                        GError   **error)
{
	GError *earlier_error = NULL;

	if (!player->priv->play)
		pt_player_setup_pipeline (player, &earlier_error);
	PROPAGATE_ERROR_FALSE

	gint pos;
	pos = pt_player_get_position (player);

	/* Before removing and adding elements, set state to NULL to be on the
	   safe side. Without changing volume didn’t work anymore after switching
	   from playback setup to ASR setup and back again. */

	pt_player_set_state_blocking (player, GST_STATE_NULL);

	remove_element (GST_BIN (player->priv->audio_bin), "sphinx-audiobin");
	add_element (GST_BIN (player->priv->audio_bin),
			player->priv->play_bin, player->priv->tee_playpad);

	g_object_set (G_OBJECT (player->priv->play),
			"audio-filter", player->priv->scaletempo, NULL);

	pt_player_set_state_blocking (player, GST_STATE_PAUSED);
	if (pos > 0)
		pt_player_jump_to_position (player, pos);

	return TRUE;
}

static void
pt_player_dispose (GObject *object)
{
	PtPlayer *player = PT_PLAYER (object);

	if (player->priv->play) {
		/* remember position */
		metadata_save_position (player);
		
		gst_element_set_state (player->priv->play, GST_STATE_NULL);

#ifdef HAVE_ASR
		/* Add all possible elements because elements without a parent
		   won't be destroyed. */
		add_element (GST_BIN (player->priv->audio_bin),
		             player->priv->play_bin, player->priv->tee_playpad);
		add_element (GST_BIN (player->priv->audio_bin),
		             player->priv->sphinx_bin, player->priv->tee_sphinxpad);
#endif

		gst_object_unref (GST_OBJECT (player->priv->play));
		player->priv->play = NULL;

		gst_object_unref (GST_OBJECT (player->priv->tee_playpad));
		gst_object_unref (GST_OBJECT (player->priv->tee_sphinxpad));
	}

	G_OBJECT_CLASS (pt_player_parent_class)->dispose (object);
}

static void
pt_player_set_property (GObject      *object,
                        guint         property_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
	PtPlayer *player = PT_PLAYER (object);
	const gchar *tmpchar;

	switch (property_id) {
	case PROP_SPEED:
		pt_player_set_speed (player, g_value_get_double (value));
		break;
	case PROP_VOLUME:
		pt_player_set_volume (player, g_value_get_double (value));
		break;
	case PROP_TIMESTAMP_PRECISION:
		player->priv->timestamp_precision = g_value_get_int (value);
		break;
	case PROP_TIMESTAMP_FIXED:
		player->priv->timestamp_fixed = g_value_get_boolean (value);
		break;
	case PROP_TIMESTAMP_DELIMITER:
		tmpchar = g_value_get_string (value);
		if (g_strcmp0 (tmpchar, "None") == 0) {
			player->priv->timestamp_left = player->priv->timestamp_right = "";
			break;
		}
		if (g_strcmp0 (tmpchar, "(") == 0) {
			player->priv->timestamp_left = "(";
			player->priv->timestamp_right = ")";
			break;
		}
		if (g_strcmp0 (tmpchar, "[") == 0) {
			player->priv->timestamp_left = "[";
			player->priv->timestamp_right = "]";
			break;
		}
		player->priv->timestamp_left = player->priv->timestamp_right = "#";
		break;
	case PROP_TIMESTAMP_FRACTION_SEP:
		tmpchar = g_value_get_string (value);
		if (g_strcmp0 (tmpchar, "-") == 0) {
			player->priv->timestamp_sep = "-";
			break;
		}
		player->priv->timestamp_sep = ".";
		break;
	case PROP_REWIND_ON_PAUSE:
		player->priv->pause = g_value_get_int (value);
		break;
	case PROP_BACK:
		player->priv->back = g_value_get_int (value);
		break;
	case PROP_FORWARD:
		player->priv->forward = g_value_get_int (value);
		break;
	case PROP_REPEAT_ALL:
		player->priv->repeat_all = g_value_get_boolean (value);
		break;
	case PROP_REPEAT_SELECTION:
		player->priv->repeat_selection = g_value_get_boolean (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void
pt_player_get_property (GObject    *object,
                        guint       property_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
	PtPlayer *player = PT_PLAYER (object);

	switch (property_id) {
	case PROP_SPEED:
		g_value_set_double (value, player->priv->speed);
		break;
	case PROP_VOLUME:
		g_value_set_double (value, player->priv->volume);
		break;
	case PROP_TIMESTAMP_PRECISION:
		g_value_set_int (value, player->priv->timestamp_precision);
		break;
	case PROP_TIMESTAMP_FIXED:
		g_value_set_boolean (value, player->priv->timestamp_fixed);
		break;
	case PROP_TIMESTAMP_DELIMITER:
		g_value_set_string (value, player->priv->timestamp_left);
		break;
	case PROP_TIMESTAMP_FRACTION_SEP:
		g_value_set_string (value, player->priv->timestamp_sep);
		break;
	case PROP_REWIND_ON_PAUSE:
		g_value_set_int (value, player->priv->pause);
		break;
	case PROP_BACK:
		g_value_set_int (value, player->priv->back);
		break;
	case PROP_FORWARD:
		g_value_set_int (value, player->priv->forward);
		break;
	case PROP_REPEAT_ALL:
		g_value_set_boolean (value, player->priv->repeat_all);
		break;
	case PROP_REPEAT_SELECTION:
		g_value_set_boolean (value, player->priv->repeat_selection);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void
pt_player_class_init (PtPlayerClass *klass)
{
	G_OBJECT_CLASS (klass)->set_property = pt_player_set_property;
	G_OBJECT_CLASS (klass)->get_property = pt_player_get_property;
	G_OBJECT_CLASS (klass)->dispose = pt_player_dispose;

	/**
	* PtPlayer::end-of-stream:
	* @player: the player emitting the signal
	*
	* The #PtPlayer::end-of-stream signal is emitted when the stream is at its end
	* or when the end of selection is reached.
	*/
	g_signal_new ("end-of-stream",
		      PT_TYPE_PLAYER,
		      G_SIGNAL_RUN_FIRST,
		      0,
		      NULL,
		      NULL,
		      g_cclosure_marshal_VOID__VOID,
		      G_TYPE_NONE,
		      0);

	/**
	* PtPlayer::error:
	* @player: the player emitting the signal
	* @error: a GError
	*
	* The #PtPlayer::error signal is emitted on errors opening the file or during
	* playback. It’s a severe error and the player is always reset.
	*/
	g_signal_new ("error",
		      PT_TYPE_PLAYER,
		      G_SIGNAL_RUN_FIRST,
		      0,
		      NULL,
		      NULL,
		      g_cclosure_marshal_VOID__BOXED,
		      G_TYPE_NONE,
		      1, G_TYPE_ERROR);

	/**
	* PtPlayer::play-toggled:
	* @player: the player emitting the signal
	*
	* The #PtPlayer::play-toggled signal is emitted when the player changed
	* to pause or play.
	*/
	g_signal_new ("play-toggled",
		      PT_TYPE_PLAYER,
		      G_SIGNAL_RUN_FIRST,
		      0,
		      NULL,
		      NULL,
		      g_cclosure_marshal_VOID__VOID,
		      G_TYPE_NONE,
		      0);

	/**
	* PtPlayer::jumped-back:
	* @player: the player emitting the signal
	*
	* The #PtPlayer::jumped-back signal is emitted when the player jumped
	* back.
	*/
	g_signal_new ("jumped-back",
		      PT_TYPE_PLAYER,
		      G_SIGNAL_RUN_FIRST,
		      0,
		      NULL,
		      NULL,
		      g_cclosure_marshal_VOID__VOID,
		      G_TYPE_NONE,
		      0);

	/**
	* PtPlayer::jumped-forward:
	* @player: the player emitting the signal
	*
	* The #PtPlayer::jumped-forward signal is emitted when the player jumped
	* forward.
	*/
	g_signal_new ("jumped-forward",
		      PT_TYPE_PLAYER,
		      G_SIGNAL_RUN_FIRST,
		      0,
		      NULL,
		      NULL,
		      g_cclosure_marshal_VOID__VOID,
		      G_TYPE_NONE,
		      0);

	/**
	* PtPlayer::asr-final:
	* @player: the player emitting the signal
	* @word: recognized word(s)
	*
	* The #PtPlayer::asr-final signal is emitted in automatic speech recognition
	* mode whenever a word or a sequence of words was recognized.
	*/
	g_signal_new ("asr-final",
		      PT_TYPE_PLAYER,
		      G_SIGNAL_RUN_FIRST,
		      0,
		      NULL,
		      NULL,
		      g_cclosure_marshal_VOID__STRING,
		      G_TYPE_NONE,
		      1, G_TYPE_STRING);

	/**
	* PtPlayer::asr-hypothesis:
	* @player: the player emitting the signal
	* @word: probably recognized word(s)
	*
	* The #PtPlayer::asr-hypothesis signal is emitted in automatic speech recognition
	* mode as an intermediate result (hypothesis) of recognized words.
	* The hypothesis can still change, an emitted hypothesis replaces the
	* former hypothesis and is finalized via the #PtPlayer::asr-final signal.
	* It’s not necessary to connect to this signal if you want the final
	* result only. However, it can take a few seconds until a final result
	* is emitted and without an intermediate hypothesis the end user might
	* have the impression that there is nothing going on.
	*/
	g_signal_new ("asr-hypothesis",
		      PT_TYPE_PLAYER,
		      G_SIGNAL_RUN_FIRST,
		      0,
		      NULL,
		      NULL,
		      g_cclosure_marshal_VOID__STRING,
		      G_TYPE_NONE,
		      1, G_TYPE_STRING);

	/**
	* PtPlayer:speed:
	*
	* The speed for playback.
	*/
	obj_properties[PROP_SPEED] =
	g_param_spec_double (
			"speed",
			"Speed of playback",
			"1 is normal speed",
			0.1,	/* minimum */
			2.0,	/* maximum */
			1.0,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

	/**
	* PtPlayer:volume:
	*
	* The volume for playback.
	*/
	obj_properties[PROP_VOLUME] =
	g_param_spec_double (
			"volume",
			"Volume of playback",
			"Volume from 0 to 1",
			0.0,	/* minimum */
			1.0,	/* maximum */
			1.0,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

	/**
	* PtPlayer:timestamp-precision:
	*
	* How precise timestamps should be.
	*/
	obj_properties[PROP_TIMESTAMP_PRECISION] =
	g_param_spec_int (
			"timestamp-precision",
			"Precision of timestamps",
			"Precision of timestamps",
			0,	/* minimum = PT_PRECISION_SECOND */
			3,	/* maximum = PT_PRECISION_INVALID */
			1,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

	/**
	* PtPlayer:timestamp-fixed:
	*
	* Whether timestamp format should have a fixed number of digits.
	*/
	obj_properties[PROP_TIMESTAMP_FIXED] =
	g_param_spec_boolean (
			"timestamp-fixed",
			"Timestamps with fixed digits",
			"Timestamps with fixed digits",
			FALSE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

	/**
	* PtPlayer:timestamp-delimiter:
	*
	* Character to delimit start and end of timestamp. Allowed values are
	* "None", hashtag "#", left bracket "(" and left square bracket "[".
	* PtPlayer will of course end with a right (square) bracket if those
	* are chosen. Any other character is changed to a hashtag "#".
	*/
	obj_properties[PROP_TIMESTAMP_DELIMITER] =
	g_param_spec_string (
			"timestamp-delimiter",
			"Timestamp delimiter",
			"Timestamp delimiter",
			"#",
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

	/**
	* PtPlayer:timestamp-fraction-sep:
	*
	* Character to separate fractions of a second from seconds. Only
	* point "." and minus "-" are allowed. Any other character is changed
	* to a point ".".
	*/
	obj_properties[PROP_TIMESTAMP_FRACTION_SEP] =
	g_param_spec_string (
			"timestamp-fraction-sep",
			"Timestamp fraction separator",
			"Timestamp fraction separator",
			".",
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

	/**
	* PtPlayer:pause:
	*
	* Milliseconds to rewind on pause.
	*/
	obj_properties[PROP_REWIND_ON_PAUSE] =
	g_param_spec_int (
			"pause",
			"Milliseconds to rewind on pause",
			"Milliseconds to rewind on pause",
			0,	/* minimum */
			10000,	/* maximum */
			0,
			G_PARAM_READWRITE);

	/**
	* PtPlayer:back:
	*
	* Milliseconds to jump back.
	*/
	obj_properties[PROP_BACK] =
	g_param_spec_int (
			"back",
			"Milliseconds to jump back",
			"Milliseconds to jump back",
			1000,	/* minimum */
			60000,	/* maximum */
			10000,
			G_PARAM_READWRITE);

	/**
	* PtPlayer:forward:
	*
	* Milliseconds to jump forward.
	*/
	obj_properties[PROP_FORWARD] =
	g_param_spec_int (
			"forward",
			"Milliseconds to jump forward",
			"Milliseconds to jump forward",
			1000,	/* minimum */
			60000,	/* maximum */
			10000,
			G_PARAM_READWRITE);

	/**
	* PtPlayer:repeat-all:
	*
	* "Play" at the end of the file replays it.
	*/
	obj_properties[PROP_REPEAT_ALL] =
	g_param_spec_boolean (
			"repeat-all",
			"Repeat all",
			"Repeat all",
			FALSE,
			G_PARAM_READWRITE);

	/**
	* PtPlayer:repeat-selection:
	*
	* "Play" at the end of a selection replays it.
	*/
	obj_properties[PROP_REPEAT_SELECTION] =
	g_param_spec_boolean (
			"repeat-selection",
			"Repeat selection",
			"Repeat selection",
			FALSE,
			G_PARAM_READWRITE);

	g_object_class_install_properties (
			G_OBJECT_CLASS (klass),
			N_PROPERTIES,
			obj_properties);
}

/**
 * pt_player_new:
 *
 * Returns a new PtPlayer. You have to set it up for playback with
 * pt_player_setup_player() before doing anything else.
 *
 * After use g_object_unref() it.
 *
 * Return value: (transfer full): a new pt_player
 *
 * Since: 1.6
 */
PtPlayer *
pt_player_new (void)
{
	_pt_i18n_init ();
	return g_object_new (PT_TYPE_PLAYER, NULL);
}
