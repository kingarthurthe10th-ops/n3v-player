// Download from PC: the converter's "Send to 3DS" window shares .n3v files and shows the PC's address.
// Here you type that address; every shared file is downloaded into sdmc:/videos/.
// Protocol (plain HTTP): GET /n3v/list -> "N3V-LIST\n" then "size\tname\n" lines; GET /n3v/file/<index> -> the file.
#define _GNU_SOURCE          // strcasestr
#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "net.h"
#include "ui.h"

#define SHARE_PORT 8690
#define CFG_DIR "sdmc:/3ds/n3vplayer"
#define IP_FILE CFG_DIR "/pc_address.txt"
#define DEST "sdmc:/videos/"
#define MAXF 64

static u32 *socBuf; static bool socOk;

void net_exit(void) {
	if (socOk) { socExit(); socOk = false; }
	free(socBuf); socBuf = NULL;
}

static bool net_init(void) {
	if (socOk) return true;
	if (!socBuf) socBuf = memalign(0x1000, 0x100000);
	if (!socBuf) return false;
	socOk = R_SUCCEEDED(socInit(socBuf, 0x100000));
	return socOk;
}

enum { E_OK = 0, E_CONNECT = -1, E_CANCEL = -2, E_HTTP = -3, E_LOST = -4, E_WRITE = -5 };
enum { ST_WAIT, ST_NOW, ST_DONE, ST_HAD, ST_FAIL };
enum { PH_CONNECT, PH_LOAD, PH_DONE, PH_ERROR };

// ---------- what the screens show
static struct {
	char addr[32]; int phase, nf, cur, ok, err;
	char names[MAXF][256]; u64 sizes[MAXF]; char st[MAXF];
	u64 got, total, t0; char msg[220];
} N;

static void drawFile(int i, float y) {
	bool now = N.st[i] == ST_NOW;
	if (now) ui_round(8, y - 3, 384, 26, 8, C_CARD2);
	switch (N.st[i]) {
	case ST_NOW: ui_spinner(24, y + 10, 7, C_ACCENT2); break;
	case ST_DONE: case ST_HAD: icon_check(16, y + 2, 16, C_OK); break;
	case ST_FAIL: icon_cross(16, y + 2, 16, C_BAD); break;
	default: ui_circle(24, y + 10, 4, C_LINE);
	}
	char sz[24]; sprintf(sz, N.st[i] == ST_HAD ? "already here" : "%.1f MB", N.sizes[i] / 1048576.0);
	float w = ui_text(384, y + 2, 0.5f, C_MUTED, AL_RIGHT, sz);
	ui_textFit(42, y + 1, 0.55f, now ? C_TEXT : C_MUTED, AL_LEFT, 384 - 42 - w - 12, N.names[i]);
}

