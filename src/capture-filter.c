/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

#include "sr-buffer.h"
#include "sr-codec.h"
#include "sr-capture.h"
#include "sr-credit.h"
#include "sr-integration.h"
#include <util/platform.h>

#define S_DURATION "duration_ms"
#define S_ENCODER "encoder"
#define S_QUALITY "quality"
#define S_KEYINT "keyint"
#define S_FPS "capture_fps"

struct sr_capture {
	obs_source_t *self;
	pthread_mutex_t state_mutex;
	char *capture_id;
	uint64_t last_arrival_ns;
	uint64_t last_packet_ns;
	uint64_t clock_resets;
	uint64_t last_reset_ns;
	struct sr_buffer buffer;
	struct sr_encoder *encoder;

	enum sr_encoder_backend backend;
	int qp;
	int keyint;
	int capture_fps; /* 0 = encode every frame the source delivers */

	/* frame rate limiter state */
	uint64_t frame_interval_ns;
	uint64_t last_kept_ts;
	bool have_last_kept;

	/* format the current encoder was opened with */
	uint32_t enc_width;
	uint32_t enc_height;
	bool encoder_failed;
	uint64_t encoder_failed_since_ns;
	bool reset_encoder;

	/* timestamp of the last frame the source delivered */
	uint64_t last_frame_ts;
	bool have_last_frame;

	uint64_t last_stats_log;
};

struct sr_buffer *sr_capture_get_buffer(void *capture_data)
{
	struct sr_capture *c = capture_data;
	return c ? &c->buffer : NULL;
}

static const char *sr_capture_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SportsReplayCapture");
}

static void sr_capture_update_impl(void *data, obs_data_t *settings)
{
	struct sr_capture *c = data;

	obs_data_set_string(settings, "capture_id", c->capture_id ? c->capture_id : "");
	int64_t duration = obs_data_get_int(settings, S_DURATION);
	if (duration < 1000)
		duration = 1000;
	if (duration > 120000)
		duration = 120000;
	pthread_mutex_lock(&c->buffer.mutex);
	c->buffer.duration_ns = (uint64_t)duration * 1000000ULL;
	pthread_mutex_unlock(&c->buffer.mutex);

	enum sr_encoder_backend backend = (enum sr_encoder_backend)obs_data_get_int(settings, S_ENCODER);
	int qp = (int)obs_data_get_int(settings, S_QUALITY);
	int keyint = (int)obs_data_get_int(settings, S_KEYINT);
	int fps = (int)obs_data_get_int(settings, S_FPS);
	if (backend < SR_ENC_AUTO || backend > SR_ENC_X264)
		backend = SR_ENC_AUTO;
	if (qp < 0)
		qp = 0;
	if (qp > 51)
		qp = 51;
	if (keyint < 1)
		keyint = 1;
	if (keyint > 300)
		keyint = 300;
	if (fps < 0 || fps > 240)
		fps = 0;

	if (backend != c->backend || qp != c->qp || keyint != c->keyint || fps != c->capture_fps) {
		c->backend = backend;
		c->qp = qp;
		c->keyint = keyint;
		c->capture_fps = fps;
		c->frame_interval_ns = (fps > 0) ? 1000000000ULL / (uint64_t)fps : 0;
		c->have_last_kept = false;
		c->reset_encoder = true;
		c->encoder_failed = false;
	}
}

static void sr_capture_update(void *data, obs_data_t *settings)
{
	struct sr_capture *c = data;
	pthread_mutex_lock(&c->state_mutex);
	sr_capture_update_impl(data, settings);
	pthread_mutex_unlock(&c->state_mutex);
}

static void *sr_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct sr_capture *c = bzalloc(sizeof(struct sr_capture));
	c->self = source;
	pthread_mutex_init(&c->state_mutex, NULL);
	const char *owner = obs_data_get_string(settings, "capture_owner_uuid");
	const char *source_uuid = obs_source_get_uuid(source);
	const char *preferred = obs_data_get_string(settings, "capture_id");
	if (owner && *owner && strcmp(owner, source_uuid))
		preferred = "";
	c->capture_id = sr_integration_register_capture(source, preferred);
	obs_data_set_string(settings, "capture_owner_uuid", source_uuid);
	obs_data_set_string(settings, "capture_id", c->capture_id);
	sr_buffer_init(&c->buffer);
	c->backend = SR_ENC_AUTO;
	c->qp = 23;
	c->keyint = SR_DEFAULT_KEYINT;
	sr_capture_update(c, settings);
	return c;
}

