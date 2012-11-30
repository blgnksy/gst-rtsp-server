/* GStreamer
 * Copyright (C) 2008 Wim Taymans <wim.taymans at gmail.com>
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

#include <string.h>
#include <stdlib.h>

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include "rtsp-media.h"

#define GST_RTSP_MEDIA_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTSP_MEDIA, GstRTSPMediaPrivate))

struct _GstRTSPMediaPrivate
{
  GMutex lock;
  GCond cond;

  gboolean shared;
  gboolean reusable;
  GstRTSPLowerTrans protocols;
  gboolean reused;
  gboolean eos_shutdown;
  guint buffer_size;
  GstRTSPAuth *auth;
  GstRTSPAddressPool *pool;

  GstElement *element;
  GRecMutex state_lock;
  GPtrArray *streams;
  GList *dynamic;
  GstRTSPMediaStatus status;
  gint n_active;
  gboolean adding;

  /* the pipeline for the media */
  GstElement *pipeline;
  GstElement *fakesink;
  GSource *source;
  guint id;

  gboolean is_live;
  gboolean seekable;
  gboolean buffering;
  GstState target_state;

  /* RTP session manager */
  GstElement *rtpbin;

  /* the range of media */
  GstRTSPTimeRange range;
  GstClockTime range_start;
  GstClockTime range_stop;
};

#define DEFAULT_SHARED          FALSE
#define DEFAULT_REUSABLE        FALSE
#define DEFAULT_PROTOCOLS       GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP
//#define DEFAULT_PROTOCOLS      GST_RTSP_LOWER_TRANS_UDP_MCAST
#define DEFAULT_EOS_SHUTDOWN    FALSE
#define DEFAULT_BUFFER_SIZE     0x80000

/* define to dump received RTCP packets */
#undef DUMP_STATS

enum
{
  PROP_0,
  PROP_SHARED,
  PROP_REUSABLE,
  PROP_PROTOCOLS,
  PROP_EOS_SHUTDOWN,
  PROP_BUFFER_SIZE,
  PROP_LAST
};

enum
{
  SIGNAL_NEW_STREAM,
  SIGNAL_PREPARED,
  SIGNAL_UNPREPARED,
  SIGNAL_NEW_STATE,
  SIGNAL_LAST
};

GST_DEBUG_CATEGORY_STATIC (rtsp_media_debug);
#define GST_CAT_DEFAULT rtsp_media_debug

static void gst_rtsp_media_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec);
static void gst_rtsp_media_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec);
static void gst_rtsp_media_finalize (GObject * obj);

static gpointer do_loop (GstRTSPMediaClass * klass);
static gboolean default_handle_message (GstRTSPMedia * media,
    GstMessage * message);
static void finish_unprepare (GstRTSPMedia * media);
static gboolean default_unprepare (GstRTSPMedia * media);

static guint gst_rtsp_media_signals[SIGNAL_LAST] = { 0 };

G_DEFINE_TYPE (GstRTSPMedia, gst_rtsp_media, G_TYPE_OBJECT);

