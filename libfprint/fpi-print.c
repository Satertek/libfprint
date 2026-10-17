/*
 * FPrint Print handling - Private APIs
 * Copyright (C) 2007 Daniel Drake <dsd@gentoo.org>
 * Copyright (C) 2019 Benjamin Berg <bberg@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "print"
#include "fpi-log.h"

#include <math.h>
#include "fp-print-private.h"
#include "fpi-device.h"
#include "fpi-compat.h"

/**
 * SECTION: fpi-print
 * @title: Internal FpPrint
 * @short_description: Internal fingerprint handling routines
 *
 * Interaction with prints and their storage. See also the public
 * #FpPrint routines.
 */

/**
 * fpi_print_add_print:
 * @print: A #FpPrint
 * @add: Print to append to @print
 *
 * Appends the single #FPI_PRINT_NBIS print from @add to the collection of
 * prints in @print. Both print objects need to be of type #FPI_PRINT_NBIS
 * for this to work.
 */
void
fpi_print_add_print (FpPrint *print, FpPrint *add)
{
  g_return_if_fail (print->type == FPI_PRINT_NBIS);
  g_return_if_fail (add->type == FPI_PRINT_NBIS);

  g_assert (add->prints->len == 1);
  g_ptr_array_add (print->prints, g_memdup2 (add->prints->pdata[0], sizeof (struct xyt_struct)));

  if (add->ridge_templates)
    {
      guint j;
      if (!print->ridge_templates)
        print->ridge_templates = g_ptr_array_new_with_free_func (fpi_ridge_map_free);
      for (j = 0; j < add->ridge_templates->len; j++)
        {
          FpiRidgeMap *orig = g_ptr_array_index (add->ridge_templates, j);
          FpiRidgeMap *copy = g_new0 (FpiRidgeMap, 1);
          copy->width = orig->width;
          copy->height = orig->height;
          copy->data = g_memdup2 (orig->data, orig->width * orig->height);
          g_ptr_array_add (print->ridge_templates, copy);
        }
    }
}

/**
 * fpi_print_set_type:
 * @print: A #FpPrint
 * @type: The newly type of the print data
 *
 * This function can only be called exactly once. Drivers should
 * call it after creating a new print, or to initialize the template
 * print passed during enrollment.
 */
void
fpi_print_set_type (FpPrint     *print,
                    FpiPrintType type)
{
  g_return_if_fail (FP_IS_PRINT (print));
  /* We only allow setting this once! */
  g_return_if_fail (print->type == FPI_PRINT_UNDEFINED);

  print->type = type;
  if (print->type == FPI_PRINT_NBIS)
    {
      g_assert_null (print->prints);
      print->prints = g_ptr_array_new_with_free_func (g_free);
      if (!print->ridge_templates)
        print->ridge_templates = g_ptr_array_new_with_free_func (fpi_ridge_map_free);
    }
  g_object_notify (G_OBJECT (print), "fpi-type");
}

/**
 * fpi_print_set_device_stored:
 * @print: A #FpPrint
 * @device_stored: Whether the print is stored on the device or not
 *
 * Drivers must set this to %TRUE for any print that is really a handle
 * for data that is stored on the device itself.
 */
void
fpi_print_set_device_stored (FpPrint *print,
                             gboolean device_stored)
{
  g_return_if_fail (FP_IS_PRINT (print));

  print->device_stored = device_stored;
  g_object_notify (G_OBJECT (print), "device-stored");
}

/* XXX: This is the old version, but wouldn't it be smarter to instead
 * use the highest quality mintutiae? Possibly just using bz_prune from
 * upstream? */
static void
minutiae_to_xyt (struct fp_minutiae *minutiae,
                 int                 bwidth,
                 int                 bheight,
                 struct xyt_struct  *xyt)
{
  int i;
  struct fp_minutia *minutia;
  struct minutiae_struct c[MAX_FILE_MINUTIAE];

  /* struct xyt_struct uses arrays of MAX_BOZORTH_MINUTIAE (200) */
  int nmin = min (minutiae->num, MAX_BOZORTH_MINUTIAE);

  for (i = 0; i < nmin; i++)
    {
      minutia = minutiae->list[i];

      lfs2nist_minutia_XYT (&c[i].col[0], &c[i].col[1], &c[i].col[2],
                            minutia, bwidth, bheight);
      c[i].col[3] = sround (minutia->reliability * 100.0);

      if (c[i].col[2] > 180)
        c[i].col[2] -= 360;
    }

