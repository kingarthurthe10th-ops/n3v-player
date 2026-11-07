// N3V Player: browse the SD card, play .n3v videos made by converter/n3vconvert.py.
// Video: 4x4 two-colour blocks (zlib-packed deltas) decoded by the CPU into the top framebuffer.
// Audio: IMA ADPCM decoded by the CPU and streamed to the DSP; its sample clock picks the frame.
// A reader thread keeps a few seconds of both read ahead from the SD card, so slow reads never pause playback.
#include <3ds.h>
#include <zlib.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <math.h>
#include "net.h"
#include "ui.h"

#define W 400
#define H 240
#define NBLK 6000
#define VRING (6u << 20)
#define ABLK_FR 32                 // ADPCM packets (64 samples each) per audio block
#define ABLK_BYTES (ABLK_FR * 68)
#define ABLK_SAMP (ABLK_FR * 64)
#define AQ 96                      // audio blocks kept read ahead (~6 s)
#define NWB 8
#define DEST_DIR "sdmc:/videos/"

typedef struct { char magic[4]; u32 fpsNum, fpsDen, frames, rate, audioOff, audioBytes, keyInt, keyCount, keyOff; } Hdr;

static u16 pic[W * H];             // the picture, in framebuffer order (column by column, bottom to top)
static u8 dbuf[1 + 750 + NBLK * 8];

// ---------- reader thread ----------
static Hdr hd; static u32 *keys;
static FILE *fv, *fa;
static u8 *vring; static volatile u32 vw, vr, vRead, vUsed;      // video ring: [u32 len][data], 0xFFFFFFFF = wrap
static u8 *aring; static u16 alen[AQ]; static volatile u32 aRead, aUsed; static u32 aLeft;
static volatile bool running; static Thread rth;
static u32 vNextFrame;

static void readerMain(void *arg) {
	(void)arg; u32 pend = 0; bool hasPend = false;
	while (running) {
		bool did = false;
		if (aLeft && aRead - aUsed < AQ) {
			u32 n = aLeft < ABLK_BYTES ? aLeft : ABLK_BYTES;
			u32 slot = aRead % AQ;
			n = fread(aring + slot * ABLK_BYTES, 1, n, fa) / 68 * 68;
			aLeft = n ? aLeft - n : 0; alen[slot] = n;
			if (n) { __sync_synchronize(); aRead++; }
			did = true;
		}
		if (vNextFrame < hd.frames) {
			if (!hasPend) { if (fread(&pend, 4, 1, fv) != 1) { vNextFrame = hd.frames; continue; } hasPend = true; }
			u32 need = 4 + pend, r = vr, at = vw; bool ok = false, wrap = false;
			if (at >= r) { if (VRING - at >= need + 4) ok = true; else if (r > need) ok = wrap = true; }
			else if (r - at > need) ok = true;
			if (ok) {
				if (wrap) { *(u32 *)(vring + at) = 0xFFFFFFFF; at = 0; }
				*(u32 *)(vring + at) = pend;
				if (pend) fread(vring + at + 4, 1, pend, fv);
				__sync_synchronize(); vw = at + need; vRead++; vNextFrame++; hasPend = false; did = true;
			}
		}
		if (!did) svcSleepThread(2000000);
	}
}

static void readerStop(void) { if (rth) { running = false; threadJoin(rth, U64_MAX); threadFree(rth); rth = NULL; } }

// start reading at keyframe k; returns its frame number
static u32 readerStart(u32 k) {
	readerStop();
	u32 frame = k * hd.keyInt;
	fseek(fv, keys[k], SEEK_SET);
	vw = vr = vRead = vUsed = 0; vNextFrame = frame;
	aRead = aUsed = 0; aLeft = 0;
	if (hd.rate) {
		u64 pk = (u64)frame * hd.fpsDen * hd.rate / hd.fpsNum / 64;
		u64 off = pk * 68; if (off > hd.audioBytes) off = hd.audioBytes;
		fseek(fa, hd.audioOff + off, SEEK_SET); aLeft = hd.audioBytes - off;
	}
	running = true;
	s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	rth = threadCreate(readerMain, NULL, 32 * 1024, prio - 1, -2, false);
	return frame;
}

