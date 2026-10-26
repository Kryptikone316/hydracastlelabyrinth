#include "graphics.h"
#include "../PHL.h"
#include "../game.h"
#include "../qda.h"
#include "hwdisp.h"
#include "joykey.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

PHL_Surface screen = NULL;

int wantFullscreen = 0;
int screenScale = 1;  /* native 320x240 output - same already-shipped config
                        * as upstream's own BITTBOY/CHIP targets, so the
                        * x*screenScale/2-style formulas below are proven,
                        * not re-derived. */
int desktopFS = 0;

int deltaX = 0;
int deltaY = 0;

int screenW = 320;
int screenH = 240;

static PHL_SurfaceData framebuffer;
static PHL_SurfaceData backbuffer; /* pre-composited background tile cache,
                                      * same role as the SDL backend's own
                                      * `backbuffer` - avoids redrawing all
                                      * 192 tiles every frame. */
static PHL_Surface drawbuffer = NULL;

static int xbrz = 0;
int getXBRZ() { return xbrz; }
void setXBRZ(int active) { (void)active; /* no xBRZ path on this device */ }

PHL_RGB PHL_NewRGB(uint8_t r, uint8_t g, uint8_t b)
{
	PHL_RGB ret = { r, g, b };
	return ret;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void PHL_GraphicsInit()
{
	joykey_init();
	hwdisp_init();
	hwdisp_set_target_aspect(4, 3); /* no-op here (320x240 is already 4:3),
	                                  * kept for parity with every other
	                                  * port's standing convention. */

	framebuffer.w = screenW;
	framebuffer.h = screenH;
	framebuffer.has_colorkey = 0;
	framebuffer.pixels = (uint16_t*)calloc((size_t)screenW * screenH, sizeof(uint16_t));

	backbuffer.w = screenW;
	backbuffer.h = screenH;
	backbuffer.has_colorkey = 0;
	backbuffer.pixels = (uint16_t*)calloc((size_t)screenW * screenH, sizeof(uint16_t));

	screen = &framebuffer;
	drawbuffer = &framebuffer;
}

void PHL_GraphicsExit()
{
	hwdisp_deinit();
	free(framebuffer.pixels);
	framebuffer.pixels = NULL;
	free(backbuffer.pixels);
	backbuffer.pixels = NULL;
}

static long frame_period_ns;
static struct timespec next_frame;
static int have_next_frame = 0;

void PHL_StartDrawing()
{
	PHL_ResetDrawbuffer();
}

void PHL_EndDrawing()
{
	hwdisp_present(framebuffer.pixels, framebuffer.w, framebuffer.h, framebuffer.w * 2);

	/* 60fps real-time pacing. Absolute-deadline, with a capped backlog
	 * (never try to "catch up" past a couple of frames) - same shape as
	 * hwaudio.c's consumer thread and the lesson from the audio pacing saga:
	 * an unbounded catch-up target just produces a slow-motion runaway
	 * later, a capped one settles. */
	if (!have_next_frame) {
		frame_period_ns = 1000000000L / 60;
		clock_gettime(CLOCK_MONOTONIC, &next_frame);
		have_next_frame = 1;
	}

	next_frame.tv_nsec += frame_period_ns;
	if (next_frame.tv_nsec >= 1000000000L) {
		next_frame.tv_nsec -= 1000000000L;
		next_frame.tv_sec += 1;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long long behind_ns = (long long)(now.tv_sec - next_frame.tv_sec) * 1000000000LL
	                    + (now.tv_nsec - next_frame.tv_nsec);
	if (behind_ns > frame_period_ns * 2) {
		/* Fell more than 2 frames behind - drop the backlog instead of
		 * letting it grow forever. */
		next_frame = now;
	} else {
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
	}
}

void PHL_ForceScreenUpdate()
{
}

void PHL_SetDrawbuffer(PHL_Surface surf)
{
	drawbuffer = surf;
}
void PHL_ResetDrawbuffer()
{
	drawbuffer = &framebuffer;
}

void PHL_SetColorKey(PHL_Surface surf, int r, int g, int b)
{
	if (!surf) return;
	surf->has_colorkey = 1;
	surf->colorkey = rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

PHL_Surface PHL_NewSurface(int w, int h)
{
	PHL_Surface surf = (PHL_Surface)malloc(sizeof(PHL_SurfaceData));
	surf->w = w;
	surf->h = h;
	surf->has_colorkey = 0;
	surf->colorkey = 0;
	surf->pixels = (uint16_t*)calloc((size_t)w * h, sizeof(uint16_t));
	return surf;
}

void PHL_FreeSurface(PHL_Surface surf)
{
	if (!surf) return;
	free(surf->pixels);
	free(surf);
}

PHL_Surface PHL_LoadBMP(int index)
{
	PHL_Surface surf = NULL;
	FILE* f;

	if ( (f = fopen("data/bmp.qda", "rb")) ) {
		uint8_t* QDAFile = (uint8_t*)malloc(headers[index].size);
		fseek(f, headers[index].offset, SEEK_SET);
		fread(QDAFile, 1, headers[index].size, f);
		fclose(f);

		uint16_t w, h;
		memcpy(&w, &QDAFile[18], 2);
		memcpy(&h, &QDAFile[22], 2);

		surf = PHL_NewSurface(w, h);

		PHL_RGBData palette[20][18];
		int count = 0;
		for (int dx = 0; dx < 20; dx++) {
			for (int dy = 0; dy < 16; dy++) {
				palette[dx][dy].b = QDAFile[54 + count];
				palette[dx][dy].g = QDAFile[54 + count + 1];
				palette[dx][dy].r = QDAFile[54 + count + 2];
				count += 4;
			}
		}

		for (int dx = 0; dx < w; dx++) {
			for (int dy = 0; dy < h; dy++) {
				int pix = dx + w * dy;
				int px = QDAFile[1078 + pix] / 16;
				int py = QDAFile[1078 + pix] % 16;

				if (dx == 0 && dy == 0) {
					surf->has_colorkey = 1;
					if (index == 27)
						surf->colorkey = rgb565(0, 0, 0);
					else
						surf->colorkey = rgb565(palette[0][0].r, palette[0][0].g, palette[0][0].b);
				}

				PHL_RGBData c = palette[px][py];
				surf->pixels[(h - 1 - dy) * w + dx] = rgb565(c.r, c.g, c.b);
			}
		}
		free(QDAFile);
	}

	return surf;
}

void PHL_DrawRect(int x, int y, int w, int h, PHL_RGB col)
{
	int dx = x * screenScale / 2;
	int dy = y * screenScale / 2;
	int dw = w * screenScale / 2;
	int dh = h * screenScale / 2;
	uint16_t c = rgb565(col.r, col.g, col.b);

	int x0 = dx < 0 ? 0 : dx;
	int y0 = dy < 0 ? 0 : dy;
	int x1 = dx + dw; if (x1 > drawbuffer->w) x1 = drawbuffer->w;
	int y1 = dy + dh; if (y1 > drawbuffer->h) y1 = drawbuffer->h;

	for (int yy = y0; yy < y1; yy++) {
		uint16_t* row = drawbuffer->pixels + (size_t)yy * drawbuffer->w;
		for (int xx = x0; xx < x1; xx++)
			row[xx] = c;
	}
}

/* Shared clipped blit: copies an axis-aligned rect of `src` (starting at
 * cropx,cropy, size cropw x croph) to `dst` at dx,dy, skipping colorkey
 * pixels. Clips against both surfaces' bounds - SDL_BlitSurface did this
 * for free, we don't get that automatically with a plain pixel buffer. */
static void blit(PHL_Surface src, int cropx, int cropy, int cropw, int croph,
                  PHL_Surface dst, int dx, int dy)
{
	if (!src || !dst) return;

	int sx0 = cropx, sy0 = cropy;
	int w = cropw, h = croph;

	/* Clip against source bounds. */
	if (sx0 < 0) { w += sx0; dx -= sx0; sx0 = 0; }
	if (sy0 < 0) { h += sy0; dy -= sy0; sy0 = 0; }
	if (sx0 + w > src->w) w = src->w - sx0;
	if (sy0 + h > src->h) h = src->h - sy0;

	/* Clip against dest bounds. */
	if (dx < 0) { w += dx; sx0 -= dx; dx = 0; }
	if (dy < 0) { h += dy; sy0 -= dy; dy = 0; }
	if (dx + w > dst->w) w = dst->w - dx;
	if (dy + h > dst->h) h = dst->h - dy;

	if (w <= 0 || h <= 0) return;

	for (int row = 0; row < h; row++) {
		const uint16_t* srow = src->pixels + (size_t)(sy0 + row) * src->w + sx0;
		uint16_t* drow = dst->pixels + (size_t)(dy + row) * dst->w + dx;
		if (!src->has_colorkey) {
			memcpy(drow, srow, (size_t)w * sizeof(uint16_t));
		} else {
			uint16_t key = src->colorkey;
			for (int col = 0; col < w; col++)
				if (srow[col] != key) drow[col] = srow[col];
		}
	}
}

void PHL_DrawSurface(double x, double y, PHL_Surface surface)
{
	if (quakeTimer > 0) {
		int val = quakeTimer % 4;
		if (val == 0) y -= 1;
		else if (val == 2) y += 1;
	}

	int dx = (int)(x * screenScale / 2);
	int dy = (int)(y * screenScale / 2);

	blit(surface, 0, 0, surface->w, surface->h, drawbuffer, dx, dy);
}

void PHL_DrawSurfacePart(double x, double y, int cropx, int cropy, int cropw, int croph, PHL_Surface surface)
{
	if (quakeTimer > 0) {
		int val = quakeTimer % 4;
		if (val == 0) y -= (screenScale == 1) ? 2 : 1;
		else if (val == 2) y += (screenScale == 1) ? 2 : 1;
	}

	int dx = (int)(x * screenScale / 2);
	int dy = (int)(y * screenScale / 2);
	int cx = cropx * screenScale / 2;
	int cy = cropy * screenScale / 2;
	int cw = cropw * screenScale / 2;
	int ch = croph * screenScale / 2;

	blit(surface, cx, cy, cw, ch, drawbuffer, dx, dy);
}

void PHL_DrawBackground(PHL_Background back, PHL_Background fore)
{
	(void)back; (void)fore;
	PHL_DrawSurface(0, 0, &backbuffer);
}

void PHL_UpdateBackground(PHL_Background back, PHL_Background fore)
{
	PHL_Surface prev = drawbuffer;
	drawbuffer = &backbuffer;

	for (int yy = 0; yy < 12; yy++) {
		for (int xx = 0; xx < 16; xx++) {
			PHL_DrawSurfacePart(xx * 40, yy * 40, back.tileX[xx][yy] * 40, back.tileY[xx][yy] * 40, 40, 40, images[imgTiles]);
			if (fore.tileX[xx][yy] != 0 || fore.tileY[xx][yy] != 0) {
				PHL_DrawSurfacePart(xx * 40, yy * 40, fore.tileX[xx][yy] * 40, fore.tileY[xx][yy] * 40, 40, 40, images[imgTiles]);
			}
		}
	}

	drawbuffer = prev;
}
