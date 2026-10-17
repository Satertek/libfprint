#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>
#include "fpi-minutiae.h"
#include "nbis.h"

static void minutiae_to_xyt(struct fp_minutiae *minutiae, int bwidth, int bheight, struct xyt_struct *xyt) {
    int i;
    struct minutiae_struct c[MAX_FILE_MINUTIAE];
    int nmin = min(minutiae->num, MAX_BOZORTH_MINUTIAE);

    for (i = 0; i < nmin; i++) {
        struct fp_minutia *m = minutiae->list[i];
        lfs2nist_minutia_XYT(&c[i].col[0], &c[i].col[1], &c[i].col[2], m, bwidth, bheight);
        c[i].col[3] = sround(m->reliability * 100.0);
        if (c[i].col[2] > 180) c[i].col[2] -= 360;
    }
    qsort((void*)&c, (size_t)nmin, sizeof(struct minutiae_struct), sort_x_y);

    for (i = 0; i < nmin; i++) {
        xyt->xcol[i] = c[i].col[0];
        xyt->ycol[i] = c[i].col[1];
        xyt->thetacol[i] = c[i].col[2];
    }
    xyt->nrows = nmin;
}

static guint8* load_pgm8(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return NULL; }
    char header[64];
    if (!fgets(header, sizeof(header), f)) { fclose(f); return NULL; }
    if (strncmp(header, "P5", 2) != 0) {
        fprintf(stderr, "Not a P5 PGM: %s\n", path);
        fclose(f);
        return NULL;
    }

    int maxval;
    if (sscanf(header, "P5 %d %d %d", w, h, &maxval) != 3) {
        while (fgets(header, sizeof(header), f) && header[0] == '#') ;
        if (sscanf(header, "%d %d", w, h) != 2) {
            fprintf(stderr, "Failed to parse dimensions: %s\n", header);
            fclose(f);
            return NULL;
        }
        if (!fgets(header, sizeof(header), f)) { fclose(f); return NULL; }
        if (sscanf(header, "%d", &maxval) != 1) {
            fprintf(stderr, "Failed to parse maxval: %s\n", header);
            fclose(f);
            return NULL;
        }
    }
    guint8 *data = g_malloc((*w) * (*h));
    if (fread(data, 1, (*w) * (*h), f) != (size_t)((*w) * (*h))) {
        fprintf(stderr, "Short read on pixel data\n");
        g_free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return data;
}

static struct fp_minutiae* extract(guint8 *data, int w, int h) {
    struct fp_minutiae *minutiae = NULL;
    int *direction_map = NULL, *low_contrast_map = NULL, *low_flow_map = NULL;
    int *high_curve_map = NULL, *quality_map = NULL;
    int map_w, map_h, bw, bh, bd;
    unsigned char *bdata = NULL;

    LFSPARMS *lfsparms = g_memdup2(&g_lfsparms_V2, sizeof(LFSPARMS));
    lfsparms->remove_perimeter_pts = g_getenv("NO_PERIM") ? FALSE : TRUE;

