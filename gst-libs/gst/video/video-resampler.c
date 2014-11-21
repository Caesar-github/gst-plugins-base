/* GStreamer
 * Copyright (C) <2014> Wim Taymans <wim.taymans@gmail.com>
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "video-resampler.h"


#define DEFAULT_OPT_CUBIC_B (1.0 / 3.0)
#define DEFAULT_OPT_CUBIC_C (1.0 / 3.0)

#define DEFAULT_OPT_ENVELOPE 2.0
#define DEFAULT_OPT_SHARPNESS 1.0
#define DEFAULT_OPT_SHARPEN 0.0

#define DEFAULT_OPT_MAX_TAPS 16

typedef struct _ResamplerParams ResamplerParams;

struct _ResamplerParams
{
  GstVideoResamplerMethod method;
  GstVideoResamplerFlags flags;

  gdouble shift;

    gdouble (*get_tap) (ResamplerParams * params, gint l, gint xi, gdouble x);

  /* for cubic */
  gdouble b, c;
  /* used by lanczos */
  gdouble ex, fx, dx;
  /* extra params */
  gdouble envelope;
  gdouble sharpness;
  gdouble sharpen;

  GstVideoResampler *resampler;
};

static gdouble
get_opt_double (GstStructure * options, const gchar * name, gdouble def)
{
  gdouble res;
  if (!options || !gst_structure_get_double (options, name, &res))
    res = def;
  return res;
}

static gint
get_opt_int (GstStructure * options, const gchar * name, gint def)
{
  gint res;
  if (!options || !gst_structure_get_int (options, name, &res))
    res = def;
  return res;
}

#define GET_OPT_CUBIC_B(options) get_opt_double(options, \
    GST_VIDEO_RESAMPLER_OPT_CUBIC_B, DEFAULT_OPT_CUBIC_B)
#define GET_OPT_CUBIC_C(options) get_opt_double(options, \
    GST_VIDEO_RESAMPLER_OPT_CUBIC_C, DEFAULT_OPT_CUBIC_C)
#define GET_OPT_ENVELOPE(options) get_opt_double(options, \
    GST_VIDEO_RESAMPLER_OPT_ENVELOPE, DEFAULT_OPT_ENVELOPE)
#define GET_OPT_SHARPNESS(options) get_opt_double(options, \
    GST_VIDEO_RESAMPLER_OPT_SHARPNESS, DEFAULT_OPT_SHARPNESS)
#define GET_OPT_SHARPEN(options) get_opt_double(options, \
    GST_VIDEO_RESAMPLER_OPT_SHARPEN, DEFAULT_OPT_SHARPEN)
#define GET_OPT_MAX_TAPS(options) get_opt_int(options, \
    GST_VIDEO_RESAMPLER_OPT_MAX_TAPS, DEFAULT_OPT_MAX_TAPS)

static double
sinc (double x)
{
  if (x == 0)
    return 1;

  return sin (G_PI * x) / (G_PI * x);
}

static double
envelope (double x)
{
  if (x <= -1 || x >= 1)
    return 0;
  return sinc (x);
}

static gdouble
get_nearest_tap (ResamplerParams * params, gint l, gint xi, gdouble x)
{
  return 1.0;
}

static gdouble
get_linear_tap (ResamplerParams * params, gint l, gint xi, gdouble x)
{
  gdouble n_taps;
  gdouble res, a;
  gint xl = xi + l;

  n_taps = (params->resampler->max_taps + 1) / 2;

  a = fabs (x - xl);

  if (a < n_taps)
    res = (n_taps - a) / (gdouble) n_taps;
  else
    res = 0.0;

  return res;
}

static gdouble
bicubic (gdouble s, gdouble b, gdouble c)
{
  gdouble s2, s3;

  s = fabs (s);
  s2 = s * s;
  s3 = s2 * s;

  if (s <= 1.0)
    return ((12.0 - 9.0 * b - 6.0 * c) * s3 +
        (-18.0 + 12.0 * b + 6.0 * c) * s2 + (6.0 - 2.0 * b)) / 6.0;
  else if (s <= 2.0)
    return ((-b - 6.0 * c) * s3 +
        (6.0 * b + 30.0 * c) * s2 +
        (-12.0 * b - 48.0 * c) * s + (8.0 * b + 24.0 * c)) / 6.0;
  else
    return 0.0;
}

