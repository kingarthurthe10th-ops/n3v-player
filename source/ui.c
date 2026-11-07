#include "ui.h"
#include <math.h>
#include <string.h>

static C3D_RenderTarget *tTop, *tBot;
static C2D_TextBuf tbuf;
static bool topIsVideo;
static u64 frameNo;

void ui_init(void) {
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 2);
	C2D_Prepare();
	tTop = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	tBot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	tbuf = C2D_TextBufNew(8192);
}

void ui_exit(void) {
	ui_thumbClear();
	C2D_TextBufDelete(tbuf);
	C2D_Fini(); C3D_Fini();
}

void ui_begin(void) {
	topIsVideo = false; frameNo++;
	C2D_TextBufClear(tbuf);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TargetClear(tTop, C_BG); C2D_TargetClear(tBot, C_BG);
}
void ui_beginBottomOnly(void) {
	topIsVideo = true; frameNo++;
	C2D_TextBufClear(tbuf);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TargetClear(tBot, C_BG);
}
void ui_top(void) { C2D_SceneBegin(tTop); }
void ui_bottom(void) { C2D_SceneBegin(tBot); }
void ui_end(void) { C3D_FrameEnd(0); }

void ui_rect(float x, float y, float w, float h, u32 c) { C2D_DrawRectSolid(x, y, 0.5f, w, h, c); }
void ui_circle(float x, float y, float r, u32 c) { C2D_DrawCircleSolid(x, y, 0.5f, r, c); }
void ui_tri(float x1, float y1, float x2, float y2, float x3, float y3, u32 c) {
	C2D_DrawTriangle(x1, y1, c, x2, y2, c, x3, y3, c, 0.5f);
}
void ui_round(float x, float y, float w, float h, float r, u32 c) {
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	if (r < 1) { ui_rect(x, y, w, h, c); return; }
	ui_rect(x + r, y, w - 2 * r, h, c);
	ui_rect(x, y + r, r, h - 2 * r, c);
	ui_rect(x + w - r, y + r, r, h - 2 * r, c);
	ui_circle(x + r, y + r, r, c); ui_circle(x + w - r, y + r, r, c);
	ui_circle(x + r, y + h - r, r, c); ui_circle(x + w - r, y + h - r, r, c);
}

float ui_textW(float size, const char *s) {
	C2D_Text t; C2D_TextParse(&t, tbuf, s);
	float w, h; C2D_TextGetDimensions(&t, size, size, &w, &h);
	return w;
}

float ui_text(float x, float y, float size, u32 c, int align, const char *s) {
	C2D_Text t; C2D_TextParse(&t, tbuf, s); C2D_TextOptimize(&t);
	float w, h; C2D_TextGetDimensions(&t, size, size, &w, &h);
	if (align == AL_CENTER) x -= w / 2; else if (align == AL_RIGHT) x -= w;
	C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, size, size, c);
	return w;
}

void ui_textFit(float x, float y, float size, u32 c, int align, float maxW, const char *s) {
	if (ui_textW(size, s) <= maxW) { ui_text(x, y, size, c, align, s); return; }
	char b[300]; size_t n = strlen(s); if (n > 290) n = 290;
	while (n > 0) {
		while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;      // keep whole UTF-8 characters
		memcpy(b, s, n); strcpy(b + n, "...");
		if (ui_textW(size, b) <= maxW) break;
		n--;
	}
	ui_text(x, y, size, c, align, b);
}

void ui_spinner(float x, float y, float r, u32 c) {
	for (int i = 0; i < 8; i++) {
		float a = (i + frameNo / 4) * (float)M_PI / 4;
		u32 alpha = 40 + i * 26;
		ui_circle(x + cosf(a) * r, y + sinf(a) * r, r * 0.22f, (c & 0x00FFFFFF) | (alpha << 24));
	}
}

