/* GStreamer
 * Copyright (C) 2011 Andoni Morales Alastruey <ylatuya@gmail.com>
 *
 * gstfragment.c:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "skippy_fragment.h"
#include "skippy_uridownloader.h"

#include <glib.h>

#define GST_CAT_DEFAULT uridownloader_debug
GST_DEBUG_CATEGORY (uridownloader_debug);

#define SKIPPY_URI_DOWNLOADER_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), \
    TYPE_SKIPPY_URI_DOWNLOADER, SkippyUriDownloaderPrivate))

struct _SkippyUriDownloaderPrivate
{
  /* Fragments fetcher */
  GstElement *urisrc;
  GstBus *bus;
  GstPad *pad;
  GTimeVal *timeout;
  SkippyFragment *download;
  GMutex download_lock;         /* used to restrict to one download only */

  GCond cond;
  gboolean cancelled;
};

static void skippy_uri_downloader_finalize (GObject * object);
static void skippy_uri_downloader_dispose (GObject * object);

static GstFlowReturn skippy_uri_downloader_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf);
static gboolean skippy_uri_downloader_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static GstBusSyncReply skippy_uri_downloader_bus_handler (GstBus * bus,
    GstMessage * message, gpointer data);

static GstStaticPadTemplate sinkpadtemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define _do_init \
{ \
  GST_DEBUG_CATEGORY_INIT (uridownloader_debug, "skippyhls-uridownloader", 0, "URI downloader"); \
}

G_DEFINE_TYPE_WITH_CODE (SkippyUriDownloader, skippy_uri_downloader, GST_TYPE_OBJECT,
    _do_init);

static void
skippy_uri_downloader_class_init (SkippyUriDownloaderClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  g_type_class_add_private (klass, sizeof (SkippyUriDownloaderPrivate));

  gobject_class->dispose = skippy_uri_downloader_dispose;
  gobject_class->finalize = skippy_uri_downloader_finalize;
}

static void
skippy_uri_downloader_init (SkippyUriDownloader * downloader)
{
  downloader->priv = SKIPPY_URI_DOWNLOADER_GET_PRIVATE (downloader);

  /* Initialize the sink pad. This pad will be connected to the src pad of the
   * element created with gst_element_make_from_uri and will handle the download */
  downloader->priv->pad =
      gst_pad_new_from_static_template (&sinkpadtemplate, "sink");
  gst_pad_set_chain_function (downloader->priv->pad,
      GST_DEBUG_FUNCPTR (skippy_uri_downloader_chain));
  gst_pad_set_event_function (downloader->priv->pad,
      GST_DEBUG_FUNCPTR (skippy_uri_downloader_sink_event));
  gst_pad_set_element_private (downloader->priv->pad, downloader);
  gst_pad_set_active (downloader->priv->pad, TRUE);

  /* Create a bus to handle error and warning message from the source element */
  downloader->priv->bus = gst_bus_new ();

  g_mutex_init (&downloader->priv->download_lock);
  g_cond_init (&downloader->priv->cond);
}

static void
skippy_uri_downloader_dispose (GObject * object)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (object);

  if (downloader->priv->urisrc != NULL) {
    gst_object_unref (downloader->priv->urisrc);
    downloader->priv->urisrc = NULL;
  }

  if (downloader->priv->bus != NULL) {
    gst_object_unref (downloader->priv->bus);
    downloader->priv->bus = NULL;
  }

  if (downloader->priv->pad) {
    gst_object_unref (downloader->priv->pad);
    downloader->priv->pad = NULL;
  }

  if (downloader->priv->download) {
    g_object_unref (downloader->priv->download);
    downloader->priv->download = NULL;
  }

  G_OBJECT_CLASS (skippy_uri_downloader_parent_class)->dispose (object);
}

static void
skippy_uri_downloader_finalize (GObject * object)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (object);

  g_mutex_clear (&downloader->priv->download_lock);
  g_cond_clear (&downloader->priv->cond);

  G_OBJECT_CLASS (skippy_uri_downloader_parent_class)->finalize (object);
}

SkippyUriDownloader *
skippy_uri_downloader_new (void)
{
  return g_object_new (TYPE_SKIPPY_URI_DOWNLOADER, NULL);
}

static gboolean
skippy_uri_downloader_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  gboolean ret = FALSE;
  SkippyUriDownloader *downloader;

  downloader = SKIPPY_URI_DOWNLOADER (gst_pad_get_element_private (pad));

  switch (event->type) {
    case GST_EVENT_EOS:{
      GST_OBJECT_LOCK (downloader);
      GST_DEBUG_OBJECT (downloader, "Got EOS on the fetcher pad");
      if (downloader->priv->download != NULL) {
        /* signal we have fetched the URI */
        downloader->priv->download->completed = TRUE;
        downloader->priv->download->download_stop_time =
            gst_util_get_timestamp ();
        GST_DEBUG_OBJECT (downloader, "Signaling chain funtion");
        g_cond_signal (&downloader->priv->cond);
      }
      GST_OBJECT_UNLOCK (downloader);
      gst_event_unref (event);
      break;
    }
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }

  return ret;
}

