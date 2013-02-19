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

#include <gio/gio.h>

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include "rtsp-stream.h"

#define GST_RTSP_STREAM_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTSP_STREAM, GstRTSPStreamPrivate))

struct _GstRTSPStreamPrivate
{
  GMutex lock;
  guint idx;
  GstPad *srcpad;
  GstElement *payloader;
  gboolean is_ipv6;
  guint buffer_size;
  gboolean is_joined;

  /* pads on the rtpbin */
  GstPad *send_rtp_sink;
  GstPad *recv_sink[2];
  GstPad *send_src[2];

  /* the RTPSession object */
  GObject *session;

  /* sinks used for sending and receiving RTP and RTCP, they share
   * sockets */
  GstElement *udpsrc[2];
  GstElement *udpsink[2];
  /* for TCP transport */
  GstElement *appsrc[2];
  GstElement *appqueue[2];
  GstElement *appsink[2];

  GstElement *tee[2];
  GstElement *funnel[2];

  /* server ports for sending/receiving */
  GstRTSPRange server_port;

  /* multicast addresses */
  GstRTSPAddressPool *pool;
  GstRTSPAddress *addr;

  /* the caps of the stream */
  gulong caps_sig;
  GstCaps *caps;

  /* transports we stream to */
  guint n_active;
  GList *transports;
};


enum
{
  PROP_0,
  PROP_LAST
};

GST_DEBUG_CATEGORY_STATIC (rtsp_stream_debug);
#define GST_CAT_DEFAULT rtsp_stream_debug

static GQuark ssrc_stream_map_key;

static void gst_rtsp_stream_finalize (GObject * obj);

G_DEFINE_TYPE (GstRTSPStream, gst_rtsp_stream, G_TYPE_OBJECT);

static void
gst_rtsp_stream_class_init (GstRTSPStreamClass * klass)
{
  GObjectClass *gobject_class;

  g_type_class_add_private (klass, sizeof (GstRTSPStreamPrivate));

  gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gst_rtsp_stream_finalize;

  GST_DEBUG_CATEGORY_INIT (rtsp_stream_debug, "rtspstream", 0, "GstRTSPStream");

  ssrc_stream_map_key = g_quark_from_static_string ("GstRTSPServer.stream");
}

static void
gst_rtsp_stream_init (GstRTSPStream * stream)
{
  GstRTSPStreamPrivate *priv = GST_RTSP_STREAM_GET_PRIVATE (stream);

  GST_DEBUG ("new stream %p", stream);

  stream->priv = priv;

  g_mutex_init (&priv->lock);
}

static void
gst_rtsp_stream_finalize (GObject * obj)
{
  GstRTSPStream *stream;
  GstRTSPStreamPrivate *priv;

  stream = GST_RTSP_STREAM (obj);
  priv = stream->priv;

  GST_DEBUG ("finalize stream %p", stream);

  /* we really need to be unjoined now */
  g_return_if_fail (!priv->is_joined);

  if (priv->addr)
    gst_rtsp_address_free (priv->addr);
  if (priv->pool)
    g_object_unref (priv->pool);
  gst_object_unref (priv->payloader);
  gst_object_unref (priv->srcpad);
  g_mutex_clear (&priv->lock);

  G_OBJECT_CLASS (gst_rtsp_stream_parent_class)->finalize (obj);
}

/**
 * gst_rtsp_stream_new:
 * @idx: an index
 * @srcpad: a #GstPad
 * @payloader: a #GstElement
 *
 * Create a new media stream with index @idx that handles RTP data on
 * @srcpad and has a payloader element @payloader.
 *
 * Returns: a new #GstRTSPStream
 */
GstRTSPStream *
gst_rtsp_stream_new (guint idx, GstElement * payloader, GstPad * srcpad)
{
  GstRTSPStreamPrivate *priv;
  GstRTSPStream *stream;

  g_return_val_if_fail (GST_IS_ELEMENT (payloader), NULL);
  g_return_val_if_fail (GST_IS_PAD (srcpad), NULL);
  g_return_val_if_fail (GST_PAD_IS_SRC (srcpad), NULL);

  stream = g_object_new (GST_TYPE_RTSP_STREAM, NULL);
  priv = stream->priv;
  priv->idx = idx;
  priv->payloader = gst_object_ref (payloader);
  priv->srcpad = gst_object_ref (srcpad);

  return stream;
}

/**
 * gst_rtsp_stream_get_index:
 * @stream: a #GstRTSPStream
 *
 * Get the stream index.
 *
 * Return: the stream index.
 */
guint
gst_rtsp_stream_get_index (GstRTSPStream * stream)
{
  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), -1);

  return stream->priv->idx;
}

/**
 * gst_rtsp_stream_set_mtu:
 * @stream: a #GstRTSPStream
 * @mtu: a new MTU
 *
 * Configure the mtu in the payloader of @stream to @mtu.
 */
void
gst_rtsp_stream_set_mtu (GstRTSPStream * stream, guint mtu)
{
  GstRTSPStreamPrivate *priv;

  g_return_if_fail (GST_IS_RTSP_STREAM (stream));

  priv = stream->priv;

  GST_LOG_OBJECT (stream, "set MTU %u", mtu);

  g_object_set (G_OBJECT (priv->payloader), "mtu", mtu, NULL);
}

/**
 * gst_rtsp_stream_get_mtu:
 * @stream: a #GstRTSPStream
 *
 * Get the configured MTU in the payloader of @stream.
 *
 * Returns: the MTU of the payloader.
 */
guint
gst_rtsp_stream_get_mtu (GstRTSPStream * stream)
{
  GstRTSPStreamPrivate *priv;
  guint mtu;

  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), 0);

  priv = stream->priv;

  g_object_get (G_OBJECT (priv->payloader), "mtu", &mtu, NULL);

  return mtu;
}