static void
gst_rtsp_media_class_init (GstRTSPMediaClass * klass)
{
  GObjectClass *gobject_class;

  g_type_class_add_private (klass, sizeof (GstRTSPMediaPrivate));

  gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = gst_rtsp_media_get_property;
  gobject_class->set_property = gst_rtsp_media_set_property;
  gobject_class->finalize = gst_rtsp_media_finalize;

  g_object_class_install_property (gobject_class, PROP_SHARED,
      g_param_spec_boolean ("shared", "Shared",
          "If this media pipeline can be shared", DEFAULT_SHARED,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_REUSABLE,
      g_param_spec_boolean ("reusable", "Reusable",
          "If this media pipeline can be reused after an unprepare",
          DEFAULT_REUSABLE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PROTOCOLS,
      g_param_spec_flags ("protocols", "Protocols",
          "Allowed lower transport protocols", GST_TYPE_RTSP_LOWER_TRANS,
          DEFAULT_PROTOCOLS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_EOS_SHUTDOWN,
      g_param_spec_boolean ("eos-shutdown", "EOS Shutdown",
          "Send an EOS event to the pipeline before unpreparing",
          DEFAULT_EOS_SHUTDOWN, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BUFFER_SIZE,
      g_param_spec_uint ("buffer-size", "Buffer Size",
          "The kernel UDP buffer size to use", 0, G_MAXUINT,
          DEFAULT_BUFFER_SIZE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_rtsp_media_signals[SIGNAL_NEW_STREAM] =
      g_signal_new ("new-stream", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstRTSPMediaClass, new_stream), NULL, NULL,
      g_cclosure_marshal_generic, G_TYPE_NONE, 1, GST_TYPE_RTSP_STREAM);

  gst_rtsp_media_signals[SIGNAL_PREPARED] =
      g_signal_new ("prepared", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstRTSPMediaClass, prepared), NULL, NULL,
      g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0, G_TYPE_NONE);

  gst_rtsp_media_signals[SIGNAL_UNPREPARED] =
      g_signal_new ("unprepared", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstRTSPMediaClass, unprepared), NULL, NULL,
      g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0, G_TYPE_NONE);

  gst_rtsp_media_signals[SIGNAL_NEW_STATE] =
      g_signal_new ("new-state", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstRTSPMediaClass, new_state), NULL, NULL,
      g_cclosure_marshal_VOID__INT, G_TYPE_NONE, 0, G_TYPE_INT);

  klass->context = g_main_context_new ();
  klass->loop = g_main_loop_new (klass->context, TRUE);

  GST_DEBUG_CATEGORY_INIT (rtsp_media_debug, "rtspmedia", 0, "GstRTSPMedia");

  klass->thread = g_thread_new ("Bus Thread", (GThreadFunc) do_loop, klass);

  klass->handle_message = default_handle_message;
  klass->unprepare = default_unprepare;
}

static void
gst_rtsp_media_init (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv = GST_RTSP_MEDIA_GET_PRIVATE (media);

  media->priv = priv;

  priv->streams = g_ptr_array_new_with_free_func (g_object_unref);
  g_mutex_init (&priv->lock);
  g_cond_init (&priv->cond);
  g_rec_mutex_init (&priv->state_lock);

  priv->shared = DEFAULT_SHARED;
  priv->reusable = DEFAULT_REUSABLE;
  priv->protocols = DEFAULT_PROTOCOLS;
  priv->eos_shutdown = DEFAULT_EOS_SHUTDOWN;
  priv->buffer_size = DEFAULT_BUFFER_SIZE;
}

static void
gst_rtsp_media_finalize (GObject * obj)
{
  GstRTSPMediaPrivate *priv;
  GstRTSPMedia *media;

  media = GST_RTSP_MEDIA (obj);
  priv = media->priv;

  GST_INFO ("finalize media %p", media);

  gst_rtsp_media_unprepare (media);

  g_ptr_array_unref (priv->streams);

  g_list_free_full (priv->dynamic, gst_object_unref);

  if (priv->pipeline)
    gst_object_unref (priv->pipeline);
  gst_object_unref (priv->element);
  if (priv->auth)
    g_object_unref (priv->auth);
  if (priv->pool)
    g_object_unref (priv->pool);
  g_mutex_clear (&priv->lock);
  g_cond_clear (&priv->cond);
  g_rec_mutex_clear (&priv->state_lock);

  G_OBJECT_CLASS (gst_rtsp_media_parent_class)->finalize (obj);
}

static void
gst_rtsp_media_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec)
{
  GstRTSPMedia *media = GST_RTSP_MEDIA (object);

  switch (propid) {
    case PROP_SHARED:
      g_value_set_boolean (value, gst_rtsp_media_is_shared (media));
      break;
    case PROP_REUSABLE:
      g_value_set_boolean (value, gst_rtsp_media_is_reusable (media));
      break;
    case PROP_PROTOCOLS:
      g_value_set_flags (value, gst_rtsp_media_get_protocols (media));
      break;
    case PROP_EOS_SHUTDOWN:
      g_value_set_boolean (value, gst_rtsp_media_is_eos_shutdown (media));
      break;
    case PROP_BUFFER_SIZE:
      g_value_set_uint (value, gst_rtsp_media_get_buffer_size (media));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static void
gst_rtsp_media_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec)
{
  GstRTSPMedia *media = GST_RTSP_MEDIA (object);

  switch (propid) {
    case PROP_SHARED:
      gst_rtsp_media_set_shared (media, g_value_get_boolean (value));
      break;
    case PROP_REUSABLE:
      gst_rtsp_media_set_reusable (media, g_value_get_boolean (value));
      break;
    case PROP_PROTOCOLS:
      gst_rtsp_media_set_protocols (media, g_value_get_flags (value));
      break;
    case PROP_EOS_SHUTDOWN:
      gst_rtsp_media_set_eos_shutdown (media, g_value_get_boolean (value));
      break;
    case PROP_BUFFER_SIZE:
      gst_rtsp_media_set_buffer_size (media, g_value_get_uint (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static gpointer
do_loop (GstRTSPMediaClass * klass)
{
  GST_INFO ("enter mainloop");
  g_main_loop_run (klass->loop);
  GST_INFO ("exit mainloop");

  return NULL;
}

/* must be called with state lock */
static void
collect_media_stats (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv = media->priv;
  gint64 position, duration;

  priv->range.unit = GST_RTSP_RANGE_NPT;

  GST_INFO ("collect media stats");

  if (priv->is_live) {
    priv->range.min.type = GST_RTSP_TIME_NOW;
    priv->range.min.seconds = -1;
    priv->range_start = -1;
    priv->range.max.type = GST_RTSP_TIME_END;
    priv->range.max.seconds = -1;
    priv->range_stop = -1;
  } else {
    /* get the position */
    if (!gst_element_query_position (priv->pipeline, GST_FORMAT_TIME,
            &position)) {
      GST_INFO ("position query failed");
      position = 0;
    }

    /* get the duration */
    if (!gst_element_query_duration (priv->pipeline, GST_FORMAT_TIME,
            &duration)) {
      GST_INFO ("duration query failed");
      duration = -1;
    }

    GST_INFO ("stats: position %" GST_TIME_FORMAT ", duration %"
        GST_TIME_FORMAT, GST_TIME_ARGS (position), GST_TIME_ARGS (duration));

    if (position == -1) {
      priv->range.min.type = GST_RTSP_TIME_NOW;
      priv->range.min.seconds = -1;
      priv->range_start = -1;
    } else {
      priv->range.min.type = GST_RTSP_TIME_SECONDS;
      priv->range.min.seconds = ((gdouble) position) / GST_SECOND;
      priv->range_start = position;
    }
    if (duration == -1) {
      priv->range.max.type = GST_RTSP_TIME_END;
      priv->range.max.seconds = -1;
      priv->range_stop = -1;
    } else {
      priv->range.max.type = GST_RTSP_TIME_SECONDS;
      priv->range.max.seconds = ((gdouble) duration) / GST_SECOND;
      priv->range_stop = duration;
    }
  }
}

/**
 * gst_rtsp_media_new:
 * @element: (transfer full): a #GstElement
 *
 * Create a new #GstRTSPMedia instance. @element is the bin element that
 * provides the different streams. The #GstRTSPMedia object contains the
 * element to produce RTP data for one or more related (audio/video/..)
 * streams.
 *
 * Ownership is taken of @element.
 *
 * Returns: a new #GstRTSPMedia object.
 */
GstRTSPMedia *
gst_rtsp_media_new (GstElement * element)
{
  GstRTSPMedia *result;

  g_return_val_if_fail (GST_IS_ELEMENT (element), NULL);

  result = g_object_new (GST_TYPE_RTSP_MEDIA, NULL);
  result->priv->element = element;

  return result;
}

/**
 * gst_rtsp_media_take_element:
 * @media: a #GstRTSPMedia
 * @pipeline: (transfer full): a #GstPipeline
 *
 * Set @pipeline as the #GstPipeline for @media. Ownership is
 * taken of @pipeline.
 */
void
gst_rtsp_media_take_pipeline (GstRTSPMedia * media, GstPipeline * pipeline)
{
  GstRTSPMediaPrivate *priv;
  GstElement *old;

  g_return_if_fail (GST_IS_RTSP_MEDIA (media));
  g_return_if_fail (GST_IS_PIPELINE (pipeline));

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  old = priv->pipeline;
  priv->pipeline = GST_ELEMENT_CAST (pipeline);
  g_mutex_unlock (&priv->lock);

  if (old)
    gst_object_unref (old);

  gst_object_ref (priv->element);
  gst_bin_add (GST_BIN_CAST (pipeline), priv->element);
}

/**
 * gst_rtsp_media_set_shared:
 * @media: a #GstRTSPMedia
 * @shared: the new value
 *
 * Set or unset if the pipeline for @media can be shared will multiple clients.
 * When @shared is %TRUE, client requests for this media will share the media
 * pipeline.
 */
void
gst_rtsp_media_set_shared (GstRTSPMedia * media, gboolean shared)
{
  GstRTSPMediaPrivate *priv;

  g_return_if_fail (GST_IS_RTSP_MEDIA (media));

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  priv->shared = shared;
  g_mutex_unlock (&priv->lock);
}

/**
 * gst_rtsp_media_is_shared:
 * @media: a #GstRTSPMedia
 *
 * Check if the pipeline for @media can be shared between multiple clients.
 *
 * Returns: %TRUE if the media can be shared between clients.
 */
gboolean
gst_rtsp_media_is_shared (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv;
  gboolean res;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), FALSE);

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  res = priv->shared;
  g_mutex_unlock (&priv->lock);

  return res;
}

/**
 * gst_rtsp_media_set_reusable:
 * @media: a #GstRTSPMedia
 * @reusable: the new value
 *
 * Set or unset if the pipeline for @media can be reused after the pipeline has
 * been unprepared.
 */
void
gst_rtsp_media_set_reusable (GstRTSPMedia * media, gboolean reusable)
{
  GstRTSPMediaPrivate *priv;

  g_return_if_fail (GST_IS_RTSP_MEDIA (media));

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  priv->reusable = reusable;
  g_mutex_unlock (&priv->lock);
}

/**
 * gst_rtsp_media_is_reusable:
 * @media: a #GstRTSPMedia
 *
 * Check if the pipeline for @media can be reused after an unprepare.
 *
 * Returns: %TRUE if the media can be reused
 */
gboolean
gst_rtsp_media_is_reusable (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv;
  gboolean res;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), FALSE);

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  res = priv->reusable;
  g_mutex_unlock (&priv->lock);

  return res;
}

/**
 * gst_rtsp_media_set_protocols:
 * @media: a #GstRTSPMedia
 * @protocols: the new flags
 *
 * Configure the allowed lower transport for @media.
 */
void
gst_rtsp_media_set_protocols (GstRTSPMedia * media, GstRTSPLowerTrans protocols)
{
  GstRTSPMediaPrivate *priv;

  g_return_if_fail (GST_IS_RTSP_MEDIA (media));

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  priv->protocols = protocols;
  g_mutex_unlock (&priv->lock);
}

/**
 * gst_rtsp_media_get_protocols:
 * @media: a #GstRTSPMedia
 *
 * Get the allowed protocols of @media.
 *
 * Returns: a #GstRTSPLowerTrans
 */
GstRTSPLowerTrans
gst_rtsp_media_get_protocols (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv;
  GstRTSPLowerTrans res;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media),
      GST_RTSP_LOWER_TRANS_UNKNOWN);

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  res = priv->protocols;
  g_mutex_unlock (&priv->lock);

  return res;
}