// ---------- video decode ----------
static inline void blockDraw(u32 b, const u8 *p) {
	u32 c0 = p[0] | p[1] << 8, c1 = p[2] | p[3] << 8, ix = p[4] | p[5] << 8 | p[6] << 16 | (u32)p[7] << 24;
	u32 r0 = c0 >> 11, g0 = (c0 >> 5) & 63, b0 = c0 & 31, r1 = c1 >> 11, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
	u16 pal[4] = { c0, c1,
		((2 * r0 + r1) / 3) << 11 | ((2 * g0 + g1) / 3) << 5 | (2 * b0 + b1) / 3,
		((r0 + 2 * r1) / 3) << 11 | ((g0 + 2 * g1) / 3) << 5 | (b0 + 2 * b1) / 3 };
	u32 bx = b / 60, by = b % 60;
	u16 *col = pic + bx * 4 * H + (H - 1 - by * 4);
	for (int px = 0; px < 4; px++, col += H)
		for (int py = 0; py < 4; py++) col[-py] = pal[(ix >> (2 * (py * 4 + px))) & 3];
}

static bool decodeNext(void) {     // false if the reader hasn't got the frame yet
	if (vUsed >= vRead) return false;
	u32 at = vr, len = *(u32 *)(vring + at);
	if (len == 0xFFFFFFFF) { at = 0; len = *(u32 *)vring; }
	if (len) {
		uLongf dl = sizeof dbuf;
		if (uncompress(dbuf, &dl, vring + at + 4, len) == Z_OK) {
			const u8 *p = dbuf + 1;
			if (dbuf[0] & 1) { for (u32 b = 0; b < NBLK; b++, p += 8) blockDraw(b, p); }
			else {
				const u8 *bm = p; p += 750;
				for (u32 i = 0; i < 750; i++) {
					u8 m = bm[i];
					while (m) { int j = __builtin_ctz(m); blockDraw(i * 8 + j, p); p += 8; m &= m - 1; }
				}
			}
		}
	}
	__sync_synchronize(); vr = at + 4 + len; vUsed++;
	return true;
}

// ---------- audio ----------
static const s16 steps[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
	157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
	2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,
	18500,20350,22385,24623,27086,29794,32767};
