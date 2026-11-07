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

static u64 lastDraw;
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

// A plain blocking connect: the non-blocking connect + poll + SO_ERROR check worked in Azahar but reported
// failure on a real 3DS even though the PC had accepted the connection. Only reads/writes are non-blocking.
static int connectTo(u32 addr, u16 port) {
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) { lastErr = errno; return E_CONNECT; }
	struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET; sa.sin_port = htons(port); sa.sin_addr.s_addr = addr;
	if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) { lastErr = errno; close(s); return E_CONNECT; }
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
	return s;
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
	case E_CONNECT: sprintf(out, "Couldn't reach your PC (error %d).\nCheck that the 3DS's Wi-Fi is on, the\naddress is right, and the \"Send to your\n3DS\" window is open on the PC.", lastErr); break;
	case E_CANCEL: strcpy(out, "You cancelled it."); break;
	case E_HTTP: strcpy(out, "The PC answered, but not like the N3V\nconverter. Is the address right?"); break;
	case E_WRITE: strcpy(out, "Couldn't save to the SD card.\nIs it full?"); break;
	default: strcpy(out, "The connection was lost. Check the\nWi-Fi and try again.");
	}
}

static void finish(void) {                       // show the result until A / OK
#ifdef N3V_NETTEST
	FILE *f = fopen("sdmc:/n3v_net.log", "w");
	if (f) { fprintf(f, "downloaded %d of %d err %d\n", N.ok, N.nf, N.err); fclose(f); }
	for (int i = 0; i < 90; i++) netDraw();
	return;
#endif
	while (aptMainLoop()) {
		hidScanInput(); ui_input(); u32 k = hidKeysDown();
		bool tap = netDraw();
		if ((k & (KEY_A | KEY_B)) || tap) break;
	}
}

bool net_download(char *first, size_t firstLen) {
	memset(&N, 0, sizeof N); first[0] = 0;
	FILE *f = fopen(IP_FILE, "r");
	if (f) { if (fgets(N.addr, sizeof N.addr, f)) N.addr[strcspn(N.addr, "\r\n")] = 0; fclose(f); }
#ifdef N3V_NETTEST
	f = fopen("sdmc:/n3v_autoip.txt", "r");
	if (f) { if (fgets(N.addr, sizeof N.addr, f)) N.addr[strcspn(N.addr, "\r\n")] = 0; fclose(f); }
#else
	SwkbdState kb; char typed[32];
	swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, 21);
	swkbdSetNumpadKeys(&kb, '.', ':');
	swkbdSetHintText(&kb, "PC address, e.g. 192.168.1.71");
	swkbdSetInitialText(&kb, N.addr);
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (swkbdInputText(&kb, typed, sizeof typed) != SWKBD_BUTTON_CONFIRM) return false;
	strcpy(N.addr, typed);
#endif
	u32 addr; u16 port;
	if (!parseAddr(N.addr, &addr, &port)) {
		N.phase = PH_ERROR;
		snprintf(N.msg, sizeof N.msg, "\"%.24s\" isn't an address like\n192.168.1.71 (or 192.168.1.71:8691).\nType it exactly as your PC shows it.", N.addr);
		finish(); return false;
	}
	mkdir("sdmc:/3ds", 0777); mkdir(CFG_DIR, 0777);
	if ((f = fopen(IP_FILE, "w"))) { fputs(N.addr, f); fclose(f); }
	N.phase = PH_CONNECT;
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
