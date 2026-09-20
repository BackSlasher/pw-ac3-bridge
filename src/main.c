/* pw-ac3-bridge: turn a 5.1 PCM sink into an AC-3 bitstream for an HDMI receiver.
 *
 * Everything the session plays lands in a 6-channel null sink. This program
 * captures that sink's monitor, encodes each 1536-sample block to AC-3, wraps it
 * in an IEC 61937 burst, and plays the bursts into a real sink as an encoded
 * passthrough stream. A receiver that decodes AC-3 but not multichannel LPCM gets
 * 5.1 that way, and the wire format never changes while the bridge runs, so the
 * receiver never re-locks its decoder between playbacks.
 *
 * Structure:
 *
 *   registry -> node names.  Both endpoints are found by node.name, never by
 *   ALSA address: the HDMI device moves between PCM numbers as the display
 *   renegotiates, and only the session manager follows it. Both streams carry
 *   node.dont-reconnect, so when the target disappears the stream errors out
 *   rather than being moved to some fallback sink that cannot take a bitstream;
 *   the registry then reports the node back under a new id and the streams are
 *   rebuilt.
 *
 *   capture -> encode -> ring -> playback.  The two streams are separate graph
 *   nodes under one driver (see NODE_GROUP), with a ring between them. It is a byte ring
 *   with no prefill: the playback side takes what is there and zero-fills the
 *   rest, which parks the steady-state fill at the phase offset between the two
 *   nodes — the smallest value the graph allows. Both streams ask for
 *   node.latency = 1536/48000, one AC-3 frame per cycle.
 *
 *   activity.  A client playing into the null sink shows up as a link whose
 *   input node is that sink. The bridge's own capture attaches to the monitor
 *   (output) side, so it never counts itself. With no clients it keeps encoding
 *   — the null sink still produces silence — until the idle timer expires, then
 *   drops both streams so the sink can return to plain PCM.
 *
 * The sink must already advertise ac3-iec61937 for the passthrough link to be
 * made. This program does not set that; on the target it is owned by a separate
 * `pactl set-sink-formats` service, which writes through the session manager's
 * saved route state in a way a plain Props update does not.
 */
#define _GNU_SOURCE

#include "ac3pack.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/iec958-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>

#define RATE        48000
#define CHANNELS    6
#define PCM_STRIDE  (CHANNELS * (int)sizeof(float))	/* --decode-to output */
#define IEC_STRIDE  4					/* 2 ch x S16LE */
#define MAX_LINKS   256
/* Both streams share a node.group, which puts the null sink's subgraph and the
 * real sink's subgraph under one driver. Without it the null sink runs on its
 * own timer and the ring slips a frame whenever the two clocks drift apart. */
#define NODE_GROUP  "pw-ac3-bridge"
/* Eight AC-3 frames, ~256 ms. Only ever holds a fraction of one in steady
 * state; the headroom is there to absorb a scheduling hiccup, not to buffer. */
#define RING_SIZE   (8 * AC3PACK_BURST_BYTES)

struct link_ref {
	uint32_t id;
	uint32_t input_node;
};

struct impl {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook core_listener;
	struct spa_hook registry_listener;
	struct spa_source *timer;

	const char *source_name;
	const char *sink_name;
	const char *decode_to;
	int idle_secs;
	int delay_ms;
	int64_t bitrate;
	bool verbose;

	uint32_t source_id;	/* the 5.1 null sink */
	uint32_t target_id;	/* the sink the bursts go to */
	struct link_ref links[MAX_LINKS];
	int n_links;

	struct pw_stream *capture;
	struct pw_stream *playback;
	struct spa_hook capture_listener;
	struct spa_hook playback_listener;
	bool streams_up;
	bool restart;

	struct ac3pack *enc;
	struct ac3unpack *dec;
	int out_stride;

	/* capture-side accumulator: one AC-3 frame of interleaved float */
	float in_buf[AC3PACK_FRAME_SAMPLES * CHANNELS];
	uint32_t in_fill;		/* frames */
	float *delay_line;		/* delay_frames * CHANNELS, circular */
	uint32_t delay_frames;
	uint32_t delay_pos;
	float dec_buf[AC3PACK_FRAME_SAMPLES * CHANNELS];	/* --decode-to */
	int capture_channels;

	struct spa_ringbuffer ring;
	uint8_t ring_data[RING_SIZE];

	/* counters, written from the data thread, read from the timer */
	uint32_t underruns;
	uint32_t overruns;
	uint32_t encoded;

