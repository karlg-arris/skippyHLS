/* skippyHLS
 *
 * Copyright (C) 2010 Marc-Andre Lureau <marcandre.lureau@gmail.com>
 * Copyright (C) 2010 Andoni Morales Alastruey <ylatuya@gmail.com>
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *  Author: Youness Alaoui <youness.alaoui@collabora.co.uk>, Collabora Ltd.
 *  Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 * Copyright (C) 2014 Sebastian Dröge <sebastian@centricular.com>
 * Copyright (C) 2015, SoundCloud Ltd. (http://soundcloud.com)
 *  Author: Stephan Hesse <stephan@soundcloud.com>, SoundCloud Ltd.
 *
 * skippy_fragment.c:
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

#include <nettle/aes.h>
#include <nettle/cbc.h>

#include <glib.h>
#include <gst/base/gsttypefindhelper.h>
#include <gst/base/gstadapter.h>

#include "skippy_fragment.h"

#define SKIPPY_FRAGMENT_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), TYPE_SKIPPY_FRAGMENT, SkippyFragmentPrivate))

GST_DEBUG_CATEGORY_STATIC (skippy_fragment_debug);
#define GST_CAT_DEFAULT skippy_fragment_debug

G_DEFINE_TYPE (SkippyFragment, skippy_fragment, G_TYPE_OBJECT);

static void skippy_fragment_dispose (GObject * object);

static void
skippy_fragment_class_init (SkippyFragmentClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = skippy_fragment_dispose;

  GST_DEBUG_CATEGORY_INIT (skippy_fragment_debug, "skippyhls-fragment", 0, "HLS fragment");
}

static void
skippy_fragment_init (SkippyFragment * fragment)
{
  fragment->download_start_time = gst_util_get_timestamp ();
  fragment->start_time = 0;
  fragment->stop_time = 0;
  fragment->duration = 0;
  fragment->range_start = 0;
  fragment->range_end = -1;
  fragment->completed = FALSE;
  fragment->cancelled = FALSE;
  fragment->discontinuous = FALSE;
  fragment->size = 0;
}

SkippyFragment *
skippy_fragment_new (const gchar* uri)
{
  SkippyFragment* fragment;

  g_return_val_if_fail (uri, NULL);

  fragment = SKIPPY_FRAGMENT (g_object_new (TYPE_SKIPPY_FRAGMENT, NULL));
  fragment->uri = g_strdup (uri);
  return fragment;
}

void
skippy_fragment_dispose (GObject * object)
{
  SkippyFragment *fragment = SKIPPY_FRAGMENT (object);

  g_free (fragment->uri);
  
  g_free (fragment->key_uri);

  G_OBJECT_CLASS (skippy_fragment_parent_class)->dispose (object);

  GST_TRACE ("Done disposing.");
}