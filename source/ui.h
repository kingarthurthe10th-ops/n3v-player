#pragma once
// Small drawing toolkit on citro2d: colours, rounded cards, text in the 3DS system font, icons, touch.
#include <citro2d.h>
#include <stdbool.h>

#define UI_RGB(r, g, b) C2D_Color32(r, g, b, 255)
#define C_BG      UI_RGB(18, 21, 31)
#define C_HEAD    UI_RGB(24, 28, 40)
#define C_CARD    UI_RGB(34, 39, 54)
#define C_CARD2   UI_RGB(45, 51, 70)
#define C_LINE    UI_RGB(58, 65, 86)
#define C_TEXT    UI_RGB(240, 243, 248)
#define C_MUTED   UI_RGB(140, 149, 170)
#define C_ACCENT  UI_RGB(42, 159, 216)
#define C_ACCENT2 UI_RGB(90, 200, 255)
#define C_OK      UI_RGB(70, 190, 120)
#define C_BAD     UI_RGB(232, 92, 80)
#define C_FOLDER  UI_RGB(240, 190, 80)

// button symbols in the system font
#define G_A "\xEE\x80\x80"
#define G_B "\xEE\x80\x81"
#define G_X "\xEE\x80\x82"
#define G_Y "\xEE\x80\x83"
#define G_L "\xEE\x80\x84"
#define G_R "\xEE\x80\x85"
#define G_DPAD "\xEE\x80\x86"

enum { AL_LEFT = 0, AL_CENTER = 1, AL_RIGHT = 2 };

void ui_init(void);
void ui_exit(void);
void ui_begin(void);            // starts a frame (top and bottom both drawn by citro2d)
void ui_beginBottomOnly(void);  // starts a frame where the top screen belongs to the video
void ui_top(void);
void ui_bottom(void);
void ui_end(void);

void ui_rect(float x, float y, float w, float h, u32 c);
void ui_round(float x, float y, float w, float h, float r, u32 c);
void ui_circle(float x, float y, float r, u32 c);
void ui_tri(float x1, float y1, float x2, float y2, float x3, float y3, u32 c);
float ui_text(float x, float y, float size, u32 c, int align, const char *s);
float ui_textW(float size, const char *s);
void ui_textFit(float x, float y, float size, u32 c, int align, float maxW, const char *s);   // ends with ... if too long
void ui_spinner(float x, float y, float r, u32 c);

void icon_folder(float x, float y, float s, u32 c);
void icon_film(float x, float y, float s, u32 c);
void icon_download(float x, float y, float s, u32 c);
void icon_play(float x, float y, float s, u32 c);
void icon_pause(float x, float y, float s, u32 c);
void icon_back10(float x, float y, float s, u32 c, bool ahead);
void icon_stop(float x, float y, float s, u32 c);
void icon_check(float x, float y, float s, u32 c);
void icon_cross(float x, float y, float s, u32 c);
void icon_up(float x, float y, float s, u32 c);

// a touch button: draws it and returns true when it was tapped (released inside) this frame
bool ui_button(float x, float y, float w, float h, const char *glyph, const char *label, bool primary, int iconKind);
enum { IC_NONE, IC_PLAY, IC_PAUSE, IC_BACK10, IC_AHEAD10, IC_STOP, IC_DOWNLOAD, IC_UP, IC_FOLDER };

void ui_input(void);            // call once per frame after hidScanInput()
bool ui_touchDown(void);
bool ui_touchIn(float x, float y, float w, float h);       // finger is down inside
bool ui_tapped(float x, float y, float w, float h);        // released inside this frame (and started inside)
void ui_touchPos(float *x, float *y);

// the preview picture (RGB565, row-major, up to 256x128)
void ui_thumbSet(const u16 *px, int w, int h);
void ui_thumbClear(void);
bool ui_thumbDraw(float x, float y, float w, float h);