static const s8 idxTab[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
static ndspWaveBuf wb[NWB]; static s16 *abuf; static bool wbQueued[NWB];
static bool audioOk; static u64 played;

static void adpcmPacket(const u8 *p, s16 *out) {          // 64 samples into every other s16
	s32 pred = (s16)((p[0] << 8 | p[1]) & 0xFF80); int ix = p[1] & 0x7F; if (ix > 88) ix = 88;
	for (int i = 0; i < 32; i++) {
		u8 b = p[2 + i];
		for (int h = 0; h < 2; h++) {
			int nib = h ? b >> 4 : b & 15, st = steps[ix], d = st >> 3;
			if (nib & 1) d += st >> 2;
			if (nib & 2) d += st >> 1;
			if (nib & 4) d += st;
			pred += nib & 8 ? -d : d;
			if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
			ix += idxTab[nib]; if (ix < 0) ix = 0; else if (ix > 88) ix = 88;
			*out = pred; out += 2;
		}
	}
}

static void audioFeed(void) {
	for (int i = 0; i < NWB; i++) {
		if (wbQueued[i] && wb[i].status != NDSP_WBUF_DONE) continue;
		if (wbQueued[i]) { played += wb[i].nsamples; wbQueued[i] = false; }
		if (aUsed >= aRead) continue;
		u32 slot = aUsed % AQ, n = alen[slot] / 68; const u8 *src = aring + slot * ABLK_BYTES;
		s16 *dst = (s16 *)wb[i].data_pcm16;
		for (u32 k = 0; k < n; k++) { adpcmPacket(src + k * 68, dst + k * 128); adpcmPacket(src + k * 68 + 34, dst + k * 128 + 1); }
		__sync_synchronize(); aUsed++;
		wb[i].nsamples = n * 64; DSP_FlushDataCache(dst, n * 256);
		ndspChnWaveBufAdd(0, &wb[i]); wbQueued[i] = true;
	}
}

static void audioReset(void) {
	ndspChnWaveBufClear(0);
	for (int i = 0; i < NWB; i++) { wbQueued[i] = false; wb[i].status = NDSP_WBUF_DONE; }
	played = 0;
}

// ---------- player ----------
static void fmtTime(char *s, u32 sec) {
	if (sec >= 3600) sprintf(s, "%lu:%02lu:%02lu", sec / 3600, sec / 60 % 60, sec % 60);
	else sprintf(s, "%lu:%02lu", sec / 60, sec % 60);
}

static void fmtSize(char *s, u64 b) {
	if (b >= 1024ull * 1024 * 1024) sprintf(s, "%.1f GB", b / 1073741824.0);
	else sprintf(s, "%.1f MB", b / 1048576.0);
}

// The top screen shows the video by CPU (RGB565, swapped by hand); otherwise citro2d draws it (BGR8).
static void videoMode(bool on) { gfxSetScreenFormat(GFX_TOP, on ? GSP_RGB565_OES : GSP_BGR8_OES); }

static void presentVideo(void) {
	u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	memcpy(fb, pic, sizeof pic);
	GSPGPU_FlushDataCache(fb, sizeof pic);
	gfxScreenSwapBuffers(GFX_TOP, false);
}

static void messageBox(const char *title, const char *body) {
	while (aptMainLoop()) {
		hidScanInput(); ui_input(); u32 k = hidKeysDown();
		ui_begin();
		ui_top();
		ui_round(30, 74, 340, 92, 14, C_CARD);
		ui_circle(70, 120, 22, C_BAD); icon_cross(58, 108, 24, C_TEXT);
		ui_textFit(104, 106, 0.72f, C_TEXT, AL_LEFT, 250, title);
		ui_bottom();
		ui_round(12, 14, 296, 150, 12, C_CARD);
		ui_text(26, 28, 0.55f, C_TEXT, AL_LEFT, body);
		bool ok = ui_button(100, 182, 120, 42, NULL, "OK", true, IC_NONE);
		ui_end();
		if ((k & (KEY_A | KEY_B)) || ok) break;
	}
}

static int play(const char *path, const char *name) {      // 0 back to browser, 1 quit
	fv = fopen(path, "rb"); fa = fopen(path, "rb");
	if (!fv || !fa || fread(&hd, sizeof hd, 1, fv) != 1 || memcmp(hd.magic, "N3V1", 4) || !hd.frames || !hd.fpsNum || !hd.keyCount) {
		if (fv) fclose(fv);
		if (fa) fclose(fa);
		messageBox("Can't play this file", "It isn't a playable .n3v video, or it\nwas damaged while copying.\n\nMake it again with the N3V converter\non your PC.");
		return 0;
	}
	setvbuf(fv, NULL, _IOFBF, 64 * 1024); setvbuf(fa, NULL, _IOFBF, 32 * 1024);
	keys = malloc(hd.keyCount * 4); fseek(fv, hd.keyOff, SEEK_SET); fread(keys, 4, hd.keyCount, fv);
	bool useAudio = audioOk && hd.rate;
	if (useAudio) { ndspChnReset(0); ndspChnSetInterp(0, NDSP_INTERP_LINEAR); ndspChnSetRate(0, hd.rate); ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
		float mix[12] = {1, 1}; ndspChnSetMix(0, mix); }

	videoMode(true);
	memset(pic, 0, sizeof pic); presentVideo(); presentVideo();
	char tot[16]; u32 totSec = (u32)((u64)hd.frames * hd.fpsDen / hd.fpsNum); fmtTime(tot, totSec);

	int result = 0; u32 seekTo = 0; bool paused = false, done = false;
	int pfocus = 2;                                     // 0 Stop, 1 back 10 s, 2 play/pause, 3 ahead 10 s
restart:;
	u32 k = seekTo / hd.keyInt; if (k >= hd.keyCount) k = hd.keyCount - 1;
	u32 base = readerStart(k), cur = base;
	if (useAudio) audioReset();
	u64 baseSample = (u64)base * hd.fpsDen * hd.rate / hd.fpsNum / 64 * 64;
	for (int t = 0; t < 120 && (vRead < 45 && vNextFrame < hd.frames); t++) svcSleepThread(10000000);  // pre-buffer
	u64 t0 = osGetTime(), pausedAt = 0;
	if (paused) pausedAt = t0;
	if (useAudio) { audioFeed(); ndspChnSetPaused(0, paused); }
	bool dragging = false; float dragFrac = 0;

	while (aptMainLoop()) {
		hidScanInput(); ui_input(); u32 kd = hidKeysDown();
		u32 target;
		if (useAudio) {
			audioFeed();
			bool any = false; for (int i = 0; i < NWB; i++) any |= wbQueued[i];
			u64 pos = baseSample + played + (any ? ndspChnGetSamplePos(0) : 0);
			target = (u32)(pos * hd.fpsNum / ((u64)hd.fpsDen * hd.rate));
			if (!any && !aLeft && aUsed >= aRead) target = hd.frames;          // the sound has finished
		} else target = base + (u32)((paused ? pausedAt - t0 : osGetTime() - t0) * hd.fpsNum / (1000ull * hd.fpsDen));
		if (target > hd.frames) target = hd.frames;
		int budget = 6;
		while (cur < target && budget-- > 0) { if (!decodeNext()) break; cur++; }
		if (useAudio) audioFeed();
		presentVideo();

		// ---- the bottom screen: player controls
		u32 sec = (u32)((u64)cur * hd.fpsDen / hd.fpsNum);
		char now[16], line[64]; fmtTime(now, sec);
		float frac = (float)cur / hd.frames, bufFrac = (float)(cur + (vRead - vUsed)) / hd.frames;
		if (bufFrac > 1) bufFrac = 1;
		ui_beginBottomOnly(); ui_bottom();
		ui_rect(0, 0, 320, 40, C_HEAD);
		icon_film(12, 11, 18, C_ACCENT2);
		ui_textFit(38, 11, 0.6f, C_TEXT, AL_LEFT, 270, name);
		// progress bar (tap or drag to jump)
		const float BX = 20, BY = 84, BW = 280;
		if (ui_touchIn(0, 62, 320, 50)) { float tx, ty; ui_touchPos(&tx, &ty); dragging = true; dragFrac = (tx - BX) / BW; }
		if (dragFrac < 0) dragFrac = 0;
		if (dragFrac > 1) dragFrac = 1;
		float show = dragging ? dragFrac : frac;
		ui_round(BX, BY, BW, 10, 5, C_CARD2);
		ui_round(BX, BY, BW * bufFrac, 10, 5, C_LINE);
		ui_round(BX, BY, BW * show < 10 ? 10 : BW * show, 10, 5, C_ACCENT);
		ui_circle(BX + BW * show, BY + 5, dragging ? 11 : 8, C_TEXT);
		if (dragging) { char p[16]; fmtTime(p, (u32)(dragFrac * totSec)); ui_text(BX + BW * show, BY - 26, 0.55f, C_ACCENT2, AL_CENTER, p); }
		ui_text(BX, BY + 18, 0.52f, C_TEXT, AL_LEFT, now);
		ui_text(BX + BW, BY + 18, 0.52f, C_MUTED, AL_RIGHT, tot);
		if (paused) ui_text(160, BY + 18, 0.52f, C_ACCENT2, AL_CENTER, "PAUSED");
		// buttons
		if (kd & KEY_RIGHT) pfocus = (pfocus + 1) % 4;
		if (kd & KEY_LEFT) pfocus = (pfocus + 3) % 4;
		bool tStop = ui_button(12, 140, 68, 60, NULL, NULL, pfocus == 0, IC_STOP);
		bool tBack = ui_button(88, 140, 68, 60, NULL, NULL, pfocus == 1, IC_BACK10);
		bool tPlay = ui_button(164, 140, 68, 60, NULL, NULL, pfocus == 2, paused ? IC_PLAY : IC_PAUSE);
		bool tAhead = ui_button(240, 140, 68, 60, NULL, NULL, pfocus == 3, IC_AHEAD10);
		static const char *plabel[4] = { "Stop", "-10 s", NULL, "+10 s" };
		for (int i = 0; i < 4; i++)
			ui_text(46 + i * 76, 206, 0.45f, pfocus == i ? C_TEXT : C_MUTED, AL_CENTER, plabel[i] ? plabel[i] : paused ? "Play" : "Pause");
		snprintf(line, sizeof line, "%s", useAudio ? "Sound on" : "No sound");
		ui_text(160, 222, 0.42f, C_MUTED, AL_CENTER, line);
		ui_end();

		if (dragging && !ui_touchDown()) {                 // let go of the bar: jump there
			dragging = false;
			seekTo = (u32)(dragFrac * (hd.frames - 1));
			goto restart;
		}
		if (kd & KEY_START) { result = 1; break; }
		int aF = (kd & KEY_A) ? pfocus : -1;               // A presses the highlighted button
		if ((kd & KEY_B) || tStop || aF == 0) break;
		if (tPlay || aF == 2) {
			paused = !paused;
			if (useAudio) ndspChnSetPaused(0, paused);
			if (paused) pausedAt = osGetTime(); else t0 += osGetTime() - pausedAt;
		}
		s32 jump = ((kd & KEY_R) || tAhead || aF == 3 ? 10 : 0) - ((kd & KEY_L) || tBack || aF == 1 ? 10 : 0) + (kd & KEY_UP ? 60 : 0) - (kd & KEY_DOWN ? 60 : 0);
		if (jump) {
			s64 f = (s64)cur + (s64)jump * hd.fpsNum / hd.fpsDen;
			if (jump < 0) f -= hd.keyInt / 2;               // back: land a little before
			seekTo = f < 0 ? 0 : f >= hd.frames ? hd.frames - 1 : f;
			goto restart;
		}
		if (cur >= hd.frames) { done = true; break; }
	}
	readerStop();
	if (useAudio) audioReset();
	free(keys); fclose(fv); fclose(fa);
	if (done) svcSleepThread(400000000);
	videoMode(false);
	return result;
}

// ---------- browser ----------
typedef struct { char name[256]; bool dir; bool info; s32 dur; u64 size; } Ent;
static Ent *ents; static int nEnts;

static int entCmp(const void *a, const void *b) {
	const Ent *x = a, *y = b;
	if (x->dir != y->dir) return x->dir ? -1 : 1;
	return strcasecmp(x->name, y->name);
}

static bool isN3v(const char *n) { size_t l = strlen(n); return l > 4 && !strcasecmp(n + l - 4, ".n3v"); }

static void listDir(const char *path) {
	nEnts = 0; DIR *d = opendir(path); if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && nEnts < 1024) {
		if (e->d_name[0] == '.') continue;
		bool dir = e->d_type == DT_DIR;
		if (!dir && !isN3v(e->d_name)) continue;
		strncpy(ents[nEnts].name, e->d_name, 255); ents[nEnts].name[255] = 0;
		ents[nEnts].dir = dir; ents[nEnts].info = false; nEnts++;
	}
	closedir(d); qsort(ents, nEnts, sizeof(Ent), entCmp);
}