  qsort ((void *) &c, (size_t) nmin, sizeof (struct minutiae_struct),
         sort_x_y);

  for (i = 0; i < nmin; i++)
    {
      xyt->xcol[i]     = c[i].col[0];
      xyt->ycol[i]     = c[i].col[1];
      xyt->thetacol[i] = c[i].col[2];
    }
  xyt->nrows = nmin;
}

/**
 * fpi_print_add_from_image:
 * @print: A #FpPrint
 * @image: A #FpImage
 * @error: Return location for error
 *
 * Extracts the minutiae from the given image and adds it to @print of
 * type #FPI_PRINT_NBIS.
 *
 * The @image will be kept so that API users can get retrieve it e.g.
 * for debugging purposes.
 *
 * Returns: %TRUE on success
 */
/**
 * fpi_print_add_from_image:
 * @print: A #FpPrint
 * @image: A #FpImage
 * @error: Return location for error
 *
 * Extracts the minutiae and/or ridge map from the given image and adds it to @print of
 * type #FPI_PRINT_NBIS.
 *
 * The @image will be kept so that API users can get retrieve it e.g.
 * for debugging purposes.
 *
 * Returns: %TRUE on success
 */
gboolean
fpi_print_add_from_image (FpPrint *print,
                          FpImage *image,
                          GError **error)
{
  GPtrArray *minutiae;
  struct fp_minutiae _minutiae;
  struct xyt_struct *xyt;

  if (print->type != FPI_PRINT_NBIS || !image)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "Cannot add print data from image!");
      return FALSE;
    }

  minutiae = fp_image_get_minutiae (image);
  if (minutiae && minutiae->len > 0)
    {
      _minutiae.num = minutiae->len;
      _minutiae.list = (struct fp_minutia **) minutiae->pdata;
      _minutiae.alloc = minutiae->len;

      xyt = g_new0 (struct xyt_struct, 1);
      minutiae_to_xyt (&_minutiae, image->width, image->height, xyt);
      g_ptr_array_add (print->prints, xyt);
    }
  else
    {
      xyt = g_new0 (struct xyt_struct, 1);
      xyt->nrows = 0;
      g_ptr_array_add (print->prints, xyt);
    }

  if (image->data && image->width > 0 && image->height > 0)
    {
      FpiRidgeMap *rm = g_new0 (FpiRidgeMap, 1);
      rm->width = image->width;
      rm->height = image->height;
      rm->data = g_memdup2 (image->data, image->width * image->height);
      if (!print->ridge_templates)
        print->ridge_templates = g_ptr_array_new_with_free_func (fpi_ridge_map_free);
      g_ptr_array_add (print->ridge_templates, rm);
    }

  g_clear_object (&print->image);
  print->image = g_object_ref (image);
  g_object_notify (G_OBJECT (print), "image");

  return TRUE;
}

static double
bilinear_sample (const double *data, int w, int h, double x, double y)
{
  if (x < 0 || x >= w - 1 || y < 0 || y >= h - 1)
    return 0.0;
  int x0 = (int) x, y0 = (int) y;
  double fx = x - x0, fy = y - y0;
  double v00 = data[y0 * w + x0];
  double v10 = data[y0 * w + (x0 + 1)];
  double v01 = data[(y0 + 1) * w + x0];
  double v11 = data[(y0 + 1) * w + (x0 + 1)];
  return (1.0 - fx) * (1.0 - fy) * v00 + fx * (1.0 - fy) * v10 +
         (1.0 - fx) * fy * v01 + fx * fy * v11;
}

static double *
ridge_map_to_norm_double (const FpiRidgeMap *rm)
{
  int n = rm->width * rm->height;
  double *d = g_malloc (n * sizeof (double));
  double sum = 0.0, sqsum = 0.0;
  int i;

  for (i = 0; i < n; i++)
    {
      d[i] = (double) rm->data[i] - 128.0;
      sum += d[i];
    }
  double mean = sum / n;
  for (i = 0; i < n; i++)
    {
      d[i] -= mean;
      sqsum += d[i] * d[i];
    }
  double norm = sqrt (sqsum);
  if (norm > 1e-6)
    {
      for (i = 0; i < n; i++)
        d[i] /= norm;
    }
  return d;
}