static gdouble
get_cubic_tap (ResamplerParams * params, gint l, gint xi, gdouble x)
{
  gdouble a, b, c, res;

  a = x - (xi + 1);

  b = params->b;
  c = params->c;

  if (l == 0)
    res = bicubic (1.0 + a, b, c);
  else if (l == 1)
    res = bicubic (a, b, c);
  else if (l == 2)
    res = bicubic (1.0 - a, b, c);
  else
    res = bicubic (2.0 - a, b, c);

  return res;
}

static gdouble
get_sinc_tap (ResamplerParams * params, gint l, gint xi, gdouble x)
{
  gint xl = xi + l;
  return sinc (x - xl);
}

static gdouble
get_lanczos_tap (ResamplerParams * params, gint l, gint xi, gdouble x)
{
  gint xl = xi + l;
  gdouble env = envelope ((x - xl) * params->ex);
  return (sinc ((x - xl) * params->fx) - params->sharpen) * env;
}

static void
resampler_calculate_taps (ResamplerParams * params)
{
  GstVideoResampler *resampler = params->resampler;
  gint j;
  guint32 *offset, *n_taps, *phase;
  gint tap_offs;
  gint max_taps;
  gint in_size, out_size;
  gdouble shift;
  gdouble corr;

  in_size = resampler->in_size;
  out_size = resampler->out_size;

  max_taps = resampler->max_taps;
  tap_offs = (max_taps - 1) / 2;
  corr = (max_taps == 1 ? 0.0 : 0.5);

  shift = params->shift;

  resampler->taps = g_malloc (sizeof (gdouble) * max_taps * out_size);
  n_taps = resampler->n_taps = g_malloc (sizeof (guint32) * out_size);
  offset = resampler->offset = g_malloc (sizeof (guint32) * out_size);
  phase = resampler->phase = g_malloc (sizeof (guint32) * out_size);

  for (j = 0; j < out_size; j++) {
    gdouble ox, x;
    gint xi;
    gint l;
    gdouble weight;
    gdouble *taps;

    /* center of the output pixel */
    ox = (0.5 + (gdouble) j - shift) / out_size;
    /* x is the source pixel to use, can be fractional */
    x = ox * (gdouble) in_size - corr;
    x = CLAMP (x, 0, in_size - 1);
    /* this is the first source pixel to use */
    xi = floor (x - tap_offs);

    offset[j] = xi;
    phase[j] = j;
    n_taps[j] = max_taps;
    weight = 0;
    taps = resampler->taps + j * max_taps;

    for (l = 0; l < max_taps; l++) {
      taps[l] = params->get_tap (params, l, xi, x);
      weight += taps[l];
    }

    for (l = 0; l < max_taps; l++)
      taps[l] /= weight;

    if (xi < 0) {
      gint sh = -xi;

      for (l = 0; l < sh; l++) {
        taps[sh] += taps[l];
      }
      for (l = 0; l < max_taps - sh; l++) {
        taps[l] = taps[sh + l];
      }
      for (; l < max_taps; l++) {
        taps[l] = 0;
      }
      offset[j] += sh;
    }
    if (xi > in_size - max_taps) {
      gint sh = xi - (in_size - max_taps);

      for (l = 0; l < sh; l++) {
        taps[max_taps - sh - 1] += taps[max_taps - sh + l];
      }
      for (l = 0; l < max_taps - sh; l++) {
        taps[max_taps - 1 - l] = taps[max_taps - 1 - sh - l];
      }
      for (l = 0; l < sh; l++) {
        taps[l] = 0;
      }
      offset[j] -= sh;
    }
  }
}

static void
resampler_dump (GstVideoResampler * resampler)
{
#if 0
  gint i, max_taps, out_size;

  out_size = resampler->out_size;
  max_taps = resampler->max_taps;

  for (i = 0; i < out_size; i++) {
    gint j, o, phase, n_taps;
    gdouble sum;

    o = resampler->offset[i];
    n_taps = resampler->n_taps[i];
    phase = resampler->phase[i];

    printf ("%u: \t%d  ", i, o);
    sum = 0;
    for (j = 0; j < n_taps; j++) {
      gdouble tap;
      tap = resampler->taps[phase * max_taps + j];
      printf ("\t%f ", tap);
      sum += tap;
    }
    printf ("\t: sum %f\n", sum);
  }
#endif
}