/**
 * gst_rtsp_stream_set_address_pool:
 * @stream: a #GstRTSPStream
 * @pool: a #GstRTSPAddressPool
 *
 * configure @pool to be used as the address pool of @stream.
 */
void
gst_rtsp_stream_set_address_pool (GstRTSPStream * stream,
    GstRTSPAddressPool * pool)
{
  GstRTSPStreamPrivate *priv;
  GstRTSPAddressPool *old;

  g_return_if_fail (GST_IS_RTSP_STREAM (stream));

  priv = stream->priv;

  GST_LOG_OBJECT (stream, "set address pool %p", pool);

  g_mutex_lock (&priv->lock);
  if ((old = priv->pool) != pool)
    priv->pool = pool ? g_object_ref (pool) : NULL;
  else
    old = NULL;
  g_mutex_unlock (&priv->lock);

  if (old)
    g_object_unref (old);
}

/**
 * gst_rtsp_stream_get_address_pool:
 * @stream: a #GstRTSPStream
 *
 * Get the #GstRTSPAddressPool used as the address pool of @stream.
 *
 * Returns: (transfer full): the #GstRTSPAddressPool of @stream. g_object_unref() after
 * usage.
 */
GstRTSPAddressPool *
gst_rtsp_stream_get_address_pool (GstRTSPStream * stream)
{
  GstRTSPStreamPrivate *priv;
  GstRTSPAddressPool *result;

  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), NULL);

  priv = stream->priv;

  g_mutex_lock (&priv->lock);
  if ((result = priv->pool))
    g_object_ref (result);
  g_mutex_unlock (&priv->lock);

  return result;
}

/**
 * gst_rtsp_stream_get_address:
 * @stream: a #GstRTSPStream
 *
 * Get the multicast address of @stream.
 *
 * Returns: the #GstRTSPAddress of @stream or %NULL when no address could be
 * allocated. gst_rtsp_address_free() after usage.
 */
GstRTSPAddress *
gst_rtsp_stream_get_address (GstRTSPStream * stream)
{
  GstRTSPStreamPrivate *priv;
  GstRTSPAddress *result;

  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), NULL);

  priv = stream->priv;

  g_mutex_lock (&priv->lock);
  if (priv->addr == NULL) {
    if (priv->pool == NULL)
      goto no_pool;

    priv->addr = gst_rtsp_address_pool_acquire_address (priv->pool,
        GST_RTSP_ADDRESS_FLAG_EVEN_PORT | GST_RTSP_ADDRESS_FLAG_MULTICAST, 2);
    if (priv->addr == NULL)
      goto no_address;
  }
  result = gst_rtsp_address_copy (priv->addr);
  g_mutex_unlock (&priv->lock);

  return result;

  /* ERRORS */
no_pool:
  {
    GST_ERROR_OBJECT (stream, "no address pool specified");
    g_mutex_unlock (&priv->lock);
    return NULL;
  }
no_address:
  {
    GST_ERROR_OBJECT (stream, "failed to acquire address from pool");
    g_mutex_unlock (&priv->lock);
    return NULL;
  }
}

/**
 * gst_rtsp_stream_reserve_address:
 * @stream: a #GstRTSPStream
 *
 * Get a specific multicast address of @stream.
 *
 * Returns: the #GstRTSPAddress of @stream or %NULL when no address could be
 * allocated. gst_rtsp_address_free() after usage.
 */
GstRTSPAddress *
gst_rtsp_stream_reserve_address (GstRTSPStream * stream,
    const gchar * address, guint port, guint n_ports, guint ttl)
{
  GstRTSPStreamPrivate *priv;
  GstRTSPAddress *result;

  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), NULL);
  g_return_val_if_fail (address != NULL, NULL);
  g_return_val_if_fail (port > 0, NULL);
  g_return_val_if_fail (n_ports > 0, NULL);
  g_return_val_if_fail (ttl > 0, NULL);

  priv = stream->priv;

  g_mutex_lock (&priv->lock);
  if (priv->addr == NULL) {
    if (priv->pool == NULL)
      goto no_pool;

    priv->addr = gst_rtsp_address_pool_reserve_address (priv->pool, address,
        port, n_ports, ttl);
    if (priv->addr == NULL)
      goto no_address;
  } else {
    if (strcmp (priv->addr->address, address) ||
        priv->addr->port != port || priv->addr->n_ports != n_ports ||
        priv->addr->ttl != ttl)
      goto different_address;
  }
  result = gst_rtsp_address_copy (priv->addr);
  g_mutex_unlock (&priv->lock);

  return result;

  /* ERRORS */
no_pool:
  {
    GST_ERROR_OBJECT (stream, "no address pool specified");
    g_mutex_unlock (&priv->lock);
    return NULL;
  }
no_address:
  {
    GST_ERROR_OBJECT (stream, "failed to acquire address %s from pool",
        address);
    g_mutex_unlock (&priv->lock);
    return NULL;
  }
different_address:
  {
    GST_ERROR_OBJECT (stream, "address %s is not the same that was already"
        " reserved", address);
    g_mutex_unlock (&priv->lock);
    return NULL;
  }
}