static void entInfo(const char *path, Ent *e) {      // size and length, read when the row is first shown
	if (e->info) return;
	e->info = true; e->dur = -1; e->size = 0;
	if (e->dir) return;
	char full[1300]; snprintf(full, sizeof full, "%s%s", path, e->name);
	FILE *f = fopen(full, "rb"); if (!f) return;
	Hdr h;
	if (fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "N3V1", 4) && h.fpsNum) e->dur = (s32)((u64)h.frames * h.fpsDen / h.fpsNum);
	fseek(f, 0, SEEK_END); e->size = ftell(f); fclose(f);
}

// decodes a keyframe ~1/8 into the video at half size for the preview picture
static void loadThumb(const char *full) {
	ui_thumbClear();
	FILE *f = fopen(full, "rb"); if (!f) return;
	Hdr h; u32 off, len; u8 *z = NULL;
	if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "N3V1", 4) || !h.keyCount) goto out;
	u32 k = h.keyCount > 8 ? h.keyCount / 8 : h.keyCount > 1 ? 1 : 0;
	fseek(f, h.keyOff + k * 4, SEEK_SET);
	if (fread(&off, 4, 1, f) != 1) goto out;
	fseek(f, off, SEEK_SET);
	if (fread(&len, 4, 1, f) != 1 || !len || len > sizeof dbuf * 2) goto out;
	z = malloc(len); if (!z || fread(z, 1, len, f) != len) goto out;
	uLongf dl = sizeof dbuf;
	if (uncompress(dbuf, &dl, z, len) != Z_OK || !(dbuf[0] & 1)) goto out;
	static u16 tb[200 * 120];
	const u8 *p = dbuf + 1;
	for (u32 b = 0; b < NBLK; b++, p += 8) {
		u32 c0 = p[0] | p[1] << 8, c1 = p[2] | p[3] << 8, ix = p[4] | p[5] << 8 | p[6] << 16 | (u32)p[7] << 24;
		u32 r0 = c0 >> 11, g0 = (c0 >> 5) & 63, b0 = c0 & 31, r1 = c1 >> 11, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
		u16 pal[4] = { c0, c1, ((2 * r0 + r1) / 3) << 11 | ((2 * g0 + g1) / 3) << 5 | (2 * b0 + b1) / 3,
			((r0 + 2 * r1) / 3) << 11 | ((g0 + 2 * g1) / 3) << 5 | (b0 + 2 * b1) / 3 };
		u32 bx = b / 60, by = b % 60;
		for (int j = 0; j < 2; j++)
			for (int i = 0; i < 2; i++) tb[(by * 2 + j) * 200 + bx * 2 + i] = pal[(ix >> (2 * ((j * 2 + 1) * 4 + i * 2 + 1))) & 3];
	}
	ui_thumbSet(tb, 200, 120);