// draws both screens; returns true if the bottom button (Cancel or OK) was tapped
static bool netDraw(void) {
	ui_begin();
	ui_top();
	ui_rect(0, 0, 400, 30, C_HEAD);
	icon_download(10, 7, 16, C_ACCENT2);
	ui_text(34, 6, 0.62f, C_TEXT, AL_LEFT, "Download from PC");
	ui_text(392, 8, 0.5f, C_MUTED, AL_RIGHT, N.addr);
	if (N.phase == PH_CONNECT) {
		ui_spinner(200, 104, 22, C_ACCENT2);
		ui_text(200, 146, 0.65f, C_TEXT, AL_CENTER, "Connecting to your PC...");
		ui_text(200, 172, 0.5f, C_MUTED, AL_CENTER, N.addr);
	} else if (N.nf) {
		ui_text(14, 40, 0.5f, C_MUTED, AL_LEFT, "Saving into the videos folder");
		int first = N.cur > 5 ? N.cur - 5 : 0;
		for (int i = first; i < N.nf && i < first + 7; i++) drawFile(i, 64 + (i - first) * 25);
	}
	if (N.phase == PH_ERROR && !N.nf) {
		ui_circle(200, 104, 26, C_BAD); icon_cross(186, 90, 28, C_TEXT);
		ui_text(200, 146, 0.65f, C_TEXT, AL_CENTER, "Couldn't download");
	}

	ui_bottom();
	bool tapped = false;
	ui_round(12, 12, 296, 166, 12, C_CARD);
	if (N.phase == PH_CONNECT) {
		ui_text(26, 26, 0.6f, C_ACCENT2, AL_LEFT, "Connecting...");
		ui_text(26, 56, 0.5f, C_MUTED, AL_LEFT, "Keep the \"Send to your 3DS\" window\nopen on your PC.");
		tapped = ui_button(100, 190, 120, 42, NULL, "Cancel", true, IC_NONE);
	} else if (N.phase == PH_LOAD) {
		double secs = (osGetTime() - N.t0) / 1000.0, kbs = secs > 0.3 ? N.got / 1024.0 / secs : 0;
		char l1[64], l2[64];
		sprintf(l1, "File %d of %d", N.cur + 1, N.nf);
		ui_text(26, 24, 0.5f, C_MUTED, AL_LEFT, l1);
		ui_textFit(26, 44, 0.62f, C_TEXT, AL_LEFT, 268, N.names[N.cur]);
		float f = N.total ? (float)N.got / N.total : 0;
		ui_round(26, 84, 268, 14, 7, C_CARD2);
		ui_round(26, 84, 268 * f < 14 ? 14 : 268 * f, 14, 7, C_ACCENT);
		sprintf(l1, "%.1f of %.1f MB", N.got / 1048576.0, N.total / 1048576.0);
		ui_text(26, 106, 0.55f, C_TEXT, AL_LEFT, l1);
		ui_text(294, 106, 0.55f, C_ACCENT2, AL_RIGHT, N.total ? (sprintf(l2, "%d%%", (int)(f * 100)), l2) : "");
		if (kbs > 1) {
			u32 left = N.total > N.got ? (u32)((N.total - N.got) / 1024.0 / kbs) : 0;
			sprintf(l2, "%.0f KB/s  \xC2\xB7  about %lu:%02lu left", kbs, left / 60, left % 60);
			ui_text(26, 132, 0.5f, C_MUTED, AL_LEFT, l2);
		}
		tapped = ui_button(100, 190, 120, 42, NULL, "Cancel", true, IC_NONE);
	} else {
		bool good = N.phase == PH_DONE && N.ok == N.nf;
		ui_circle(44, 46, 18, good ? C_OK : N.ok ? C_ACCENT : C_BAD);
		if (good || N.ok) icon_check(34, 36, 20, C_TEXT); else icon_cross(34, 36, 20, C_TEXT);
		char t[64];
		if (N.nf) sprintf(t, "Downloaded %d of %d", N.ok, N.nf); else strcpy(t, "Couldn't download");
		ui_text(72, 34, 0.65f, C_TEXT, AL_LEFT, t);
		ui_text(26, 74, 0.5f, good ? C_MUTED : C_TEXT, AL_LEFT, N.msg);
		tapped = ui_button(100, 190, 120, 42, NULL, "OK", true, IC_NONE);
	}
	ui_end();
	return tapped;
}

static u64 lastDraw, tStart;
static void redraw(bool force) {
	if (!force && osGetTime() - lastDraw < 40) return;
	lastDraw = osGetTime(); netDraw();
#ifdef N3V_SHOTS
	static bool shot;
	if (!shot && N.phase == PH_LOAD && N.total && N.got * 2 >= N.total) { shot = true; netDraw(); ui_shot("4_download", NULL); }
#endif
}

static bool cancelPressed(void) {                // B, or Cancel tapped
	hidScanInput(); ui_input();
	return (hidKeysDown() & KEY_B) || ui_tapped(100, 190, 120, 42);
}

// ---------- HTTP
static int lastErr;

// Connecting: a plain blocking connect (the non-blocking connect + poll + SO_ERROR check reported failure on a
// real 3DS even though the PC had accepted). But when nothing listens, Windows Firewall ignores the request and a
// blocking connect waits a minute or more - the app froze. So the blocking connect runs in its own thread while
// the screen keeps drawing; after 6 s (or Cancel) we give up and leave the thread to finish and clean up alone.
typedef struct { LightLock lock; u32 addr; u16 port; int state, sock, err; bool abandoned; } ConnJob;   // state 0 waiting, 1 ok, 2 failed

static void connThread(void *arg) {
	ConnJob *j = arg; int err = 0;
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) err = errno;
	else {
		struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
		sa.sin_family = AF_INET; sa.sin_port = htons(j->port); sa.sin_addr.s_addr = j->addr;
		if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) { err = errno ? errno : -1; close(s); s = -1; }
	}
	LightLock_Lock(&j->lock);
	if (j->abandoned) {                              // nobody is waiting any more: tidy up and go
		LightLock_Unlock(&j->lock);
		if (s >= 0) close(s);
		free(j); return;
	}
	j->sock = s; j->err = err; j->state = s >= 0 ? 1 : 2;
	LightLock_Unlock(&j->lock);
}

