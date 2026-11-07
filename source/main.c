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

#define W 400
#define H 240
#define NBLK 6000
#define VRING (6u << 20)
#define ABLK_FR 32                 // ADPCM packets (64 samples each) per audio block
#define ABLK_BYTES (ABLK_FR * 68)
#define ABLK_SAMP (ABLK_FR * 64)
#define AQ 96                      // audio blocks kept read ahead (~6 s)
#define NWB 8

typedef struct { char magic[4]; u32 fpsNum, fpsDen, frames, rate, audioOff, audioBytes, keyInt, keyCount, keyOff; } Hdr;

static PrintConsole topCon, botCon;
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
static void fmtTime(char *s, u32 sec) { sprintf(s, "%lu:%02lu:%02lu", sec / 3600, sec / 60 % 60, sec % 60); }

static void present(void) {
	u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	memcpy(fb, pic, sizeof pic);
	gfxFlushBuffers(); gfxSwapBuffers();
}

static int play(const char *path, const char *name) {      // 0 back to browser, 1 quit
	consoleSelect(&botCon); consoleClear();
	fv = fopen(path, "rb"); fa = fopen(path, "rb");
	if (!fv || !fa || fread(&hd, sizeof hd, 1, fv) != 1 || memcmp(hd.magic, "N3V1", 4) || !hd.frames || !hd.fpsNum || !hd.keyCount) {
		printf("\x1b[2;1HNot a playable .n3v file.\n\n B: back");
		if (fv) fclose(fv);
		if (fa) fclose(fa);
		while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & (KEY_B | KEY_A)) break; gspWaitForVBlank(); }
		return 0;
	}
	setvbuf(fv, NULL, _IOFBF, 64 * 1024); setvbuf(fa, NULL, _IOFBF, 32 * 1024);
	keys = malloc(hd.keyCount * 4); fseek(fv, hd.keyOff, SEEK_SET); fread(keys, 4, hd.keyCount, fv);
	bool useAudio = audioOk && hd.rate;
	if (useAudio) { ndspChnReset(0); ndspChnSetInterp(0, NDSP_INTERP_LINEAR); ndspChnSetRate(0, hd.rate); ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
		float mix[12] = {1, 1}; ndspChnSetMix(0, mix); }

	gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES); gfxSetDoubleBuffering(GFX_TOP, true);
	memset(pic, 0, sizeof pic); present(); present();
	char tot[16]; fmtTime(tot, (u32)((u64)hd.frames * hd.fpsDen / hd.fpsNum));
	printf("\x1b[1;1H\x1b[37;1m%.38s\x1b[0m", name);
	printf("\x1b[14;1HA  pause      L / R  back / ahead 10 s\n Up/Down  back / ahead 1 min\n B  stop     START  quit");

	int result = 0; u32 seekTo = 0; bool paused = false, done = false;
restart:;
	u32 k = seekTo / hd.keyInt; if (k >= hd.keyCount) k = hd.keyCount - 1;
	u32 base = readerStart(k), cur = base;
	if (useAudio) audioReset();
	u64 baseSample = (u64)base * hd.fpsDen * hd.rate / hd.fpsNum / 64 * 64;
	for (int t = 0; t < 120 && (vRead < 45 && vNextFrame < hd.frames); t++) svcSleepThread(10000000);  // pre-buffer
	u64 t0 = osGetTime(), pausedAt = 0;
	if (paused) pausedAt = t0;
	if (useAudio) { audioFeed(); ndspChnSetPaused(0, paused); }
	u32 lastSec = ~0u, lates = 0;

	while (aptMainLoop()) {
		hidScanInput(); u32 kd = hidKeysDown();
		if (kd & KEY_START) { result = 1; break; }
		if (kd & KEY_B) break;
		if (kd & KEY_A) {
			paused = !paused;
			if (useAudio) ndspChnSetPaused(0, paused);
			if (paused) pausedAt = osGetTime(); else t0 += osGetTime() - pausedAt;
		}
		s32 jump = (kd & KEY_R ? 10 : 0) - (kd & KEY_L ? 10 : 0) + (kd & KEY_UP ? 60 : 0) - (kd & KEY_DOWN ? 60 : 0);
		if (jump) {
			s64 f = (s64)cur + (s64)jump * hd.fpsNum / hd.fpsDen;
			if (jump < 0) f -= hd.keyInt / 2;               // back: land a little before
			seekTo = f < 0 ? 0 : f >= hd.frames ? hd.frames - 1 : f;
			goto restart;
		}
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
		while (cur < target && budget-- > 0) { if (!decodeNext()) { lates++; break; } cur++; }
		if (useAudio) audioFeed();
		present();

		u32 sec = (u32)((u64)cur * hd.fpsDen / hd.fpsNum);
		if (sec != lastSec || (kd & KEY_A)) {
			lastSec = sec; char now[16]; fmtTime(now, sec);
			u32 bar = (u32)((u64)cur * 36 / hd.frames);
			printf("\x1b[4;1H%s / %s  %s", now, tot, paused ? "PAUSED" : "      ");
			printf("\x1b[6;1H"); for (u32 i = 0; i < 36; i++) putchar(i < bar ? '#' : '-');
			printf("\x1b[8;1Hbuffer %2lu.%lu s video   %s", (vRead - vUsed) * hd.fpsDen / hd.fpsNum,
			       (vRead - vUsed) * 10 * hd.fpsDen / hd.fpsNum % 10, useAudio ? "sound on " : "no sound ");
		}
		if (cur >= hd.frames && !done) { done = true; }
		if (done) break;
		gspWaitForVBlank();
	}
	readerStop();
	if (useAudio) audioReset();
	free(keys); fclose(fv); fclose(fa);
	if (done) svcSleepThread(500000000);
	(void)lates;
	return result;
}

