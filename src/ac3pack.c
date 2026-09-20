/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "ac3pack.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

/* IEC 61937 preamble. Pa and Pb are the sync words; Pc carries the data type
 * (1 = AC-3) in its low bits and the stream's bsmod in bits 8..10; Pd is the
 * payload length in BITS, not bytes. */
#define IEC61937_PA 0xf872
#define IEC61937_PB 0x4e1f
#define IEC61937_DATA_TYPE_AC3 0x01

struct ac3pack {
	AVCodecContext *ctx;
	AVFrame *frame;		/* planar float, AC3PACK_FRAME_SAMPLES per channel */
	AVPacket *pkt;
	int channels;
	uint8_t silence[AC3PACK_BURST_BYTES];
};

struct ac3unpack {
	AVCodecContext *ctx;
	AVFrame *frame;
	AVPacket *pkt;
	int channels;
	uint8_t raw[AC3PACK_BURST_BYTES];
};

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;
	if (err == NULL || errlen == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

/* Write the AC-3 syncframe in raw into a full-length burst.
 *
 * The burst is a stream of little-endian 16-bit samples, and the payload rides
 * in those words big-endian, so each pair of AC-3 bytes is swapped. Everything
 * after the payload is zero stuffing up to the fixed burst length, which is what
 * keeps the wire rate at 48 kHz x 2 ch x 16 bit no matter the bitrate. */
static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static void pack_burst(const uint8_t *raw, int len, uint8_t *out)
{
	int i;

	memset(out, 0, AC3PACK_BURST_BYTES);
	if (len > AC3PACK_BURST_BYTES - 8)
		len = AC3PACK_BURST_BYTES - 8;

	/* byte 5 of the syncframe is bsid<<3 | bsmod */
	put_le16(out + 0, IEC61937_PA);
	put_le16(out + 2, IEC61937_PB);
	put_le16(out + 4, (uint16_t)(((raw[5] & 0x07) << 8) |
				     IEC61937_DATA_TYPE_AC3));
	put_le16(out + 6, (uint16_t)(len * 8));

	for (i = 0; i + 1 < len; i += 2) {
		out[8 + i] = raw[i + 1];
		out[8 + i + 1] = raw[i];
	}
	if (i < len)
		out[8 + i + 1] = raw[i];
}

int ac3pack_unpack(const uint8_t *burst, uint8_t *out)
{
	int len, i;

	if (get_le16(burst) != IEC61937_PA || get_le16(burst + 2) != IEC61937_PB)
		return -EINVAL;
	if ((get_le16(burst + 4) & 0x1f) != IEC61937_DATA_TYPE_AC3)
		return -EINVAL;
	len = get_le16(burst + 6) / 8;
	if (len <= 0 || len > AC3PACK_BURST_BYTES - 8)
		return -EINVAL;

	for (i = 0; i + 1 < len; i += 2) {
		out[i] = burst[8 + i + 1];
		out[i + 1] = burst[8 + i];
	}
	if (i < len)
		out[i] = burst[8 + i + 1];
	return len;
}

static int encode_one(struct ac3pack *p, const float *interleaved, uint8_t *out)
{
	int ret, c, i;

	if (interleaved != NULL) {
		for (c = 0; c < p->channels; c++) {
			float *dst = (float *)p->frame->data[c];
			for (i = 0; i < AC3PACK_FRAME_SAMPLES; i++)
				dst[i] = interleaved[i * p->channels + c];
		}
	} else {
		for (c = 0; c < p->channels; c++)
			memset(p->frame->data[c], 0,
			       AC3PACK_FRAME_SAMPLES * sizeof(float));
	}

	ret = avcodec_send_frame(p->ctx, p->frame);
	if (ret < 0)
		return ret;

	ret = avcodec_receive_packet(p->ctx, p->pkt);
	if (ret == AVERROR(EAGAIN)) {
		memcpy(out, p->silence, AC3PACK_BURST_BYTES);
		return -EAGAIN;
	}
	if (ret < 0)
		return ret;

	pack_burst(p->pkt->data, p->pkt->size, out);
	av_packet_unref(p->pkt);
	return 0;
}

struct ac3pack *ac3pack_new(int rate, int channels, int64_t bitrate,
			    char *err, size_t errlen)
{
	const AVCodec *codec;
	struct ac3pack *p;
	int ret;

	if (channels != 6) {
		seterr(err, errlen, "ac3pack: %d channels, expected 6", channels);
		return NULL;
	}
	codec = avcodec_find_encoder(AV_CODEC_ID_AC3);
	if (codec == NULL) {
		seterr(err, errlen, "ac3pack: libavcodec has no ac3 encoder");
		return NULL;
	}
	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return NULL;
	p->channels = channels;

	p->ctx = avcodec_alloc_context3(codec);
	p->frame = av_frame_alloc();
	p->pkt = av_packet_alloc();
	if (p->ctx == NULL || p->frame == NULL || p->pkt == NULL) {
		seterr(err, errlen, "ac3pack: out of memory");
		goto fail;
	}

	p->ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
	p->ctx->sample_rate = rate;
	p->ctx->bit_rate = bitrate;
	/* FL FR FC LFE SL SR, index for index the same buffer layout as
	 * PipeWire's FL FR FC LFE RL RR. AC-3's 3/2 mode does not distinguish
	 * side from rear surrounds. */
	av_channel_layout_copy(&p->ctx->ch_layout,
			       &(AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1);

	ret = avcodec_open2(p->ctx, codec, NULL);
	if (ret < 0) {
		seterr(err, errlen, "ac3pack: cannot open ac3 encoder at %" PRId64
		       " bps: %s", bitrate, av_err2str(ret));
		goto fail;
	}
	if (p->ctx->frame_size != AC3PACK_FRAME_SAMPLES) {
		seterr(err, errlen, "ac3pack: encoder frame size %d, expected %d",
		       p->ctx->frame_size, AC3PACK_FRAME_SAMPLES);
		goto fail;
	}

	p->frame->format = AV_SAMPLE_FMT_FLTP;
	p->frame->nb_samples = AC3PACK_FRAME_SAMPLES;
	av_channel_layout_copy(&p->frame->ch_layout, &p->ctx->ch_layout);
	ret = av_frame_get_buffer(p->frame, 0);
	if (ret < 0) {
		seterr(err, errlen, "ac3pack: cannot allocate frame: %s",
		       av_err2str(ret));
		goto fail;
	}

	/* A burst of encoded silence to fall back on, and the two pushes that
	 * produce it: the encoder holds the first frame it is given back. After
	 * this the encoder is primed, so the first real frame comes straight
	 * out. */
	encode_one(p, NULL, p->silence);
	encode_one(p, NULL, p->silence);
	return p;

fail:
	ac3pack_free(p);
	return NULL;
}

void ac3pack_free(struct ac3pack *p)
{
	if (p == NULL)
		return;
	av_packet_free(&p->pkt);
	av_frame_free(&p->frame);
	avcodec_free_context(&p->ctx);
	free(p);
}

int ac3pack_delay_samples(const struct ac3pack *p)
{
	return p->ctx->initial_padding;
}

int ac3pack_encode(struct ac3pack *p, const float *interleaved, uint8_t *out)
{
	return encode_one(p, interleaved, out);
}

struct ac3unpack *ac3unpack_new(int channels, char *err, size_t errlen)
{
	const AVCodec *codec;
	struct ac3unpack *u;
	int ret;

	codec = avcodec_find_decoder(AV_CODEC_ID_AC3);
	if (codec == NULL) {
		seterr(err, errlen, "ac3unpack: libavcodec has no ac3 decoder");
		return NULL;
	}
	u = calloc(1, sizeof(*u));
	if (u == NULL)
		return NULL;
	u->channels = channels;
	u->ctx = avcodec_alloc_context3(codec);
	u->frame = av_frame_alloc();
	u->pkt = av_packet_alloc();
	if (u->ctx == NULL || u->frame == NULL || u->pkt == NULL) {
		seterr(err, errlen, "ac3unpack: out of memory");
		goto fail;
	}
	ret = avcodec_open2(u->ctx, codec, NULL);
	if (ret < 0) {
		seterr(err, errlen, "ac3unpack: cannot open ac3 decoder: %s",
		       av_err2str(ret));
		goto fail;
	}
	return u;

fail:
	ac3unpack_free(u);
	return NULL;
}

void ac3unpack_free(struct ac3unpack *u)
{
	if (u == NULL)
		return;
	av_packet_free(&u->pkt);
	av_frame_free(&u->frame);
	avcodec_free_context(&u->ctx);
	free(u);
}

int ac3unpack_decode(struct ac3unpack *u, const uint8_t *burst, float *out)
{
	int len, ret, c, i, n, ch;

	len = ac3pack_unpack(burst, u->raw);
	if (len < 0)
		return len;

	u->pkt->data = u->raw;
	u->pkt->size = len;
	ret = avcodec_send_packet(u->ctx, u->pkt);
	if (ret < 0)
		return ret;

	ret = avcodec_receive_frame(u->ctx, u->frame);
	if (ret == AVERROR(EAGAIN))
		return 0;
	if (ret < 0)
		return ret;

	n = u->frame->nb_samples;
	ch = u->frame->ch_layout.nb_channels;
	if (ch > u->channels)
		ch = u->channels;

	memset(out, 0, (size_t)n * u->channels * sizeof(float));
	for (c = 0; c < ch; c++) {
		const float *src = (const float *)u->frame->data[c];
		for (i = 0; i < n; i++)
			out[i * u->channels + c] = src[i];
	}
	av_frame_unref(u->frame);
	return n;
}