/* must be called with lock */
static gboolean
alloc_ports (GstRTSPStream * stream)
{
  GstRTSPStreamPrivate *priv = stream->priv;
  GstStateChangeReturn ret;
  GstElement *udpsrc0, *udpsrc1;
  GstElement *udpsink0, *udpsink1;
  gint tmp_rtp, tmp_rtcp;
  guint count;
  gint rtpport, rtcpport;
  GSocket *socket;
  const gchar *host;

  udpsrc0 = NULL;
  udpsrc1 = NULL;
  udpsink0 = NULL;
  udpsink1 = NULL;
  count = 0;

  /* Start with random port */
  tmp_rtp = 0;

  if (priv->is_ipv6)
    host = "udp://[::0]";
  else
    host = "udp://0.0.0.0";

  /* try to allocate 2 UDP ports, the RTP port should be an even
   * number and the RTCP port should be the next (uneven) port */
again:
  udpsrc0 = gst_element_make_from_uri (GST_URI_SRC, host, NULL, NULL);
  if (udpsrc0 == NULL)
    goto no_udp_protocol;
  g_object_set (G_OBJECT (udpsrc0), "port", tmp_rtp, NULL);

  ret = gst_element_set_state (udpsrc0, GST_STATE_PAUSED);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    if (tmp_rtp != 0) {
      tmp_rtp += 2;
      if (++count > 20)
        goto no_ports;

      gst_element_set_state (udpsrc0, GST_STATE_NULL);
      gst_object_unref (udpsrc0);

      goto again;
    }
    goto no_udp_protocol;
  }

  g_object_get (G_OBJECT (udpsrc0), "port", &tmp_rtp, NULL);

  /* check if port is even */
  if ((tmp_rtp & 1) != 0) {
    /* port not even, close and allocate another */
    if (++count > 20)
      goto no_ports;

    gst_element_set_state (udpsrc0, GST_STATE_NULL);
    gst_object_unref (udpsrc0);

    tmp_rtp++;
    goto again;
  }

  /* allocate port+1 for RTCP now */
  udpsrc1 = gst_element_make_from_uri (GST_URI_SRC, host, NULL, NULL);
  if (udpsrc1 == NULL)
    goto no_udp_rtcp_protocol;

  /* set port */
  tmp_rtcp = tmp_rtp + 1;
  g_object_set (G_OBJECT (udpsrc1), "port", tmp_rtcp, NULL);

  ret = gst_element_set_state (udpsrc1, GST_STATE_PAUSED);
  /* tmp_rtcp port is busy already : retry to make rtp/rtcp pair */
  if (ret == GST_STATE_CHANGE_FAILURE) {

    if (++count > 20)
      goto no_ports;

    gst_element_set_state (udpsrc0, GST_STATE_NULL);
    gst_object_unref (udpsrc0);

    gst_element_set_state (udpsrc1, GST_STATE_NULL);
    gst_object_unref (udpsrc1);

    tmp_rtp += 2;
    goto again;
  }
  /* all fine, do port check */
  g_object_get (G_OBJECT (udpsrc0), "port", &rtpport, NULL);
  g_object_get (G_OBJECT (udpsrc1), "port", &rtcpport, NULL);

  /* this should not happen... */
  if (rtpport != tmp_rtp || rtcpport != tmp_rtcp)
    goto port_error;

  udpsink0 = gst_element_factory_make ("multiudpsink", NULL);
  if (!udpsink0)
    goto no_udp_protocol;

  g_object_get (G_OBJECT (udpsrc0), "used-socket", &socket, NULL);
  g_object_set (G_OBJECT (udpsink0), "socket", socket, NULL);
  g_object_unref (socket);
  g_object_set (G_OBJECT (udpsink0), "close-socket", FALSE, NULL);

  udpsink1 = gst_element_factory_make ("multiudpsink", NULL);
  if (!udpsink1)
    goto no_udp_protocol;

  if (g_object_class_find_property (G_OBJECT_GET_CLASS (udpsink0),
          "send-duplicates")) {
    g_object_set (G_OBJECT (udpsink0), "send-duplicates", FALSE, NULL);
    g_object_set (G_OBJECT (udpsink1), "send-duplicates", FALSE, NULL);
  } else {
    g_warning
        ("old multiudpsink version found without send-duplicates property");
  }

  if (g_object_class_find_property (G_OBJECT_GET_CLASS (udpsink0),
          "buffer-size")) {
    g_object_set (G_OBJECT (udpsink0), "buffer-size", priv->buffer_size, NULL);
  } else {
    GST_WARNING ("multiudpsink version found without buffer-size property");
  }

  g_object_get (G_OBJECT (udpsrc1), "used-socket", &socket, NULL);
  g_object_set (G_OBJECT (udpsink1), "socket", socket, NULL);
  g_object_unref (socket);
  g_object_set (G_OBJECT (udpsink1), "close-socket", FALSE, NULL);
  g_object_set (G_OBJECT (udpsink1), "sync", FALSE, NULL);
  g_object_set (G_OBJECT (udpsink1), "async", FALSE, NULL);
  g_object_set (G_OBJECT (udpsink0), "auto-multicast", FALSE, NULL);
  g_object_set (G_OBJECT (udpsink0), "loop", FALSE, NULL);
  g_object_set (G_OBJECT (udpsink1), "auto-multicast", FALSE, NULL);
  g_object_set (G_OBJECT (udpsink1), "loop", FALSE, NULL);

  /* we keep these elements, we will further configure them when the
   * client told us to really use the UDP ports. */
  priv->udpsrc[0] = udpsrc0;
  priv->udpsrc[1] = udpsrc1;
  priv->udpsink[0] = udpsink0;
  priv->udpsink[1] = udpsink1;
  priv->server_port.min = rtpport;
  priv->server_port.max = rtcpport;

  return TRUE;

  /* ERRORS */
no_udp_protocol:
  {
    goto cleanup;
  }
no_ports:
  {
    goto cleanup;
  }
no_udp_rtcp_protocol:
  {
    goto cleanup;
  }
port_error:
  {
    goto cleanup;
  }
cleanup:
  {
    if (udpsrc0) {
      gst_element_set_state (udpsrc0, GST_STATE_NULL);
      gst_object_unref (udpsrc0);
    }
    if (udpsrc1) {
      gst_element_set_state (udpsrc1, GST_STATE_NULL);
      gst_object_unref (udpsrc1);
    }
    if (udpsink0) {
      gst_element_set_state (udpsink0, GST_STATE_NULL);
      gst_object_unref (udpsink0);
    }
    if (udpsink1) {
      gst_element_set_state (udpsink1, GST_STATE_NULL);
      gst_object_unref (udpsink1);
    }
    return FALSE;
  }
}

