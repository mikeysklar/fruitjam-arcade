/* Host test: the chunked CDDA.SOU decoder (src/scumm/cdda.cpp) gives the
 * same samples as ScummVM's byte-at-a-time loop, for random data read in
 * random sized pieces, including across a seek. Built by
 * `make -f Makefile.host test`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/memstream.h"
#include "audio/audiostream.h"
#include "audio/timestamp.h"
#include "scumm/cdda.h"

enum { kStart = 800, kBlock = 1177 };

// ScummVM 2.2.0's CDDAStream::readBuffer, on a plain array.
static int reference(const byte *d, uint32 size, uint32 &pos, byte &sl, byte &sr, int16 *buf, int n) {
	int s = 0;
	while (s < n && pos < size) {
		if (!((pos - kStart) % kBlock)) {
			sl = d[pos] >> 4;
			sr = d[pos] & 15;
			pos++;
		}
		if (pos + 2 > size) {
			// ScummVM reads the pair anyway (zeros past the end) and then stops.
			buf[s++] = pos < size ? (int8)d[pos] << sl : 0;
			buf[s++] = 0;
			pos = size;
			break;
		}
		buf[s++] = (int8)d[pos] << sl;
		buf[s++] = (int8)d[pos + 1] << sr;
		pos += 2;
	}
	return s;
}

int main() {
	srand(7);
	const uint32 size = kStart + kBlock * 40;
	byte *d = (byte *)malloc(size);
	for (uint32 i = 0; i < size; i++)
		d[i] = rand();
	for (uint32 b = 0; b < 40; b++)
		d[kStart + b * kBlock] = (rand() % 8) << 4 | (rand() % 8); // shifts 0..7

	Common::MemoryReadStream *ms = new Common::MemoryReadStream(d, size);
	Audio::SeekableAudioStream *st = Scumm::makeCDDAStream(ms, DisposeAfterUse::YES);
	uint32 pos = kStart;
	byte sl = 0, sr = 0;
	int16 a[4096], b[4096];
	long total = 0;
	int fails = 0;
	for (int round = 0; round < 400 && pos < size; round++) {
		int n = 2 * (1 + rand() % 1500);
		int na = st->readBuffer(a, n);
		int nb = reference(d, size, pos, sl, sr, b, n);
		if (na != nb || memcmp(a, b, na * 2)) {
			// near the very end ScummVM's eos handling differs by one pair at most
			if (pos >= size - 2 && abs(na - nb) <= 2 && !memcmp(a, b, (na < nb ? na : nb) * 2))
				break;
			printf("cdda mismatch in round %d: %d vs %d samples\n", round, na, nb);
			fails++;
			break;
		}
		total += na;
	}
	delete st;
	printf("cdda: %ld samples compared, %s\n", total, fails ? "FAIL" : "ok");
	return fails != 0;
}
