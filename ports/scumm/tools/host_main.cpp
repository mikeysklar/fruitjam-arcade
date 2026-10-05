/* fruitjam-scumm - SCUMM v3/v4 on the Adafruit Fruit Jam
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Host harness. Runs a game folder headless through the same fj_core
 * interface the board uses, with a 320x240 RGB565 framebuffer, and writes
 * PNG screenshots and a WAV of the audio.
 *
 *   scumm_host GAMEDIR [-n FRAMES] [-s F1,F2,...] [-o OUTDIR]
 *              [-c FRAME:X,Y[:r]]... [-k FRAME:KEY]... [-m MUSIC] [-w out.wav]
 *              [-S FRAME:SLOT] [-L FRAME:SLOT] [-a ARENA_KB] [-C key=value]...
 *
 * -c clicks the left (or :r right) button at X,Y on FRAME (held 3 frames),
 * -k presses a key (number, or esc, f1..f9, space, or a single character).
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include <vector>
#include <string>

#include "backend/fj_core.h"

// After fj_core_init the engine's allocator owns operator new (see
// src/libc/fj_cxx.cpp), so nothing below allocates once the game runs.
static char gameDir[512];
static FILE *handles[32];

static bool findFile(const char *name, char *out, size_t size) {
	DIR *d = opendir(gameDir);
	bool found = false;
	if (!d)
		return false;
	while (struct dirent *e = readdir(d)) {
		if (!strcasecmp(e->d_name, name)) {
			snprintf(out, size, "%s/%s", gameDir, e->d_name);
			found = true;
			break;
		}
	}
	closedir(d);
	return found;
}

static int ioOpen(void *ctx, const char *name, int write) {
	char path[1024];
	if (!findFile(name, path, sizeof(path))) {
		if (!write)
			return -1;
		snprintf(path, sizeof(path), "%s/%s", gameDir, name);
	}
	FILE *f = fopen(path, write ? "wb" : "rb");
	if (!f)
		return -1;
	for (int i = 0; i < 32; i++) {
		if (!handles[i]) {
			handles[i] = f;
			return i;
		}
	}
	fclose(f);
	return -1;
}

static int ioRead(void *ctx, int h, void *buf, int len) {
	return (int)fread(buf, 1, len, handles[h]);
}

static int ioWrite(void *ctx, int h, const void *buf, int len) {
	return (int)fwrite(buf, 1, len, handles[h]);
}

static int ioSeek(void *ctx, int h, int32_t pos) {
	return fseek(handles[h], pos, SEEK_SET);
}

static int32_t ioSize(void *ctx, int h) {
	long cur = ftell(handles[h]);
	fseek(handles[h], 0, SEEK_END);
	long n = ftell(handles[h]);
	fseek(handles[h], cur, SEEK_SET);
	return (int32_t)n;
}

static void ioClose(void *ctx, int h) {
	fclose(handles[h]);
	handles[h] = 0;
}

static void ioLog(void *ctx, const char *msg) {
	fprintf(stderr, "%s%s", msg, msg[0] && msg[strlen(msg) - 1] == '\n' ? "" : "\n");
}

// ---- PNG writer (stored deflate, no zlib) ----

static uint32_t crcTable[256];

static uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xffffffffu) {
	if (!crcTable[1]) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t v = i;
			for (int k = 0; k < 8; k++)
				v = (v & 1) ? 0xedb88320u ^ (v >> 1) : v >> 1;
			crcTable[i] = v;
		}
	}
	for (size_t i = 0; i < n; i++)
		c = crcTable[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c;
}

// A byte buffer on the system allocator (std::vector would take the
// engine's arena once the game runs).
struct Buf {
	uint8_t *p;
	size_t n, cap;
	Buf() : p(0), n(0), cap(0) {}
	~Buf() { free(p); }
	void push_back(uint8_t b) {
		if (n == cap) {
			cap = cap ? cap * 2 : 4096;
			p = (uint8_t *)realloc(p, cap);
		}
		p[n++] = b;
	}
	void append(const uint8_t *src, size_t len) {
		for (size_t i = 0; i < len; i++)
			push_back(src[i]);
	}
	size_t size() const { return n; }
	uint8_t &operator[](size_t i) { return p[i]; }
	const uint8_t *data() const { return p; }
};

static void put32(Buf &v, uint32_t x) {
	v.push_back(x >> 24);
	v.push_back(x >> 16);
	v.push_back(x >> 8);
	v.push_back(x);
}

static void chunk(FILE *f, const char *type, const Buf &data) {
	Buf v;
	put32(v, (uint32_t)data.size());
	size_t start = v.size();
	v.append((const uint8_t *)type, 4);
	v.append(data.data(), data.size());
	put32(v, crc(&v[start], v.size() - start) ^ 0xffffffffu);
	fwrite(v.data(), 1, v.size(), f);
}

static void writePNG(const char *path, const uint16_t *fb, int w, int h) {
	Buf raw;
	for (int y = 0; y < h; y++) {
		raw.push_back(0);
		for (int x = 0; x < w; x++) {
			uint16_t p = fb[y * w + x];
			raw.push_back(((p >> 11) & 31) * 255 / 31);
			raw.push_back(((p >> 5) & 63) * 255 / 63);
			raw.push_back((p & 31) * 255 / 31);
		}
	}
	Buf z;
	z.push_back(0x78);
	z.push_back(0x01);
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < raw.size(); i++) {
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	for (size_t i = 0; i < raw.size(); i += 65535) {
		size_t n = raw.size() - i < 65535 ? raw.size() - i : 65535;
		z.push_back(i + n == raw.size());
		z.push_back(n & 0xff);
		z.push_back(n >> 8);
		z.push_back(~n & 0xff);
		z.push_back((~n >> 8) & 0xff);
		z.append(raw.data() + i, n);
	}
	put32(z, (b << 16) | a);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
	fwrite(sig, 1, 8, f);
	Buf ihdr;
	put32(ihdr, w);
	put32(ihdr, h);
	ihdr.push_back(8);
	ihdr.push_back(2);
	ihdr.push_back(0);
	ihdr.push_back(0);
	ihdr.push_back(0);
	chunk(f, "IHDR", ihdr);
	chunk(f, "IDAT", z);
	chunk(f, "IEND", Buf());
	fclose(f);
}

// ---- WAV, streamed, header patched at the end ----

static FILE *wavOpen(const char *path) {
	FILE *f = fopen(path, "wb");
	if (f) {
		static const uint8_t blank[44] = {0};
		fwrite(blank, 1, 44, f);
	}
	return f;
}

static void wavClose(FILE *f) {
	uint32_t bytes = (uint32_t)ftell(f) - 44;
	uint32_t rate = FJ_AUDIO_RATE;
	uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
	                   0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
	uint32_t riff = 36 + bytes, br = rate * 4;
	memcpy(hdr + 4, &riff, 4);
	memcpy(hdr + 24, &rate, 4);
	memcpy(hdr + 28, &br, 4);
	memcpy(hdr + 40, &bytes, 4);
	fseek(f, 0, SEEK_SET);
	fwrite(hdr, 1, 44, f);
	fclose(f);
}

// ---- main ----

struct Click {
	int frame, x, y, right;
};
struct Key {
	int frame, code, ascii;
};

static void parseKey(const char *s, Key &k) {
	k.ascii = 0;
	if (!strcasecmp(s, "esc")) {
		k.code = FJ_KEY_ESCAPE;
		k.ascii = 27;
	} else if (!strcasecmp(s, "space")) {
		k.code = k.ascii = ' ';
	} else if (!strcasecmp(s, "enter")) {
		k.code = k.ascii = 13;
	} else if ((s[0] == 'f' || s[0] == 'F') && s[1] >= '1' && s[1] <= '9' && !s[2]) {
		k.code = FJ_KEY_F1 + s[1] - '1';
		k.ascii = 315 + s[1] - '1';
	} else if (s[0] && !s[1]) {
		k.code = k.ascii = s[0];
	} else {
		k.code = k.ascii = atoi(s);
	}
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s GAMEDIR [-n frames] [-s f1,f2] [-o outdir] [-c f:x,y[:r]] [-k f:key] [-m music] [-w wav]\n", argv[0]);
		return 2;
	}
	snprintf(gameDir, sizeof(gameDir), "%s", argv[1]);
	int frames = 600;
	std::vector<int> shots;
	std::vector<Click> clicks;
	std::vector<Key> keys;
	std::vector<std::pair<int, int> > saves, loads;
	const char *outDir = ".";
	std::vector<const char *> configs; // -C key=value
	const char *music = 0;
	const char *wav = 0;
	int arenaKB = 4096;
	int every = 0;
	for (int i = 2; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : "";
		if (!strcmp(a, "-n")) {
			frames = atoi(v);
			i++;
		} else if (!strcmp(a, "-s")) {
			for (const char *p = v; *p;) {
				shots.push_back(atoi(p));
				const char *c = strchr(p, ',');
				if (!c)
					break;
				p = c + 1;
			}
			i++;
		} else if (!strcmp(a, "-e")) {
			every = atoi(v);
			i++;
		} else if (!strcmp(a, "-o")) {
			outDir = v;
			i++;
		} else if (!strcmp(a, "-c")) {
			Click c = {0, 0, 0, 0};
			char r = 0;
			sscanf(v, "%d:%d,%d:%c", &c.frame, &c.x, &c.y, &r);
			c.right = r == 'r';
			clicks.push_back(c);
			i++;
		} else if (!strcmp(a, "-k")) {
			Key k;
			k.frame = atoi(v);
			const char *c = strchr(v, ':');
			parseKey(c ? c + 1 : "", k);
			keys.push_back(k);
			i++;
		} else if (!strcmp(a, "-S") || !strcmp(a, "-L")) {
			int f = 0, s = 0;
			sscanf(v, "%d:%d", &f, &s);
			(a[1] == 'S' ? saves : loads).push_back(std::make_pair(f, s));
			i++;
		} else if (!strcmp(a, "-m")) {
			music = v;
			i++;
		} else if (!strcmp(a, "-w")) {
			wav = v;
			i++;
		} else if (!strcmp(a, "-C")) {
			configs.push_back(v);
			i++;
		} else if (!strcmp(a, "-a")) {
			arenaKB = atoi(v);
			i++;
		}
	}

	static fj_io io = {ioOpen, ioRead, ioWrite, ioSeek, ioSize, ioClose, ioLog, 0};
	size_t arenaSize = (size_t)arenaKB * 1024;
	uint8_t *arena = (uint8_t *)malloc(arenaSize);
	int err = fj_core_init(arena, arenaSize, &io);
	if (err) {
		fprintf(stderr, "init failed (%d): %s\n", err, fj_core_error());
		return 1;
	}
	fprintf(stderr, "game: %s\n", fj_core_game());
	if (music)
		fj_core_config("music", music);
	for (size_t i = 0; i < configs.size(); i++) {
		char key[64];
		const char *eq = strchr(configs[i], '=');
		if (!eq || eq - configs[i] >= (int)sizeof(key))
			continue;
		memcpy(key, configs[i], eq - configs[i]);
		key[eq - configs[i]] = 0;
		fj_core_config(key, eq + 1);
	}

	static uint16_t fb[320 * 240];
	fj_core_set_output(fb, 320, 240, 16, 0, 20);
	const int ringFrames = 4096;
	static int16_t ring[ringFrames * 2];
	fj_core_set_audio(ring, ringFrames, 0);
	FILE *wavFile = wav ? wavOpen(wav) : 0;
	uint32_t lastSamples = 0;
	int ringRead = 0;

	int mx = 160, my = 100, btn = 0, btnUntil = -1;
	struct timeval t0, t1;
	gettimeofday(&t0, 0);
	int f;
	for (f = 1; f <= frames; f++) {
		for (size_t i = 0; i < clicks.size(); i++) {
			if (clicks[i].frame == f) {
				mx = clicks[i].x;
				my = clicks[i].y;
				btn = clicks[i].right ? FJ_BTN_RIGHT : FJ_BTN_LEFT;
				btnUntil = f + 3;
			}
		}
		if (f == btnUntil)
			btn = 0;
		for (size_t i = 0; i < keys.size(); i++) {
			if (keys[i].frame == f)
				fj_core_key(keys[i].code, keys[i].ascii);
		}
		for (size_t i = 0; i < saves.size(); i++) {
			if (saves[i].first == f)
				fj_core_save(saves[i].second);
		}
		for (size_t i = 0; i < loads.size(); i++) {
			if (loads[i].first == f)
				fj_core_load(loads[i].second);
		}
		fj_core_input(mx, my, btn);
		int alive = fj_core_frame();

		uint32_t s = fj_core_samples();
		for (uint32_t k = lastSamples; k < s; k++) {
			if (wavFile)
				fwrite(&ring[ringRead * 2], 2, 2, wavFile);
			ringRead = (ringRead + 1) % ringFrames;
		}
		lastSamples = s;

		bool shoot = every && f % every == 0;
		for (size_t i = 0; i < shots.size(); i++)
			shoot |= shots[i] == f;
		if (shoot) {
			char path[512];
			snprintf(path, sizeof(path), "%s/frame%05d.png", outDir, f);
			writePNG(path, fb, 320, 240);
		}
		if (!alive) {
			fprintf(stderr, "stopped at frame %d: %s\n", f, fj_core_error());
			break;
		}
	}
	gettimeofday(&t1, 0);
	double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0;
	fprintf(stderr, "%d frames in %.0f ms (%.2f ms/frame)\n", f - 1, ms, ms / (f - 1 > 0 ? f - 1 : 1));
	if (wavFile)
		wavClose(wavFile);
	fprintf(stderr, "arena: %zu bytes in use, peak %zu; engine stack %zu of %zu\n", fj_core_mem_used(),
	        fj_core_mem_peak(), fj_core_stack_used(), fj_core_stack_size());
	char path[512];
	snprintf(path, sizeof(path), "%s/last.png", outDir);
	writePNG(path, fb, 320, 240);
	return 0;
}