static int connectTo(u32 addr, u16 port) {
	ConnJob *j = calloc(1, sizeof *j);
	if (!j) { lastErr = ENOMEM; return E_CONNECT; }
	LightLock_Init(&j->lock); j->addr = addr; j->port = port;
	s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	if (!threadCreate(connThread, j, 16 * 1024, prio + 1, -2, true)) { free(j); lastErr = EAGAIN; return E_CONNECT; }
	u64 t0 = osGetTime();
	while (1) {
		bool cancel = cancelPressed(), late = osGetTime() - t0 > 6000;
		LightLock_Lock(&j->lock);
		int st = j->state, s = j->sock, err = j->err;
		if (!st && (cancel || late)) j->abandoned = true;
		LightLock_Unlock(&j->lock);
		if (st == 1) { free(j); fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); return s; }
		if (st == 2) { free(j); lastErr = err; return E_CONNECT; }
		if (cancel) return E_CANCEL;                  // (the thread frees j)
		if (late) { lastErr = ETIMEDOUT; return E_CONNECT; }
		redraw(false); svcSleepThread(20000000);
	}
}

static int sendAll(int s, const char *b, int n) {
	while (n > 0) {
		int r = send(s, b, n, 0);
		if (r > 0) { b += r; n -= r; continue; }
		if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return E_LOST;
		struct pollfd p = { s, POLLOUT, 0 }; poll(&p, 1, 50);
		if (cancelPressed()) return E_CANCEL;
	}
	return E_OK;
}

static int recvSome(int s, void *b, int n) {      // >0 bytes, 0 closed, <0 error/cancel/15 s timeout
	for (int t = 0; t < 300; t++) {
		int r = recv(s, b, n, 0);
		if (r >= 0) return r;
		if (errno != EAGAIN && errno != EWOULDBLOCK) return E_LOST;
		struct pollfd p = { s, POLLIN, 0 }; poll(&p, 1, 50);
		if (cancelPressed()) return E_CANCEL;
		redraw(false);
	}
	return E_LOST;
}

typedef struct { char *mem; u32 memLen, memCap; FILE *f; } Sink;

static int httpGet(u32 addr, u16 port, const char *path, Sink *k) {
	int s = connectTo(addr, port);
	if (s < 0) return s;
	char req[256]; int rl = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: n3v\r\nUser-Agent: N3VPlayer\r\n\r\n", path);
	int rc = sendAll(s, req, rl);
	static char buf[65536]; int have = 0; char *body = NULL;
	while (rc == E_OK && !body) {                        // headers
		int r = recvSome(s, buf + have, sizeof buf - 1 - have);
		if (r <= 0) { rc = r ? r : E_HTTP; break; }
		have += r; buf[have] = 0;
		char *e = strstr(buf, "\r\n\r\n");
		if (e) body = e + 4; else if (have >= (int)sizeof buf - 1) rc = E_HTTP;
	}
	if (rc == E_OK && (strncmp(buf, "HTTP/1.", 7) || strncmp(buf + 9, "200", 3))) rc = E_HTTP;
	if (rc == E_OK) {
		char *cl = strcasestr(buf, "\r\nContent-Length:");
		N.total = cl && cl < body ? strtoull(cl + 17, NULL, 10) : 0;
		N.got = 0; N.t0 = osGetTime();
		int n = have - (int)(body - buf); char *p = body;
		while (1) {
			if (n > 0) {
				if (k->f) { if (fwrite(p, 1, n, k->f) != (size_t)n) { rc = E_WRITE; break; } }
				else {
					if (k->memLen + n + 1 > k->memCap) { k->memCap = (k->memLen + n + 1) * 2; k->mem = realloc(k->mem, k->memCap); }
					memcpy(k->mem + k->memLen, p, n); k->memLen += n; k->mem[k->memLen] = 0;
				}
				N.got += n;
				if (k->f) redraw(false);
			}
			if (N.total && N.got >= N.total) break;
			n = recvSome(s, buf, sizeof buf); p = buf;
			if (n == 0) { if (N.total && N.got < N.total) rc = E_LOST; break; }
			if (n < 0) { rc = n; break; }
		}
	}
	close(s);
	return rc;
}

