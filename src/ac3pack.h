/* AC-3 encoding and IEC 61937 framing, with no PipeWire in it.
 *
 * The unit of work is one AC-3 syncframe: 1536 samples per channel, which on the
 * wire becomes a fixed 6144-byte burst (1536 frames of 2 channels x 16 bit, the
 * shape an S/PDIF or HDMI receiver expects). The burst is fixed-size regardless of
 * bitrate; the encoded frame sits at the front behind an 8-byte preamble and the
 * rest is stuffing.
 *
 * Keeping this free of PipeWire is what lets the encode+pack path be tested as a
 * pure function against ffmpeg's spdif demuxer.
 */
#ifndef AC3PACK_H
#define AC3PACK_H

#include <stddef.h>
#include <stdint.h>

/* Samples per channel in one AC-3 syncframe. */
#define AC3PACK_FRAME_SAMPLES 1536
/* Bytes in one IEC 61937 burst: AC3PACK_FRAME_SAMPLES * 2 ch * 2 bytes. */
#define AC3PACK_BURST_BYTES   (AC3PACK_FRAME_SAMPLES * 4)

struct ac3pack;

/* channels must be 6; the layout is FL FR FC LFE RL RR, matching PipeWire's
 * position list for the surround sink. err receives a message on failure. */
struct ac3pack *ac3pack_new(int rate, int channels, int64_t bitrate,
			    char *err, size_t errlen);
void ac3pack_free(struct ac3pack *p);

/* The encoder's algorithmic delay in samples, as libavcodec reports it. */
int ac3pack_delay_samples(const struct ac3pack *p);

/* Encode AC3PACK_FRAME_SAMPLES interleaved float samples per channel and write a
 * complete AC3PACK_BURST_BYTES burst. out must hold AC3PACK_BURST_BYTES.
 * Returns 0, or a negative errno. A return of -EAGAIN means the encoder has not
 * emitted a packet yet (its start-up delay) and out holds a silent burst. */
int ac3pack_encode(struct ac3pack *p, const float *interleaved, uint8_t *out);

/* Recover the AC-3 syncframe from a burst: undoes the preamble and the byte
 * swap. Returns the frame length in bytes written to out (which must hold
 * AC3PACK_BURST_BYTES), or a negative errno if the burst is not well formed. */
int ac3pack_unpack(const uint8_t *burst, uint8_t *out);

/* Decoder half, used by --decode-to and by the round-trip test. */
struct ac3unpack;
struct ac3unpack *ac3unpack_new(int channels, char *err, size_t errlen);
void ac3unpack_free(struct ac3unpack *u);

/* Decode one burst into interleaved float. out must hold room for
 * AC3PACK_FRAME_SAMPLES * channels floats. Returns the number of samples per
 * channel written (0 while the decoder is still filling), or a negative errno. */
int ac3unpack_decode(struct ac3unpack *u, const uint8_t *burst, float *out);

#endif /* AC3PACK_H */
