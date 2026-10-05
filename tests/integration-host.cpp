/* Sports Replay test host. GPL-2.0-or-later.
 * Loads the actual plugin DLL into isolated libobs with a public Vendor API
 * proc-handler adapter. No installed plugin or production OBS config is changed. */
#include <obs-module.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "obs-websocket-api.h"
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QThread>
#include <QDateTime>
#include <QCryptographicHash>
#include <cstdio>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>
#include <util/platform.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
static proc_handler_t *handler;
static std::map<std::string, obs_websocket_request_callback> callbacks;
static std::mutex events_mutex;
static std::vector<QJsonObject> events;
static void ensure(bool ok, const char *message)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", message);
		std::exit(2);
	}
	fprintf(stdout, "PASS: %s\n", message);
	fflush(stdout);
}
static void get_handler(void *, calldata_t *cd)
{
	calldata_set_ptr(cd, "ph", handler);
}
static void register_vendor(void *, calldata_t *cd)
{
	calldata_set_ptr(cd, "vendor", handler);
}
static void register_request(void *, calldata_t *cd)
{
	auto *cb = static_cast<obs_websocket_request_callback *>(calldata_ptr(cd, "callback"));
	callbacks[calldata_string(cd, "type")] = *cb;
	calldata_set_bool(cd, "success", true);
}
static void unregister_request(void *, calldata_t *cd)
{
	callbacks.erase(calldata_string(cd, "type"));
	calldata_set_bool(cd, "success", true);
}
static void event(void *, calldata_t *cd)
{
	obs_data_t *data = static_cast<obs_data_t *>(calldata_ptr(cd, "data"));
	QJsonObject value = QJsonDocument::fromJson(QByteArray(obs_data_get_json(data))).object();
	value["event_type"] = calldata_string(cd, "type");
	std::lock_guard<std::mutex> lock(events_mutex);
	events.push_back(value);
	calldata_set_bool(cd, "success", true);
}
static QJsonObject request(const char *type, QJsonObject data = {})
{
	ensure(callbacks.count(type) != 0, type);
	QByteArray json = QJsonDocument(data).toJson(QJsonDocument::Compact);
	obs_data_t *input = obs_data_create_from_json(json.constData()), *output = obs_data_create();
	auto cb = callbacks.at(type);
	cb.callback(input, output, cb.priv_data);
	QJsonObject result = QJsonDocument::fromJson(QByteArray(obs_data_get_json(output))).object();
	obs_data_release(input);
	obs_data_release(output);
	return result;
}
static QJsonObject await_saved(const QString &event_id, const QString &id)
{
	for (int i = 0; i < 200; i++) {
		QJsonObject status = request("GetReplayStatus", {{"event_id", event_id}, {"capture_id", id}});
		if (status["status"] == "SAVED" || status["status"] == "FAILED")
			return status;
		QThread::msleep(25);
	}
	return {};
}
static const char *source_name(void *)
{
	return "Integration test camera";
}
static void *source_create(obs_data_t *, obs_source_t *source)
{
	return source;
}
static void source_destroy(void *) {}
static obs_source_t *camera(const char *name, obs_source_t **filter)
{
	obs_source_t *source = obs_source_create("sr-test-camera", name, nullptr, nullptr);
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "capture_fps", 30);
	obs_data_set_int(settings, "encoder", 4);
	obs_data_set_int(settings, "duration_ms", 60000);
	obs_data_set_int(settings, "keyint", 15);
	*filter = obs_source_create("sports_replay_capture", "Capture", settings, nullptr);
	obs_data_release(settings);
	obs_source_filter_add(source, *filter);
	return source;
}
static void feed(obs_source_t *source, uint64_t base)
{
	std::vector<uint8_t> y(64 * 64, 128), uv(32 * 32, 128);
	std::vector<float> left(1600), right(1600);
	for (int i = 0; i < 75; i++) {
		obs_source_frame frame = {};
		frame.format = VIDEO_FORMAT_I420;
		frame.width = frame.height = 64;
		frame.data[0] = y.data();
		frame.data[1] = frame.data[2] = uv.data();
		frame.linesize[0] = 64;
		frame.linesize[1] = frame.linesize[2] = 32;
		frame.timestamp = base + static_cast<uint64_t>(i) * 33333333ULL;
		obs_source_output_video(source, &frame);
		for (int j = 0; j < 1600; j++)
			left[j] = right[j] =
				static_cast<float>(0.1 * sin((i * 1600 + j) * 2 * 3.141592653589793 * 440 / 48000));
		obs_source_audio audio = {};
		audio.data[0] = reinterpret_cast<uint8_t *>(left.data());
		audio.data[1] = reinterpret_cast<uint8_t *>(right.data());
		audio.frames = 1600;
		audio.speakers = SPEAKERS_STEREO;
		audio.samples_per_sec = 48000;
		audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
		audio.timestamp = frame.timestamp;
		obs_source_output_audio(source, &audio);
		QThread::msleep(34);
	}
}
int main(int argc, char **argv)
{
#ifdef _WIN32
	AddDllDirectory(LR"(C:\Program Files\obs-studio\bin\64bit)");
#endif
	QApplication app(argc, argv);
	ensure(argc >= 4, "arguments: DLL, data, isolated root");
	QString root = QString::fromUtf8(argv[3]);
	QDir().mkpath(root);
	ensure(obs_startup("en-US", root.toUtf8().constData(), nullptr), "isolated libobs startup");
	obs_audio_info audio = {};
	audio.samples_per_sec = 48000;
	audio.speakers = SPEAKERS_STEREO;
	ensure(obs_reset_audio(&audio), "48kHz stereo audio");
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
	obs_add_data_path("C:/Program Files/obs-studio/data/libobs/");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
	obs_video_info video = {};
	video.graphics_module = "C:/Program Files/obs-studio/bin/64bit/libobs-d3d11.dll";
	video.fps_num = 30;
	video.fps_den = 1;
	video.base_width = video.base_height = video.output_width = video.output_height = 64;
	video.output_format = VIDEO_FORMAT_NV12;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_PARTIAL;
	ensure(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "isolated 64x64 video clock");
	handler = proc_handler_create();
	proc_handler_add(obs_get_proc_handler(), "void obs_websocket_api_get_ph(out ptr ph)", get_handler, nullptr);
	proc_handler_add(handler, "void vendor_register(string name, out ptr vendor)", register_vendor, nullptr);
	proc_handler_add(handler,
			 "void vendor_request_register(ptr vendor, string type, ptr callback, out bool success)",
			 register_request, nullptr);
	proc_handler_add(handler, "void vendor_request_unregister(ptr vendor, string type, out bool success)",
			 unregister_request, nullptr);
	proc_handler_add(handler, "void vendor_event_emit(ptr vendor, string type, ptr data, out bool success)", event,
			 nullptr);
	QString config = QDir(root).filePath("sports-replay");
	QDir().mkpath(config);
	QFile cfg(QDir(config).filePath("config.json"));
	ensure(cfg.open(QIODevice::WriteOnly), "isolated plugin config");
	cfg.write(QJsonDocument(QJsonObject{{"save_dir", QDir(root).filePath("replays")}}).toJson());
	cfg.close();
	obs_module_t *module = nullptr;
	ensure(obs_open_module(&module, argv[1], argv[2]) == MODULE_SUCCESS, "DLL loads with OBS 32.2.2 dependencies");
	ensure(obs_init_module(module), "plugin module initializes");
	obs_post_load_modules();
	if (argc > 4 && !strcmp(argv[4], "recover")) {
		QFile report(QDir(root).filePath("test-report.json"));
		ensure(report.open(QIODevice::ReadOnly), "previous report exists");
		QJsonObject old = QJsonDocument::fromJson(report.readAll()).object();
		QJsonObject saved = old["camera_a"].toObject();
		QString id = saved["capture_id"].toString();
		ensure(request("GetPluginInfo")["session_id"] != old["health"].toObject()["session_id"],
		       "session ID changes after process restart");
		QJsonObject existing = request("GetReplayStatus", {{"event_id", "test-event-1"}, {"capture_id", id}});
		ensure(existing["status"] == "SAVED" && existing["recovered"].toBool(),
		       "published file recovers after status-write interruption");
		QJsonObject again = request("CaptureEvent", {{"event_id", "test-event-1"},
							     {"duration_ms", 1000},
							     {"save_audio", true},
							     {"cameras", QJsonArray{QJsonObject{{"capture_id", id}}}}});
		ensure(again["cameras"].toArray()[0].toObject()["result_code"] == "ALREADY_SAVED",
		       "persistent deduplication works without camera or old buffer");
		QJsonObject interrupted =
			request("GetReplayStatus", {{"event_id", "test-interrupted"}, {"capture_id", id}});
		ensure(interrupted["status"] == "FAILED" && interrupted["error_code"] == "INTERRUPTED",
		       "accepted snapshot lost after crash becomes INTERRUPTED");
		ensure(!QFile::exists(QDir(root).filePath("replays/interrupted.mp4.partial")),
		       "abandoned partial recovered and removed");
		obs_shutdown();
		ensure(callbacks.empty(), "recovery shutdown clean");
		proc_handler_destroy(handler);
		return 0;
	}
	ensure(callbacks.size() == 6, "all six Vendor Requests registered");
	obs_source_info info = {};
	info.id = "sr-test-camera";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO;
	info.get_name = source_name;
	info.create = source_create;
	info.destroy = source_destroy;
	obs_register_source(&info);
	obs_source_t *fa, *fb;
	obs_source_t *a = camera("CAM A", &fa), *b = camera("CAM B", &fb);
	ensure(a && b && fa && fb, "two capture filters without playback sources");
	obs_data_t *sa = obs_source_get_settings(fa), *sb = obs_source_get_settings(fb);
	QString ida = obs_data_get_string(sa, "capture_id"), idb = obs_data_get_string(sb, "capture_id");
	ensure(!ida.isEmpty() && ida != idb, "distinct persistent capture UUIDs");
	obs_data_release(sa);
	obs_data_release(sb);
	uint64_t base = os_gettime_ns();
	feed(a, base);
	feed(b, os_gettime_ns());
	QThread::msleep(100);
	QJsonArray list = request("ListCaptureSources")["capture_sources"].toArray();
	ensure(list.size() == 2 && list[0].toObject()["buffer_frames"].toInt() > 0,
	       "direct capture registry reports encoded frames");
	obs_source_set_name(a, "CAM A renamed");
	QJsonObject health = request("GetCaptureSource", {{"capture_id", ida}});
	ensure(health["source_name"] == "CAM A renamed", "rename preserves capture identity");
	// A copied/imported settings object must not duplicate an active filter ID.
	obs_data_t *copy_settings = obs_source_get_settings(fa);
	obs_source_t *copy = obs_source_create("sports_replay_capture", "Copy", copy_settings, nullptr);
	obs_data_t *copied = obs_source_get_settings(copy);
	ensure(QString::fromUtf8(obs_data_get_string(copied, "capture_id")) != ida,
	       "duplicated filter receives new capture ID");
	obs_data_release(copied);
	obs_data_release(copy_settings);
	obs_source_release(copy);
	QJsonObject capture = {{"event_id", "test-event-1"},
			       {"duration_ms", 1000},
			       {"save_audio", true},
			       {"cameras",
				QJsonArray{QJsonObject{{"capture_id", ida}}, QJsonObject{{"capture_id", idb}}}}};
	QJsonObject accepted = request("CaptureEvent", capture);
	ensure(accepted["accepted"].toBool(), "multicamera CaptureEvent accepted");
	QJsonObject duplicate = request("CaptureEvent", capture);
	ensure(duplicate["cameras"].toArray()[0].toObject()["duplicate"].toBool(),
	       "duplicate event deduplicates per camera");
	QJsonObject saved_a = await_saved("test-event-1", ida), saved_b = await_saved("test-event-1", idb);
	ensure(saved_a["status"] == "SAVED" && saved_b["status"] == "SAVED", "both asynchronous saves finish");
	ensure(!QFile::exists(saved_a["path"].toString() + ".partial"), "partial is absent after publication");
	ensure(QFileInfo(saved_a["path"].toString()).size() > 0, "final MP4 exists");
	AVFormatContext *format = nullptr;
	QByteArray path = saved_a["path"].toString().toUtf8();
	ensure(avformat_open_input(&format, path.constData(), nullptr, nullptr) == 0 &&
		       avformat_find_stream_info(format, nullptr) == 0,
	       "saved MP4 demuxes successfully");
	ensure(av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) >= 0 &&
		       av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0,
	       "MP4 contains H264 and AAC audio");
	// Decode audio, not merely its stream header: silence would hide timestamp bugs.
	int ai = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	const AVCodec *ac = avcodec_find_decoder(format->streams[ai]->codecpar->codec_id);
	AVCodecContext *ad = avcodec_alloc_context3(ac);
	avcodec_parameters_to_context(ad, format->streams[ai]->codecpar);
	ensure(avcodec_open2(ad, ac, nullptr) == 0, "AAC decoder opens");
	AVPacket *ap = av_packet_alloc();
	AVFrame *af = av_frame_alloc();
	double energy = 0;
	while (av_read_frame(format, ap) >= 0) {
		if (ap->stream_index == ai && avcodec_send_packet(ad, ap) == 0)
			while (avcodec_receive_frame(ad, af) == 0) {
				if (af->format == AV_SAMPLE_FMT_FLTP)
					for (int sample = 0; sample < af->nb_samples; sample++) {
						double v = reinterpret_cast<float *>(af->data[0])[sample];
						energy += v * v;
					}
			}
		av_packet_unref(ap);
	}
	ensure(energy > 1.0, "saved AAC contains captured signal");
	av_packet_free(&ap);
	av_frame_free(&af);
	avcodec_free_context(&ad);
	avformat_close_input(&format);
	ensure(saved_a["actual_duration_ms"].toDouble() >= 1000 && saved_a["actual_duration_ms"].toDouble() <= 1600,
	       "duration selection includes at most one extra GOP");
	capture["duration_ms"] = 2000;
	QJsonObject conflict = request("CaptureEvent", capture);
	ensure(conflict["cameras"].toArray()[0].toObject()["error_code"] == "REQUEST_CONFLICT",
	       "duplicate with changed parameters rejected");
	QJsonObject invalid =
		request("CaptureEvent", {{"event_id", ""}, {"cameras", QJsonArray{QJsonObject{{"capture_id", ida}}}}});
	ensure(invalid["error_code"] == "INVALID_EVENT_ID", "invalid event ID rejected");
	ensure(QDir(QDir(root).filePath("replays")).entryList({"*.mp4"}, QDir::Files).size() == 2,
	       "exactly one file per event/camera");
	QJsonObject silent = request("CaptureEvent", {{"event_id", "test-video-only"},
						      {"duration_ms", 1000},
						      {"save_audio", false},
						      {"cameras", QJsonArray{QJsonObject{{"capture_id", idb}}}}});
	ensure(silent["accepted"].toBool(), "video-only request accepted");
	QJsonObject noaudio = await_saved("test-video-only", idb);
	QByteArray silentpath = noaudio["path"].toString().toUtf8();
	ensure(avformat_open_input(&format, silentpath.constData(), nullptr, nullptr) == 0, "video-only MP4 opens");
	ensure(av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) < 0,
	       "save_audio=false produces no audio track");
	avformat_close_input(&format);
	// Refresh the finite test stream before checking per-camera isolation.
	feed(b, os_gettime_ns());
	QJsonObject badcamera =
		request("CaptureEvent",
			{{"event_id", "test-mixed"},
			 {"save_audio", false},
			 {"cameras", QJsonArray{QJsonObject{{"capture_id", idb}},
						QJsonObject{{"capture_id", "550e8400-e29b-41d4-a716-446655440001"}}}}});
	fprintf(stderr, "Mixed response: %s\n", QJsonDocument(badcamera).toJson(QJsonDocument::Compact).constData());
	ensure(badcamera["accepted"].toBool() &&
		       badcamera["cameras"].toArray()[1].toObject()["error_code"] == "CAPTURE_NOT_FOUND",
	       "one missing camera does not cancel another");
	ensure(await_saved("test-mixed", idb)["status"] == "SAVED",
	       "healthy camera saves despite another camera failure");
	obs_data_t *play_settings = obs_data_create();
	obs_data_set_string(play_settings, "capture_source", "CAM B");
	obs_data_set_string(play_settings, "capture_id", idb.toUtf8().constData());
	obs_data_set_bool(play_settings, "autoplay", false);
	obs_data_t *legacy = obs_data_create();
	obs_data_array_t *bindings = obs_data_array_create();
	obs_data_t *binding = obs_data_create();
	obs_data_set_string(binding, "key", "OBS_KEY_F9");
	obs_data_array_push_back(bindings, binding);
	obs_data_set_array(legacy, "SportsReplay.CaptureOnly", bindings);
	obs_source_t *playback = obs_source_create("sports_replay", "Playback B", play_settings, legacy);
	obs_data_t *keys = obs_hotkeys_save_source(playback);
	QString unique = "SportsReplay." + QString::fromUtf8(obs_source_get_uuid(playback)) + ".CaptureOnly";
	obs_data_array_t *migrated = obs_data_get_array(keys, unique.toUtf8().constData());
	ensure(migrated && obs_data_array_count(migrated) == 1, "manual hotkey has unique ID and migrates F9 binding");
	obs_data_array_release(migrated);
	obs_data_release(keys);
	obs_data_release(binding);
	obs_data_array_release(bindings);
	obs_data_release(legacy);
	obs_data_release(play_settings);
	obs_source_release(playback);
	QFile report(QDir(root).filePath("test-report.json"));
	ensure(report.open(QIODevice::WriteOnly), "test report writable");
	report.write(
		QJsonDocument(
			QJsonObject{{"health", request("GetHealth")}, {"camera_a", saved_a}, {"camera_b", saved_b}})
			.toJson());
	report.close();
	// Simulate a crash after rename but before terminal status persistence.
	QString key = QFileInfo(saved_a["path"].toString()).baseName().mid(7);
	QJsonObject stale = saved_a;
	stale["status"] = "ACCEPTED";
	QFile journal(QDir(config).filePath("integration-jobs/" + key + ".json"));
	ensure(journal.open(QIODevice::WriteOnly), "recovery fixture writable");
	journal.write(QJsonDocument(stale).toJson());
	journal.close();
	QByteArray material = QByteArray("test-interrupted");
	material.append('\0');
	material.append(ida.toUtf8());
	QString interruptedKey =
		QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
	stale["event_id"] = "test-interrupted";
	stale["path"] = QDir(root).filePath("replays/interrupted.mp4");
	QFile interruptedJournal(QDir(config).filePath("integration-jobs/" + interruptedKey + ".json"));
	ensure(interruptedJournal.open(QIODevice::WriteOnly), "interrupted fixture writable");
	interruptedJournal.write(QJsonDocument(stale).toJson());
	interruptedJournal.close();
	QFile partial(QDir(root).filePath("replays/interrupted.mp4.partial"));
	ensure(partial.open(QIODevice::WriteOnly), "partial fixture writable");
	partial.write("incomplete");
	partial.close();
	obs_source_filter_remove(a, fa);
	obs_source_filter_remove(b, fb);
	obs_source_release(fa);
	obs_source_release(fb);
	obs_source_release(a);
	obs_source_release(b);
	obs_shutdown();
	ensure(callbacks.empty(), "shutdown unregisters Vendor Requests and joins worker");
	proc_handler_destroy(handler);
	return 0;
}