/**
 * gst_rtsp_media_set_eos_shutdown:
 * @media: a #GstRTSPMedia
 * @eos_shutdown: the new value
 *
 * Set or unset if an EOS event will be sent to the pipeline for @media before
 * it is unprepared.
 */
void
gst_rtsp_media_set_eos_shutdown (GstRTSPMedia * media, gboolean eos_shutdown)
{
  GstRTSPMediaPrivate *priv;

  g_return_if_fail (GST_IS_RTSP_MEDIA (media));

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  priv->eos_shutdown = eos_shutdown;
  g_mutex_unlock (&priv->lock);
}

/**
 * gst_rtsp_media_is_eos_shutdown:
 * @media: a #GstRTSPMedia
 *
 * Check if the pipeline for @media will send an EOS down the pipeline before
 * unpreparing.
 *
 * Returns: %TRUE if the media will send EOS before unpreparing.
 */
gboolean
gst_rtsp_media_is_eos_shutdown (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv;
  gboolean res;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), FALSE);

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  res = priv->eos_shutdown;
  g_mutex_unlock (&priv->lock);

  return res;
}

/**
 * gst_rtsp_media_set_buffer_size:
 * @media: a #GstRTSPMedia
 * @size: the new value
 *
 * Set the kernel UDP buffer size.
 */
void
gst_rtsp_media_set_buffer_size (GstRTSPMedia * media, guint size)
{
  GstRTSPMediaPrivate *priv;

  g_return_if_fail (GST_IS_RTSP_MEDIA (media));

  GST_LOG_OBJECT (media, "set buffer size %u", size);

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  priv->buffer_size = size;
  g_mutex_unlock (&priv->lock);
}

/**
 * gst_rtsp_media_get_buffer_size:
 * @media: a #GstRTSPMedia
 *
 * Get the kernel UDP buffer size.
 *
 * Returns: the kernel UDP buffer size.
 */
guint
gst_rtsp_media_get_buffer_size (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv;
  guint res;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), FALSE);

  priv = media->priv;

  g_mutex_unlock (&priv->lock);
  res = priv->buffer_size;
  g_mutex_unlock (&priv->lock);

  return res;
}

/**
 * gst_rtsp_media_set_auth:
 * @media: a #GstRTSPMedia
 * @auth: a #GstRTSPAuth
 *
 * configure @auth to be used as the authentication manager of @media.
 */
void
gst_rtsp_media_set_auth (GstRTSPMedia * media, GstRTSPAuth * auth)
{
  GstRTSPMediaPrivate *priv;
  GstRTSPAuth *old;

  g_return_if_fail (GST_IS_RTSP_MEDIA (media));

  priv = media->priv;

  GST_LOG_OBJECT (media, "set auth %p", auth);

  g_mutex_lock (&priv->lock);
  if ((old = priv->auth) != auth)
    priv->auth = auth ? g_object_ref (auth) : NULL;
  else
    old = NULL;
  g_mutex_unlock (&priv->lock);

  if (old)
    g_object_unref (old);
}