static void sr_capture_destroy(void *data)
{
	struct sr_capture *c = data;
	sr_integration_unregister_capture(c->self);
	sr_encoder_destroy(c->encoder);
	sr_buffer_free(&c->buffer);
	bfree(c->capture_id);
	pthread_mutex_destroy(&c->state_mutex);
	bfree(c);
}

/* Every capture filter carries the same default name, so log lines name the
 * source the filter is attached to. */
static const char *sr_capture_log_name(const struct sr_capture *c)
{
	obs_source_t *parent = obs_filter_get_parent(c->self);
	return parent ? obs_source_get_name(parent) : obs_source_get_name(c->self);
}

static void log_buffer_stats(struct sr_capture *c, uint64_t now)
{
	if (c->last_stats_log && now - c->last_stats_log < 60000000000ULL)
		return;
	c->last_stats_log = now;
	const size_t bytes = sr_buffer_video_bytes(&c->buffer);
	obs_log(LOG_INFO, "'%s': replay buffer using %.1f MB", sr_capture_log_name(c),
		(double)bytes / (1024.0 * 1024.0));
}

/* A source that restarts (a capture device reopened with new settings, a
 * feed coming back after a drop) starts its clock over. The buffered frames
 * belong to the old timeline and would never expire against the new one, so
 * the buffer grew without bound: start over with a fresh encoder, whose first
 * packet is a keyframe. */
static void sr_capture_check_clock(struct sr_capture *c, uint64_t ts)
{
	if (c->have_last_frame && ts < c->last_frame_ts) {
		obs_log(LOG_INFO, "'%s': source clock went backwards, restarting the replay buffer",
			sr_capture_log_name(c));
		c->reset_encoder = true;
		c->clock_resets++;
		c->last_reset_ns = os_gettime_ns();
		c->last_stats_log = 0;
	}
	c->have_last_frame = true;
	c->last_frame_ts = ts;
}

/* Frame rate limiter. With a capture rate configured, frames that arrive
 * ahead of the next slot never reach the encoder. The quarter-interval
 * tolerance keeps a 60 fps source asked for 30 fps from falling to 20 when
 * its timestamps jitter. */
static bool sr_capture_keep_frame(struct sr_capture *c, uint64_t ts)
{
	if (!c->frame_interval_ns)
		return true;

	/* first frame, or the source restarted and its clock went backwards */
	if (!c->have_last_kept || ts < c->last_kept_ts) {
		c->have_last_kept = true;
		c->last_kept_ts = ts;
		return true;
	}

	if (ts - c->last_kept_ts + c->frame_interval_ns / 4 < c->frame_interval_ns)
		return false;

	c->last_kept_ts = ts;
	return true;
}

static struct obs_source_frame *sr_capture_filter_video_impl(void *data, struct obs_source_frame *frame)
{
	struct sr_capture *c = data;

	if (!frame || !frame->data[0])
		return frame;
	c->last_arrival_ns = os_gettime_ns();
	if (c->encoder_failed) {
		if (c->last_arrival_ns - c->encoder_failed_since_ns < 10000000000ULL)
			return frame;
		c->encoder_failed = false;
	}

	sr_capture_check_clock(c, frame->timestamp);

	if (!sr_capture_keep_frame(c, frame->timestamp))
		return frame;

	if (c->encoder && (c->reset_encoder || frame->width != c->enc_width || frame->height != c->enc_height)) {
		sr_encoder_destroy(c->encoder);
		c->encoder = NULL;
		sr_buffer_clear(&c->buffer);
	}

	if (!c->encoder) {
		uint32_t fps_num = (uint32_t)c->capture_fps;
		uint32_t fps_den = 1;
		if (!c->capture_fps) {
			struct obs_video_info ovi;
			obs_get_video_info(&ovi);
			fps_num = ovi.fps_num;
			fps_den = ovi.fps_den;
		}
		c->encoder = sr_encoder_create(sr_capture_log_name(c), frame->width, frame->height, fps_num, fps_den,
					       c->backend, c->qp, c->keyint);
		if (!c->encoder) {
			obs_log(LOG_ERROR, "'%s': no H.264 encoder available, replay capture disabled",
				sr_capture_log_name(c));
			c->encoder_failed = true;
			c->encoder_failed_since_ns = os_gettime_ns();
			return frame;
		}
		c->reset_encoder = false;
		c->enc_width = frame->width;
		c->enc_height = frame->height;

		pthread_mutex_lock(&c->buffer.mutex);
		c->buffer.codec_id = sr_encoder_codec_id(c->encoder);
		c->buffer.width = frame->width & ~1u;
		c->buffer.height = frame->height & ~1u;
		pthread_mutex_unlock(&c->buffer.mutex);

		const uint8_t *extradata = NULL;
		int extradata_size = 0;
		sr_encoder_get_extradata(c->encoder, &extradata, &extradata_size);
		sr_buffer_set_extradata(&c->buffer, extradata, extradata_size);
	}

