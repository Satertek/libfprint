#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "fp-print-private.h"
#include "fpi-image.h"

static guint8* load_pgm8(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char header[64];
    if (!fgets(header, sizeof(header), f)) { fclose(f); return NULL; }
    int maxval;
    if (sscanf(header, "P5 %d %d %d", w, h, &maxval) != 3) {
        while (fgets(header, sizeof(header), f) && header[0] == '#') ;
        sscanf(header, "%d %d", w, h);
        fgets(header, sizeof(header), f);
    }
    guint8 *data = g_malloc((*w) * (*h));
    if (fread(data, 1, (*w) * (*h), f) != (size_t)((*w) * (*h))) {
        g_free(data); fclose(f); return NULL;
    }
    fclose(f);
    return data;
}

int main(int argc, char **argv) {
    g_print("=== Testing Full Libfprint Option A Ridge Pipeline ===\n");

    /* 1. Create enrollment gallery print */
    g_autoptr(FpPrint) gallery = g_object_new(FP_TYPE_PRINT,
                                             "driver", "goodixtls53xd",
                                             "device-id", "27c6:532d",
                                             NULL);
    fpi_print_set_type(gallery, FPI_PRINT_NBIS);

    /* Load all 10 stages */
    for (int stage = 1; stage <= 10; stage++) {
        char path[256];
        snprintf(path, sizeof(path), "test_clean64/enroll_idx_%02d_s1.pgm", stage);
        int w = 0, h = 0;
        guint8 *data = load_pgm8(path, &w, &h);
        g_assert_nonnull(data);
        g_assert_cmpint(w, >, 0);
        g_assert_cmpint(h, >, 0);

        FpImage *img = fp_image_new(w, h);
        memcpy(img->data, data, w * h);
        g_free(data);

        g_autoptr(FpPrint) stage_print = g_object_new(FP_TYPE_PRINT,
                                                      "driver", "goodixtls53xd",
                                                      "device-id", "27c6:532d",
                                                      NULL);
        fpi_print_set_type(stage_print, FPI_PRINT_NBIS);

        GError *err = NULL;
        gboolean ok = fpi_print_add_from_image(stage_print, img, &err);
        g_assert_no_error(err);
        g_assert_true(ok);
        g_assert_cmpint(stage_print->ridge_templates->len, ==, 1);

        fpi_print_add_print(gallery, stage_print);
        g_object_unref(img);
    }

    g_assert_cmpint(gallery->prints->len, ==, 10);
    g_assert_cmpint(gallery->ridge_templates->len, ==, 10);
    g_print("[✓] Successfully enrolled 10 stages into gallery print (10 ridge templates)\n");

    /* 2. Test Serialization & Deserialization */
    guchar *serialized = NULL;
    gsize length = 0;
    GError *err = NULL;
    gboolean ser_ok = fp_print_serialize(gallery, &serialized, &length, &err);
    g_assert_no_error(err);
    g_assert_true(ser_ok);
    g_assert_cmpuint(length, >, 0);
    g_print("[✓] Successfully serialized gallery to %zu bytes\n", length);

    g_autoptr(FpPrint) deserialized_gallery = fp_print_deserialize(serialized, length, &err);
    g_free(serialized);
    g_assert_no_error(err);
    g_assert_nonnull(deserialized_gallery);
    g_assert_cmpint(deserialized_gallery->prints->len, ==, 10);
    g_assert_cmpint(deserialized_gallery->ridge_templates->len, ==, 10);
    g_print("[✓] Successfully deserialized gallery with all 10 ridge templates intact\n");

    /* 3. Test Genuine Probe 1 (Normal Index) */
    {
        int w = 0, h = 0;
        guint8 *data = load_pgm8("test_clean64/probe_idx_normal_s1.pgm", &w, &h);
        g_assert_nonnull(data);
        FpImage *img = fp_image_new(w, h);
        memcpy(img->data, data, w * h);
        g_free(data);

        g_autoptr(FpPrint) probe = g_object_new(FP_TYPE_PRINT,
                                                "driver", "goodixtls53xd",
                                                "device-id", "27c6:532d",
                                                NULL);
        fpi_print_set_type(probe, FPI_PRINT_NBIS);
        fpi_print_add_from_image(probe, img, &err);
        g_assert_no_error(err);
        g_object_unref(img);

        FpiMatchResult res = fpi_print_bz3_match(deserialized_gallery, probe, 36, &err);
        g_assert_no_error(err);
        g_print("[Test Genuine Normal Index Probe]: Result = %s\n",
                res == FPI_MATCH_SUCCESS ? "MATCH (SUCCESS)" : "FAIL");
        g_assert_cmpint(res, ==, FPI_MATCH_SUCCESS);
    }

    /* 4. Test Genuine Probe 2 (Shifted Index) */
    {
        int w = 0, h = 0;
        guint8 *data = load_pgm8("test_clean64/probe_idx_shifted_s1.pgm", &w, &h);
        g_assert_nonnull(data);
        FpImage *img = fp_image_new(w, h);
        memcpy(img->data, data, w * h);
        g_free(data);

        g_autoptr(FpPrint) probe = g_object_new(FP_TYPE_PRINT,
                                                "driver", "goodixtls53xd",
                                                "device-id", "27c6:532d",
                                                NULL);
        fpi_print_set_type(probe, FPI_PRINT_NBIS);
        fpi_print_add_from_image(probe, img, &err);
        g_assert_no_error(err);
        g_object_unref(img);

        FpiMatchResult res = fpi_print_bz3_match(deserialized_gallery, probe, 36, &err);
        g_assert_no_error(err);
        g_print("[Test Genuine Shifted Index Probe]: Result = %s\n",
                res == FPI_MATCH_SUCCESS ? "MATCH (SUCCESS)" : "FAIL");
        g_assert_cmpint(res, ==, FPI_MATCH_SUCCESS);
    }

    /* 5. Test Impostor Probe 1 (Pinky) */
    {
        int w = 0, h = 0;
        guint8 *data = load_pgm8("test_clean64/impostor_pinky_s1.pgm", &w, &h);
        g_assert_nonnull(data);
        FpImage *img = fp_image_new(w, h);
        memcpy(img->data, data, w * h);
        g_free(data);

        g_autoptr(FpPrint) probe = g_object_new(FP_TYPE_PRINT,
                                                "driver", "goodixtls53xd",
                                                "device-id", "27c6:532d",
                                                NULL);
        fpi_print_set_type(probe, FPI_PRINT_NBIS);
        fpi_print_add_from_image(probe, img, &err);
        g_assert_no_error(err);
        g_object_unref(img);

        FpiMatchResult res = fpi_print_bz3_match(deserialized_gallery, probe, 36, &err);
        g_assert_no_error(err);
        g_print("[Test Impostor Pinky Probe]: Result = %s\n",
                res == FPI_MATCH_FAIL ? "REJECTED (SUCCESS)" : "FALSE ACCEPT");
        g_assert_cmpint(res, ==, FPI_MATCH_FAIL);
    }

    /* 6. Test Impostor Probe 2 (Thumb) */
    {
        int w = 0, h = 0;
        guint8 *data = load_pgm8("test_clean64/impostor_thumb_s1.pgm", &w, &h);
        g_assert_nonnull(data);
        FpImage *img = fp_image_new(w, h);
        memcpy(img->data, data, w * h);
        g_free(data);

        g_autoptr(FpPrint) probe = g_object_new(FP_TYPE_PRINT,
                                                "driver", "goodixtls53xd",
                                                "device-id", "27c6:532d",
                                                NULL);
        fpi_print_set_type(probe, FPI_PRINT_NBIS);
        fpi_print_add_from_image(probe, img, &err);
        g_assert_no_error(err);
        g_object_unref(img);

        FpiMatchResult res = fpi_print_bz3_match(deserialized_gallery, probe, 36, &err);
        g_assert_no_error(err);
        g_print("[Test Impostor Thumb Probe]: Result = %s\n",
                res == FPI_MATCH_FAIL ? "REJECTED (SUCCESS)" : "FALSE ACCEPT");
        g_assert_cmpint(res, ==, FPI_MATCH_FAIL);
    }

    g_print("\n======================================================\n");
    g_print(" ALL PIPELINE TESTS PASSED! OPTION A IS FULLY VERIFIED!\n");
    g_print("======================================================\n");
    return 0;
}
