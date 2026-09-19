/* A sink node that accepts nothing but iec958/AC3, and writes what it receives.
 *
 * There is no sound card in the test container, and a null sink advertises only
 * raw formats, so without this there is no way to exercise the one thing the
 * bridge's real output half depends on: the session manager seeing an
 * encoded-only stream and an encoded-capable sink, deciding the two share a
 * non-raw format, and linking them in passthrough mode. This stands in for the
 * HDMI sink up to the point where a real one would open an ALSA device.
 *
 * The bytes it receives are a raw IEC 61937 stream, so ffmpeg's spdif demuxer
 * can be pointed straight at the output file.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/iec958-utils.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>

struct sink {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_hook listener;
	FILE *out;
	uint64_t bytes;
	bool negotiated;
};

static void on_process(void *data)
{
	struct sink *s = data;
	struct pw_buffer *b;
	struct spa_data *d;

	b = pw_stream_dequeue_buffer(s->stream);
	if (b == NULL)
		return;
	d = &b->buffer->datas[0];
	if (d->data != NULL && d->chunk->size > 0) {
		fwrite(SPA_PTROFF(d->data, d->chunk->offset, void), 1,
		       d->chunk->size, s->out);
		s->bytes += d->chunk->size;
	}
	pw_stream_queue_buffer(s->stream, b);
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct sink *s = data;
	struct spa_audio_info info;

	if (param == NULL || id != SPA_PARAM_Format)
		return;
	spa_zero(info);
	if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
		return;
	if (info.media_subtype != SPA_MEDIA_SUBTYPE_iec958) {
		fprintf(stderr, "NEGOTIATED A RAW FORMAT, not iec958\n");
		return;
	}
	if (spa_format_audio_iec958_parse(param, &info.info.iec958) < 0)
		return;
	s->negotiated = true;
	fprintf(stderr, "negotiated iec958 codec=%u rate=%u\n",
		info.info.iec958.codec, info.info.iec958.rate);
	fflush(stderr);
}

static void on_state_changed(void *data, enum pw_stream_state old,
			     enum pw_stream_state state, const char *error)
{
	fprintf(stderr, "sink state: %s%s%s\n", pw_stream_state_as_string(state),
		error ? " - " : "", error ? error : "");
	if (state == PW_STREAM_STATE_STREAMING)
		fprintf(stderr, "linked and streaming\n");
	fflush(stderr);
}

static const struct pw_stream_events events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.process = on_process,
};

static void on_signal(void *data, int sig)
{
	struct sink *s = data;
	pw_main_loop_quit(s->loop);
}

int main(int argc, char **argv)
{
	struct sink sink = { 0 };
	struct sink *s = &sink;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];
	struct spa_audio_info_iec958 iec = {
		.codec = SPA_AUDIO_IEC958_CODEC_AC3,
		.rate = 48000,
	};
	const char *name = argc > 1 ? argv[1] : "iec958_probe";
	const char *path = argc > 2 ? argv[2] : "/tmp/iec958-sink.spdif";
	int res;

	pw_init(&argc, &argv);

	s->out = fopen(path, "wb");
	if (s->out == NULL) {
		perror(path);
		return 1;
	}
	s->loop = pw_main_loop_new(NULL);
	pw_loop_add_signal(pw_main_loop_get_loop(s->loop), SIGINT, on_signal, s);
	pw_loop_add_signal(pw_main_loop_get_loop(s->loop), SIGTERM, on_signal, s);

	s->context = pw_context_new(pw_main_loop_get_loop(s->loop), NULL, 0);
	s->core = pw_context_connect(s->context, NULL, 0);
	if (s->core == NULL) {
		fprintf(stderr, "cannot connect to PipeWire: %m\n");
		return 1;
	}

	s->stream = pw_stream_new(s->core, name, pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Duplex",
		PW_KEY_MEDIA_CLASS, "Audio/Sink",
		PW_KEY_NODE_NAME, name,
		PW_KEY_NODE_DESCRIPTION, "iec958 passthrough test sink",
		PW_KEY_NODE_VIRTUAL, "true",
		PW_KEY_AUDIO_RATE, "48000",
		NULL));
	pw_stream_add_listener(s->stream, &s->listener, &events, s);

	params[0] = spa_format_audio_iec958_build(&b, SPA_PARAM_EnumFormat, &iec);

	res = pw_stream_connect(s->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
				PW_STREAM_FLAG_MAP_BUFFERS |
				PW_STREAM_FLAG_RT_PROCESS,
				params, 1);
	if (res < 0) {
		fprintf(stderr, "cannot connect sink: %s\n", spa_strerror(res));
		return 1;
	}

	pw_main_loop_run(s->loop);

	fprintf(stderr, "received %" PRIu64 " bytes, negotiated=%d\n",
		s->bytes, s->negotiated);
	fclose(s->out);
	pw_stream_destroy(s->stream);
	pw_core_disconnect(s->core);
	pw_context_destroy(s->context);
	pw_main_loop_destroy(s->loop);
	pw_deinit();
	return s->negotiated && s->bytes > 0 ? 0 : 1;
}