	AVPacket *pkt = sr_encoder_encode(c->encoder, frame);
	if (pkt) {
		c->last_packet_ns = os_gettime_ns();
		sr_buffer_push_video(&c->buffer, pkt, (uint64_t)pkt->pts);
	}

	log_buffer_stats(c, frame->timestamp);
	return frame;
}

static struct obs_source_frame *sr_capture_filter_video(void *data, struct obs_source_frame *frame)
{
	struct sr_capture *c = data;
	pthread_mutex_lock(&c->state_mutex);
	frame = sr_capture_filter_video_impl(data, frame);
	pthread_mutex_unlock(&c->state_mutex);
	return frame;
}

static struct obs_audio_data *sr_capture_filter_audio_impl(void *data, struct obs_audio_data *audio)
{
	struct sr_capture *c = data;

	if (!c->buffer.samples_per_sec) {
		struct obs_audio_info oai;
		if (obs_get_audio_info(&oai)) {
			pthread_mutex_lock(&c->buffer.mutex);
			c->buffer.samples_per_sec = oai.samples_per_sec;
			c->buffer.speakers = oai.speakers;
			pthread_mutex_unlock(&c->buffer.mutex);
		}
	}

	if (c->buffer.samples_per_sec)
		sr_buffer_push_audio(&c->buffer, audio, get_audio_channels(c->buffer.speakers));

	return audio;
}

static struct obs_audio_data *sr_capture_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct sr_capture *c = data;
	pthread_mutex_lock(&c->state_mutex);
	if (audio)
		audio = sr_capture_filter_audio_impl(data, audio);
	pthread_mutex_unlock(&c->state_mutex);
	return audio;
}

bool sr_capture_snapshot_at(void *data, struct sr_replay *out, uint64_t cutoff_ns, uint64_t duration_ns)
{
	struct sr_capture *c = data;
	pthread_mutex_lock(&c->state_mutex);
	bool ok = sr_buffer_snapshot_at(&c->buffer, out, cutoff_ns, duration_ns);
	pthread_mutex_unlock(&c->state_mutex);
	return ok;
}

obs_data_t *sr_capture_health(void *data)
{
	struct sr_capture *c = data;
	obs_data_t *d = obs_data_create();
	pthread_mutex_lock(&c->state_mutex);
	uint64_t now = os_gettime_ns();
	obs_data_set_string(d, "capture_id", c->capture_id);
	obs_source_t *parent = obs_filter_get_parent(c->self);
	obs_data_set_string(d, "source_name", parent ? obs_source_get_name(parent) : "");
	obs_data_set_string(d, "filter_name", obs_source_get_name(c->self));
	obs_data_set_int(d, "last_frame_age_ms",
			 c->last_arrival_ns ? (int64_t)((now - c->last_arrival_ns) / 1000000) : -1);
	obs_data_set_int(d, "last_packet_age_ms",
			 c->last_packet_ns ? (int64_t)((now - c->last_packet_ns) / 1000000) : -1);
	obs_data_set_int(d, "clock_resets", (int64_t)c->clock_resets);
	obs_data_set_int(d, "last_reset_monotonic_ns", (int64_t)c->last_reset_ns);
	static const char *backends[] = {"auto", "h264_nvenc", "h264_amf", "h264_qsv", "libx264"};
	const char *requested = c->backend >= 0 && c->backend <= SR_ENC_X264 ? backends[c->backend] : "auto";
	const char *actual = c->encoder ? sr_encoder_name(c->encoder) : "";
	obs_data_set_string(d, "encoder_requested", requested);
	obs_data_set_string(d, "encoder_actual", actual);
	obs_data_set_string(d, "encoder_error", c->encoder_failed ? "ENCODER_UNAVAILABLE" : "");
	obs_data_set_int(d, "capture_fps", c->capture_fps);
	pthread_mutex_lock(&c->buffer.mutex);
	size_t count = c->buffer.video.size / sizeof(struct sr_packet);
	uint64_t span = 0;
	size_t memory = 0;
	if (count) {
		struct sr_packet first, last;
		deque_peek_front(&c->buffer.video, &first, sizeof(first));
		deque_peek_back(&c->buffer.video, &last, sizeof(last));
		span = last.ts >= first.ts ? last.ts - first.ts : 0;
	}
	for (size_t i = 0; i < count; i++) {
		struct sr_packet *packet = deque_data(&c->buffer.video, i * sizeof(struct sr_packet));
		memory += (size_t)packet->pkt->size;
	}
	for (size_t i = 0; i < c->buffer.audio.size / sizeof(struct sr_audio_chunk); i++) {
		struct sr_audio_chunk *chunk = deque_data(&c->buffer.audio, i * sizeof(*chunk));
		for (size_t ch = 0; ch < MAX_AV_PLANES; ch++)
			if (chunk->data[ch])
				memory += chunk->frames * sizeof(float);
	}
	obs_data_set_int(d, "buffer_frames", (int64_t)count);
	obs_data_set_int(d, "buffer_available_ms", (int64_t)(span / 1000000));
	obs_data_set_int(d, "buffer_duration_ms", (int64_t)(c->buffer.duration_ns / 1000000));
	obs_data_set_int(d, "memory_bytes", (int64_t)memory);
	obs_data_set_int(d, "width", c->buffer.width);
	obs_data_set_int(d, "height", c->buffer.height);
	obs_data_set_double(d, "fps", span && count > 1 ? (double)(count - 1) * 1e9 / (double)span : 0.0);
	pthread_mutex_unlock(&c->buffer.mutex);
	const char *status = c->encoder_failed                                                         ? "ERROR"
			     : !c->last_arrival_ns || now - c->last_arrival_ns > 5000000000ULL         ? "OFFLINE"
			     : !count || !c->last_packet_ns || now - c->last_packet_ns > 5000000000ULL ? "ERROR"
			     : c->backend != SR_ENC_AUTO && strcmp(requested, actual)                  ? "DEGRADED"
												       : "OK";
	obs_data_set_string(d, "status", status);
	pthread_mutex_unlock(&c->state_mutex);
	return d;
}