// ---- icons (x, y = top-left, s = size)
void icon_folder(float x, float y, float s, u32 c) {
	ui_round(x, y + s * 0.12f, s * 0.45f, s * 0.2f, s * 0.06f, c);
	ui_round(x, y + s * 0.22f, s, s * 0.68f, s * 0.08f, c);
}
void icon_film(float x, float y, float s, u32 c) {
	ui_round(x, y + s * 0.1f, s, s * 0.8f, s * 0.12f, c);
	for (int i = 0; i < 3; i++) {
		ui_rect(x + s * 0.08f, y + s * (0.2f + 0.22f * i), s * 0.1f, s * 0.12f, C_BG);
		ui_rect(x + s * 0.82f, y + s * (0.2f + 0.22f * i), s * 0.1f, s * 0.12f, C_BG);
	}
	ui_tri(x + s * 0.38f, y + s * 0.32f, x + s * 0.38f, y + s * 0.68f, x + s * 0.66f, y + s * 0.5f, C_BG);
}
void icon_download(float x, float y, float s, u32 c) {
	ui_rect(x + s * 0.42f, y, s * 0.16f, s * 0.5f, c);
	ui_tri(x + s * 0.2f, y + s * 0.42f, x + s * 0.8f, y + s * 0.42f, x + s * 0.5f, y + s * 0.75f, c);
	ui_rect(x + s * 0.1f, y + s * 0.84f, s * 0.8f, s * 0.14f, c);
}
void icon_play(float x, float y, float s, u32 c) { ui_tri(x + s * 0.2f, y, x + s * 0.2f, y + s, x + s, y + s * 0.5f, c); }
void icon_pause(float x, float y, float s, u32 c) {
	ui_rect(x + s * 0.15f, y, s * 0.25f, s, c); ui_rect(x + s * 0.6f, y, s * 0.25f, s, c);
}
void icon_back10(float x, float y, float s, u32 c, bool ahead) {
	if (ahead) { ui_tri(x, y, x, y + s, x + s * 0.5f, y + s * 0.5f, c); ui_tri(x + s * 0.5f, y, x + s * 0.5f, y + s, x + s, y + s * 0.5f, c); }
	else { ui_tri(x + s, y, x + s, y + s, x + s * 0.5f, y + s * 0.5f, c); ui_tri(x + s * 0.5f, y, x + s * 0.5f, y + s, x, y + s * 0.5f, c); }
}
void icon_stop(float x, float y, float s, u32 c) { ui_round(x + s * 0.1f, y + s * 0.1f, s * 0.8f, s * 0.8f, s * 0.12f, c); }
void icon_check(float x, float y, float s, u32 c) {
	ui_tri(x, y + s * 0.55f, x + s * 0.15f, y + s * 0.4f, x + s * 0.4f, y + s * 0.85f, c);
	ui_tri(x + s * 0.15f, y + s * 0.4f, x + s * 0.4f, y + s * 0.65f, x + s * 0.4f, y + s * 0.85f, c);
	ui_tri(x + s * 0.4f, y + s * 0.65f, x + s * 0.85f, y + s * 0.1f, x + s, y + s * 0.25f, c);
	ui_tri(x + s * 0.4f, y + s * 0.65f, x + s, y + s * 0.25f, x + s * 0.4f, y + s * 0.85f, c);
}
void icon_cross(float x, float y, float s, u32 c) {
	float t = s * 0.12f;
	C2D_DrawLine(x + t, y + t, c, x + s - t, y + s - t, c, s * 0.18f, 0.5f);
	C2D_DrawLine(x + s - t, y + t, c, x + t, y + s - t, c, s * 0.18f, 0.5f);
}
void icon_up(float x, float y, float s, u32 c) {
	ui_tri(x + s * 0.5f, y, x + s * 0.1f, y + s * 0.45f, x + s * 0.9f, y + s * 0.45f, c);
	ui_rect(x + s * 0.38f, y + s * 0.4f, s * 0.24f, s * 0.6f, c);
}

// ---- touch
static touchPosition tp; static bool down, wasDown; static float startX, startY, lastX, lastY;
void ui_input(void) {
	wasDown = down;
	down = hidKeysHeld() & KEY_TOUCH;
	if (down) {
		hidTouchRead(&tp); lastX = tp.px; lastY = tp.py;
		if (!wasDown) { startX = lastX; startY = lastY; }
	}
}
bool ui_touchDown(void) { return down; }
void ui_touchPos(float *x, float *y) { *x = lastX; *y = lastY; }
static bool inside(float px, float py, float x, float y, float w, float h) { return px >= x && px < x + w && py >= y && py < y + h; }
bool ui_touchIn(float x, float y, float w, float h) { return down && inside(lastX, lastY, x, y, w, h); }
bool ui_tapped(float x, float y, float w, float h) {
	return !down && wasDown && inside(lastX, lastY, x, y, w, h) && inside(startX, startY, x, y, w, h);
}

