/* Host test: the IMA ADPCM WAV decoder for Loom PC-Engine CD tracks
 * (src/scumm/fj_cdtrack.cpp) gives the same samples as ffmpeg's decoder,
 * read in random sized pieces, and stops after a CD-frame duration.
 *
 * usage: build/test_cdtrack IN.wav REF.s16 (REF = ffmpeg's decode of IN).
 * Built and run by `make -f Makefile.host test`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/memstream.h"
#include "audio/audiostream.h"
#include "scumm/fj_cdtrack.h"

#include "backend/fj_alloc.h"

static byte *slurp(const char *name, long *len) {
	FILE *f = fopen(name, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	byte *d = (byte *)malloc(*len);
	fread(d, 1, *len, f);
	fclose(f);
	return d;
}

int main(int argc, char **argv) {
	long wlen, rlen;
	byte *w = slurp(argv[1], &wlen);
	int16 *ref = (int16 *)slurp(argv[2], &rlen);
	if (!w || !ref) {
		printf("test_cdtrack: missing input\n");
		return 1;
	}
	long nref = rlen / 2;
	// The engine allocates from the arena, as on the board.
	static uint32 arena[64 * 1024];
	fj_alloc_init(arena, sizeof(arena));

	srand(3);
	Audio::AudioStream *st = Scumm::makeFjCDTrackStream(new Common::MemoryReadStream(w, wlen), 0);
	if (!st) {
		printf("test_cdtrack: not recognised\n");
		return 1;
	}
	int ch = st->isStereo() ? 2 : 1;
	int16 *out = (int16 *)malloc(rlen + 65536);
	long n = 0;
	while (!st->endOfData()) {
		int want = ch * (1 + rand() % 3000);
		n += st->readBuffer(out + n, want);
	}
	delete st;
	// ffmpeg drops the padding of a short last block; we may decode a few
	// more samples, never fewer.
	int fails = n < nref || memcmp(out, ref, nref * 2) != 0;
	for (long i = 0; i < nref && i < n; i++) {
		if (out[i] != ref[i]) {
			printf("  sample %ld: %d, ffmpeg %d\n", i, out[i], ref[i]);
			break;
		}
	}

	// 75 CD frames is one second.
	st = Scumm::makeFjCDTrackStream(new Common::MemoryReadStream(w, wlen), 75);
	long m = 0;
	while (!st->endOfData())
		m += st->readBuffer(out, 1000 * ch);
	long rate = st->getRate();
	delete st;
	if (m != rate * ch && nref >= rate * ch) {
		printf("  duration 75: %ld samples, want %ld\n", m, rate * ch);
		fails++;
	}

	printf("test_cdtrack %s: %ld samples, %d ch, %s\n", argv[1], n, ch, fails ? "FAIL" : "ok");
	free(out);
	return fails != 0;
}
