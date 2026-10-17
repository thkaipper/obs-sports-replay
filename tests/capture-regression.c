/* Real capture state machine with controllable codec outcomes. GPL-2.0-or-later. */
#include <obs-module.h>
#include "sr-codec.h"
#include <util/platform.h>
static int encoder_token, creates, encodes, destroys;
static int codec_mode; /* 0 packet; 1 EAGAIN; 2 fatal error */
static struct sr_encoder *test_create(const char *n, uint32_t w, uint32_t h, uint32_t fn, uint32_t fd,
				      enum sr_encoder_backend b, int q, int k)
{
	(void)n;
	(void)w;
	(void)h;
	(void)fn;
	(void)fd;
	(void)b;
	(void)q;
	(void)k;
	creates++;
	return (struct sr_encoder *)&encoder_token;
}
static void test_destroy(struct sr_encoder *e)
{
	if (e)
		destroys++;
}
static AVPacket *test_encode(struct sr_encoder *e, const struct obs_source_frame *f)
{
	(void)e;
	encodes++;
	if (codec_mode)
		return NULL;
	AVPacket *p = av_packet_alloc();
	p->pts = (int64_t)f->timestamp;
	p->flags = AV_PKT_FLAG_KEY;
	return p;
}
static bool test_failed(const struct sr_encoder *e)
{
	(void)e;
	return codec_mode == 2;
}
static enum AVCodecID test_codec(const struct sr_encoder *e)
{
	(void)e;
	return AV_CODEC_ID_H264;
}
static const char *test_name(const struct sr_encoder *e)
{
	(void)e;
	return "libx264";
}
static void test_extra(const struct sr_encoder *e, const uint8_t **d, int *s)
{
	(void)e;
	*d = NULL;
	*s = 0;
}
#define sr_encoder_create test_create
#define sr_encoder_destroy test_destroy
#define sr_encoder_encode test_encode
#define sr_encoder_failed test_failed
#define sr_encoder_codec_id test_codec
#define sr_encoder_name test_name
#define sr_encoder_get_extradata test_extra
#include "../src/capture-filter.c"
const char *obs_module_text(const char *key)
{
	return key;
}
char *sr_integration_register_capture(obs_source_t *s, const char *id)
{
	(void)s;
	(void)id;
	return bstrdup("test");
}
void sr_integration_unregister_capture(obs_source_t *s)
{
	(void)s;
}
static void check(bool ok, const char *message)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
	fflush(stdout);
	if (!ok)
		exit(2);
}
static const char *camera_name(void *unused)
{
	(void)unused;
	return "Regression camera";
}
static void *camera_create(obs_data_t *settings, obs_source_t *s)
{
	(void)settings;
	return s;
}
static void camera_destroy(void *unused)
{
	(void)unused;
}
int main(void)
{
	obs_data_t *h = sr_capture_health(NULL);
	check(!strcmp(obs_data_get_string(h, "status"), "OFFLINE"), "uninitialized filter health is safe");
	obs_data_release(h);
	struct sr_replay snapshot;
	check(!sr_capture_snapshot_at(NULL, &snapshot, 0, 0) && !snapshot.video.num,
	      "uninitialized filter snapshot is safe");
	check(obs_startup("en-US", NULL, NULL), "isolated capture context starts");
	struct obs_source_info info = {0};
	info.id = "regression-camera";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	info.get_name = camera_name;
	info.create = camera_create;
	info.destroy = camera_destroy;
	obs_register_source(&info);
	struct sr_capture c = {0};
	c.self = obs_source_create(info.id, "Capture regression", NULL, NULL);
	c.capture_id = bstrdup("regression");
	pthread_mutex_init(&c.state_mutex, NULL);
	sr_buffer_init(&c.buffer);
	c.buffer.duration_ns = 1000000000ULL;
	c.backend = SR_ENC_X264;
	c.capture_fps = 30;
	uint8_t pixels[4096] = {0};
	struct obs_source_frame frame = {0};
	frame.data[0] = pixels;
	frame.width = frame.height = 64;
	frame.timestamp = 1000000000ULL;
	sr_capture_filter_video(&c, &frame);
	check(creates == 1 && encodes == 1 && c.buffer.video.size, "first valid frame creates an encoder and buffer");
	for (int i = 0; i < 1000; i++)
		sr_capture_filter_video(&c, &frame);
	check(encodes == 1 && c.buffer.video.size == sizeof(struct sr_packet),
	      "frozen source clock does not encode duplicates");
	codec_mode = 1;
	frame.timestamp += 33333333;
	sr_capture_filter_video(&c, &frame);
	check(c.encoder && !c.encoder_failed && destroys == 0, "temporary codec buffering does not trigger recovery");
	codec_mode = 2;
	frame.timestamp += 33333333;
	sr_capture_filter_video(&c, &frame);
	check(!c.encoder && c.encoder_failed && destroys == 1 && !c.buffer.video.size,
	      "fatal codec error releases the encoder and stale GOP");
	frame.timestamp += 33333333;
	sr_capture_filter_video(&c, &frame);
	check(creates == 1, "encoder recovery observes its retry delay");
	c.encoder_failed_since_ns = os_gettime_ns() - 11000000000ULL;
	codec_mode = 0;
	frame.timestamp += 33333333;
	sr_capture_filter_video(&c, &frame);
	check(creates == 2 && c.encoder && !c.encoder_failed && c.buffer.video.size,
	      "encoder recovers automatically after the delay");
	c.last_packet_ns = os_gettime_ns() - 11000000000ULL;
	codec_mode = 1;
	frame.timestamp += 33333333;
	sr_capture_filter_video(&c, &frame);
	check(c.encoder_failed && !c.encoder,
	      "watchdog recovers a codec that stops producing packets without an error");
	c.encoder_failed_since_ns = os_gettime_ns() - 11000000000ULL;
	codec_mode = 0;
	frame.timestamp += 33333333;
	sr_capture_filter_video(&c, &frame);
	frame.timestamp = 100;
	sr_capture_filter_video(&c, &frame);
	check(c.clock_resets == 1 && c.buffer.video.size == sizeof(struct sr_packet),
	      "backward clock reset retains only the new timeline");
	c.frame_interval_ns = 33333333;
	c.have_last_kept = false;
	int kept = 0;
	for (int i = 0; i < 60; i++)
		kept += sr_capture_keep_frame(&c, (uint64_t)i * 16666667ULL);
	check(kept == 30, "60 to 30 fps capture limiter remains stable");
	test_destroy(c.encoder);
	sr_buffer_free(&c.buffer);
	pthread_mutex_destroy(&c.state_mutex);
	bfree(c.capture_id);
	obs_source_release(c.self);
	obs_wait_for_destroy_queue();
	obs_shutdown();
	return 0;
}