static int
match_normalized_ridge (const double *d1, const double *d2, int w, int h)
{
  double best_corr = -1.0;
  static const double angles_deg[] = {-12.0, -6.0, 0.0, 6.0, 12.0};
  double *rot_buf = g_malloc (w * h * sizeof (double));
  int a;

  for (a = 0; a < 5; a++)
    {
      double rad = angles_deg[a] * (G_PI / 180.0);
      double cos_a = cos (rad), sin_a = sin (rad);
      double cx = w / 2.0, cy = h / 2.0;

      double rot_sqsum = 0.0;
      int y, x;
      for (y = 0; y < h; y++)
        {
          for (x = 0; x < w; x++)
            {
              double rot_x = cos_a * (x - cx) - sin_a * (y - cy) + cx;
              double rot_y = sin_a * (x - cx) + cos_a * (y - cy) + cy;
              double v = bilinear_sample (d1, w, h, rot_x, rot_y);
              rot_buf[y * w + x] = v;
              rot_sqsum += v * v;
            }
        }
      if (rot_sqsum < 1e-6)
        continue;

      int dy, dx;
      for (dy = -24; dy <= 24; dy += 2)
        {
          int y1a = dy > 0 ? dy : 0;
          int y1b = dy > 0 ? h : h + dy;
          int y2a = dy > 0 ? 0 : -dy;
          int seg_h = y1b - y1a;
          if (seg_h < 30)
            continue;

          for (dx = -20; dx <= 20; dx += 2)
            {
              int x1a = dx > 0 ? dx : 0;
              int x1b = dx > 0 ? w : w + dx;
              int x2a = dx > 0 ? 0 : -dx;
              int seg_w = x1b - x1a;
              if (seg_w < 25)
                continue;

              double dot = 0.0;
              double e1 = 0.0, e2 = 0.0;
              for (y = 0; y < seg_h; y++)
                {
                  int idx1 = (y1a + y) * w + x1a;
                  int idx2 = (y2a + y) * w + x2a;
                  for (x = 0; x < seg_w; x++)
                    {
                      double v1 = rot_buf[idx1 + x];
                      double v2 = d2[idx2 + x];
                      dot += v1 * v2;
                      e1 += v1 * v1;
                      e2 += v2 * v2;
                    }
                }
              if (e1 > 1e-6 && e2 > 1e-6)
                {
                  double overlap_frac = (double)(seg_w * seg_h) / (w * h);
                  double weight = pow (overlap_frac, 0.35);
                  double c = (dot / (sqrt (e1) * sqrt (e2))) * weight;
                  if (c > best_corr)
                    best_corr = c;
                }
            }
        }
    }
  g_free (rot_buf);
  return (int) round (CLAMP (best_corr * 100.0, 0.0, 100.0));
}

/**
 * fpi_print_bz3_match:
 * @template: A #FpPrint containing one or more prints
 * @print: A newly scanned #FpPrint to test
 * @bz3_threshold: The match threshold
 * @error: Return location for error
 *
 * Match the newly scanned @print (containing exactly one print) against the
 * prints contained in @template which will have been stored during enrollment.
 *
 * Both @template and @print need to be of type #FPI_PRINT_NBIS for this to
 * work.
 *
 * Returns: Whether the prints match, @error will be set if #FPI_MATCH_ERROR is returned
 */
FpiMatchResult
fpi_print_bz3_match (FpPrint *template, FpPrint *print, gint bz3_threshold, GError **error)
{
  struct xyt_struct *pstruct;
  gint probe_len;
  gint i;

  /* XXX: Use a different error type? */
  if (template->type != FPI_PRINT_NBIS || print->type != FPI_PRINT_NBIS)
    {
      *error = fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED,
                                         "It is only possible to match NBIS type print data");
      return FPI_MATCH_ERROR;
    }

  /* 2D Ridge Map Correlation Matching */
  if (template->ridge_templates && template->ridge_templates->len > 0 &&
      print->ridge_templates && print->ridge_templates->len > 0)
    {
      FpiRidgeMap *probe_rm = g_ptr_array_index (print->ridge_templates, 0);
      double *probe_d = ridge_map_to_norm_double (probe_rm);
      gint max_ridge_score = 0;
      guint j;

      for (j = 0; j < template->ridge_templates->len; j++)
        {
          FpiRidgeMap *gal_rm = g_ptr_array_index (template->ridge_templates, j);
          if (gal_rm->width == probe_rm->width && gal_rm->height == probe_rm->height)
            {
              double *gal_d = ridge_map_to_norm_double (gal_rm);
              gint s = match_normalized_ridge (probe_d, gal_d, probe_rm->width, probe_rm->height);
              g_free (gal_d);
              if (s > max_ridge_score)
                max_ridge_score = s;
            }
        }
      g_free (probe_d);

      gint ridge_thresh = bz3_threshold > 0 ? bz3_threshold : 36;
      fp_dbg ("2D Ridge Match: max_score = %d, threshold = %d", max_ridge_score, ridge_thresh);

      if (max_ridge_score >= ridge_thresh)
        return FPI_MATCH_SUCCESS;
      else
        return FPI_MATCH_FAIL;
    }

  if (print->prints->len != 1)
    {
      *error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                         "New print contains more than one print!");
      return FPI_MATCH_ERROR;
    }

  pstruct = g_ptr_array_index (print->prints, 0);
  probe_len = bozorth_probe_init (pstruct);

  for (i = 0; i < template->prints->len; i++)
    {
      struct xyt_struct *gstruct;
      gint score;
      gstruct = g_ptr_array_index (template->prints, i);
      score = bozorth_to_gallery (probe_len, pstruct, gstruct);
      fp_dbg ("score %d/%d", score, bz3_threshold);

      if (score >= bz3_threshold)
        return FPI_MATCH_SUCCESS;
    }

  return FPI_MATCH_FAIL;
}