	time_t idle_since;		/* 0 while a client is linked */
	time_t last_start;		/* rebuild rate limit */
};

static void logmsg(struct impl *i, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

#define vlog(i, ...) do { if ((i)->verbose) logmsg(i, __VA_ARGS__); } while (0)

/* ---------------------------------------------------------------- streams */

static void on_capture_process(void *data)
{
	struct impl *i = data;
	struct pw_buffer *b;
	struct spa_data *d;
	const float *src;
	uint32_t n_frames, off = 0;

	b = pw_stream_dequeue_buffer(i->capture);
	if (b == NULL)
		return;

	d = &b->buffer->datas[0];
	if (d->data == NULL || i->capture_channels != CHANNELS)
		goto done;

	src = SPA_PTROFF(d->data, d->chunk->offset, float);
	n_frames = d->chunk->size / (uint32_t)(sizeof(float) * CHANNELS);

	while (off < n_frames) {
		uint32_t want = AC3PACK_FRAME_SAMPLES - i->in_fill;
		uint32_t take = SPA_MIN(want, n_frames - off);
		uint8_t burst[AC3PACK_BURST_BYTES];
		const uint8_t *out_bytes;
		uint32_t out_len;
		int32_t filled;
		uint32_t widx;

		if (i->delay_frames == 0) {
			memcpy(&i->in_buf[i->in_fill * CHANNELS],
			       &src[(size_t)off * CHANNELS],
			       (size_t)take * PCM_STRIDE);
		} else {
			/* --delay: every sample goes through a circular delay line
			 * on its way to the encoder. Done on the PCM, not in the
			 * ring, so the amount is exact to the sample and does not
			 * depend on how the two streams happened to start up. */
			float *in = &i->in_buf[i->in_fill * CHANNELS];
			const float *cur = &src[(size_t)off * CHANNELS];
			uint32_t k;
			for (k = 0; k < take; k++) {
				float *slot = &i->delay_line[(size_t)i->delay_pos * CHANNELS];
				memcpy(&in[(size_t)k * CHANNELS], slot, PCM_STRIDE);
				memcpy(slot, &cur[(size_t)k * CHANNELS], PCM_STRIDE);
				if (++i->delay_pos == i->delay_frames)
					i->delay_pos = 0;
			}
		}
		i->in_fill += take;
		off += take;
		if (i->in_fill < AC3PACK_FRAME_SAMPLES)
			break;
		i->in_fill = 0;

		if (ac3pack_encode(i->enc, i->in_buf, burst) < 0)
			continue;
		i->encoded++;

		if (i->dec != NULL) {
			/* --decode-to: unpack and decode the burst back to PCM,
			 * so the path can end in a sink that takes raw audio. */
			int n = ac3unpack_decode(i->dec, burst, i->dec_buf);
			if (n <= 0)
				continue;
			out_bytes = (const uint8_t *)i->dec_buf;
			out_len = (uint32_t)n * PCM_STRIDE;
		} else {
			out_bytes = burst;
			out_len = AC3PACK_BURST_BYTES;
		}

		filled = spa_ringbuffer_get_write_index(&i->ring, &widx);
		if (filled < 0 || (uint32_t)filled + out_len > RING_SIZE) {
			i->overruns++;
			continue;
		}
		spa_ringbuffer_write_data(&i->ring, i->ring_data, RING_SIZE,
					  widx % RING_SIZE, out_bytes, out_len);
		spa_ringbuffer_write_update(&i->ring, widx + out_len);
	}

done:
	pw_stream_queue_buffer(i->capture, b);
}

static void on_playback_process(void *data)
{
	struct impl *i = data;
	struct pw_buffer *b;
	struct spa_data *d;
	uint32_t want, avail, ridx;
	int32_t readable;
	uint8_t *dst;

	b = pw_stream_dequeue_buffer(i->playback);
	if (b == NULL)
		return;

	d = &b->buffer->datas[0];
	if (d->data == NULL) {
		pw_stream_queue_buffer(i->playback, b);
		return;
	}
	dst = d->data;

	want = d->maxsize;
	if (b->requested != 0)
		want = SPA_MIN(want, (uint32_t)b->requested * i->out_stride);
	else
		/* No quantum hint: one AC-3 frame's worth, not a whole buffer. */
		want = SPA_MIN(want, (uint32_t)AC3PACK_FRAME_SAMPLES * i->out_stride);
	want -= want % i->out_stride;

	readable = spa_ringbuffer_get_read_index(&i->ring, &ridx);
	avail = readable > 0 ? (uint32_t)readable : 0;
	avail -= avail % i->out_stride;
	if (avail > want)
		avail = want;

	if (avail > 0) {
		spa_ringbuffer_read_data(&i->ring, i->ring_data, RING_SIZE,
					 ridx % RING_SIZE, dst, avail);
		spa_ringbuffer_read_update(&i->ring, ridx + avail);
	}
	if (avail < want) {
		/* Nothing queued yet. Zeroes are silence on the PCM path, and
		 * on the bitstream path they are the null data IEC 61937 already
		 * carries between bursts, which a receiver resyncs from at the
		 * next preamble. The ring gains a frame doing this, which is how
		 * the fill settles at the two nodes' phase offset. */
		memset(dst + avail, 0, want - avail);
		i->underruns++;
	}

	d->chunk->offset = 0;
	d->chunk->size = want;
	d->chunk->stride = i->out_stride;
	b->size = want / (uint64_t)i->out_stride;

	pw_stream_queue_buffer(i->playback, b);
}

static void on_capture_param_changed(void *data, uint32_t id,
				     const struct spa_pod *param)
{
	struct impl *i = data;
	struct spa_audio_info info;

	if (param == NULL || id != SPA_PARAM_Format)
		return;
	spa_zero(info);
	if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
		return;
	if (info.media_type != SPA_MEDIA_TYPE_audio ||
	    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;
	if (spa_format_audio_raw_parse(param, &info.info.raw) < 0)
		return;

	i->capture_channels = info.info.raw.channels;
	vlog(i, "capture format: %d ch, %d Hz", info.info.raw.channels,
	     info.info.raw.rate);
	if (info.info.raw.channels != CHANNELS)
		logmsg(i, "warning: capture negotiated %d channels, expected %d",
		       info.info.raw.channels, CHANNELS);
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
				    enum pw_stream_state state,
				    const char *error)
{
	struct impl *i = data;

	vlog(i, "stream state: %s%s%s", pw_stream_state_as_string(state),
	     error ? " - " : "", error ? error : "");

	if (state == PW_STREAM_STATE_ERROR ||
	    (state == PW_STREAM_STATE_UNCONNECTED && i->streams_up)) {
		/* Torn down under us: the target vanished, or the session
		 * manager refused the link. Rebuild from the timer rather than
		 * destroying a stream from inside its own callback. */
		logmsg(i, "stream lost (%s); will rebuild",
		       error ? error : pw_stream_state_as_string(state));
		i->restart = true;
	}
}

static const struct pw_stream_events capture_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.param_changed = on_capture_param_changed,
	.process = on_capture_process,
};

static const struct pw_stream_events playback_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.process = on_playback_process,
};