static bool parseAddr(const char *in, u32 *addr, u16 *port) {
	char ip[32]; strncpy(ip, in, 31); ip[31] = 0;
	*port = SHARE_PORT;
	char *c = strchr(ip, ':');
	if (c) { *c = 0; int p = atoi(c + 1); if (p <= 0 || p > 65535) return false; *port = p; }
	int dots = 0; for (char *q = ip; *q; q++) dots += *q == '.';
	if (dots != 3) return false;
	*addr = inet_addr(ip);
	return *addr != INADDR_NONE && *addr != 0;
}

static void cleanName(char *s) {
	for (; *s; s++) if (strchr("/\\:*?\"<>|", *s) || (unsigned char)*s < 32) *s = '_';
}

static void errText(int e, char *out) {
	switch (e) {
	case E_CONNECT:
		if (lastErr == ETIMEDOUT) { strcpy(out, "Your PC didn't answer. Is the \"Send to\nyour 3DS\" window open on it, and is this\nthe address it shows? Both need the\nsame Wi-Fi."); break; }
		sprintf(out, "Couldn't reach your PC (error %d).\nCheck that the 3DS's Wi-Fi is on, the\naddress is right, and the \"Send to your\n3DS\" window is open on the PC.", lastErr); break;
	case E_CANCEL: strcpy(out, "You cancelled it."); break;
	case E_HTTP: strcpy(out, "The PC answered, but not like the N3V\nconverter. Is the address right?"); break;
	case E_WRITE: strcpy(out, "Couldn't save to the SD card.\nIs it full?"); break;
	default: strcpy(out, "The connection was lost. Check the\nWi-Fi and try again.");
	}
}

static void finish(void) {                       // show the result until A / OK
#ifdef N3V_NETTEST
	FILE *f = fopen("sdmc:/n3v_net.log", "w");
	if (f) { fprintf(f, "downloaded %d of %d err %d lastErr %d after %lu ms\n", N.ok, N.nf, N.err, lastErr, (u32)(osGetTime() - tStart)); fclose(f); }
	for (int i = 0; i < 90; i++) netDraw();
	return;
#endif
	while (aptMainLoop()) {
		hidScanInput(); ui_input(); u32 k = hidKeysDown();
		bool tap = netDraw();
		if ((k & (KEY_A | KEY_B)) || tap) break;
	}
}

// ---------- saved PCs: the first time you type an address; after that you pick one (or "Add new PC address")
#define PCS_FILE CFG_DIR "/pcs.txt"
#define MAXPC 8
static char pcs[MAXPC][32]; static int nPcs;

static void loadPcs(void) {
	nPcs = 0;
	FILE *f = fopen(PCS_FILE, "r");
	if (!f) f = fopen(IP_FILE, "r");                 // (1.3 kept just the last address)
	if (!f) return;
	char line[64];
	while (nPcs < MAXPC && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0]) { strncpy(pcs[nPcs], line, 31); pcs[nPcs][31] = 0; nPcs++; }
	}
	fclose(f);
}

static void savePcs(void) {
	mkdir("sdmc:/3ds", 0777); mkdir(CFG_DIR, 0777);
	FILE *f = fopen(PCS_FILE, "w"); if (!f) return;
	for (int i = 0; i < nPcs; i++) fprintf(f, "%s\n", pcs[i]);
	fclose(f);
}

static void rememberPc(const char *a) {           // move (or add) it to the top of the list
	loadPcs();
	char keep[MAXPC][32]; int n = 0;
	strncpy(keep[n++], a, 31); keep[0][31] = 0;
	for (int i = 0; i < nPcs && n < MAXPC; i++) if (strcmp(pcs[i], a)) strcpy(keep[n++], pcs[i]);
	memcpy(pcs, keep, sizeof keep); nPcs = n; savePcs();
}

static bool typeAddress(char *out) {
	SwkbdState kb; char typed[32];
	swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, 21);
	swkbdSetNumpadKeys(&kb, '.', ':');
	swkbdSetHintText(&kb, "PC address, e.g. 192.168.1.71");
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (swkbdInputText(&kb, typed, sizeof typed) != SWKBD_BUTTON_CONFIRM) return false;
	strcpy(out, typed); return true;
}