// ---------- browser ----------
typedef struct { char name[256]; bool dir; } Ent;
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
		strncpy(ents[nEnts].name, e->d_name, 255); ents[nEnts].name[255] = 0; ents[nEnts].dir = dir; nEnts++;
	}
	closedir(d); qsort(ents, nEnts, sizeof(Ent), entCmp);
}

static void browserScreens(void) {
	consoleInit(GFX_TOP, &topCon); consoleInit(GFX_BOTTOM, &botCon);
	consoleSelect(&botCon);
	printf("\x1b[1;1H\x1b[37;1mN3V Player\x1b[0m\n\n Shows folders and .n3v videos.\n Make .n3v files on a PC with\n n3vconvert.py (see the GitHub page).\n\n"
	       " D-pad    move (left/right: page)\n A        open / play\n B        up a folder\n START    quit");
}

int main(void) {
	gfxInitDefault(); romfsInit(); osSetSpeedupEnable(true);
	vring = malloc(VRING); aring = malloc(AQ * ABLK_BYTES); ents = malloc(1024 * sizeof(Ent));
	audioOk = R_SUCCEEDED(ndspInit());
	if (audioOk) {
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		abuf = linearAlloc(NWB * ABLK_SAMP * 4); memset(wb, 0, sizeof wb);
		for (int i = 0; i < NWB; i++) wb[i].data_pcm16 = abuf + i * ABLK_SAMP * 2;
	}
	char path[1024] = "sdmc:/";
	{ struct stat st; if (!stat("sdmc:/videos", &st) && S_ISDIR(st.st_mode)) strcpy(path, "sdmc:/videos/"); }
	hidSetRepeatParameters(18, 4); browserScreens(); listDir(path);
	int sel = 0, top = 0; bool redraw = true;
	const int ROWS = 26;
#ifdef N3V_AUTOTEST
	for (int i = 0; i < nEnts; i++) if (!ents[i].dir) { sel = i; break; }
	int autoKeys = 90;
#endif
	while (aptMainLoop()) {
		hidScanInput(); u32 kd = hidKeysDown() | (hidKeysDownRepeat() & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT));
#ifdef N3V_AUTOTEST
		if (autoKeys > 0 && --autoKeys == 0) kd |= KEY_A;
#endif
		if (kd & KEY_START) break;
		if (nEnts) {
			if (kd & KEY_DOWN) sel = (sel + 1) % nEnts;
			if (kd & KEY_UP) sel = (sel + nEnts - 1) % nEnts;
			if (kd & KEY_RIGHT) { sel += ROWS; if (sel >= nEnts) sel = nEnts - 1; }
			if (kd & KEY_LEFT) { sel -= ROWS; if (sel < 0) sel = 0; }
		}
		if ((kd & KEY_A) && nEnts) {
			char full[1300]; snprintf(full, sizeof full, "%s%s", path, ents[sel].name);
			if (ents[sel].dir) { if (strlen(full) < 1000) { snprintf(path, sizeof path, "%s/", full); listDir(path); sel = top = 0; } }
			else { if (play(full, ents[sel].name)) break; browserScreens(); }
			redraw = true;
		}
		if ((kd & KEY_B) && strcmp(path, "sdmc:/")) {
			char *s = path + strlen(path) - 1; *s = 0; s = strrchr(path, '/'); s[1] = 0;
			listDir(path); sel = top = 0; redraw = true;
		}
		if (kd & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) redraw = true;
		if (redraw) {
			redraw = false;
			if (sel < top) top = sel;
			if (sel >= top + ROWS) top = sel - ROWS + 1;
			consoleSelect(&topCon); consoleClear();
			printf("\x1b[1;1H\x1b[36;1m%.48s\x1b[0m", path + 5);
			if (!nEnts) printf("\x1b[4;2H(no folders or .n3v files here)");
			for (int i = top; i < nEnts && i < top + ROWS; i++) {
				printf("\x1b[%d;1H%s%c %s%.44s%s\x1b[0m", 3 + i - top, i == sel ? "\x1b[47;30m" : "", i == sel ? '>' : ' ',
				       ents[i].dir ? "[" : "", ents[i].name, ents[i].dir ? "]" : "");
			}
		}
		gspWaitForVBlank();
	}
	if (audioOk) { ndspExit(); linearFree(abuf); }
	free(vring); free(aring); free(ents);
	romfsExit(); gfxExit();
	return 0;
}