/**
 * gst_rtsp_media_get_auth:
 * @media: a #GstRTSPMedia
 *
 * Get the #GstRTSPAuth used as the authentication manager of @media.
 *
 * Returns: (transfer full): the #GstRTSPAuth of @media. g_object_unref() after
 * usage.
 */
GstRTSPAuth *
gst_rtsp_media_get_auth (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv;
  GstRTSPAuth *result;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), NULL);

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  if ((result = priv->auth))
    g_object_ref (result);
  g_mutex_unlock (&priv->lock);

  return result;
}

/**
 * gst_rtsp_media_set_address_pool:
 * @media: a #GstRTSPMedia
 * @pool: a #GstRTSPAddressPool
 *
 * configure @pool to be used as the address pool of @media.
 */
void
gst_rtsp_media_set_address_pool (GstRTSPMedia * media,
    GstRTSPAddressPool * pool)
{
  GstRTSPMediaPrivate *priv;
  GstRTSPAddressPool *old;

  g_return_if_fail (GST_IS_RTSP_MEDIA (media));

  priv = media->priv;

  GST_LOG_OBJECT (media, "set address pool %p", pool);

  g_mutex_lock (&priv->lock);
  if ((old = priv->pool) != pool)
    priv->pool = pool ? g_object_ref (pool) : NULL;
  else
    old = NULL;
  g_ptr_array_foreach (priv->streams, (GFunc) gst_rtsp_stream_set_address_pool,
      pool);
  g_mutex_unlock (&priv->lock);

  if (old)
    g_object_unref (old);
}

/**
 * gst_rtsp_media_get_address_pool:
 * @media: a #GstRTSPMedia
 *
 * Get the #GstRTSPAddressPool used as the address pool of @media.
 *
 * Returns: (transfer full): the #GstRTSPAddressPool of @media. g_object_unref() after
 * usage.
 */
GstRTSPAddressPool *
gst_rtsp_media_get_address_pool (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv;
  GstRTSPAddressPool *result;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), NULL);

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  if ((result = priv->pool))
    g_object_ref (result);
  g_mutex_unlock (&priv->lock);

  return result;
}

/**
 * gst_rtsp_media_collect_streams:
 * @media: a #GstRTSPMedia
 *
 * Find all payloader elements, they should be named pay%d in the
 * element of @media, and create #GstRTSPStreams for them.
 *
 * Collect all dynamic elements, named dynpay%d, and add them to
 * the list of dynamic elements.
 */
void
gst_rtsp_media_collect_streams (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv;
  GstElement *element, *elem;
  GstPad *pad;
  gint i;
  gboolean have_elem;

  g_return_if_fail (GST_IS_RTSP_MEDIA (media));

  priv = media->priv;
  element = priv->element;

  have_elem = TRUE;
  for (i = 0; have_elem; i++) {
    gchar *name;

    have_elem = FALSE;

    name = g_strdup_printf ("pay%d", i);
    if ((elem = gst_bin_get_by_name (GST_BIN (element), name))) {
      GST_INFO ("found stream %d with payloader %p", i, elem);

      /* take the pad of the payloader */
      pad = gst_element_get_static_pad (elem, "src");
      /* create the stream */
      gst_rtsp_media_create_stream (media, elem, pad);
      gst_object_unref (pad);
      gst_object_unref (elem);

      have_elem = TRUE;
    }
    g_free (name);

    name = g_strdup_printf ("dynpay%d", i);
    if ((elem = gst_bin_get_by_name (GST_BIN (element), name))) {
      /* a stream that will dynamically create pads to provide RTP packets */

      GST_INFO ("found dynamic element %d, %p", i, elem);

      g_mutex_lock (&priv->lock);
      priv->dynamic = g_list_prepend (priv->dynamic, elem);
      g_mutex_unlock (&priv->lock);

      have_elem = TRUE;
    }
    g_free (name);
  }
}

/**
 * gst_rtsp_media_create_stream:
 * @media: a #GstRTSPMedia
 * @payloader: a #GstElement
 * @srcpad: a source #GstPad
 *
 * Create a new stream in @media that provides RTP data on @srcpad.
 * @srcpad should be a pad of an element inside @media->element.
 *
 * Returns: (transfer none): a new #GstRTSPStream that remains valid for as long
 *          as @media exists.
 */
GstRTSPStream *
gst_rtsp_media_create_stream (GstRTSPMedia * media, GstElement * payloader,
    GstPad * pad)
{
  GstRTSPMediaPrivate *priv;
  GstRTSPStream *stream;
  GstPad *srcpad;
  gchar *name;
  gint idx;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), NULL);
  g_return_val_if_fail (GST_IS_ELEMENT (payloader), NULL);
  g_return_val_if_fail (GST_IS_PAD (pad), NULL);
  g_return_val_if_fail (GST_PAD_IS_SRC (pad), NULL);

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  idx = priv->streams->len;

  GST_DEBUG ("media %p: creating stream with index %d", media, idx);

  name = g_strdup_printf ("src_%u", idx);
  srcpad = gst_ghost_pad_new (name, pad);
  gst_pad_set_active (srcpad, TRUE);
  gst_element_add_pad (priv->element, srcpad);
  g_free (name);

  stream = gst_rtsp_stream_new (idx, payloader, srcpad);
  if (priv->pool)
    gst_rtsp_stream_set_address_pool (stream, priv->pool);

  g_ptr_array_add (priv->streams, stream);
  g_mutex_unlock (&priv->lock);

  g_signal_emit (media, gst_rtsp_media_signals[SIGNAL_NEW_STREAM], 0, stream,
      NULL);

  return stream;
}

/**
 * gst_rtsp_media_n_streams:
 * @media: a #GstRTSPMedia
 *
 * Get the number of streams in this media.
 *
 * Returns: The number of streams.
 */
guint
gst_rtsp_media_n_streams (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv;
  guint res;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), 0);

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  res = priv->streams->len;
  g_mutex_unlock (&priv->lock);

  return res;
}

/**
 * gst_rtsp_media_get_stream:
 * @media: a #GstRTSPMedia
 * @idx: the stream index
 *
 * Retrieve the stream with index @idx from @media.
 *
 * Returns: (transfer none): the #GstRTSPStream at index @idx or %NULL when a stream with
 * that index did not exist.
 */
