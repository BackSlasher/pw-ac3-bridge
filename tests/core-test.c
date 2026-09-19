/* Headless test of the encode+pack path, with no PipeWire involved.
 *
 * "gen" synthesizes the six-tone sequence — one tone per channel in
 * FL FR FC LFE RL RR order, SEG_SECS seconds each, silence on the other five —
 * pushes it through ac3pack and writes the resulting IEC 61937 byte stream.
 * ffmpeg's spdif demuxer and AC-3 decoder then turn that back into f32le PCM,
 * and "check" reads the PCM and asserts that each segment's energy landed on the
 * channel it was written to.
 *
 * The LFE tone is 55 Hz because AC-3 band-limits the LFE channel to 120 Hz; a
 * mid-band tone there would be filtered away by the encoder, not by a bug.
 */
#include "../src/ac3pack.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE      48000
#define CHANNELS  6
#define SEG_SECS  2
#define SEG_LEN   (RATE * SEG_SECS)
#define AMPLITUDE 0.5

static const double tone_hz[CHANNELS] = { 220, 330, 440, 55, 660, 880 };
static const char *ch_name[CHANNELS] = { "FL", "FR", "FC", "LFE", "RL", "RR" };

static int cmd_gen(const char *path)
{
	struct ac3pack *p;
	FILE *f;
	float block[AC3PACK_FRAME_SAMPLES * CHANNELS];
	uint8_t burst[AC3PACK_BURST_BYTES];
	char err[256] = "";
	long total = (long)SEG_LEN * CHANNELS;
	long pos = 0;
	int bursts = 0, skipped = 0;

	p = ac3pack_new(RATE, CHANNELS, 640000, err, sizeof(err));
	if (p == NULL) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	f = fopen(path, "wb");
	if (f == NULL) {
		perror(path);
		ac3pack_free(p);
		return 1;
	}

	/* One extra frame past the end so the encoder's last real frame comes
	 * out; its delay means output lags input by a frame or so. */
	while (pos < total + AC3PACK_FRAME_SAMPLES) {
		int i, c, ret;

		memset(block, 0, sizeof(block));
		for (i = 0; i < AC3PACK_FRAME_SAMPLES; i++) {
			long n = pos + i;
			if (n >= total)
				break;
			c = (int)(n / SEG_LEN);
			block[i * CHANNELS + c] = (float)(AMPLITUDE *
				sin(2.0 * M_PI * tone_hz[c] * (double)n / RATE));
		}
		ret = ac3pack_encode(p, block, burst);
		if (ret == 0) {
			fwrite(burst, 1, sizeof(burst), f);
			bursts++;
		} else {
			skipped++;
		}
		pos += AC3PACK_FRAME_SAMPLES;
	}

	printf("encoder delay (libavcodec initial_padding): %d samples\n",
	       ac3pack_delay_samples(p));
	printf("bursts written: %d (%d frames held back by encoder start-up)\n",
	       bursts, skipped);
	printf("burst size: %d bytes, %d samples per channel per frame\n",
	       AC3PACK_BURST_BYTES, AC3PACK_FRAME_SAMPLES);

	fclose(f);
	ac3pack_free(p);
	return 0;
}

/* RMS of a channel over [from, to) samples. */
static double rms(const float *pcm, long frames, int ch, long from, long to)
{
	double sum = 0.0;
	long n, count = 0;

	if (from < 0)
		from = 0;
	if (to > frames)
		to = frames;
	for (n = from; n < to; n++) {
		double v = pcm[n * CHANNELS + ch];
		sum += v * v;
		count++;
	}
	return count ? sqrt(sum / (double)count) : 0.0;
}

static double db(double v)
{
	return v > 1e-9 ? 20.0 * log10(v) : -180.0;
}