/**
 * gst_rtsp_stream_get_server_port:
 * @stream: a #GstRTSPStream
 * @server_port: (out): result server port
 *
 * Fill @server_port with the port pair used by the server. This function can
 * only be called when @stream has been joined.
 */
void
gst_rtsp_stream_get_server_port (GstRTSPStream * stream,
    GstRTSPRange * server_port)
{
  GstRTSPStreamPrivate *priv;

  g_return_if_fail (GST_IS_RTSP_STREAM (stream));
  priv = stream->priv;
  g_return_if_fail (priv->is_joined);

  g_mutex_lock (&priv->lock);
  if (server_port)
    *server_port = priv->server_port;
  g_mutex_unlock (&priv->lock);
}

/**
 * gst_rtsp_stream_get_ssrc:
 * @stream: a #GstRTSPStream
 * @ssrc: (out): result ssrc
 *
 * Get the SSRC used by the RTP session of this stream. This function can only
 * be called when @stream has been joined.
 */
void
gst_rtsp_stream_get_ssrc (GstRTSPStream * stream, guint * ssrc)
{
  GstRTSPStreamPrivate *priv;

  g_return_if_fail (GST_IS_RTSP_STREAM (stream));
  priv = stream->priv;
  g_return_if_fail (priv->is_joined);

  g_mutex_lock (&priv->lock);
  if (ssrc && priv->session)
    g_object_get (priv->session, "internal-ssrc", ssrc, NULL);
  g_mutex_unlock (&priv->lock);
}

/* executed from streaming thread */
static void
caps_notify (GstPad * pad, GParamSpec * unused, GstRTSPStream * stream)
{
  GstRTSPStreamPrivate *priv = stream->priv;
  GstCaps *newcaps, *oldcaps;

  newcaps = gst_pad_get_current_caps (pad);

  GST_INFO ("stream %p received caps %p, %" GST_PTR_FORMAT, stream, newcaps,
      newcaps);

  g_mutex_lock (&priv->lock);
  oldcaps = priv->caps;
  priv->caps = newcaps;
  g_mutex_unlock (&priv->lock);

  if (oldcaps)
    gst_caps_unref (oldcaps);
}

static void
dump_structure (const GstStructure * s)
{
  gchar *sstr;

  sstr = gst_structure_to_string (s);
  GST_INFO ("structure: %s", sstr);
  g_free (sstr);
}

static GstRTSPStreamTransport *
find_transport (GstRTSPStream * stream, const gchar * rtcp_from)
{
  GstRTSPStreamPrivate *priv = stream->priv;
  GList *walk;
  GstRTSPStreamTransport *result = NULL;
  const gchar *tmp;
  gchar *dest;
  guint port;

  if (rtcp_from == NULL)
    return NULL;

  tmp = g_strrstr (rtcp_from, ":");
  if (tmp == NULL)
    return NULL;

  port = atoi (tmp + 1);
  dest = g_strndup (rtcp_from, tmp - rtcp_from);

  g_mutex_lock (&priv->lock);
  GST_INFO ("finding %s:%d in %d transports", dest, port,
      g_list_length (priv->transports));

  for (walk = priv->transports; walk; walk = g_list_next (walk)) {
    GstRTSPStreamTransport *trans = walk->data;
    const GstRTSPTransport *tr;
    gint min, max;

    tr = gst_rtsp_stream_transport_get_transport (trans);

    min = tr->client_port.min;
    max = tr->client_port.max;

    if ((strcmp (tr->destination, dest) == 0) && (min == port || max == port)) {
      result = trans;
      break;
    }
  }
  g_mutex_unlock (&priv->lock);

  g_free (dest);

  return result;
}

static GstRTSPStreamTransport *
check_transport (GObject * source, GstRTSPStream * stream)
{
  GstStructure *stats;
  GstRTSPStreamTransport *trans;

  /* see if we have a stream to match with the origin of the RTCP packet */
  trans = g_object_get_qdata (source, ssrc_stream_map_key);
  if (trans == NULL) {
    g_object_get (source, "stats", &stats, NULL);
    if (stats) {
      const gchar *rtcp_from;

      dump_structure (stats);

      rtcp_from = gst_structure_get_string (stats, "rtcp-from");
      if ((trans = find_transport (stream, rtcp_from))) {
        GST_INFO ("%p: found transport %p for source  %p", stream, trans,
            source);
        g_object_set_qdata (source, ssrc_stream_map_key, trans);
      }
      gst_structure_free (stats);
    }
  }
  return trans;
}


static void
on_new_ssrc (GObject * session, GObject * source, GstRTSPStream * stream)
{
  GstRTSPStreamTransport *trans;

  GST_INFO ("%p: new source %p", stream, source);

  trans = check_transport (source, stream);

  if (trans)
    GST_INFO ("%p: source %p for transport %p", stream, source, trans);
}

static void
on_ssrc_sdes (GObject * session, GObject * source, GstRTSPStream * stream)
{
  GST_INFO ("%p: new SDES %p", stream, source);
}

static void
on_ssrc_active (GObject * session, GObject * source, GstRTSPStream * stream)
{
  GstRTSPStreamTransport *trans;

  trans = check_transport (source, stream);

  if (trans) {
    GST_INFO ("%p: source %p in transport %p is active", stream, source, trans);
    gst_rtsp_stream_transport_keep_alive (trans);
  }
#ifdef DUMP_STATS
  {
    GstStructure *stats;
    g_object_get (source, "stats", &stats, NULL);
    if (stats) {
      dump_structure (stats);
      gst_structure_free (stats);
    }
  }
#endif
}

