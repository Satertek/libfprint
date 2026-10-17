#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>

#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))

typedef struct {
    int w, h;
    double *data;
} RidgeMap;

static RidgeMap* load_pgm_ridge(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char header[64];
    if (!fgets(header, sizeof(header), f)) { fclose(f); return NULL; }
    int w, h, maxval;
    if (sscanf(header, "P5 %d %d %d", &w, &h, &maxval) != 3) {
        while (fgets(header, sizeof(header), f) && header[0] == '#') ;
        sscanf(header, "%d %d", &w, &h);
        fgets(header, sizeof(header), f);
    }
    guint8 *raw = g_malloc(w * h);
    if (fread(raw, 1, w * h, f) != (size_t)(w * h)) { g_free(raw); fclose(f); return NULL; }
    fclose(f);

    RidgeMap *rm = g_new0(RidgeMap, 1);
    rm->w = w;
    rm->h = h;
    rm->data = g_malloc(w * h * sizeof(double));

    /* Normalize to zero mean, unit energy */
    double sum = 0, sqsum = 0;
    for (int i = 0; i < w * h; i++) {
        rm->data[i] = (double)raw[i] - 128.0;
        sum += rm->data[i];
    }
    double mean = sum / (w * h);
    for (int i = 0; i < w * h; i++) {
        rm->data[i] -= mean;
        sqsum += rm->data[i] * rm->data[i];
    }
    double norm = sqrt(sqsum);
    if (norm > 1e-6) {
        for (int i = 0; i < w * h; i++) rm->data[i] /= norm;
    }
    g_free(raw);
    return rm;
}

static double bilinear_sample(const double *data, int w, int h, double x, double y) {
    if (x < 0 || x >= w - 1 || y < 0 || y >= h - 1) return 0.0;
    int x0 = (int)x, y0 = (int)y;
    double fx = x - x0, fy = y - y0;
    double v00 = data[y0 * w + x0];
    double v10 = data[y0 * w + (x0 + 1)];
    double v01 = data[(y0 + 1) * w + x0];
    double v11 = data[(y0 + 1) * w + (x0 + 1)];
    return (1.0 - fx) * (1.0 - fy) * v00 + fx * (1.0 - fy) * v10 +
           (1.0 - fx) * fy * v01 + fx * fy * v11;
}

static int match_ridge_pair(const RidgeMap *m1, const RidgeMap *m2) {
    int w = m1->w, h = m1->h;
    if (m2->w != w || m2->h != h) return 0;

    double best_corr = -1.0;
    static const double angles_deg[] = {-12.0, -6.0, 0.0, 6.0, 12.0};
    double *rot_buf = g_malloc(w * h * sizeof(double));

    for (int a = 0; a < 5; a++) {
        double rad = angles_deg[a] * (M_PI / 180.0);
        double cos_a = cos(rad), sin_a = sin(rad);
        double cx = w / 2.0, cy = h / 2.0;

        /* Rotate m1 into rot_buf */
        double rot_sqsum = 0.0;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                double rx = cos_a * (x - cx) - sin_a * (y - cy) + cx;
                double ry = sin_a * (x - cx) + cos_a * (y - cy) + cy;
                double v = bilinear_sample(m1->data, w, h, rx, ry);
                rot_buf[y * w + x] = v;
                rot_sqsum += v * v;
            }
        }
        if (rot_sqsum < 1e-6) continue;

        /* Search translations: dy in [-24..24], dx in [-20..20] */
        for (int dy = -24; dy <= 24; dy += 2) {
            int y1a = dy > 0 ? dy : 0;
            int y1b = dy > 0 ? h : h + dy;
            int y2a = dy > 0 ? 0 : -dy;
            int y2b = dy > 0 ? h - dy : h;
            int seg_h = y1b - y1a;
            if (seg_h < 30) continue;

            for (int dx = -20; dx <= 20; dx += 2) {
                int x1a = dx > 0 ? dx : 0;
                int x1b = dx > 0 ? w : w + dx;
                int x2a = dx > 0 ? 0 : -dx;
                int x2b = dx > 0 ? w - dx : w;
                int seg_w = x1b - x1a;
                if (seg_w < 25) continue;

                double dot = 0;
                double e1 = 0, e2 = 0;
                for (int y = 0; y < seg_h; y++) {
                    int idx1 = (y1a + y) * w + x1a;
                    int idx2 = (y2a + y) * w + x2a;
                    for (int x = 0; x < seg_w; x++) {
                        double v1 = rot_buf[idx1 + x];
                        double v2 = m2->data[idx2 + x];
                        dot += v1 * v2;
                        e1 += v1 * v1;
                        e2 += v2 * v2;
                    }
                }
                if (e1 > 1e-6 && e2 > 1e-6) {
                    double overlap_frac = (double)(seg_w * seg_h) / (w * h);
                    double weight = pow(overlap_frac, 0.35);
                    double c = (dot / (sqrt(e1) * sqrt(e2))) * weight;
                    if (c > best_corr) best_corr = c;
                }
            }
        }
    }
    g_free(rot_buf);
    return (int)round(CLAMP(best_corr * 100.0, 0, 100));
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <probe.pgm> <gallery1.pgm> [gallery2.pgm ...]\n", argv[0]);
        return 1;
    }
    RidgeMap *probe = load_pgm_ridge(argv[1]);
    if (!probe) { printf("Failed to load probe\n"); return 1; }

    int max_score = 0;
    int best_stage = -1;

    for (int i = 2; i < argc; i++) {
        RidgeMap *gal = load_pgm_ridge(argv[i]);
        if (!gal) continue;
        int score = match_ridge_pair(probe, gal);
        printf("  [Stage %d] vs Gallery [%s]: score = %d\n", i - 1, argv[i], score);
        if (score > max_score) {
            max_score = score;
            best_stage = i - 1;
        }
        g_free(gal->data);
        g_free(gal);
    }

    printf("2D Ridge Match Score: %d (Best Stage: %d, Threshold = 36) -> %s\n",
           max_score, best_stage, (max_score >= 36) ? "MATCH!" : "NO MATCH!");

    g_free(probe->data);
    g_free(probe);
    return 0;
}