static GstBusSyncReply
skippy_uri_downloader_bus_handler (GstBus * bus,
    GstMessage * message, gpointer data)
{
  SkippyUriDownloader *downloader = (SkippyUriDownloader *) (data);

  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR ||
      GST_MESSAGE_TYPE (message) == GST_MESSAGE_WARNING) {
    GError *err = NULL;
    gchar *dbg_info = NULL;

    gst_message_parse_error (message, &err, &dbg_info);
    GST_WARNING_OBJECT (downloader,
        "Received error: %s from %s, the download will be cancelled",
        GST_OBJECT_NAME (message->src), err->message);
    GST_DEBUG ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
    g_error_free (err);
    g_free (dbg_info);

    /* remove the sync handler to avoid duplicated messages */
    gst_bus_set_sync_handler (downloader->priv->bus, NULL, NULL, NULL);

    /* stop the download */
    GST_OBJECT_LOCK (downloader);
    if (downloader->priv->download != NULL) {
      GST_DEBUG_OBJECT (downloader, "Stopping download");
      g_object_unref (downloader->priv->download);
      downloader->priv->download = NULL;
      downloader->priv->cancelled = TRUE;
      g_cond_signal (&downloader->priv->cond);
    }
    GST_OBJECT_UNLOCK (downloader);
  }

  gst_message_unref (message);
  return GST_BUS_DROP;
}

static GstFlowReturn
skippy_uri_downloader_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  SkippyUriDownloader *downloader;

  downloader = SKIPPY_URI_DOWNLOADER (gst_pad_get_element_private (pad));

  /* HTML errors (404, 500, etc...) are also pushed through this pad as
   * response but the source element will also post a warning or error message
   * in the bus, which is handled synchronously cancelling the download.
   */
  GST_OBJECT_LOCK (downloader);
  if (downloader->priv->download == NULL) {
    /* Download cancelled, quit */
    GST_OBJECT_UNLOCK (downloader);
    goto done;
  }

  GST_LOG_OBJECT (downloader, "The uri fetcher received a new buffer "
      "of size %" G_GSIZE_FORMAT, gst_buffer_get_size (buf));
  if (!skippy_fragment_add_buffer (downloader->priv->download, buf))
    GST_WARNING_OBJECT (downloader, "Could not add buffer to fragment");
  GST_OBJECT_UNLOCK (downloader);

done:
  {
    return GST_FLOW_OK;
  }
}

void
skippy_uri_downloader_reset (SkippyUriDownloader * downloader)
{
  g_return_if_fail (downloader != NULL);

  GST_OBJECT_LOCK (downloader);
  downloader->priv->cancelled = FALSE;
  GST_OBJECT_UNLOCK (downloader);
}

void
skippy_uri_downloader_cancel (SkippyUriDownloader * downloader)
{
  GST_OBJECT_LOCK (downloader);
  if (downloader->priv->download != NULL) {
    GST_DEBUG_OBJECT (downloader, "Cancelling download");
    g_object_unref (downloader->priv->download);
    downloader->priv->download = NULL;
    downloader->priv->cancelled = TRUE;
    GST_DEBUG_OBJECT (downloader, "Signaling chain funtion");
    g_cond_signal (&downloader->priv->cond);
  } else {
    gboolean cancelled;

    cancelled = downloader->priv->cancelled;
    downloader->priv->cancelled = TRUE;
    if (cancelled)
      GST_DEBUG_OBJECT (downloader,
          "Trying to cancel a download that was alredy cancelled");
  }
  GST_OBJECT_UNLOCK (downloader);
}

static gboolean
skippy_uri_downloader_set_range (SkippyUriDownloader * downloader,
    gint64 range_start, gint64 range_end)
{
  g_return_val_if_fail (range_start >= 0, FALSE);
  g_return_val_if_fail (range_end >= -1, FALSE);

  if (range_start || (range_end >= 0)) {
    GstEvent *seek;

    seek = gst_event_new_seek (1.0, GST_FORMAT_BYTES, GST_SEEK_FLAG_FLUSH,
        GST_SEEK_TYPE_SET, range_start, GST_SEEK_TYPE_SET, range_end);

    return gst_element_send_event (downloader->priv->urisrc, seek);
  }
  return TRUE;
}

static gboolean
skippy_uri_downloader_set_uri (SkippyUriDownloader * downloader, const gchar * uri)
{
  GstPad *pad;

  if (!gst_uri_is_valid (uri))
    return FALSE;

  g_assert (downloader->priv->urisrc == NULL);

  GST_DEBUG_OBJECT (downloader, "Creating source element for the URI:%s", uri);
  downloader->priv->urisrc =
      gst_element_make_from_uri (GST_URI_SRC, uri, NULL, NULL);
  if (!downloader->priv->urisrc)
    return FALSE;

  /* add a sync handler for the bus messages to detect errors in the download */
  gst_element_set_bus (GST_ELEMENT (downloader->priv->urisrc),
      downloader->priv->bus);
  gst_bus_set_sync_handler (downloader->priv->bus,
      skippy_uri_downloader_bus_handler, downloader, NULL);

  pad = gst_element_get_static_pad (downloader->priv->urisrc, "src");
  if (!pad)
    return FALSE;
  gst_pad_link (pad, downloader->priv->pad);
  gst_object_unref (pad);
  return TRUE;
}