static void
on_bye_ssrc (GObject * session, GObject * source, GstRTSPStream * stream)
{
  GST_INFO ("%p: source %p bye", stream, source);
}

static void
on_bye_timeout (GObject * session, GObject * source, GstRTSPStream * stream)
{
  GstRTSPStreamTransport *trans;

  GST_INFO ("%p: source %p bye timeout", stream, source);

  if ((trans = g_object_get_qdata (source, ssrc_stream_map_key))) {
    gst_rtsp_stream_transport_set_timed_out (trans, TRUE);
  }
}

static void
on_timeout (GObject * session, GObject * source, GstRTSPStream * stream)
{
  GstRTSPStreamTransport *trans;

  GST_INFO ("%p: source %p timeout", stream, source);

  if ((trans = g_object_get_qdata (source, ssrc_stream_map_key))) {
    gst_rtsp_stream_transport_set_timed_out (trans, TRUE);
  }
}

static GstFlowReturn
handle_new_sample (GstAppSink * sink, gpointer user_data)
{
  GstRTSPStreamPrivate *priv;
  GList *walk;
  GstSample *sample;
  GstBuffer *buffer;
  GstRTSPStream *stream;

  sample = gst_app_sink_pull_sample (sink);
  if (!sample)
    return GST_FLOW_OK;

  stream = (GstRTSPStream *) user_data;
  priv = stream->priv;
  buffer = gst_sample_get_buffer (sample);

  g_mutex_lock (&priv->lock);
  for (walk = priv->transports; walk; walk = g_list_next (walk)) {
    GstRTSPStreamTransport *tr = (GstRTSPStreamTransport *) walk->data;

    if (GST_ELEMENT_CAST (sink) == priv->appsink[0]) {
      gst_rtsp_stream_transport_send_rtp (tr, buffer);
    } else {
      gst_rtsp_stream_transport_send_rtcp (tr, buffer);
    }
  }
  g_mutex_unlock (&priv->lock);

  gst_sample_unref (sample);

  return GST_FLOW_OK;
}

static GstAppSinkCallbacks sink_cb = {
  NULL,                         /* not interested in EOS */
  NULL,                         /* not interested in preroll samples */
  handle_new_sample,
};

/**
 * gst_rtsp_stream_join_bin:
 * @stream: a #GstRTSPStream
 * @bin: a #GstBin to join
 * @rtpbin: a rtpbin element in @bin
 * @state: the target state of the new elements
 *
 * Join the #Gstbin @bin that contains the element @rtpbin.
 *
 * @stream will link to @rtpbin, which must be inside @bin. The elements
 * added to @bin will be set to the state given in @state.
 *
 * Returns: %TRUE on success.
 */