GstRTSPStream *
gst_rtsp_media_get_stream (GstRTSPMedia * media, guint idx)
{
  GstRTSPMediaPrivate *priv;
  GstRTSPStream *res;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), NULL);

  priv = media->priv;

  g_mutex_lock (&priv->lock);
  if (idx < priv->streams->len)
    res = g_ptr_array_index (priv->streams, idx);
  else
    res = NULL;
  g_mutex_unlock (&priv->lock);

  return res;
}

/**
 * gst_rtsp_media_get_range_string:
 * @media: a #GstRTSPMedia
 * @play: for the PLAY request
 *
 * Get the current range as a string. @media must be prepared with
 * gst_rtsp_media_prepare ().
 *
 * Returns: The range as a string, g_free() after usage.
 */
gchar *
gst_rtsp_media_get_range_string (GstRTSPMedia * media, gboolean play)
{
  GstRTSPMediaPrivate *priv;
  gchar *result;
  GstRTSPTimeRange range;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), NULL);

  priv = media->priv;

  g_rec_mutex_lock (&priv->state_lock);
  if (priv->status != GST_RTSP_MEDIA_STATUS_PREPARED)
    goto not_prepared;

  g_mutex_lock (&priv->lock);
  /* make copy */
  range = priv->range;

  if (!play && priv->n_active > 0) {
    range.min.type = GST_RTSP_TIME_NOW;
    range.min.seconds = -1;
  }
  g_mutex_unlock (&priv->lock);
  g_rec_mutex_unlock (&priv->state_lock);

  result = gst_rtsp_range_to_string (&range);

  return result;

  /* ERRORS */
not_prepared:
  {
    GST_WARNING ("media %p was not prepared", media);
    g_rec_mutex_unlock (&priv->state_lock);
    return NULL;
  }
}

/**
 * gst_rtsp_media_seek:
 * @media: a #GstRTSPMedia
 * @range: a #GstRTSPTimeRange
 *
 * Seek the pipeline of @media to @range. @media must be prepared with
 * gst_rtsp_media_prepare().
 *
 * Returns: %TRUE on success.
 */
gboolean
gst_rtsp_media_seek (GstRTSPMedia * media, GstRTSPTimeRange * range)
{
  GstRTSPMediaPrivate *priv;
  GstSeekFlags flags;
  gboolean res;
  GstClockTime start, stop;
  GstSeekType start_type, stop_type;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), FALSE);
  g_return_val_if_fail (range != NULL, FALSE);

  priv = media->priv;

  g_rec_mutex_lock (&priv->state_lock);
  if (priv->status != GST_RTSP_MEDIA_STATUS_PREPARED)
    goto not_prepared;

  if (!priv->seekable)
    goto not_seekable;

  /* depends on the current playing state of the pipeline. We might need to
   * queue this until we get EOS. */
  flags = GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE | GST_SEEK_FLAG_KEY_UNIT;

  start_type = stop_type = GST_SEEK_TYPE_NONE;

  if (!gst_rtsp_range_get_times (range, &start, &stop))
    goto not_supported;

  GST_INFO ("got %" GST_TIME_FORMAT " - %" GST_TIME_FORMAT,
      GST_TIME_ARGS (start), GST_TIME_ARGS (stop));
  GST_INFO ("current %" GST_TIME_FORMAT " - %" GST_TIME_FORMAT,
      GST_TIME_ARGS (priv->range_start), GST_TIME_ARGS (priv->range_stop));

  if (priv->range_start == start)
    start = GST_CLOCK_TIME_NONE;
  else if (start != GST_CLOCK_TIME_NONE)
    start_type = GST_SEEK_TYPE_SET;

  if (priv->range_stop == stop)
    stop = GST_CLOCK_TIME_NONE;
  else if (stop != GST_CLOCK_TIME_NONE)
    stop_type = GST_SEEK_TYPE_SET;

  if (start != GST_CLOCK_TIME_NONE || stop != GST_CLOCK_TIME_NONE) {
    GST_INFO ("seeking to %" GST_TIME_FORMAT " - %" GST_TIME_FORMAT,
        GST_TIME_ARGS (start), GST_TIME_ARGS (stop));

    res = gst_element_seek (priv->pipeline, 1.0, GST_FORMAT_TIME,
        flags, start_type, start, stop_type, stop);

    /* and block for the seek to complete */
    GST_INFO ("done seeking %d", res);
    gst_element_get_state (priv->pipeline, NULL, NULL, -1);
    GST_INFO ("prerolled again");

    collect_media_stats (media);
  } else {
    GST_INFO ("no seek needed");
    res = TRUE;
  }
  g_rec_mutex_unlock (&priv->state_lock);

  return res;

  /* ERRORS */
not_prepared:
  {
    g_rec_mutex_unlock (&priv->state_lock);
    GST_INFO ("media %p is not prepared", media);
    return FALSE;
  }
not_seekable:
  {
    g_rec_mutex_unlock (&priv->state_lock);
    GST_INFO ("pipeline is not seekable");
    return TRUE;
  }
not_supported:
  {
    g_rec_mutex_unlock (&priv->state_lock);
    GST_WARNING ("seek unit %d not supported", range->unit);
    return FALSE;
  }
}

static void
gst_rtsp_media_set_status (GstRTSPMedia * media, GstRTSPMediaStatus status)
{
  GstRTSPMediaPrivate *priv = media->priv;

  g_mutex_lock (&priv->lock);
  /* never overwrite the error status */
  if (priv->status != GST_RTSP_MEDIA_STATUS_ERROR)
    priv->status = status;
  GST_DEBUG ("setting new status to %d", status);
  g_cond_broadcast (&priv->cond);
  g_mutex_unlock (&priv->lock);
}

/**
 * gst_rtsp_media_get_status:
 * @media: a #GstRTSPMedia
 *
 * Get the status of @media. When @media is busy preparing, this function waits
 * until @media is prepared or in error.
 *
 * Returns: the status of @media.
 */