SkippyFragment *
skippy_uri_downloader_fetch_uri (SkippyUriDownloader * downloader, const gchar * uri)
{
  return skippy_uri_downloader_fetch_uri_with_range (downloader, uri, 0, -1);
}

/**
 * skippy_uri_downloader_fetch_uri_with_range:
 * @downloader: the #SkippyUriDownloader
 * @uri: the uri
 * @range_start: the starting byte index
 * @range_end: the final byte index, use -1 for unspecified
 *
 * Returns the downloaded #SkippyFragment
 */
SkippyFragment *
skippy_uri_downloader_fetch_uri_with_range (SkippyUriDownloader * downloader,
    const gchar * uri, gint64 range_start, gint64 range_end)
{
  GstStateChangeReturn ret;
  SkippyFragment *download = NULL;

  GST_DEBUG_OBJECT (downloader, "Fetching URI %s", uri);

  g_mutex_lock (&downloader->priv->download_lock);

  GST_OBJECT_LOCK (downloader);
  if (downloader->priv->cancelled) {
    GST_DEBUG_OBJECT (downloader, "Cancelled, aborting fetch");
    goto quit;
  }

  if (!skippy_uri_downloader_set_uri (downloader, uri)) {
    GST_WARNING_OBJECT (downloader, "Failed to set URI");
    goto quit;
  }

  gst_bus_set_flushing (downloader->priv->bus, FALSE);
  downloader->priv->download = skippy_fragment_new ();
  GST_OBJECT_UNLOCK (downloader);
  ret = gst_element_set_state (downloader->priv->urisrc, GST_STATE_READY);
  GST_OBJECT_LOCK (downloader);
  if (ret == GST_STATE_CHANGE_FAILURE || downloader->priv->download == NULL) {
    GST_WARNING_OBJECT (downloader, "Failed to set src to READY");
    goto quit;
  }

  /* might have been cancelled because of failures in state change */
  if (downloader->priv->cancelled) {
    goto quit;
  }

  if (!skippy_uri_downloader_set_range (downloader, range_start, range_end)) {
    GST_WARNING_OBJECT (downloader, "Failed to set range");
    goto quit;
  }

  GST_OBJECT_UNLOCK (downloader);
  ret = gst_element_set_state (downloader->priv->urisrc, GST_STATE_PLAYING);
  GST_OBJECT_LOCK (downloader);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    if (downloader->priv->download) {
      g_object_unref (downloader->priv->download);
      downloader->priv->download = NULL;
    }
    goto quit;
  }

  /* might have been cancelled because of failures in state change */
  if (downloader->priv->cancelled) {
    goto quit;
  }

  /* wait until:
   *   - the download succeed (EOS in the src pad)
   *   - the download failed (Error message on the fetcher bus)
   *   - the download was canceled
   */
  GST_DEBUG_OBJECT (downloader, "Waiting to fetch the URI %s", uri);
  while (!downloader->priv->cancelled && !downloader->priv->download->completed)
    g_cond_wait (&downloader->priv->cond, GST_OBJECT_GET_LOCK (downloader));

  if (downloader->priv->cancelled) {
    if (downloader->priv->download) {
      g_object_unref (downloader->priv->download);
      downloader->priv->download = NULL;
    }
    goto quit;
  }

  download = downloader->priv->download;
  downloader->priv->download = NULL;

  if (download != NULL)
    GST_INFO_OBJECT (downloader, "URI fetched successfully");
  else
    GST_INFO_OBJECT (downloader, "Error fetching URI");

quit:
  {
    if (downloader->priv->urisrc) {
      GstPad *pad;
      GstElement *urisrc;

      GST_DEBUG_OBJECT (downloader, "Stopping source element %s",
          GST_ELEMENT_NAME (downloader->priv->urisrc));

      /* remove the bus' sync handler */
      gst_bus_set_sync_handler (downloader->priv->bus, NULL, NULL, NULL);
      /* unlink the source element from the internal pad */
      pad = gst_pad_get_peer (downloader->priv->pad);
      if (pad) {
        gst_pad_unlink (pad, downloader->priv->pad);
        gst_object_unref (pad);
      }
      urisrc = downloader->priv->urisrc;
      downloader->priv->urisrc = NULL;
      GST_OBJECT_UNLOCK (downloader);

      GST_DEBUG_OBJECT (downloader, "Stopping source element %s",
          GST_ELEMENT_NAME (urisrc));

      /* set the element state to NULL */
      gst_bus_set_flushing (downloader->priv->bus, TRUE);
      gst_element_set_state (urisrc, GST_STATE_NULL);
      gst_element_get_state (urisrc, NULL, NULL, GST_CLOCK_TIME_NONE);
      gst_element_set_bus (urisrc, NULL);
      gst_object_unref (urisrc);
    } else {
      GST_OBJECT_UNLOCK (downloader);
    }

    g_mutex_unlock (&downloader->priv->download_lock);
    return download;
  }
}