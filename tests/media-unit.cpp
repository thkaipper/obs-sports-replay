/* Sports Replay media validation. GPL-2.0-or-later. */
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#include "sr-buffer.h"
#include "sr-codec.h"
#include "sr-load.h"
#include "sr-save.h"
#include "sr-clip.h"
#include <util/platform.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <cmath>
#include <cstdio>
#include <vector>
static void check(bool ok, const char *message)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
	fflush(stdout);
	if (!ok)
		std::exit(2);
}
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	check(argc == 2, "test output directory provided");
	QString root = QString::fromUtf8(argv[1]);
	QDir().mkpath(root);
	sr_buffer buffer;
	sr_buffer_init(&buffer);
	buffer.duration_ns = 60000000000ULL;
	buffer.width = buffer.height = 64;
	buffer.codec_id = AV_CODEC_ID_H264;
	buffer.samples_per_sec = 48000;
	buffer.speakers = SPEAKERS_STEREO;
	sr_encoder *encoder = sr_encoder_create("unit", 64, 64, 30, 1, SR_ENC_X264, 18, 15);
	check(encoder != nullptr, "x264 encoder creates");
	const uint8_t *extra;
	int extra_size;
	sr_encoder_get_extradata(encoder, &extra, &extra_size);
	sr_buffer_set_extradata(&buffer, extra, extra_size);
	std::vector<uint8_t> y(4096), uv(1024, 128);
	std::vector<float> left(1600), right(1600);
	uint64_t base = os_gettime_ns(), cutoff = 0;
	for (int i = 0; i < 75; i++) {
		std::fill(y.begin(), y.end(), static_cast<uint8_t>(16 + i * 2));
		obs_source_frame frame = {};
		frame.width = frame.height = 64;
		frame.format = VIDEO_FORMAT_I420;
		frame.data[0] = y.data();
		frame.data[1] = frame.data[2] = uv.data();
		frame.linesize[0] = 64;
		frame.linesize[1] = frame.linesize[2] = 32;
		frame.timestamp = base + static_cast<uint64_t>(i) * 33333333ULL;
		AVPacket *packet = sr_encoder_encode(encoder, &frame);
		check(packet != nullptr && static_cast<uint64_t>(packet->pts) == frame.timestamp,
		      "encoded packet preserves source timestamp");
		sr_buffer_push_video(&buffer, packet, static_cast<uint64_t>(packet->pts));
		for (int j = 0; j < 1600; j++)
			left[j] = right[j] =
				static_cast<float>(0.1 * sin((i * 1600 + j) * 2 * 3.141592653589793 * 440 / 48000));
		obs_audio_data audio = {};
		audio.frames = 1600;
		audio.timestamp = frame.timestamp;
		audio.data[0] = reinterpret_cast<uint8_t *>(left.data());
		audio.data[1] = reinterpret_cast<uint8_t *>(right.data());
		sr_buffer_push_audio(&buffer, &audio, 2);
		if (i == 30)
			cutoff = os_gettime_ns();
	}
	sr_replay at = {}, crop = {}, full = {};
	check(sr_buffer_snapshot_at(&buffer, &at, cutoff, 0) && at.video.num == 31,
	      "common cutoff excludes later arrivals");
	check(sr_buffer_snapshot_at(&buffer, &crop, 0, 1000000000ULL), "duration snapshot succeeds");
	check(crop.video.array[0].pkt->flags & AV_PKT_FLAG_KEY, "duration snapshot starts on a keyframe");
	check(crop.last_ts - crop.first_ts <= 1500000000ULL, "GOP-aligned crop is bounded");
	check(sr_buffer_snapshot(&buffer, &full), "full snapshot succeeds");
	sr_buffer_clear(&buffer);
	QString path = QDir(root).filePath("audio.mp4");
	QByteArray utf8 = path.toUtf8();
	check(sr_save_replay_ex(&full, utf8.constData(), true) == nullptr,
	      "snapshot remains valid after ring is cleared; AAC save succeeds");
	check(!QFile::exists(path + ".partial"), "successful publication leaves no partial");
	QFile before(path);
	check(before.open(QIODevice::ReadOnly), "published replay readable");
	QByteArray original = before.readAll();
	before.close();
	const char *error = sr_save_replay_ex(&full, utf8.constData(), true);
	check(error && !strcmp(error, "FILE_EXISTS"), "atomic saver refuses to overwrite existing replay");
	QFile after(path);
	check(after.open(QIODevice::ReadOnly) && after.readAll() == original, "existing replay unchanged");
	after.close();
	QByteArray invalid = QDir(root).filePath("missing/sub/file.mp4").toUtf8();
	error = sr_save_replay_ex(&full, invalid.constData(), true);
	check(error && !strcmp(error, "SAVE_DIR_UNWRITABLE") && !QFile::exists(QString::fromUtf8(invalid)),
	      "write failure never exposes final MP4");
	sr_replay loaded = {};
	check(sr_load_replay(utf8.constData(), &loaded) && loaded.audio.num > 0,
	      "saved AAC reloads for manual playback");
	double energy = 0;
	for (size_t i = 0; i < loaded.audio.num; i++)
		for (uint32_t j = 0; j < loaded.audio.array[i].frames; j++) {
			double sample = loaded.audio.array[i].data[0][j];
			energy += sample * sample;
		}
	check(energy > 1, "reloaded audio retains captured signal");
	AVFormatContext *format = nullptr;
	check(avformat_open_input(&format, utf8.constData(), nullptr, nullptr) == 0 &&
		      avformat_find_stream_info(format, nullptr) == 0,
	      "saved MP4 validates with libavformat");
	int vi = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0),
	    ai = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	check(vi >= 0 && ai >= 0, "both tracks present");
	double vd = format->streams[vi]->duration * av_q2d(format->streams[vi]->time_base),
	       ad = format->streams[ai]->duration * av_q2d(format->streams[ai]->time_base);
	check(fabs(vd - ad) < 0.05, "AAC and video durations differ by less than 50ms");
	avformat_close_input(&format);
	sr_clip *clip = sr_clip_open(utf8.constData());
	check(clip != nullptr, "intro clip opens");
	sr_clip_rewind(clip);
	AVFrame *shown = nullptr;
	bool ended = false;
	check(sr_clip_advance(clip, 0, &shown, &ended) && shown && abs(shown->data[0][0] - 16) <= 1,
	      "intro shows first frame rather than overwriting it with lookahead");
	sr_clip_close(clip);
	sr_replay_free(&loaded);
	sr_replay_free(&at);
	sr_replay_free(&crop);
	sr_replay_free(&full);
	sr_buffer_free(&buffer);
	sr_encoder_destroy(encoder);
	return 0;
}