GstRTSPMediaStatus
gst_rtsp_media_get_status (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv = media->priv;
  GstRTSPMediaStatus result;
  gint64 end_time;

  g_mutex_lock (&priv->lock);
  end_time = g_get_monotonic_time () + 20 * G_TIME_SPAN_SECOND;
  /* while we are preparing, wait */
  while (priv->status == GST_RTSP_MEDIA_STATUS_PREPARING) {
    GST_DEBUG ("waiting for status change");
    if (!g_cond_wait_until (&priv->cond, &priv->lock, end_time)) {
      GST_DEBUG ("timeout, assuming error status");
      priv->status = GST_RTSP_MEDIA_STATUS_ERROR;
    }
  }
  /* could be success or error */
  result = priv->status;
  GST_DEBUG ("got status %d", result);
  g_mutex_unlock (&priv->lock);

  return result;
}

/* called with state-lock */
static gboolean
default_handle_message (GstRTSPMedia * media, GstMessage * message)
{
  GstRTSPMediaPrivate *priv = media->priv;
  GstMessageType type;

  type = GST_MESSAGE_TYPE (message);

  switch (type) {
    case GST_MESSAGE_STATE_CHANGED:
      break;
    case GST_MESSAGE_BUFFERING:
    {
      gint percent;

      gst_message_parse_buffering (message, &percent);

      /* no state management needed for live pipelines */
      if (priv->is_live)
        break;

      if (percent == 100) {
        /* a 100% message means buffering is done */
        priv->buffering = FALSE;
        /* if the desired state is playing, go back */
        if (priv->target_state == GST_STATE_PLAYING) {
          GST_INFO ("Buffering done, setting pipeline to PLAYING");
          gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
        } else {
          GST_INFO ("Buffering done");
        }
      } else {
        /* buffering busy */
        if (priv->buffering == FALSE) {
          if (priv->target_state == GST_STATE_PLAYING) {
            /* we were not buffering but PLAYING, PAUSE  the pipeline. */
            GST_INFO ("Buffering, setting pipeline to PAUSED ...");
            gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
          } else {
            GST_INFO ("Buffering ...");
          }
        }
        priv->buffering = TRUE;
      }
      break;
    }
    case GST_MESSAGE_LATENCY:
    {
      gst_bin_recalculate_latency (GST_BIN_CAST (priv->pipeline));
      break;
    }
    case GST_MESSAGE_ERROR:
    {
      GError *gerror;
      gchar *debug;

      gst_message_parse_error (message, &gerror, &debug);
      GST_WARNING ("%p: got error %s (%s)", media, gerror->message, debug);
      g_error_free (gerror);
      g_free (debug);

      gst_rtsp_media_set_status (media, GST_RTSP_MEDIA_STATUS_ERROR);
      break;
    }
    case GST_MESSAGE_WARNING:
    {
      GError *gerror;
      gchar *debug;

      gst_message_parse_warning (message, &gerror, &debug);
      GST_WARNING ("%p: got warning %s (%s)", media, gerror->message, debug);
      g_error_free (gerror);
      g_free (debug);
      break;
    }
    case GST_MESSAGE_ELEMENT:
      break;
    case GST_MESSAGE_STREAM_STATUS:
      break;
    case GST_MESSAGE_ASYNC_DONE:
      if (!priv->adding) {
        /* when we are dynamically adding pads, the addition of the udpsrc will
         * temporarily produce ASYNC_DONE messages. We have to ignore them and
         * wait for the final ASYNC_DONE after everything prerolled */
        GST_INFO ("%p: got ASYNC_DONE", media);
        collect_media_stats (media);

        gst_rtsp_media_set_status (media, GST_RTSP_MEDIA_STATUS_PREPARED);
      } else {
        GST_INFO ("%p: ignoring ASYNC_DONE", media);
      }
      break;
    case GST_MESSAGE_EOS:
      GST_INFO ("%p: got EOS", media);

      if (priv->status == GST_RTSP_MEDIA_STATUS_UNPREPARING) {
        GST_DEBUG ("shutting down after EOS");
        finish_unprepare (media);
        g_object_unref (media);
      }
      break;
    default:
      GST_INFO ("%p: got message type %s", media,
          gst_message_type_get_name (type));
      break;
  }
  return TRUE;
}

static gboolean
bus_message (GstBus * bus, GstMessage * message, GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv = media->priv;
  GstRTSPMediaClass *klass;
  gboolean ret;

  klass = GST_RTSP_MEDIA_GET_CLASS (media);

  g_rec_mutex_lock (&priv->state_lock);
  if (klass->handle_message)
    ret = klass->handle_message (media, message);
  else
    ret = FALSE;
  g_rec_mutex_unlock (&priv->state_lock);

  return ret;
}

static void
watch_destroyed (GstRTSPMedia * media)
{
  GST_DEBUG_OBJECT (media, "source destroyed");
  g_object_unref (media);
}

/* called from streaming threads */
static void
pad_added_cb (GstElement * element, GstPad * pad, GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv = media->priv;
  GstRTSPStream *stream;

  /* FIXME, element is likely not a payloader, find the payloader here */
  stream = gst_rtsp_media_create_stream (media, element, pad);

  GST_INFO ("pad added %s:%s, stream %p", GST_DEBUG_PAD_NAME (pad), stream);

  g_rec_mutex_lock (&priv->state_lock);
  /* we will be adding elements below that will cause ASYNC_DONE to be
   * posted in the bus. We want to ignore those messages until the
   * pipeline really prerolled. */
  priv->adding = TRUE;

  /* join the element in the PAUSED state because this callback is
   * called from the streaming thread and it is PAUSED */
  gst_rtsp_stream_join_bin (stream, GST_BIN (priv->pipeline),
      priv->rtpbin, GST_STATE_PAUSED);

  priv->adding = FALSE;
  g_rec_mutex_unlock (&priv->state_lock);
}

static void
no_more_pads_cb (GstElement * element, GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv = media->priv;
  GstElement *fakesink;

  g_mutex_lock (&priv->lock);
  GST_INFO ("no more pads");
  if ((fakesink = priv->fakesink)) {
    gst_object_ref (fakesink);
    priv->fakesink = NULL;
    g_mutex_unlock (&priv->lock);

    gst_bin_remove (GST_BIN (priv->pipeline), fakesink);
    gst_element_set_state (fakesink, GST_STATE_NULL);
    gst_object_unref (fakesink);
    GST_INFO ("removed fakesink");
  }
}