static void icon_pc(float x, float y, float s, u32 c) {
	ui_round(x, y + s * 0.08f, s, s * 0.62f, s * 0.1f, c);
	ui_rect(x + s * 0.08f, y + s * 0.16f, s * 0.84f, s * 0.46f, C_BG);
	ui_rect(x + s * 0.42f, y + s * 0.7f, s * 0.16f, s * 0.14f, c);
	ui_round(x + s * 0.25f, y + s * 0.84f, s * 0.5f, s * 0.1f, s * 0.05f, c);
}

static bool pickAddress(char *out) {
	loadPcs();
	if (!nPcs) return typeAddress(out);              // the first time: just ask
	int sel = 1, focus = 0;                          // row 0 is "Add new PC address"
	float selY = 32;
	while (aptMainLoop()) {
		hidScanInput(); ui_input();
		u32 kd = hidKeysDown() | (hidKeysDownRepeat() & (KEY_UP | KEY_DOWN));
		int rows = nPcs + 1;
		if (kd & KEY_DOWN) { sel = (sel + 1) % rows; focus = 0; }
		if (kd & KEY_UP) { sel = (sel + rows - 1) % rows; focus = 0; }
		int nBtn = sel ? 3 : 2;                         // Connect / Remove / Back, or Add / Back
		if (kd & KEY_RIGHT) focus = (focus + 1) % nBtn;
		if (kd & KEY_LEFT) focus = (focus + nBtn - 1) % nBtn;
		selY += (sel * 32 - selY) * 0.4f;

		ui_begin();
		ui_top();
		ui_rect(0, 0, 400, 30, C_HEAD);
		icon_download(10, 7, 16, C_ACCENT2);
		ui_text(34, 6, 0.62f, C_TEXT, AL_LEFT, "Download from PC");
		ui_text(392, 8, 0.5f, C_MUTED, AL_RIGHT, "Choose your PC");
		ui_round(6, 40 + selY, 388, 30, 9, C_ACCENT);
		for (int i = 0; i < rows; i++) {
			float y = 40 + i * 32; bool s = i == sel;
			if (!i) {
				icon_pc(16, y + 6, 18, s ? C_TEXT : C_MUTED);
				ui_text(44, y + 6, 0.58f, C_TEXT, AL_LEFT, "Add new PC address");
			} else {
				icon_pc(16, y + 6, 18, s ? C_TEXT : C_ACCENT2);
				ui_text(44, y + 6, 0.58f, C_TEXT, AL_LEFT, pcs[i - 1]);
				if (i == 1) ui_text(384, y + 8, 0.5f, s ? C_TEXT : C_MUTED, AL_RIGHT, "last used");
			}
		}
		ui_bottom();
		ui_round(12, 12, 296, 160, 12, C_CARD);
		bool t0, t1, t2 = false;
		if (!sel) {
			ui_text(26, 24, 0.62f, C_ACCENT2, AL_LEFT, "Add a new PC");
			ui_text(26, 54, 0.52f, C_TEXT, AL_LEFT, "Type the address shown in the\n\"Send to your 3DS\" window on the PC.\nIt's saved here for next time.");
			t0 = ui_button(30, 190, 130, 42, NULL, "Add", focus == 0, IC_NONE);
			t1 = ui_button(170, 190, 120, 42, NULL, "Back", focus == 1, IC_NONE);
		} else {
			icon_pc(134, 26, 52, C_ACCENT2);
			ui_text(160, 92, 0.72f, C_TEXT, AL_CENTER, pcs[sel - 1]);
			ui_text(160, 124, 0.5f, C_MUTED, AL_CENTER, "Saved PC address");
			t0 = ui_button(8, 190, 112, 42, NULL, "Connect", focus == 0, IC_DOWNLOAD);
			t1 = ui_button(126, 190, 90, 42, NULL, "Remove", focus == 1, IC_NONE);
			t2 = ui_button(222, 190, 90, 42, NULL, "Back", focus == 2, IC_NONE);
		}
		ui_end();
#ifdef N3V_PICKTEST
		{ static int fr; fr++;
		  if (fr == 40) ui_shot("5_choose_pc", NULL);
		  if (fr == 50) sel = 0;
		  if (fr == 90) ui_shot("6_add_pc", NULL);
		  if (fr == 100) return false; }
#endif

		bool a = kd & KEY_A;
		int act = a ? focus : t0 ? 0 : t1 ? 1 : t2 ? 2 : -1;
		if ((kd & KEY_B) || (!sel && act == 1) || (sel && act == 2)) return false;
		if (act == 0) {
			if (!sel) return typeAddress(out);
			strcpy(out, pcs[sel - 1]); return true;
		}
		if (sel && act == 1) {                         // Remove
			for (int i = sel - 1; i < nPcs - 1; i++) strcpy(pcs[i], pcs[i + 1]);
			nPcs--; savePcs(); remove(IP_FILE);
			if (sel > nPcs) sel = nPcs;
			focus = 0;
		}
	}
	return false;
}