out:
	free(z); fclose(f);
}

static void drawList(const char *path, int sel, float scroll, float selY) {
	const float ROW = 32, LY = 36;
	if (!nEnts) {
		ui_circle(200, 96, 34, C_CARD);
		icon_download(184, 80, 32, C_ACCENT2);
		ui_text(200, 142, 0.72f, C_TEXT, AL_CENTER, "No videos here yet");
		ui_text(200, 170, 0.55f, C_MUTED, AL_CENTER, "Choose From PC below to get videos,");
		ui_text(200, 188, 0.55f, C_MUTED, AL_CENTER, "or copy .n3v files into this folder.");
	} else {
		ui_round(6, LY + selY - scroll, 382, ROW - 2, 9, C_ACCENT);
		int first = (int)(scroll / ROW); if (first < 0) first = 0;
		for (int i = first; i < nEnts && i < first + 8; i++) {
			float y = LY + i * ROW - scroll;
			Ent *e = &ents[i]; entInfo(path, e);
			bool s = i == sel;
			if (e->dir) icon_folder(16, y + 6, 18, s ? C_TEXT : C_FOLDER);
			else icon_film(16, y + 6, 18, s ? C_TEXT : C_ACCENT2);
			char right[48] = "";
			if (!e->dir) {
				char d[16] = "?", z[16]; if (e->dur >= 0) fmtTime(d, e->dur);
				fmtSize(z, e->size); snprintf(right, sizeof right, "%s  \xC2\xB7  %s", d, z);
			}
			float rw = right[0] ? ui_text(380, y + 8, 0.5f, s ? C_TEXT : C_MUTED, AL_RIGHT, right) : 0;
			ui_textFit(44, y + 6, 0.58f, C_TEXT, AL_LEFT, 380 - 44 - rw - 12, e->name);
		}
		if (nEnts > 6) {                                    // scroll bar
			float th = 192.0f * 6 / nEnts, ty = LY + (192 - th) * (scroll / ((nEnts - 6) * ROW));
			ui_round(393, LY, 4, 192, 2, C_CARD); ui_round(393, ty, 4, th, 2, C_MUTED);
		}
	}
	ui_rect(0, 0, 400, 30, C_HEAD);                         // header on top of the scrolled rows
	icon_film(10, 7, 17, C_ACCENT2);
	ui_text(34, 6, 0.62f, C_TEXT, AL_LEFT, "N3V Player");
	char shown[300]; snprintf(shown, sizeof shown, "SD card%s", path + 5);
	if (strlen(shown) > 8 && shown[strlen(shown) - 1] == '/') shown[strlen(shown) - 1] = 0;
	ui_textFit(392, 8, 0.5f, C_MUTED, AL_RIGHT, 230, shown);
}