static void stop_streams(struct impl *i)
{
	bool was_up = i->streams_up;

	i->streams_up = false;
	i->restart = false;

	if (i->capture != NULL) {
		spa_hook_remove(&i->capture_listener);
		pw_stream_destroy(i->capture);
		i->capture = NULL;
	}
	if (i->playback != NULL) {
		spa_hook_remove(&i->playback_listener);
		pw_stream_destroy(i->playback);
		i->playback = NULL;
	}
	if (was_up)
		logmsg(i, "streams down (%u frames encoded, %u underruns, "
		       "%u overruns)", i->encoded, i->underruns, i->overruns);
}

static int start_streams(struct impl *i)
{
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];
	struct pw_properties *props;
	int res;

	if (i->streams_up)
		return 0;

	spa_ringbuffer_init(&i->ring);
	i->in_fill = 0;
	i->delay_pos = 0;
	if (i->delay_frames > 0)
		memset(i->delay_line, 0, (size_t)i->delay_frames * PCM_STRIDE);
	i->capture_channels = CHANNELS;
	i->underruns = i->overruns = i->encoded = 0;

	/* Capture: the null sink's monitor, one AC-3 frame per cycle. */
	props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_ROLE, "Production",
		PW_KEY_NODE_NAME, "pw-ac3-bridge-capture",
		PW_KEY_NODE_DESCRIPTION, "AC-3 bridge capture",
		PW_KEY_APP_NAME, "pw-ac3-bridge",
		PW_KEY_STREAM_CAPTURE_SINK, "true",
		PW_KEY_NODE_GROUP, NODE_GROUP,
		PW_KEY_NODE_DONT_RECONNECT, "true",
		PW_KEY_TARGET_OBJECT, i->source_name,
		NULL);
	pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d",
			   AC3PACK_FRAME_SAMPLES, RATE);
	pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", RATE);

	i->capture = pw_stream_new(i->core, "pw-ac3-bridge-capture", props);
	if (i->capture == NULL)
		return -errno;
	pw_stream_add_listener(i->capture, &i->capture_listener,
			       &capture_events, i);

	{
		struct spa_audio_info_raw raw = {
			.format = SPA_AUDIO_FORMAT_F32,
			.rate = RATE,
			.channels = CHANNELS,
			.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR,
				      SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
				      SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR },
		};
		params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
						       &raw);
	}
	res = pw_stream_connect(i->capture, PW_DIRECTION_INPUT, PW_ID_ANY,
				PW_STREAM_FLAG_AUTOCONNECT |
				PW_STREAM_FLAG_MAP_BUFFERS |
				PW_STREAM_FLAG_DONT_RECONNECT |
				PW_STREAM_FLAG_RT_PROCESS,
				params, 1);
	if (res < 0) {
		logmsg(i, "cannot connect capture to %s: %s", i->source_name,
		       spa_strerror(res));
		return res;
	}

	/* Playback: either the bitstream, or decoded PCM in test mode. */
	props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback",
		PW_KEY_MEDIA_ROLE, "Production",
		PW_KEY_NODE_NAME, "pw-ac3-bridge",
		PW_KEY_NODE_DESCRIPTION, "AC-3 bridge",
		PW_KEY_APP_NAME, "pw-ac3-bridge",
		PW_KEY_NODE_GROUP, NODE_GROUP,
		PW_KEY_NODE_DONT_RECONNECT, "true",
		PW_KEY_TARGET_OBJECT,
			i->decode_to ? i->decode_to : i->sink_name,
		NULL);
	pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d",
			   AC3PACK_FRAME_SAMPLES, RATE);
	pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", RATE);

	i->playback = pw_stream_new(i->core, "pw-ac3-bridge", props);
	if (i->playback == NULL)
		return -errno;
	pw_stream_add_listener(i->playback, &i->playback_listener,
			       &playback_events, i);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	if (i->decode_to != NULL) {
		struct spa_audio_info_raw raw = {
			.format = SPA_AUDIO_FORMAT_F32,
			.rate = RATE,
			.channels = CHANNELS,
			.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR,
				      SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
				      SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR },
		};
		params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
						       &raw);
	} else {
		/* The only format offered, which is what makes the session
		 * manager treat this node as encoded-only: it will link it to a
		 * sink that advertises the same iec958 codec and put that sink
		 * in passthrough mode, or refuse to link it at all. */
		struct spa_audio_info_iec958 iec = {
			.codec = SPA_AUDIO_IEC958_CODEC_AC3,
			.rate = RATE,
		};
		params[0] = spa_format_audio_iec958_build(&b,
							  SPA_PARAM_EnumFormat,
							  &iec);
	}
	res = pw_stream_connect(i->playback, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				PW_STREAM_FLAG_AUTOCONNECT |
				PW_STREAM_FLAG_MAP_BUFFERS |
				PW_STREAM_FLAG_DONT_RECONNECT |
				PW_STREAM_FLAG_RT_PROCESS,
				params, 1);
	if (res < 0) {
		logmsg(i, "cannot connect playback to %s: %s",
		       i->decode_to ? i->decode_to : i->sink_name,
		       spa_strerror(res));
		return res;
	}

	i->streams_up = true;
	i->restart = false;
	logmsg(i, "streams up: %s.monitor -> %s (%s)", i->source_name,
	       i->decode_to ? i->decode_to : i->sink_name,
	       i->decode_to ? "decoded PCM, test mode" : "AC-3 passthrough");
	return 0;
}

