// Draws the icon the homebrew menu shows for DGLAB-NX: a 256x256 PPM on stdout.
//
// The icon is a flat mark in the UI's own palette - the page background and the
// accent (nro/source/ui/theme.c) - and it is drawn from this one file rather than
// kept as an unexplained JPEG, so the palette and the shape can be changed and
// re-rendered. nro/Makefile picks up nro/DGLAB-NX.jpg directly.
//
//   cc -O2 -o /tmp/make_icon nro/tools/make_icon.c -lm
//   /tmp/make_icon > /tmp/icon.ppm
//   cjpeg -quality 92 /tmp/icon.ppm > nro/DGLAB-NX.jpg
//
// (cjpeg is libjpeg-turbo's; devkitPro does not ship a JPEG encoder. e.g.
// `brew install jpeg-turbo`.) The icon has to be a 256x256 JPEG for elf2nro.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define ICON_SIZE 256

// The UI's palette, straight out of nro/source/ui/theme.c.
#define BACKGROUND_R 0x2D
#define BACKGROUND_G 0x2D
#define BACKGROUND_B 0x2D
#define ACCENT_R 0x00
#define ACCENT_G 0xFF
#define ACCENT_B 0xC8

// The waveform: two cycles of a sine, tapering back to the baseline at both ends
// so the mark reads as a wave rather than as a line that starts nowhere. The
// amplitude peaks in the middle, which is what a "the harder it moves the
// stronger it gets" waveform looks like - the thing this front end is for.
#define WAVE_X0 30.0
#define WAVE_X1 226.0
#define WAVE_MID_Y 128.0
#define WAVE_AMPLITUDE 62.0
#define WAVE_CYCLES 2.0
/// The pen: the stroke is the capsule around each segment, so the ends and the
/// joins are round.
#define WAVE_HALF_WIDTH 8.0
#define WAVE_SEGMENTS 192
#define SUBSAMPLES 4

typedef struct {
    double x;
    double y;
} Point;

static Point g_wave[WAVE_SEGMENTS + 1];

// The stroke is one capsule per segment: the distance from a point to the
// polyline is the smallest of the distances to its segments.
static double segmentDistance(double px, double py, const Point* a, const Point* b)
{
    double dx = b->x - a->x;
    double dy = b->y - a->y;
    double length_squared = dx * dx + dy * dy;
    double t = 0.0;
    double cx;
    double cy;

    if (length_squared > 0.0)
        t = ((px - a->x) * dx + (py - a->y) * dy) / length_squared;

    if (t < 0.0)
        t = 0.0;
    else if (t > 1.0)
        t = 1.0;

    cx = a->x + t * dx;
    cy = a->y + t * dy;

    return sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

static double waveDistance(double px, double py)
{
    double best = 1.0e9;

    for (int i = 0; i < WAVE_SEGMENTS; i++) {
        double distance = segmentDistance(px, py, &g_wave[i], &g_wave[i + 1]);

        if (distance < best)
            best = distance;
    }

    return best;
}

static void buildWave(void)
{
    for (int i = 0; i <= WAVE_SEGMENTS; i++) {
        double t = (double)i / (double)WAVE_SEGMENTS;
        // The envelope keeps the wave on the baseline at both ends and lets it
        // reach the full amplitude in between.
        double envelope = pow(sin(M_PI * t), 0.6);

        g_wave[i].x = WAVE_X0 + t * (WAVE_X1 - WAVE_X0);
        g_wave[i].y = WAVE_MID_Y -
            WAVE_AMPLITUDE * envelope * sin(2.0 * M_PI * WAVE_CYCLES * t);
    }
}

// The accent over the background for one pixel, antialiased: the fraction of the
// pixel's subsamples that fall inside the stroke is the mix.
static void iconPixel(int x, int y, unsigned char* out)
{
    int inside = 0;

    for (int sy = 0; sy < SUBSAMPLES; sy++) {
        for (int sx = 0; sx < SUBSAMPLES; sx++) {
            double px = (double)x + ((double)sx + 0.5) / (double)SUBSAMPLES;
            double py = (double)y + ((double)sy + 0.5) / (double)SUBSAMPLES;

            if (waveDistance(px, py) <= WAVE_HALF_WIDTH)
                inside++;
        }
    }

    double coverage = (double)inside / (double)(SUBSAMPLES * SUBSAMPLES);

    out[0] = (unsigned char)(BACKGROUND_R + (ACCENT_R - BACKGROUND_R) * coverage + 0.5);
    out[1] = (unsigned char)(BACKGROUND_G + (ACCENT_G - BACKGROUND_G) * coverage + 0.5);
    out[2] = (unsigned char)(BACKGROUND_B + (ACCENT_B - BACKGROUND_B) * coverage + 0.5);
}

int main(void)
{
    unsigned char row[ICON_SIZE * 3];

    buildWave();
    printf("P6\n%d %d\n255\n", ICON_SIZE, ICON_SIZE);

    for (int y = 0; y < ICON_SIZE; y++) {
        for (int x = 0; x < ICON_SIZE; x++)
            iconPixel(x, y, &row[x * 3]);

        if (fwrite(row, 1, sizeof(row), stdout) != sizeof(row)) {
            fprintf(stderr, "make_icon: write failed\n");
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}