static obs_properties_t *sr_capture_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_int(props, S_DURATION, obs_module_text("Duration"), 1000, 120000, 500);
	obs_property_int_set_suffix(p, " ms");

	p = obs_properties_add_list(props, S_ENCODER, obs_module_text("Encoder"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Encoder.Auto"), SR_ENC_AUTO);
	obs_property_list_add_int(p, obs_module_text("Encoder.NVENC"), SR_ENC_NVENC);
	obs_property_list_add_int(p, obs_module_text("Encoder.AMF"), SR_ENC_AMF);
	obs_property_list_add_int(p, obs_module_text("Encoder.QSV"), SR_ENC_QSV);
	obs_property_list_add_int(p, obs_module_text("Encoder.X264"), SR_ENC_X264);

	p = obs_properties_add_list(props, S_QUALITY, obs_module_text("Quality"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Quality.High"), 18);
	obs_property_list_add_int(p, obs_module_text("Quality.Medium"), 23);
	obs_property_list_add_int(p, obs_module_text("Quality.Low"), 28);

	p = obs_properties_add_list(props, S_FPS, obs_module_text("CaptureFps"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("CaptureFps.Auto"), 0);
	obs_property_list_add_int(p, "60", 60);
	obs_property_list_add_int(p, "50", 50);
	obs_property_list_add_int(p, "30", 30);
	obs_property_list_add_int(p, "25", 25);
	obs_property_set_long_description(p, obs_module_text("CaptureFps.Tip"));

	p = obs_properties_add_list(props, S_KEYINT, obs_module_text("Keyint"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Keyint.All"), 1);
	obs_property_list_add_int(p, obs_module_text("Keyint.Balanced"), 15);
	obs_property_list_add_int(p, obs_module_text("Keyint.Light"), 30);
	obs_property_set_long_description(p, obs_module_text("Keyint.Tip"));

	char credit[256];
	obs_properties_add_text(props, "sr_credit", sr_plugin_credit_html(credit, sizeof(credit)), OBS_TEXT_INFO);

	return props;
}

static void sr_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_DURATION, 15000);
	obs_data_set_default_int(settings, S_ENCODER, SR_ENC_AUTO);
	obs_data_set_default_int(settings, S_QUALITY, 23);
	obs_data_set_default_int(settings, S_FPS, 0);
	obs_data_set_default_int(settings, S_KEYINT, SR_DEFAULT_KEYINT);
}

struct obs_source_info sr_capture_info = {
	.id = SR_CAPTURE_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = sr_capture_get_name,
	.create = sr_capture_create,
	.destroy = sr_capture_destroy,
	.update = sr_capture_update,
	.get_defaults = sr_capture_defaults,
	.get_properties = sr_capture_properties,
	.filter_video = sr_capture_filter_video,
	.filter_audio = sr_capture_filter_audio,
};