gboolean
gst_rtsp_stream_join_bin (GstRTSPStream * stream, GstBin * bin,
    GstElement * rtpbin, GstState state)
{
  GstRTSPStreamPrivate *priv;
  gint i, idx;
  gchar *name;
  GstPad *pad, *teepad, *queuepad, *selpad;
  GstPadLinkReturn ret;

  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), FALSE);
  g_return_val_if_fail (GST_IS_BIN (bin), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (rtpbin), FALSE);

  priv = stream->priv;

  g_mutex_lock (&priv->lock);
  if (priv->is_joined)
    goto was_joined;

  /* create a session with the same index as the stream */
  idx = priv->idx;

  GST_INFO ("stream %p joining bin as session %d", stream, idx);

  if (!alloc_ports (stream))
    goto no_ports;

  /* get a pad for sending RTP */
  name = g_strdup_printf ("send_rtp_sink_%u", idx);
  priv->send_rtp_sink = gst_element_get_request_pad (rtpbin, name);
  g_free (name);
  /* link the RTP pad to the session manager, it should not really fail unless
   * this is not really an RTP pad */
  ret = gst_pad_link (priv->srcpad, priv->send_rtp_sink);
  if (ret != GST_PAD_LINK_OK)
    goto link_failed;

  /* get pads from the RTP session element for sending and receiving
   * RTP/RTCP*/
  name = g_strdup_printf ("send_rtp_src_%u", idx);
  priv->send_src[0] = gst_element_get_static_pad (rtpbin, name);
  g_free (name);
  name = g_strdup_printf ("send_rtcp_src_%u", idx);
  priv->send_src[1] = gst_element_get_request_pad (rtpbin, name);
  g_free (name);
  name = g_strdup_printf ("recv_rtp_sink_%u", idx);
  priv->recv_sink[0] = gst_element_get_request_pad (rtpbin, name);
  g_free (name);
  name = g_strdup_printf ("recv_rtcp_sink_%u", idx);
  priv->recv_sink[1] = gst_element_get_request_pad (rtpbin, name);
  g_free (name);

  /* get the session */
  g_signal_emit_by_name (rtpbin, "get-internal-session", idx, &priv->session);

  g_signal_connect (priv->session, "on-new-ssrc", (GCallback) on_new_ssrc,
      stream);
  g_signal_connect (priv->session, "on-ssrc-sdes", (GCallback) on_ssrc_sdes,
      stream);
  g_signal_connect (priv->session, "on-ssrc-active",
      (GCallback) on_ssrc_active, stream);
  g_signal_connect (priv->session, "on-bye-ssrc", (GCallback) on_bye_ssrc,
      stream);
  g_signal_connect (priv->session, "on-bye-timeout",
      (GCallback) on_bye_timeout, stream);
  g_signal_connect (priv->session, "on-timeout", (GCallback) on_timeout,
      stream);

  for (i = 0; i < 2; i++) {
    /* For the sender we create this bit of pipeline for both
     * RTP and RTCP. Sync and preroll are enabled on udpsink so
     * we need to add a queue before appsink to make the pipeline
     * not block. For the TCP case, we want to pump data to the
     * client as fast as possible anyway.
     *
     * .--------.      .-----.    .---------.
     * | rtpbin |      | tee |    | udpsink |
     * |       send->sink   src->sink       |
     * '--------'      |     |    '---------'
     *                 |     |    .---------.    .---------.
     *                 |     |    |  queue  |    | appsink |
     *                 |    src->sink      src->sink       |
     *                 '-----'    '---------'    '---------'
     */
    /* make tee for RTP/RTCP */
    priv->tee[i] = gst_element_factory_make ("tee", NULL);
    gst_bin_add (bin, priv->tee[i]);

    /* and link to rtpbin send pad */
    pad = gst_element_get_static_pad (priv->tee[i], "sink");
    gst_pad_link (priv->send_src[i], pad);
    gst_object_unref (pad);

    /* add udpsink */
    gst_bin_add (bin, priv->udpsink[i]);

    /* link tee to udpsink */
    teepad = gst_element_get_request_pad (priv->tee[i], "src_%u");
    pad = gst_element_get_static_pad (priv->udpsink[i], "sink");
    gst_pad_link (teepad, pad);
    gst_object_unref (pad);
    gst_object_unref (teepad);

    /* make queue */
    priv->appqueue[i] = gst_element_factory_make ("queue", NULL);
    gst_bin_add (bin, priv->appqueue[i]);
    /* and link to tee */
    teepad = gst_element_get_request_pad (priv->tee[i], "src_%u");
    pad = gst_element_get_static_pad (priv->appqueue[i], "sink");
    gst_pad_link (teepad, pad);
    gst_object_unref (pad);
    gst_object_unref (teepad);

    /* make appsink */
    priv->appsink[i] = gst_element_factory_make ("appsink", NULL);
    g_object_set (priv->appsink[i], "async", FALSE, "sync", FALSE, NULL);
    g_object_set (priv->appsink[i], "emit-signals", FALSE, NULL);
    gst_bin_add (bin, priv->appsink[i]);
    gst_app_sink_set_callbacks (GST_APP_SINK_CAST (priv->appsink[i]),
        &sink_cb, stream, NULL);
    /* and link to queue */
    queuepad = gst_element_get_static_pad (priv->appqueue[i], "src");
    pad = gst_element_get_static_pad (priv->appsink[i], "sink");
    gst_pad_link (queuepad, pad);
    gst_object_unref (pad);
    gst_object_unref (queuepad);

    /* For the receiver we create this bit of pipeline for both
     * RTP and RTCP. We receive RTP/RTCP on appsrc and udpsrc
     * and it is all funneled into the rtpbin receive pad.
     *
     * .--------.     .--------.    .--------.
     * | udpsrc |     | funnel |    | rtpbin |
     * |       src->sink      src->sink      |
     * '--------'     |        |    '--------'
     * .--------.     |        |
     * | appsrc |     |        |
     * |       src->sink       |
     * '--------'     '--------'
     */
    /* make funnel for the RTP/RTCP receivers */
    priv->funnel[i] = gst_element_factory_make ("funnel", NULL);
    gst_bin_add (bin, priv->funnel[i]);

    pad = gst_element_get_static_pad (priv->funnel[i], "src");
    gst_pad_link (pad, priv->recv_sink[i]);
    gst_object_unref (pad);

    /* we set and keep these to playing so that they don't cause NO_PREROLL return
     * values */
    gst_element_set_state (priv->udpsrc[i], GST_STATE_PLAYING);
    gst_element_set_locked_state (priv->udpsrc[i], TRUE);
    /* add udpsrc */
    gst_bin_add (bin, priv->udpsrc[i]);
    /* and link to the funnel */
    selpad = gst_element_get_request_pad (priv->funnel[i], "sink_%u");
    pad = gst_element_get_static_pad (priv->udpsrc[i], "src");
    gst_pad_link (pad, selpad);
    gst_object_unref (pad);
    gst_object_unref (selpad);

    /* make and add appsrc */
    priv->appsrc[i] = gst_element_factory_make ("appsrc", NULL);
    gst_bin_add (bin, priv->appsrc[i]);
    /* and link to the funnel */
    selpad = gst_element_get_request_pad (priv->funnel[i], "sink_%u");
    pad = gst_element_get_static_pad (priv->appsrc[i], "src");
    gst_pad_link (pad, selpad);
    gst_object_unref (pad);
    gst_object_unref (selpad);

    /* check if we need to set to a special state */
    if (state != GST_STATE_NULL) {
      gst_element_set_state (priv->udpsink[i], state);
      gst_element_set_state (priv->appsink[i], state);
      gst_element_set_state (priv->appqueue[i], state);
      gst_element_set_state (priv->tee[i], state);
      gst_element_set_state (priv->funnel[i], state);
      gst_element_set_state (priv->appsrc[i], state);
    }
  }

  /* be notified of caps changes */
  priv->caps_sig = g_signal_connect (priv->send_rtp_sink, "notify::caps",
      (GCallback) caps_notify, stream);

  priv->is_joined = TRUE;
  g_mutex_unlock (&priv->lock);

  return TRUE;

  /* ERRORS */
was_joined:
  {
    g_mutex_unlock (&priv->lock);
    return TRUE;
  }
no_ports:
  {
    g_mutex_unlock (&priv->lock);
    GST_WARNING ("failed to allocate ports %d", idx);
    return FALSE;
  }
link_failed:
  {
    GST_WARNING ("failed to link stream %d", idx);
    gst_object_unref (priv->send_rtp_sink);
    priv->send_rtp_sink = NULL;
    g_mutex_unlock (&priv->lock);
    return FALSE;
  }
}