/**
 * gst_rtsp_media_prepare:
 * @media: a #GstRTSPMedia
 *
 * Prepare @media for streaming. This function will create the pipeline and
 * other objects to manage the streaming.
 *
 * It will preroll the pipeline and collect vital information about the streams
 * such as the duration.
 *
 * Returns: %TRUE on success.
 */
gboolean
gst_rtsp_media_prepare (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv;
  GstStateChangeReturn ret;
  GstRTSPMediaStatus status;
  guint i;
  GstRTSPMediaClass *klass;
  GstBus *bus;
  GList *walk;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), FALSE);

  priv = media->priv;

  g_rec_mutex_lock (&priv->state_lock);
  if (priv->status == GST_RTSP_MEDIA_STATUS_PREPARED)
    goto was_prepared;

  if (priv->status == GST_RTSP_MEDIA_STATUS_PREPARING)
    goto wait_status;

  if (priv->status != GST_RTSP_MEDIA_STATUS_UNPREPARED)
    goto not_unprepared;

  if (!priv->reusable && priv->reused)
    goto is_reused;

  priv->rtpbin = gst_element_factory_make ("rtpbin", NULL);
  if (priv->rtpbin == NULL)
    goto no_rtpbin;

  GST_INFO ("preparing media %p", media);

  /* reset some variables */
  priv->is_live = FALSE;
  priv->seekable = FALSE;
  priv->buffering = FALSE;
  /* we're preparing now */
  priv->status = GST_RTSP_MEDIA_STATUS_PREPARING;

  bus = gst_pipeline_get_bus (GST_PIPELINE_CAST (priv->pipeline));

  /* add the pipeline bus to our custom mainloop */
  priv->source = gst_bus_create_watch (bus);
  gst_object_unref (bus);

  g_source_set_callback (priv->source, (GSourceFunc) bus_message,
      g_object_ref (media), (GDestroyNotify) watch_destroyed);

  klass = GST_RTSP_MEDIA_GET_CLASS (media);
  priv->id = g_source_attach (priv->source, klass->context);

  /* add stuff to the bin */
  gst_bin_add (GST_BIN (priv->pipeline), priv->rtpbin);

  /* link streams we already have, other streams might appear when we have
   * dynamic elements */
  for (i = 0; i < priv->streams->len; i++) {
    GstRTSPStream *stream;

    stream = g_ptr_array_index (priv->streams, i);

    gst_rtsp_stream_join_bin (stream, GST_BIN (priv->pipeline),
        priv->rtpbin, GST_STATE_NULL);
  }

  for (walk = priv->dynamic; walk; walk = g_list_next (walk)) {
    GstElement *elem = walk->data;

    GST_INFO ("adding callbacks for dynamic element %p", elem);

    g_signal_connect (elem, "pad-added", (GCallback) pad_added_cb, media);
    g_signal_connect (elem, "no-more-pads", (GCallback) no_more_pads_cb, media);

    /* we add a fakesink here in order to make the state change async. We remove
     * the fakesink again in the no-more-pads callback. */
    priv->fakesink = gst_element_factory_make ("fakesink", "fakesink");
    gst_bin_add (GST_BIN (priv->pipeline), priv->fakesink);
  }

  GST_INFO ("setting pipeline to PAUSED for media %p", media);
  /* first go to PAUSED */
  ret = gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
  priv->target_state = GST_STATE_PAUSED;

  switch (ret) {
    case GST_STATE_CHANGE_SUCCESS:
      GST_INFO ("SUCCESS state change for media %p", media);
      priv->seekable = TRUE;
      break;
    case GST_STATE_CHANGE_ASYNC:
      GST_INFO ("ASYNC state change for media %p", media);
      priv->seekable = TRUE;
      break;
    case GST_STATE_CHANGE_NO_PREROLL:
      /* we need to go to PLAYING */
      GST_INFO ("NO_PREROLL state change: live media %p", media);
      /* FIXME we disable seeking for live streams for now. We should perform a
       * seeking query in preroll instead */
      priv->seekable = FALSE;
      priv->is_live = TRUE;
      ret = gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
      if (ret == GST_STATE_CHANGE_FAILURE)
        goto state_failed;
      break;
    case GST_STATE_CHANGE_FAILURE:
      goto state_failed;
  }
wait_status:
  g_rec_mutex_unlock (&priv->state_lock);

  /* now wait for all pads to be prerolled, FIXME, we should somehow be
   * able to do this async so that we don't block the server thread. */
  status = gst_rtsp_media_get_status (media);
  if (status == GST_RTSP_MEDIA_STATUS_ERROR)
    goto state_failed;

  g_signal_emit (media, gst_rtsp_media_signals[SIGNAL_PREPARED], 0, NULL);

  GST_INFO ("object %p is prerolled", media);

  return TRUE;

  /* OK */
was_prepared:
  {
    GST_LOG ("media %p was prepared", media);
    g_rec_mutex_unlock (&priv->state_lock);
    return TRUE;
  }
  /* ERRORS */
not_unprepared:
  {
    GST_WARNING ("media %p was not unprepared", media);
    g_rec_mutex_unlock (&priv->state_lock);
    return FALSE;
  }
is_reused:
  {
    g_rec_mutex_unlock (&priv->state_lock);
    GST_WARNING ("can not reuse media %p", media);
    return FALSE;
  }
no_rtpbin:
  {
    g_rec_mutex_unlock (&priv->state_lock);
    GST_WARNING ("no rtpbin element");
    g_warning ("failed to create element 'rtpbin', check your installation");
    return FALSE;
  }
state_failed:
  {
    GST_WARNING ("failed to preroll pipeline");
    gst_rtsp_media_unprepare (media);
    g_rec_mutex_unlock (&priv->state_lock);
    return FALSE;
  }
}

