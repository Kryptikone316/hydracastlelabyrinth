#ifndef SF3500_GRAPHICS_H
#define SF3500_GRAPHICS_H

#include <stdint.h>

/* Plain RGB565 pixel buffer - no SDL surface, no GL. hwdisp_present() takes
 * this format directly (see hwdisp.h), so blitting never needs a format
 * conversion pass. Native res is 320x240 (screenScale fixed at 1) which is
 * already exactly 4:3, matching hwdisp's internal canvas aspect - same
 * "source already matches, no pad needed" case as Manic Miner's 256x192. */
typedef struct {
	uint16_t *pixels;
	int w, h;
	int has_colorkey;
	uint16_t colorkey;
} PHL_SurfaceData;

#define PHL_Surface PHL_SurfaceData*

typedef struct {
	uint8_t r, g, b;
} PHL_RGBData;
#define PHL_RGB PHL_RGBData

typedef struct {
	int tileX[16][12];
	int tileY[16][12];
} PHL_Background;

extern PHL_Surface screen;

extern int wantFullscreen;
extern int screenScale;
extern int desktopFS;

extern int deltaX;
extern int deltaY;

extern int screenW;
extern int screenH;

PHL_RGB PHL_NewRGB(uint8_t r, uint8_t g, uint8_t b);

void PHL_GraphicsInit();
void PHL_GraphicsExit();

void PHL_StartDrawing();
void PHL_EndDrawing();

void PHL_ForceScreenUpdate();

void PHL_SetDrawbuffer(PHL_Surface surf);
void PHL_ResetDrawbuffer();

void PHL_SetColorKey(PHL_Surface surf, int r, int g, int b);

PHL_Surface PHL_NewSurface(int w, int h);
void PHL_FreeSurface(PHL_Surface surf);

PHL_Surface PHL_LoadBMP(int index);

void PHL_DrawRect(int x, int y, int w, int h, PHL_RGB col);

void PHL_DrawSurface(double x, double y, PHL_Surface surface);
void PHL_DrawSurfacePart(double x, double y, int cropx, int cropy, int cropw, int croph, PHL_Surface surface);

void PHL_DrawBackground(PHL_Background back, PHL_Background fore);
void PHL_UpdateBackground(PHL_Background back, PHL_Background fore);

int getXBRZ();
void setXBRZ(int active);

#endif