/**
 * gst_rtsp_stream_leave_bin:
 * @stream: a #GstRTSPStream
 * @bin: a #GstBin
 * @rtpbin: a rtpbin #GstElement
 *
 * Remove the elements of @stream from @bin. @bin must be set
 * to the NULL state before calling this.
 *
 * Return: %TRUE on success.
 */
gboolean
gst_rtsp_stream_leave_bin (GstRTSPStream * stream, GstBin * bin,
    GstElement * rtpbin)
{
  GstRTSPStreamPrivate *priv;
  gint i;

  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), FALSE);
  g_return_val_if_fail (GST_IS_BIN (bin), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (rtpbin), FALSE);

  priv = stream->priv;

  g_mutex_lock (&priv->lock);
  if (!priv->is_joined)
    goto was_not_joined;

  /* all transports must be removed by now */
  g_return_val_if_fail (priv->transports == NULL, FALSE);

  GST_INFO ("stream %p leaving bin", stream);

  gst_pad_unlink (priv->srcpad, priv->send_rtp_sink);
  g_signal_handler_disconnect (priv->send_rtp_sink, priv->caps_sig);
  gst_element_release_request_pad (rtpbin, priv->send_rtp_sink);
  gst_object_unref (priv->send_rtp_sink);
  priv->send_rtp_sink = NULL;

  for (i = 0; i < 2; i++) {
    /* and set udpsrc to NULL now before removing */
    gst_element_set_locked_state (priv->udpsrc[i], FALSE);
    gst_element_set_state (priv->udpsrc[i], GST_STATE_NULL);

    /* removing them should also nicely release the request
     * pads when they finalize */
    gst_bin_remove (bin, priv->udpsrc[i]);
    gst_bin_remove (bin, priv->udpsink[i]);
    gst_bin_remove (bin, priv->appsrc[i]);
    gst_bin_remove (bin, priv->appsink[i]);
    gst_bin_remove (bin, priv->appqueue[i]);
    gst_bin_remove (bin, priv->tee[i]);
    gst_bin_remove (bin, priv->funnel[i]);

    gst_element_release_request_pad (rtpbin, priv->recv_sink[i]);
    gst_object_unref (priv->recv_sink[i]);
    priv->recv_sink[i] = NULL;

    priv->udpsrc[i] = NULL;
    priv->udpsink[i] = NULL;
    priv->appsrc[i] = NULL;
    priv->appsink[i] = NULL;
    priv->appqueue[i] = NULL;
    priv->tee[i] = NULL;
    priv->funnel[i] = NULL;
  }
  gst_object_unref (priv->send_src[0]);
  priv->send_src[0] = NULL;

  gst_element_release_request_pad (rtpbin, priv->send_src[1]);
  gst_object_unref (priv->send_src[1]);
  priv->send_src[1] = NULL;

  g_object_unref (priv->session);
  if (priv->caps)
    gst_caps_unref (priv->caps);

  priv->is_joined = FALSE;
  g_mutex_unlock (&priv->lock);

  return TRUE;

was_not_joined:
  {
    return TRUE;
  }
}

/**
 * gst_rtsp_stream_get_rtpinfo:
 * @stream: a #GstRTSPStream
 * @rtptime: result RTP timestamp
 * @seq: result RTP seqnum
 *
 * Retrieve the current rtptime and seq. This is used to
 * construct a RTPInfo reply header.
 *
 * Returns: %TRUE when rtptime and seq could be determined.
 */
gboolean
gst_rtsp_stream_get_rtpinfo (GstRTSPStream * stream,
    guint * rtptime, guint * seq)
{
  GstRTSPStreamPrivate *priv;
  GObjectClass *payobjclass;

  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), FALSE);
  g_return_val_if_fail (rtptime != NULL, FALSE);
  g_return_val_if_fail (seq != NULL, FALSE);

  priv = stream->priv;

  payobjclass = G_OBJECT_GET_CLASS (priv->payloader);

  if (!g_object_class_find_property (payobjclass, "seqnum") ||
      !g_object_class_find_property (payobjclass, "timestamp"))
    return FALSE;

  g_object_get (priv->payloader, "seqnum", seq, "timestamp", rtptime, NULL);

  return TRUE;
}

/**
 * gst_rtsp_stream_get_caps:
 * @stream: a #GstRTSPStream
 *
 * Retrieve the current caps of @stream.
 *
 * Returns: (transfer full): the #GstCaps of @stream. use gst_caps_unref()
 *    after usage.
 */
GstCaps *
gst_rtsp_stream_get_caps (GstRTSPStream * stream)
{
  GstRTSPStreamPrivate *priv;
  GstCaps *result;

  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), NULL);

  priv = stream->priv;

  g_mutex_lock (&priv->lock);
  if ((result = priv->caps))
    gst_caps_ref (result);
  g_mutex_unlock (&priv->lock);

  return result;
}

/**
 * gst_rtsp_stream_recv_rtp:
 * @stream: a #GstRTSPStream
 * @buffer: (transfer full): a #GstBuffer
 *
 * Handle an RTP buffer for the stream. This method is usually called when a
 * message has been received from a client using the TCP transport.
 *
 * This function takes ownership of @buffer.
 *
 * Returns: a GstFlowReturn.
 */
GstFlowReturn
gst_rtsp_stream_recv_rtp (GstRTSPStream * stream, GstBuffer * buffer)
{
  GstRTSPStreamPrivate *priv;
  GstFlowReturn ret;
  GstElement *element;

  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), GST_FLOW_ERROR);
  priv = stream->priv;
  g_return_val_if_fail (GST_IS_BUFFER (buffer), GST_FLOW_ERROR);
  g_return_val_if_fail (priv->is_joined, FALSE);

  g_mutex_lock (&priv->lock);
  element = gst_object_ref (priv->appsrc[0]);
  g_mutex_unlock (&priv->lock);

  ret = gst_app_src_push_buffer (GST_APP_SRC_CAST (element), buffer);

  gst_object_unref (element);

  return ret;
}