bool net_download(char *first, size_t firstLen) {
	memset(&N, 0, sizeof N); first[0] = 0;
#ifdef N3V_NETTEST
	FILE *f = fopen("sdmc:/n3v_autoip.txt", "r");
	if (f) { if (fgets(N.addr, sizeof N.addr, f)) N.addr[strcspn(N.addr, "\r\n")] = 0; fclose(f); }
#else
	if (!pickAddress(N.addr)) return false;
#endif
	u32 addr; u16 port;
	if (!parseAddr(N.addr, &addr, &port)) {
		N.phase = PH_ERROR;
		snprintf(N.msg, sizeof N.msg, "\"%.24s\" isn't an address like\n192.168.1.71 (or 192.168.1.71:8691).\nType it exactly as your PC shows it.", N.addr);
		finish(); return false;
	}
	rememberPc(N.addr);
	N.phase = PH_CONNECT; tStart = osGetTime();
	netDraw(); netDraw();                         // (both buffers) - the connect below can take a few seconds
	if (!net_init()) { N.phase = PH_ERROR; strcpy(N.msg, "Couldn't start the 3DS's networking."); finish(); return false; }

	Sink k; memset(&k, 0, sizeof k);
	int rc = httpGet(addr, port, "/n3v/list", &k);
	if (rc == E_OK && (!k.mem || strncmp(k.mem, "N3V-LIST\n", 9))) rc = E_HTTP;
	if (rc != E_OK) { free(k.mem); N.phase = PH_ERROR; N.err = rc; errText(rc, N.msg); finish(); return false; }
	for (char *line = k.mem + 9; *line && N.nf < MAXF;) {
		char *end = strchr(line, '\n'); if (end) *end = 0;
		char *tab = strchr(line, '\t');
		if (tab) { N.sizes[N.nf] = strtoull(line, NULL, 10); strncpy(N.names[N.nf], tab + 1, 255); cleanName(N.names[N.nf]); N.nf++; }
		if (!end) break;
		line = end + 1;
	}
	free(k.mem);
	if (!N.nf) { N.phase = PH_ERROR; strcpy(N.msg, "Your PC isn't sharing any videos.\nConvert one and choose \"Yes, send it\"."); finish(); return false; }

	mkdir("sdmc:/videos", 0777);
	N.phase = PH_LOAD; rc = E_OK;
	for (int i = 0; i < N.nf && rc != E_CANCEL; i++) {
		char path[300], part[310], url[32];
		snprintf(path, sizeof path, DEST "%s", N.names[i]); snprintf(part, sizeof part, "%s.part", path);
		N.cur = i; N.st[i] = ST_NOW; N.got = 0; N.total = N.sizes[i]; N.t0 = osGetTime();
		struct stat st;
		if (!stat(path, &st) && (u64)st.st_size == N.sizes[i]) {             // already on the SD card
			N.st[i] = ST_HAD; N.ok++;
			if (!first[0]) snprintf(first, firstLen, "%s", path);
			continue;
		}
		redraw(true);
		memset(&k, 0, sizeof k);
		k.f = fopen(part, "wb");
		if (!k.f) { rc = E_WRITE; N.st[i] = ST_FAIL; break; }
		setvbuf(k.f, NULL, _IOFBF, 128 * 1024);
		snprintf(url, sizeof url, "/n3v/file/%d", i);
		rc = httpGet(addr, port, url, &k);
		if (fclose(k.f) && rc == E_OK) rc = E_WRITE;
		if (rc == E_OK) {
			remove(path); rename(part, path); N.ok++; N.st[i] = ST_DONE;
			if (!first[0]) snprintf(first, firstLen, "%s", path);
		} else {
			remove(part); N.st[i] = ST_FAIL;
			if (rc == E_CANCEL || rc == E_CONNECT) break;
		}
	}
	N.phase = PH_DONE; N.err = rc;
	if (rc != E_OK) errText(rc, N.msg);
	else strcpy(N.msg, "They're in the videos folder.\nPress OK to see them in the list.");
	finish();
	return N.ok > 0;
}