/**
 * gst_video_resampler_new:
 * @resampler: a #GstVideoResampler
 * @method: a #GstVideoResamplerMethod
 * @flags: #GstVideoResamplerFlags
 * @n_phases: number of phases to use
 * @n_taps: number of taps to use
 * @in_size: number of source elements
 * @out_size: number of destination elements
 * @options: extra options
 *
 * Make a new resampler. @in_size source elements will
 * be resampled to @out_size destination elements.
 *
 * @n_taps specifies the amount of elements to use from the source for one output
 * element. If n_taps is 0, this function chooses a good value automatically based
 * on the @method and @in_size/@out_size.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.6
 */
gboolean
gst_video_resampler_init (GstVideoResampler * resampler,
    GstVideoResamplerMethod method, GstVideoResamplerFlags flags,
    guint n_phases, guint n_taps, gdouble shift, guint in_size, guint out_size,
    GstStructure * options)
{
  ResamplerParams params;
  gint max_taps;

  g_return_val_if_fail (in_size != 0, FALSE);
  g_return_val_if_fail (out_size != 0, FALSE);
  g_return_val_if_fail (n_phases == out_size, FALSE);

  resampler->in_size = in_size;
  resampler->out_size = out_size;
  resampler->n_phases = n_phases;

  params.method = method;
  params.flags = flags;
  params.shift = shift;
  params.resampler = resampler;

  GST_DEBUG ("%d %u  %u->%u", method, n_taps, in_size, out_size);

  max_taps = GET_OPT_MAX_TAPS (options);

  switch (method) {
    case GST_VIDEO_RESAMPLER_METHOD_NEAREST:
      params.get_tap = get_nearest_tap;
      if (n_taps == 0)
        n_taps = 1;
      break;
    case GST_VIDEO_RESAMPLER_METHOD_LINEAR:
      params.get_tap = get_linear_tap;
      if (n_taps == 0)
        n_taps = 2;
      break;
    case GST_VIDEO_RESAMPLER_METHOD_CUBIC:
      params.b = GET_OPT_CUBIC_B (options);
      params.c = GET_OPT_CUBIC_C (options);
      n_taps = 4;
      params.get_tap = get_cubic_tap;
      break;
    case GST_VIDEO_RESAMPLER_METHOD_SINC:
      params.get_tap = get_sinc_tap;
      if (n_taps == 0)
        n_taps = 4;
      break;
    case GST_VIDEO_RESAMPLER_METHOD_LANCZOS:
    {
      gdouble resample_inc = in_size / (gdouble) out_size;

      params.envelope = GET_OPT_ENVELOPE (options);
      params.sharpness = GET_OPT_SHARPNESS (options);
      params.sharpen = GET_OPT_SHARPEN (options);

      if (resample_inc > 1.0) {
        params.fx = (1.0 / resample_inc) * params.sharpness;
      } else {
        params.fx = (1.0) * params.sharpness;
      }
      params.ex = params.fx / params.envelope;
      params.dx = ceil (params.envelope / params.fx);

      if (n_taps == 0)
        n_taps = 2 * params.dx;
      params.get_tap = get_lanczos_tap;
      break;
    }
    default:
      break;
  }

  n_taps = CLAMP (n_taps, 0, max_taps);

  if (n_taps > in_size)
    n_taps = in_size;

  resampler->max_taps = n_taps;

  resampler_calculate_taps (&params);

  resampler_dump (resampler);

  return TRUE;
}

/**
 * gst_video_resampler_clear:
 * @resampler: a #GstVideoResampler
 *
 * Clear a previously initialized #GstVideoResampler @resampler.
 *
 * Since: 1.6
 */
void
gst_video_resampler_clear (GstVideoResampler * resampler)
{
  g_return_if_fail (resampler != NULL);

  g_free (resampler->phase);
  g_free (resampler->offset);
  g_free (resampler->n_taps);
  g_free (resampler->taps);
}