/**
 * gst_rtsp_stream_recv_rtcp:
 * @stream: a #GstRTSPStream
 * @buffer: (transfer full): a #GstBuffer
 *
 * Handle an RTCP buffer for the stream. This method is usually called when a
 * message has been received from a client using the TCP transport.
 *
 * This function takes ownership of @buffer.
 *
 * Returns: a GstFlowReturn.
 */
GstFlowReturn
gst_rtsp_stream_recv_rtcp (GstRTSPStream * stream, GstBuffer * buffer)
{
  GstRTSPStreamPrivate *priv;
  GstFlowReturn ret;
  GstElement *element;

  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), GST_FLOW_ERROR);
  priv = stream->priv;
  g_return_val_if_fail (GST_IS_BUFFER (buffer), GST_FLOW_ERROR);
  g_return_val_if_fail (priv->is_joined, FALSE);

  g_mutex_lock (&priv->lock);
  element = gst_object_ref (priv->appsrc[1]);
  g_mutex_unlock (&priv->lock);

  ret = gst_app_src_push_buffer (GST_APP_SRC_CAST (element), buffer);

  gst_object_unref (element);

  return ret;
}

/* must be called with lock */
static gboolean
update_transport (GstRTSPStream * stream, GstRTSPStreamTransport * trans,
    gboolean add)
{
  GstRTSPStreamPrivate *priv = stream->priv;
  const GstRTSPTransport *tr;

  tr = gst_rtsp_stream_transport_get_transport (trans);

  switch (tr->lower_transport) {
    case GST_RTSP_LOWER_TRANS_UDP:
    case GST_RTSP_LOWER_TRANS_UDP_MCAST:
    {
      gchar *dest;
      gint min, max;
      guint ttl = 0;

      dest = tr->destination;
      if (tr->lower_transport == GST_RTSP_LOWER_TRANS_UDP_MCAST) {
        min = tr->port.min;
        max = tr->port.max;
        ttl = tr->ttl;
      } else {
        min = tr->client_port.min;
        max = tr->client_port.max;
      }

      if (add) {
        GST_INFO ("adding %s:%d-%d", dest, min, max);
        g_signal_emit_by_name (priv->udpsink[0], "add", dest, min, NULL);
        g_signal_emit_by_name (priv->udpsink[1], "add", dest, max, NULL);
        if (ttl > 0) {
          GST_INFO ("setting ttl-mc %d", ttl);
          g_object_set (G_OBJECT (priv->udpsink[0]), "ttl-mc", ttl, NULL);
          g_object_set (G_OBJECT (priv->udpsink[1]), "ttl-mc", ttl, NULL);
        }
        priv->transports = g_list_prepend (priv->transports, trans);
      } else {
        GST_INFO ("removing %s:%d-%d", dest, min, max);
        g_signal_emit_by_name (priv->udpsink[0], "remove", dest, min, NULL);
        g_signal_emit_by_name (priv->udpsink[1], "remove", dest, max, NULL);
        priv->transports = g_list_remove (priv->transports, trans);
      }
      break;
    }
    case GST_RTSP_LOWER_TRANS_TCP:
      if (add) {
        GST_INFO ("adding TCP %s", tr->destination);
        priv->transports = g_list_prepend (priv->transports, trans);
      } else {
        GST_INFO ("removing TCP %s", tr->destination);
        priv->transports = g_list_remove (priv->transports, trans);
      }
      break;
    default:
      goto unknown_transport;
  }
  return TRUE;

  /* ERRORS */
unknown_transport:
  {
    GST_INFO ("Unknown transport %d", tr->lower_transport);
    return FALSE;
  }
}


/**
 * gst_rtsp_stream_add_transport:
 * @stream: a #GstRTSPStream
 * @trans: a #GstRTSPStreamTransport
 *
 * Add the transport in @trans to @stream. The media of @stream will
 * then also be send to the values configured in @trans.
 *
 * @stream must be joined to a bin.
 *
 * @trans must contain a valid #GstRTSPTransport.
 *
 * Returns: %TRUE if @trans was added
 */
gboolean
gst_rtsp_stream_add_transport (GstRTSPStream * stream,
    GstRTSPStreamTransport * trans)
{
  GstRTSPStreamPrivate *priv;
  gboolean res;

  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), FALSE);
  priv = stream->priv;
  g_return_val_if_fail (GST_IS_RTSP_STREAM_TRANSPORT (trans), FALSE);
  g_return_val_if_fail (priv->is_joined, FALSE);

  g_mutex_lock (&priv->lock);
  res = update_transport (stream, trans, TRUE);
  g_mutex_unlock (&priv->lock);

  return res;
}

/**
 * gst_rtsp_stream_remove_transport:
 * @stream: a #GstRTSPStream
 * @trans: a #GstRTSPStreamTransport
 *
 * Remove the transport in @trans from @stream. The media of @stream will
 * not be sent to the values configured in @trans.
 *
 * @stream must be joined to a bin.
 *
 * @trans must contain a valid #GstRTSPTransport.
 *
 * Returns: %TRUE if @trans was removed
 */
gboolean
gst_rtsp_stream_remove_transport (GstRTSPStream * stream,
    GstRTSPStreamTransport * trans)
{
  GstRTSPStreamPrivate *priv;
  gboolean res;

  g_return_val_if_fail (GST_IS_RTSP_STREAM (stream), FALSE);
  priv = stream->priv;
  g_return_val_if_fail (GST_IS_RTSP_STREAM_TRANSPORT (trans), FALSE);
  g_return_val_if_fail (priv->is_joined, FALSE);

  g_mutex_lock (&priv->lock);
  res = update_transport (stream, trans, FALSE);
  g_mutex_unlock (&priv->lock);

  return res;
}