/**
 * fpi_print_generate_user_id:
 * @print: #FpPrint to generate the ID for
 *
 * Generates a string identifier for the represented print. This identifier
 * encodes some metadata about the print. It also includes a random string
 * and may be assumed to be unique.
 *
 * This is useful if devices are able to store a string identifier, but more
 * storing more metadata may be desirable. In effect, this means the driver
 * can provide somewhat more meaningful data to fp_device_list_prints().
 *
 * The generated ID may be truncated after 23 characters. However, more space
 * is required to include the username, and it is recommended to store at
 * at least 31 bytes.
 *
 * The generated format may change in the future. It is versioned though and
 * decoding should remain functional.
 *
 * Returns: A unique string of 23 + strlen(username) characters
 */
gchar *
fpi_print_generate_user_id (FpPrint *print)
{
  const gchar *username = NULL;
  gchar *user_id = NULL;
  const GDate *date;
  gint y = 0, m = 0, d = 0;
  gint32 rand_id = 0;

  g_assert (print);
  date = fp_print_get_enroll_date (print);
  if (date && g_date_valid (date))
    {
      y = g_date_get_year (date);
      m = g_date_get_month (date);
      d = g_date_get_day (date);
    }

  username = fp_print_get_username (print);
  if (!username)
    username = "nobody";

  if (g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") == 0)
    rand_id = 0;
  else
    rand_id = g_random_int ();

  user_id = g_strdup_printf ("FP1-%04d%02d%02d-%X-%08X-%s",
                             y, m, d,
                             fp_print_get_finger (print),
                             rand_id,
                             username);

  return user_id;

}

/**
 * fpi_print_fill_from_user_id:
 * @print: #FpPrint to fill metadata into
 * @user_id: An ID that was likely encoded using fpi_print_generate_user_id()
 *
 * This is the reverse operation of fpi_print_generate_user_id(), allowing
 * the driver to encode some print metadata in a string.
 *
 * Returns: Whether a valid ID was found
 */
gboolean
fpi_print_fill_from_user_id (FpPrint *print, const char *user_id)
{
  g_return_val_if_fail (user_id, FALSE);

  /* The format has 24 bytes at the start and some dashes in the right places */
  if (g_str_has_prefix (user_id, "FP1-") && strlen (user_id) >= 24 &&
      user_id[12] == '-' && user_id[14] == '-' && user_id[23] == '-')
    {
      g_autofree gchar *copy = g_strdup (user_id);
      g_autoptr(GDate) date = NULL;
      gint32 date_ymd;
      gint32 finger;
      gchar *username;
      /* Try to parse information from the string. */

      copy[12] = '\0';
      date_ymd = g_ascii_strtod (copy + 4, NULL);
      if (date_ymd > 0)
        date = g_date_new_dmy (date_ymd % 100,
                               (date_ymd / 100) % 100,
                               date_ymd / 10000);
      else
        date = g_date_new ();

      fp_print_set_enroll_date (print, date);

      copy[14] = '\0';
      finger = g_ascii_strtoll (copy + 13, NULL, 16);
      fp_print_set_finger (print, finger);

      /* We ignore the next chunk, it is just random data.
       * Then comes the username; nobody is the default if the metadata
       * is unknown */
      username = copy + 24;
      if (strlen (username) > 0 && g_strcmp0 (username, "nobody") != 0)
        fp_print_set_username (print, username);

      return TRUE;
    }

  return FALSE;
}