static int cmd_check(const char *path)
{
	FILE *f;
	float *pcm;
	long frames, n;
	int seg, ch, failures = 0, onset = -1;
	size_t cap;

	f = fopen(path, "rb");
	if (f == NULL) {
		perror(path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	cap = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	frames = (long)(cap / (CHANNELS * sizeof(float)));
	pcm = malloc(cap);
	if (pcm == NULL || fread(pcm, 1, cap, f) != cap) {
		fprintf(stderr, "cannot read %s\n", path);
		fclose(f);
		free(pcm);
		return 1;
	}
	fclose(f);

	printf("decoded: %ld frames (%.2f s), %d channels\n",
	       frames, (double)frames / RATE, CHANNELS);
	if (frames < (long)SEG_LEN * CHANNELS / 2) {
		printf("FAIL: decoded stream is far shorter than the %d s written\n",
		       SEG_SECS * CHANNELS);
		free(pcm);
		return 1;
	}

	/* Round-trip onset: the first point where FL rises out of the noise.
	 * The source tone starts at sample 0, so this is the encoder's delay
	 * plus the decoder's. */
	for (n = 0; n + 128 < frames && onset < 0; n += 16)
		if (rms(pcm, frames, 0, n, n + 128) > 0.05)
			onset = (int)n;
	printf("round-trip onset (encode+pack+decode): %d samples (%.1f ms)\n",
	       onset, onset * 1000.0 / RATE);

	printf("\nper-segment channel energy, dBFS RMS "
	       "(rows: 2 s segment, cols: decoded channel)\n");
	printf("%-12s", "segment");
	for (ch = 0; ch < CHANNELS; ch++)
		printf("%8s", ch_name[ch]);
	printf("   verdict\n");

	for (seg = 0; seg < CHANNELS; seg++) {
		/* Middle half of the segment, which keeps the window clear of the
		 * onset, of the segment edges, and of the round-trip delay. */
		long from = (long)seg * SEG_LEN + SEG_LEN / 4;
		long to = from + SEG_LEN / 2;
		double on, worst_off = 0.0;
		int ok;

		printf("%-12s", ch_name[seg]);
		on = rms(pcm, frames, seg, from, to);
		for (ch = 0; ch < CHANNELS; ch++) {
			double v = rms(pcm, frames, ch, from, to);
			if (ch != seg && v > worst_off)
				worst_off = v;
			printf("%8.1f", db(v));
		}
		/* 20 dB of separation: AC-3 at 640 kbps leaks a little between
		 * channels, a misrouted channel would show none. */
		ok = on > 0.05 && db(on) - db(worst_off) > 20.0;
		printf("   %s (%.1f dB apart)\n", ok ? "ok" : "FAIL",
		       db(on) - db(worst_off));
		if (!ok)
			failures++;
	}

	free(pcm);
	if (failures) {
		printf("\nFAIL: %d segment(s) did not land on their own channel\n",
		       failures);
		return 1;
	}
	printf("\nPASS: 6 channels, each segment's energy on its own channel\n");
	return 0;
}

/* The signal the latency probe plays: short bursts, one channel at a time,
 * separated by silence long enough that an onset can never be confused with the
 * previous burst's tail. Written as raw s16le for ffmpeg to wrap. */
#define PROBE_BURSTS   10
#define PROBE_PERIOD   1.5	/* seconds between burst starts */
#define PROBE_ON       0.30	/* seconds of tone */
#define PROBE_LEAD     0.5	/* silence before the first burst */

static int cmd_probe(const char *path)
{
	FILE *f;
	long total = (long)((PROBE_LEAD + PROBE_BURSTS * PROBE_PERIOD) * RATE);
	long n;

	f = fopen(path, "wb");
	if (f == NULL) {
		perror(path);
		return 1;
	}
	for (n = 0; n < total; n++) {
		int16_t s[CHANNELS] = { 0 };
		double t = (double)n / RATE - PROBE_LEAD;
		long burst = (long)(t / PROBE_PERIOD);

		if (t >= 0 && burst < PROBE_BURSTS) {
			double into = t - burst * PROBE_PERIOD;
			if (into < PROBE_ON) {
				int c = (int)(burst % CHANNELS);
				/* 5 ms raised-cosine edges, so the onset is a
				 * clean step for silencedetect but there is no
				 * click for the encoder to smear. */
				double env = 1.0;
				if (into < 0.005)
					env = 0.5 - 0.5 * cos(M_PI * into / 0.005);
				else if (into > PROBE_ON - 0.005)
					env = 0.5 - 0.5 * cos(M_PI *
						(PROBE_ON - into) / 0.005);
				s[c] = (int16_t)(32767 * AMPLITUDE * env *
					sin(2.0 * M_PI * tone_hz[c] *
					    (double)n / RATE));
			}
		}
		fwrite(s, sizeof(int16_t), CHANNELS, f);
	}
	fclose(f);
	printf("probe signal: %ld frames (%.1f s), %d bursts of %.0f ms "
	       "cycling %s..%s\n", total, (double)total / RATE, PROBE_BURSTS,
	       PROBE_ON * 1000, ch_name[0], ch_name[CHANNELS - 1]);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "gen") == 0)
		return cmd_gen(argv[2]);
	if (argc == 3 && strcmp(argv[1], "check") == 0)
		return cmd_check(argv[2]);
	if (argc == 3 && strcmp(argv[1], "probe") == 0)
		return cmd_probe(argv[2]);
	fprintf(stderr, "usage: core-test gen OUT.spdif\n"
			"       core-test check IN.f32le\n"
			"       core-test probe OUT.s16le\n");
	return 2;
}