    int r = get_minutiae(&minutiae, &quality_map, &direction_map,
                         &low_contrast_map, &low_flow_map, &high_curve_map,
                         &map_w, &map_h, &bdata, &bw, &bh, &bd,
                         data, w, h, 8, 0, lfsparms);
    g_free(lfsparms);
    g_free(direction_map);
    g_free(low_contrast_map);
    g_free(low_flow_map);
    g_free(high_curve_map);
    g_free(quality_map);
    g_free(bdata);
    if (r != 0) {
        printf("get_minutiae failed with %d\n", r);
        return NULL;
    }
    return minutiae;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <probe.pgm> <gallery1.pgm> [gallery2.pgm ...]\n", argv[0]);
        printf("       %s --minutiae <file.pgm>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--minutiae") == 0 && argc >= 3) {
        int w, h;
        guint8 *data = load_pgm8(argv[2], &w, &h);
        if (!data) return 1;
        struct fp_minutiae *minutiae = NULL;
        int *direction_map = NULL, *low_contrast_map = NULL, *low_flow_map = NULL;
        int *high_curve_map = NULL, *quality_map = NULL;
        int map_w, map_h, bw, bh, bd;
        unsigned char *bdata = NULL;
        LFSPARMS *lfsparms = g_memdup2(&g_lfsparms_V2, sizeof(LFSPARMS));
        lfsparms->remove_perimeter_pts = (argc > 3 && strcmp(argv[3], "--no-perim") == 0) ? FALSE : TRUE;
        get_minutiae(&minutiae, &quality_map, &direction_map,
                     &low_contrast_map, &low_flow_map, &high_curve_map,
                     &map_w, &map_h, &bdata, &bw, &bh, &bd,
                     data, w, h, 8, 0, lfsparms);
        if (argc > 3 && strncmp(argv[3], "--bdata=", 8) == 0) {
            FILE *bf = fopen(argv[3] + 8, "wb");
            if (bf) {
                fprintf(bf, "P5\n%d %d\n255\n", bw, bh);
                fwrite(bdata, 1, bw * bh, bf);
                fclose(bf);
            }
        }
        if (!minutiae) { g_free(data); return 1; }
        struct xyt_struct xyt;
        minutiae_to_xyt(minutiae, w, h, &xyt);
        printf("# Minutiae for %s: %d points\n", argv[2], xyt.nrows);
        printf("# X Y Theta\n");
        for (int i = 0; i < xyt.nrows; i++) {
            printf("%d %d %d\n", xyt.xcol[i], xyt.ycol[i], xyt.thetacol[i]);
        }
        g_free(data);
        return 0;
    }

    if (argc < 3) {
        printf("Usage: %s <probe.pgm> <gallery1.pgm> [gallery2.pgm ...]\n", argv[0]);
        return 1;
    }

    const char *probe_path = argv[1];
    int pw, ph;
    guint8 *pdata = load_pgm8(probe_path, &pw, &ph);
    if (!pdata) { printf("Failed to load probe image: %s\n", probe_path); return 1; }

    struct fp_minutiae *pm = extract(pdata, pw, ph);
    if (!pm || pm->num == 0) {
        printf("No minutiae found in probe: %s\n", probe_path);
        g_free(pdata);
        return 1;
    }

    struct xyt_struct pxyt;
    minutiae_to_xyt(pm, pw, ph, &pxyt);
    int probe_len = bozorth_probe_init(&pxyt);

    printf("Probe [%s]: %dx%d, %d minutiae\n", probe_path, pw, ph, pm->num);

    int max_score = 0;
    int best_idx = -1;

    for (int i = 2; i < argc; i++) {
        const char *gal_path = argv[i];
        int gw, gh;
        guint8 *gdata = load_pgm8(gal_path, &gw, &gh);
        if (!gdata) {
            printf("  [Stage %d] Failed to load %s\n", i - 1, gal_path);
            continue;
        }

        struct fp_minutiae *gm = extract(gdata, gw, gh);
        if (!gm || gm->num == 0) {
            printf("  [Stage %d] No minutiae in %s\n", i - 1, gal_path);
            g_free(gdata);
            continue;
        }

        struct xyt_struct gxyt;
        minutiae_to_xyt(gm, gw, gh, &gxyt);
        int score = bozorth_to_gallery(probe_len, &pxyt, &gxyt);
        printf("  [Stage %d] vs Gallery [%s] (%d minutiae): score = %d\n", i - 1, gal_path, gm->num, score);

        if (score > max_score) {
            max_score = score;
            best_idx = i - 1;
        }
        g_free(gdata);
    }

    printf("Bozorth match score between %s and gallery: %d\n", probe_path, max_score);
    printf("Result: Max score = %d (Best Stage: %d, Threshold = 24) -> %s\n",
           max_score, best_idx, (max_score >= 24) ? "MATCH!" : "NO MATCH!");

    g_free(pdata);
    return 0;
}