int main(void) {
	gfxInitDefault(); romfsInit(); osSetSpeedupEnable(true);
	ui_init();
	vring = malloc(VRING); aring = malloc(AQ * ABLK_BYTES); ents = malloc(1024 * sizeof(Ent));
	audioOk = R_SUCCEEDED(ndspInit());
	if (audioOk) {
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		abuf = linearAlloc(NWB * ABLK_SAMP * 4); memset(wb, 0, sizeof wb);
		for (int i = 0; i < NWB; i++) wb[i].data_pcm16 = abuf + i * ABLK_SAMP * 2;
	}
	char path[1024] = "sdmc:/";
	{ struct stat st; if (!stat("sdmc:/videos", &st) && S_ISDIR(st.st_mode)) strcpy(path, "sdmc:/videos/"); }
	hidSetRepeatParameters(18, 4); listDir(path);
	int focus = 0;                                      // highlighted bottom button: 0 Play/Open, 1 Back, 2 From PC
	int sel = 0, thumbFor = -2; float scroll = 0, selY = 0; u64 selChanged = 0;
	const int ROWS = 6; const float ROW = 32;
#ifdef N3V_AUTOTEST
	for (int i = 0; i < nEnts; i++) if (!ents[i].dir) { sel = i; break; }
	int autoKeys = 200;
#endif
#ifdef N3V_NETTEST
	int autoNet = 90;
#endif
	while (aptMainLoop()) {
		hidScanInput(); ui_input();
		u32 kd = hidKeysDown() | (hidKeysDownRepeat() & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT));
#ifdef N3V_AUTOTEST
		if (autoKeys > 0 && --autoKeys == 0) kd |= KEY_A;
#endif
#ifdef N3V_NETTEST
		if (autoNet > 0 && --autoNet == 0) kd |= KEY_Y;
#endif
		int oldSel = sel;
		if (nEnts) {
			if (kd & KEY_DOWN) sel = (sel + 1) % nEnts;
			if (kd & KEY_UP) sel = (sel + nEnts - 1) % nEnts;
		}
		if (sel != oldSel) selChanged = osGetTime();
		// smooth scrolling and highlight
		float wantScroll = scroll;
		if (sel * ROW < wantScroll) wantScroll = sel * ROW;
		if ((sel + 1) * ROW > wantScroll + ROWS * ROW) wantScroll = (sel + 1) * ROW - ROWS * ROW;
		scroll += (wantScroll - scroll) * 0.35f; selY += (sel * ROW - selY) * 0.4f;
		if (fabsf(wantScroll - scroll) < 0.5f) scroll = wantScroll;
		if (fabsf(sel * ROW - selY) < 0.5f) selY = sel * ROW;
		// preview picture, once the selection has rested a moment
		Ent *cur = nEnts ? &ents[sel] : NULL;
		if (cur && !cur->dir && thumbFor != sel && osGetTime() - selChanged > 180) {
			char full[1300]; snprintf(full, sizeof full, "%s%s", path, cur->name); loadThumb(full); thumbFor = sel;
		}
		if (cur && cur->dir && thumbFor != -1) { ui_thumbClear(); thumbFor = -1; }

		bool atRoot = !strcmp(path, "sdmc:/");
		bool avail[3] = { cur != NULL, !atRoot, true };
		if (kd & KEY_RIGHT) for (int t = 0; t < 3; t++) { focus = (focus + 1) % 3; if (avail[focus]) break; }
		if (kd & KEY_LEFT) for (int t = 0; t < 3; t++) { focus = (focus + 2) % 3; if (avail[focus]) break; }
		if (!avail[focus]) focus = avail[0] ? 0 : 2;
		ui_begin();
		ui_top(); drawList(path, sel, scroll, selY);
		ui_bottom();
		bool tA = false, tB = false, tY = false, tPic = false;
		if (!cur) {
			ui_round(12, 12, 296, 160, 12, C_CARD);
			ui_text(26, 22, 0.62f, C_ACCENT2, AL_LEFT, "Get videos onto your 3DS");
			ui_text(26, 52, 0.52f, C_TEXT, AL_LEFT, "1.  On your PC, open the N3V converter\n     and convert a video.\n2.  Choose \"Yes, send it\".\n3.  Choose From PC below and type the\n     address your PC shows.");
		} else if (cur->dir) {
			ui_round(12, 12, 296, 160, 12, C_CARD);
			icon_folder(128, 34, 64, C_FOLDER);
			ui_textFit(160, 112, 0.66f, C_TEXT, AL_CENTER, 270, cur->name);
			ui_text(160, 140, 0.5f, C_MUTED, AL_CENTER, "Folder");
			tPic = ui_tapped(12, 12, 296, 160);
		} else {
			entInfo(path, cur);
			ui_round(56, 8, 208, 128, 10, C_CARD);
			if (!ui_thumbDraw(60, 12, 200, 120)) icon_film(130, 42, 60, C_CARD2);
			else { ui_circle(160, 72, 20, UI_RGB(0, 0, 0) & 0x88FFFFFF); icon_play(152, 63, 18, C_TEXT); }
			tPic = ui_tapped(56, 8, 208, 128);
			ui_textFit(160, 140, 0.6f, C_TEXT, AL_CENTER, 296, cur->name);
			char d[16] = "?", z[16], info[64]; if (cur->dur >= 0) fmtTime(d, cur->dur);
			fmtSize(z, cur->size); snprintf(info, sizeof info, "%s  \xC2\xB7  %s", d, z);
			ui_text(160, 162, 0.5f, C_MUTED, AL_CENTER, info);
		}
		if (cur) tA = ui_button(8, 190, 100, 42, NULL, cur->dir ? "Open" : "Play", focus == 0, cur->dir ? IC_FOLDER : IC_PLAY);
		tB = ui_button(114, 190, 90, 42, NULL, "Back", focus == 1, IC_UP);
		if (atRoot) ui_round(114, 190, 90, 42, 10, C_BG & 0x99FFFFFF);       // nothing to go back to
		tY = ui_button(210, 190, 102, 42, NULL, "From PC", focus == 2, IC_DOWNLOAD);
		ui_text(312, 176, 0.42f, C_MUTED, AL_RIGHT, "START: quit");
		ui_end();

		if (kd & KEY_START) break;
		bool aOn = kd & KEY_A;                             // A presses the highlighted button
		if (((aOn && focus == 0) || tA || tPic) && cur) {
			char full[1300]; snprintf(full, sizeof full, "%s%s", path, cur->name);
			if (cur->dir) {
				if (strlen(full) < 1000) { snprintf(path, sizeof path, "%s/", full); listDir(path); sel = 0; scroll = selY = 0; thumbFor = -2; selChanged = osGetTime(); }
			} else if (play(full, cur->name)) break;
		}
		if (((kd & KEY_B) || tB || (aOn && focus == 1)) && !atRoot) {
			char *s = path + strlen(path) - 1; *s = 0; s = strrchr(path, '/'); s[1] = 0;
			listDir(path); sel = 0; scroll = selY = 0; thumbFor = -2; selChanged = osGetTime();
		}
		if ((kd & KEY_Y) || tY || (aOn && focus == 2)) {    // download from PC
			char got[300];
			if (net_download(got, sizeof got)) {
				strcpy(path, DEST_DIR); listDir(path); sel = 0;
				const char *nm = strrchr(got, '/') ? strrchr(got, '/') + 1 : got;
				for (int i = 0; i < nEnts; i++) if (!strcmp(ents[i].name, nm)) { sel = i; break; }
				scroll = selY = sel * ROW; thumbFor = -2; selChanged = 0;
			}
		}
	}
	net_exit();
	if (audioOk) { ndspExit(); linearFree(abuf); }
	free(vring); free(aring); free(ents);
	ui_exit(); romfsExit(); gfxExit();
	return 0;
}