bool ui_button(float x, float y, float w, float h, const char *glyph, const char *label, bool primary, int ic) {
	bool held = ui_touchIn(x, y, w, h) && inside(startX, startY, x, y, w, h);
	u32 bg = primary ? (held ? UI_RGB(30, 130, 180) : C_ACCENT) : (held ? C_LINE : C_CARD2);
	ui_round(x, y, w, h, 10, bg);
	float cy = y + h / 2;
	if (ic != IC_NONE && (!label || !label[0])) {                         // icon-only button
		float s = h * 0.38f, ix = x + w / 2 - s / 2, iy = cy - s / 2 - (glyph ? 5 : 0);
		switch (ic) {
		case IC_PLAY: icon_play(ix, iy, s, C_TEXT); break;
		case IC_PAUSE: icon_pause(ix, iy, s, C_TEXT); break;
		case IC_BACK10: icon_back10(ix, iy, s, C_TEXT, false); break;
		case IC_AHEAD10: icon_back10(ix, iy, s, C_TEXT, true); break;
		case IC_STOP: icon_stop(ix, iy, s, C_TEXT); break;
		}
		if (glyph) ui_text(x + w / 2, y + h - 17, 0.5f, primary ? C_TEXT : C_MUTED, AL_CENTER, glyph);
	} else {
		float lw = ui_textW(0.55f, label), gw = glyph ? ui_textW(0.62f, glyph) + 5 : 0;
		float iw = ic != IC_NONE ? 20 : 0, tx = x + w / 2 - (lw + gw + iw) / 2;
		if (ic == IC_DOWNLOAD) icon_download(tx, cy - 8, 16, C_TEXT);
		else if (ic == IC_UP) icon_up(tx, cy - 8, 16, C_TEXT);
		else if (ic == IC_PLAY) icon_play(tx + 2, cy - 7, 14, C_TEXT);
		else if (ic == IC_FOLDER) icon_folder(tx, cy - 8, 16, C_TEXT);
		tx += iw;
		if (glyph) { ui_text(tx, cy - 9, 0.62f, primary ? C_TEXT : C_ACCENT2, AL_LEFT, glyph); tx += gw; }
		ui_text(tx, cy - 8, 0.55f, C_TEXT, AL_LEFT, label);
	}
	return ui_tapped(x, y, w, h);
}

// ---- preview picture
static C3D_Tex thumb; static bool thumbOk; static C2D_Image thumbImg; static Tex3DS_SubTexture thumbSub;
void ui_thumbClear(void) { if (thumbOk) { C3D_TexDelete(&thumb); thumbOk = false; } }
void ui_thumbSet(const u16 *px, int w, int h) {
	if (!thumbOk) { if (!C3D_TexInit(&thumb, 256, 128, GPU_RGB565)) return; thumbOk = true; C3D_TexSetFilter(&thumb, GPU_LINEAR, GPU_LINEAR); }
	u16 *dst = thumb.data;
	memset(dst, 0, 256 * 128 * 2);
	for (int y = 0; y < h && y < 128; y++)
		for (int x = 0; x < w && x < 256; x++) {
			u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
			dst[((y >> 3) * 32 + (x >> 3)) * 64 + m] = px[y * w + x];
		}
	GSPGPU_FlushDataCache(dst, 256 * 128 * 2);
	thumbSub = (Tex3DS_SubTexture){ (u16)w, (u16)h, 0.0f, 1.0f, w / 256.0f, 1.0f - h / 128.0f };
	thumbImg = (C2D_Image){ &thumb, &thumbSub };
}
bool ui_thumbDraw(float x, float y, float w, float h) {
	if (!thumbOk || !thumbImg.subtex) return false;
	C2D_DrawImageAt(thumbImg, x, y, 0.5f, NULL, w / thumbSub.width, h / thumbSub.height);
	return true;
}