/* must be called with state-lock */
static void
finish_unprepare (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv = media->priv;
  gint i;

  GST_DEBUG ("shutting down");

  gst_element_set_state (priv->pipeline, GST_STATE_NULL);

  for (i = 0; i < priv->streams->len; i++) {
    GstRTSPStream *stream;

    GST_INFO ("Removing elements of stream %d from pipeline", i);

    stream = g_ptr_array_index (priv->streams, i);

    gst_rtsp_stream_leave_bin (stream, GST_BIN (priv->pipeline), priv->rtpbin);
  }
  g_ptr_array_set_size (priv->streams, 0);

  gst_bin_remove (GST_BIN (priv->pipeline), priv->rtpbin);
  priv->rtpbin = NULL;

  gst_object_unref (priv->pipeline);
  priv->pipeline = NULL;

  priv->reused = TRUE;
  priv->status = GST_RTSP_MEDIA_STATUS_UNPREPARED;

  if (priv->source) {
    g_source_destroy (priv->source);
    g_source_unref (priv->source);
    priv->source = NULL;
  }

  /* when the media is not reusable, this will effectively unref the media and
   * recreate it */
  g_signal_emit (media, gst_rtsp_media_signals[SIGNAL_UNPREPARED], 0, NULL);
}

/* called with state-lock */
static gboolean
default_unprepare (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv = media->priv;

  if (priv->eos_shutdown) {
    GST_DEBUG ("sending EOS for shutdown");
    /* ref so that we don't disappear */
    g_object_ref (media);
    gst_element_send_event (priv->pipeline, gst_event_new_eos ());
    /* we need to go to playing again for the EOS to propagate, normally in this
     * state, nothing is receiving data from us anymore so this is ok. */
    gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
    priv->status = GST_RTSP_MEDIA_STATUS_UNPREPARING;
  } else {
    finish_unprepare (media);
  }
  return TRUE;
}

/**
 * gst_rtsp_media_unprepare:
 * @media: a #GstRTSPMedia
 *
 * Unprepare @media. After this call, the media should be prepared again before
 * it can be used again. If the media is set to be non-reusable, a new instance
 * must be created.
 *
 * Returns: %TRUE on success.
 */
gboolean
gst_rtsp_media_unprepare (GstRTSPMedia * media)
{
  GstRTSPMediaPrivate *priv;
  gboolean success;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), FALSE);

  priv = media->priv;

  g_rec_mutex_lock (&priv->state_lock);
  if (priv->status == GST_RTSP_MEDIA_STATUS_UNPREPARED)
    goto was_unprepared;

  GST_INFO ("unprepare media %p", media);
  priv->target_state = GST_STATE_NULL;
  success = TRUE;

  if (priv->status == GST_RTSP_MEDIA_STATUS_PREPARED) {
    GstRTSPMediaClass *klass;

    klass = GST_RTSP_MEDIA_GET_CLASS (media);
    if (klass->unprepare)
      success = klass->unprepare (media);
  } else {
    finish_unprepare (media);
  }
  g_rec_mutex_unlock (&priv->state_lock);

  return success;

was_unprepared:
  {
    g_rec_mutex_unlock (&priv->state_lock);
    GST_INFO ("media %p was already unprepared", media);
    return TRUE;
  }
}

/**
 * gst_rtsp_media_set_state:
 * @media: a #GstRTSPMedia
 * @state: the target state of the media
 * @transports: a #GPtrArray of #GstRTSPStreamTransport pointers
 *
 * Set the state of @media to @state and for the transports in @transports.
 *
 * @media must be prepared with gst_rtsp_media_prepare();
 *
 * Returns: %TRUE on success.
 */
gboolean
gst_rtsp_media_set_state (GstRTSPMedia * media, GstState state,
    GPtrArray * transports)
{
  GstRTSPMediaPrivate *priv;
  gint i;
  gboolean activate, deactivate, do_state;
  gint old_active;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA (media), FALSE);
  g_return_val_if_fail (transports != NULL, FALSE);

  priv = media->priv;

  g_rec_mutex_lock (&priv->state_lock);
  if (priv->status != GST_RTSP_MEDIA_STATUS_PREPARED)
    goto not_prepared;

  /* NULL and READY are the same */
  if (state == GST_STATE_READY)
    state = GST_STATE_NULL;

  activate = deactivate = FALSE;

  GST_INFO ("going to state %s media %p", gst_element_state_get_name (state),
      media);

  switch (state) {
    case GST_STATE_NULL:
    case GST_STATE_PAUSED:
      /* we're going from PLAYING to PAUSED, READY or NULL, deactivate */
      if (priv->target_state == GST_STATE_PLAYING)
        deactivate = TRUE;
      break;
    case GST_STATE_PLAYING:
      /* we're going to PLAYING, activate */
      activate = TRUE;
      break;
    default:
      break;
  }
  old_active = priv->n_active;

  for (i = 0; i < transports->len; i++) {
    GstRTSPStreamTransport *trans;

    /* we need a non-NULL entry in the array */
    trans = g_ptr_array_index (transports, i);
    if (trans == NULL)
      continue;

    if (activate) {
      if (gst_rtsp_stream_transport_set_active (trans, TRUE))
        priv->n_active++;
    } else if (deactivate) {
      if (gst_rtsp_stream_transport_set_active (trans, FALSE))
        priv->n_active--;
    }
  }

  /* we just activated the first media, do the playing state change */
  if (old_active == 0 && activate)
    do_state = TRUE;
  /* if we have no more active media, do the downward state changes */
  else if (priv->n_active == 0)
    do_state = TRUE;
  else
    do_state = FALSE;

  GST_INFO ("state %d active %d media %p do_state %d", state, priv->n_active,
      media, do_state);

  if (priv->target_state != state) {
    if (do_state) {
      if (state == GST_STATE_NULL) {
        gst_rtsp_media_unprepare (media);
      } else {
        GST_INFO ("state %s media %p", gst_element_state_get_name (state),
            media);
        priv->target_state = state;
        gst_element_set_state (priv->pipeline, state);
      }
    }
    g_signal_emit (media, gst_rtsp_media_signals[SIGNAL_NEW_STATE], 0, state,
        NULL);
  }

  /* remember where we are */
  if (state != GST_STATE_NULL && (state == GST_STATE_PAUSED ||
          old_active != priv->n_active))
    collect_media_stats (media);

  g_rec_mutex_unlock (&priv->state_lock);

  return TRUE;

  /* ERRORS */
not_prepared:
  {
    GST_WARNING ("media %p was not prepared", media);
    g_rec_mutex_unlock (&priv->state_lock);
    return FALSE;
  }
}