/* --------------------------------------------------------------- registry */

static bool client_active(struct impl *i)
{
	int n;

	if (i->source_id == SPA_ID_INVALID)
		return false;
	for (n = 0; n < i->n_links; n++)
		if (i->links[n].input_node == i->source_id)
			return true;
	return false;
}

static void update_state(struct impl *i)
{
	const char *target = i->decode_to ? i->decode_to : i->sink_name;
	bool have_target = i->target_id != SPA_ID_INVALID;
	bool active = client_active(i);
	time_t now = time(NULL);
	bool want;

	if (active)
		i->idle_since = 0;
	else if (i->idle_since == 0)
		i->idle_since = now;

	/* Run while a client is playing, and for --idle seconds after the last
	 * one leaves so the receiver keeps its lock across a pause. */
	want = i->source_id != SPA_ID_INVALID && have_target &&
	       (active || (i->streams_up &&
			   now - i->idle_since < i->idle_secs));

	if (i->restart && i->streams_up) {
		stop_streams(i);
		/* fall through: rebuilt below if still wanted */
		want = i->source_id != SPA_ID_INVALID && have_target && active;
	}

	if (want && !i->streams_up) {
		/* A target that is present but will not take a bitstream — a
		 * sink whose codec list has lost AC3 — fails on connect and
		 * would otherwise be retried as fast as the registry churns.
		 * Once a second is often enough to catch it coming back. */
		if (now == i->last_start)
			return;
		i->last_start = now;
		if (start_streams(i) < 0)
			stop_streams(i);
	} else if (!want && i->streams_up) {
		if (!have_target)
			logmsg(i, "target sink %s went away", target);
		else
			logmsg(i, "idle for %ds", i->idle_secs);
		stop_streams(i);
	}
}

static void registry_global(void *data, uint32_t id, uint32_t permissions,
			    const char *type, uint32_t version,
			    const struct spa_dict *props)
{
	struct impl *i = data;
	const char *name;

	if (props == NULL)
		return;

	if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
		if (name == NULL)
			return;
		if (spa_streq(name, i->source_name)) {
			i->source_id = id;
			logmsg(i, "source sink %s is node %u", name, id);
		} else if (spa_streq(name, i->decode_to ? i->decode_to
						       : i->sink_name)) {
			i->target_id = id;
			logmsg(i, "target sink %s is node %u", name, id);
		} else {
			return;
		}
		update_state(i);
		return;
	}

	if (spa_streq(type, PW_TYPE_INTERFACE_Link)) {
		const char *in = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
		if (in == NULL || i->n_links >= MAX_LINKS)
			return;
		i->links[i->n_links].id = id;
		i->links[i->n_links].input_node = (uint32_t)atoi(in);
		i->n_links++;
		update_state(i);
	}
}

static void registry_global_remove(void *data, uint32_t id)
{
	struct impl *i = data;
	int n;

	for (n = 0; n < i->n_links; n++) {
		if (i->links[n].id != id)
			continue;
		i->links[n] = i->links[i->n_links - 1];
		i->n_links--;
		update_state(i);
		return;
	}

	if (id == i->source_id) {
		logmsg(i, "source sink %s (node %u) removed", i->source_name, id);
		i->source_id = SPA_ID_INVALID;
		update_state(i);
	} else if (id == i->target_id) {
		logmsg(i, "target sink (node %u) removed", id);
		i->target_id = SPA_ID_INVALID;
		update_state(i);
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void on_core_error(void *data, uint32_t id, int seq, int res,
			  const char *message)
{
	struct impl *i = data;

	logmsg(i, "core error id:%u seq:%d res:%d (%s): %s", id, seq, res,
	       spa_strerror(res), message);
	if (id == PW_ID_CORE)
		pw_main_loop_quit(i->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

/* ------------------------------------------------------------------- main */

static void on_timer(void *data, uint64_t expirations)
{
	struct impl *i = data;
	static uint32_t last_under, last_over;

	if (i->verbose && i->streams_up &&
	    (i->underruns != last_under || i->overruns != last_over)) {
		vlog(i, "encoded %u frames, underruns %u, overruns %u",
		     i->encoded, i->underruns, i->overruns);
		last_under = i->underruns;
		last_over = i->overruns;
	}
	update_state(i);
}

static void on_signal(void *data, int signal_number)
{
	struct impl *i = data;

	pw_main_loop_quit(i->loop);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
"usage: %s [options]\n"
"  --source NAME      5.1 null sink whose monitor is captured\n"
"                     (default: surround51)\n"
"  --sink NAME        sink to play the AC-3 bitstream into\n"
"                     (default: alsa_output.pci-0000_00_1f.3.hdmi-stereo)\n"
"  --decode-to NAME   test mode: decode the packed frames back to 6-channel\n"
"                     PCM and play that into NAME instead of bitstreaming\n"
"  --idle SECONDS     keep the streams up this long after the last client\n"
"                     leaves, so the receiver keeps its lock (default: 1800)\n"
"  --delay MS         hold the audio back by this many milliseconds, for a\n"
"                     display that is slower than the receiver (default: 0,\n"
"                     maximum 500)\n"
"  --bitrate BPS      AC-3 bitrate (default: 640000)\n"
"  --verbose          log format negotiation and buffer counters\n"
"  --help\n", argv0);
}

int main(int argc, char **argv)
{
	struct impl impl = {
		.source_name = "surround51",
		.sink_name = "alsa_output.pci-0000_00_1f.3.hdmi-stereo",
		.idle_secs = 1800,
		.bitrate = 640000,
		.source_id = SPA_ID_INVALID,
		.target_id = SPA_ID_INVALID,
	};
	struct impl *i = &impl;
	static const struct option opts[] = {
		{ "source",    required_argument, NULL, 's' },
		{ "sink",      required_argument, NULL, 'k' },
		{ "decode-to", required_argument, NULL, 'd' },
		{ "idle",      required_argument, NULL, 'i' },
		{ "delay",     required_argument, NULL, 'D' },
		{ "bitrate",   required_argument, NULL, 'b' },
		{ "verbose",   no_argument,       NULL, 'v' },
		{ "help",      no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct timespec value, interval;
	char err[256] = "";
	int c;

	while ((c = getopt_long(argc, argv, "s:k:d:i:D:b:vh", opts, NULL)) != -1) {
		switch (c) {
		case 's': i->source_name = optarg; break;
		case 'k': i->sink_name = optarg; break;
		case 'd': i->decode_to = optarg; break;
		case 'i': i->idle_secs = atoi(optarg); break;
		case 'D': i->delay_ms = atoi(optarg); break;
		case 'b': i->bitrate = atoll(optarg); break;
		case 'v': i->verbose = true; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	i->out_stride = i->decode_to ? PCM_STRIDE : IEC_STRIDE;
	if (i->delay_ms < 0 || i->delay_ms > 500) {
		fprintf(stderr, "--delay must be between 0 and 500 ms\n");
		return 2;
	}
	i->delay_frames = (uint32_t)i->delay_ms * RATE / 1000;
	if (i->delay_frames > 0) {
		i->delay_line = calloc(i->delay_frames, PCM_STRIDE);
		if (i->delay_line == NULL) {
			fprintf(stderr, "out of memory\n");
			return 1;
		}
	}

	i->enc = ac3pack_new(RATE, CHANNELS, i->bitrate, err, sizeof(err));
	if (i->enc == NULL) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	if (i->decode_to != NULL) {
		i->dec = ac3unpack_new(CHANNELS, err, sizeof(err));
		if (i->dec == NULL) {
			fprintf(stderr, "%s\n", err);
			return 1;
		}
	}

	pw_init(&argc, &argv);
	spa_ringbuffer_init(&i->ring);

	i->loop = pw_main_loop_new(NULL);
	if (i->loop == NULL) {
		fprintf(stderr, "cannot create main loop\n");
		return 1;
	}
	pw_loop_add_signal(pw_main_loop_get_loop(i->loop), SIGINT, on_signal, i);
	pw_loop_add_signal(pw_main_loop_get_loop(i->loop), SIGTERM, on_signal, i);

	i->context = pw_context_new(pw_main_loop_get_loop(i->loop), NULL, 0);
	i->core = i->context ? pw_context_connect(i->context, NULL, 0) : NULL;
	if (i->core == NULL) {
		fprintf(stderr, "cannot connect to PipeWire: %m\n");
		return 1;
	}
	pw_core_add_listener(i->core, &i->core_listener, &core_events, i);

	i->registry = pw_core_get_registry(i->core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(i->registry, &i->registry_listener,
				 &registry_events, i);

	/* One second is plenty: everything real arrives as a registry event,
	 * and the timer only ages the idle window and retries a rebuild. */
	i->timer = pw_loop_add_timer(pw_main_loop_get_loop(i->loop), on_timer, i);
	value.tv_sec = 1; value.tv_nsec = 0;
	interval.tv_sec = 1; interval.tv_nsec = 0;
	pw_loop_update_timer(pw_main_loop_get_loop(i->loop), i->timer,
			     &value, &interval, false);

	logmsg(i, "pw-ac3-bridge: %s.monitor -> %s, %" PRId64 " bps, "
	       "encoder delay %d samples, idle %ds",
	       i->source_name, i->decode_to ? i->decode_to : i->sink_name,
	       i->bitrate, ac3pack_delay_samples(i->enc), i->idle_secs);

	pw_main_loop_run(i->loop);

	stop_streams(i);
	pw_proxy_destroy((struct pw_proxy *)i->registry);
	pw_core_disconnect(i->core);
	pw_context_destroy(i->context);
	pw_main_loop_destroy(i->loop);
	pw_deinit();
	ac3unpack_free(i->dec);
	ac3pack_free(i->enc);
	return 0;
}
