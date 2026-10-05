/* Sports Replay, Copyright (C) 2026 Systec. GPL-2.0-or-later. */
#include "sr-integration.h"
#include "sr-save.h"
#include "sr-config.h"
#include "sr-dock.h"
#include "plugin-support.h"
#include "obs-websocket-api.h"
#include <util/platform.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QUuid>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>
extern "C" {
#include <libavformat/avformat.h>
}
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
constexpr size_t max_jobs = 32;
constexpr size_t max_bytes = 512ULL * 1024 * 1024;
constexpr size_t max_cameras = 64;
struct Capture {
	QString id;
	obs_weak_source_t *weak;
};
struct Job {
	QString key;
	QJsonObject metadata;
	sr_replay replay = {};
	size_t bytes = 0;
	bool played = false;
	~Job() { sr_replay_free(&replay); }
};
std::mutex registry_mutex, jobs_mutex, admission_mutex, event_mutex;
std::condition_variable jobs_cv;
std::vector<Capture> registry;
std::map<QString, std::shared_ptr<Job>> active;
std::deque<std::shared_ptr<Job>> queue;
std::thread worker;
bool stopping = true;
size_t reserved_bytes = 0;
size_t saving = 0;
QString session, store, last_error, last_save;
obs_websocket_vendor vendor = nullptr;
const char *requests[] = {"GetPluginInfo", "ListCaptureSources", "GetCaptureSource",
			  "GetHealth",     "CaptureEvent",       "GetReplayStatus"};

QString uuid()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QJsonObject from_obs(obs_data_t *d)
{
	return d ? QJsonDocument::fromJson(QByteArray(obs_data_get_json(d))).object() : QJsonObject();
}
obs_data_t *to_obs(const QJsonObject &d)
{
	QByteArray json = QJsonDocument(d).toJson(QJsonDocument::Compact);
	return obs_data_create_from_json(json.constData());
}
QString key_for(const QString &event, const QString &capture)
{
	QByteArray data = event.toUtf8();
	data.append('\0');
	data.append(capture.toUtf8());
	return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
bool valid_event(const QString &id)
{
	if (id.isEmpty() || id.size() > 128 || id.toUtf8().size() > 256)
		return false;
	for (QChar c : id)
		if (c.unicode() < 32 || c.unicode() == 127)
			return false;
	return true;
}
bool valid_capture(const QString &id)
{
	return !QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id;
}
bool durable_json(const QString &path, const QJsonObject &value)
{
	QSaveFile file(path);
	file.setDirectWriteFallback(false);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	QByteArray json = QJsonDocument(value).toJson(QJsonDocument::Compact);
	if (file.write(json) != json.size() || !file.flush())
		return false;
#ifdef _WIN32
	intptr_t native = _get_osfhandle(file.handle());
	if (native == -1 || !FlushFileBuffers(reinterpret_cast<HANDLE>(native)))
		return false;
#else
	if (fsync(file.handle()) != 0)
		return false;
#endif
	return file.commit();
}
QJsonObject read_json(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024)
		return {};
	return QJsonDocument::fromJson(f.readAll()).object();
}
QString journal(const QString &key)
{
	return QDir(store).filePath(key + ".json");
}
bool persist(const std::shared_ptr<Job> &job)
{
	return durable_json(journal(job->key), job->metadata);
}
void emit_vendor(const char *type, const QJsonObject &data)
{
	std::lock_guard<std::mutex> lock(event_mutex);
	if (!vendor)
		return;
	obs_data_t *d = to_obs(data);
	if (d) {
		obs_websocket_vendor_emit_event(vendor, type, d);
		obs_data_release(d);
	}
}
std::vector<obs_source_t *> capture_refs()
{
	std::vector<obs_source_t *> sources;
	std::lock_guard<std::mutex> lock(registry_mutex);
	for (const Capture &c : registry) {
		obs_source_t *s = obs_weak_source_get_source(c.weak);
		if (s)
			sources.push_back(s);
	}
	return sources;
}
obs_source_t *find_capture(const QString &id)
{
	std::lock_guard<std::mutex> lock(registry_mutex);
	for (const Capture &c : registry)
		if (c.id == id)
			return obs_weak_source_get_source(c.weak);
	return nullptr;
}
QJsonArray capture_list()
{
	QJsonArray list;
	for (obs_source_t *s : capture_refs()) {
		void *data = obs_obj_get_data(s);
		if (data) {
			obs_data_t *health = sr_capture_health(data);
			list.append(from_obs(health));
			obs_data_release(health);
		}
		obs_source_release(s);
	}
	return list;
}
QJsonObject failure(const QString &capture, const char *code)
{
	return {{"capture_id", capture}, {"status", "FAILED"}, {"error_code", code}};
}
QJsonObject status_locked(const QString &key)
{
	auto it = active.find(key);
	if (it != active.end())
		return it->second->metadata;
	return read_json(journal(key));
}
size_t replay_bytes(const sr_replay &r)
{
	size_t bytes = static_cast<size_t>(std::max(0, r.extradata_size));
	for (size_t i = 0; i < r.video.num; i++)
		bytes += static_cast<size_t>(r.video.array[i].pkt->size) + sizeof(sr_packet);
	for (size_t i = 0; i < r.audio.num; i++)
		for (size_t ch = 0; ch < MAX_AV_PLANES; ch++)
			if (r.audio.array[i].data[ch])
				bytes += static_cast<size_t>(r.audio.array[i].frames) * sizeof(float);
	return bytes;
}
void enrich(Job &job)
{
	const sr_replay &r = job.replay;
	uint64_t span = r.last_ts - r.first_ts;
	uint64_t tail = r.video.num > 1 ? r.last_ts - r.video.array[r.video.num - 2].ts : 33333333;
	job.metadata["actual_duration_ms"] = static_cast<double>(span + tail) / 1e6;
	job.metadata["duration_ms"] = job.metadata["actual_duration_ms"];
	job.metadata["frames"] = static_cast<double>(r.video.num);
	job.metadata["width"] = static_cast<int>(r.width);
	job.metadata["height"] = static_cast<int>(r.height);
	job.metadata["fps"] =
		span && r.video.num > 1 ? static_cast<double>(r.video.num - 1) * 1e9 / static_cast<double>(span) : 0;
	job.metadata["codec"] = avcodec_get_name(r.codec_id);
	job.metadata["audio_present"] = job.metadata["save_audio"].toBool() && r.audio.num > 0;
	job.metadata["first_frame_arrival_ns"] = QString::number(r.video.array[0].arrival_ns);
	job.metadata["last_frame_arrival_ns"] = QString::number(r.video.array[r.video.num - 1].arrival_ns);
	job.bytes = replay_bytes(r);
}

/* Caller holds admission_mutex. Reserve queue capacity before copying audio. */
QJsonObject submit(const QString &event, const QString &capture, int duration_ms, bool audio, uint64_t cutoff,
		   const sr_replay *manual, const QString &name, bool played)
{
	const QString key = key_for(event, capture);
	{
		std::lock_guard<std::mutex> lock(jobs_mutex);
		if (stopping)
			return failure(capture, "PLUGIN_SHUTTING_DOWN");
		QJsonObject previous = status_locked(key);
		if (QFile::exists(journal(key)) && previous.isEmpty())
			return failure(capture, "JOURNAL_INVALID");
		if (!previous.isEmpty()) {
			if (previous["requested_duration_ms"].toInt() != duration_ms ||
			    previous["save_audio"].toBool() != audio)
				return failure(capture, "REQUEST_CONFLICT");
			previous["duplicate"] = true;
			previous["result_code"] = previous["status"].toString() == "SAVED" ? "ALREADY_SAVED"
											   : "ALREADY_ACCEPTED";
			return previous;
		}
		if (active.size() >= max_jobs)
			return failure(capture, "QUEUE_FULL");
	}
	std::shared_ptr<Job> job = std::make_shared<Job>();
	job->key = key;
	job->played = played;
	job->metadata = {{"event_id", event},
			 {"capture_id", capture},
			 {"session_id", session},
			 {"plugin_version", PLUGIN_VERSION},
			 {"integration_api_version", 1},
			 {"requested_duration_ms", duration_ms},
			 {"save_audio", audio},
			 {"created_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
			 {"event_cutoff_monotonic_ns", QString::number(cutoff)}};
	if (manual) {
		{
			std::lock_guard<std::mutex> lock(jobs_mutex);
			if (replay_bytes(*manual) > max_bytes - reserved_bytes)
				return failure(capture, "MEMORY_LIMIT");
		}
		/* Ref video, own audio: playback keeps its original snapshot. */
		job->replay = *manual;
		memset(&job->replay.video, 0, sizeof(job->replay.video));
		memset(&job->replay.audio, 0, sizeof(job->replay.audio));
		job->replay.extradata = nullptr;
		for (size_t i = 0; i < manual->video.num; i++) {
			sr_packet packet = manual->video.array[i];
			packet.pkt = av_packet_clone(packet.pkt);
			if (!packet.pkt)
				return failure(capture, "MEMORY_LIMIT");
			da_push_back(job->replay.video, &packet);
		}
		if (manual->extradata_size > 0)
			job->replay.extradata = static_cast<uint8_t *>(
				bmemdup(manual->extradata, static_cast<size_t>(manual->extradata_size)));
		for (size_t i = 0; i < manual->audio.num; i++) {
			sr_audio_chunk chunk = manual->audio.array[i];
			for (size_t ch = 0; ch < MAX_AV_PLANES; ch++)
				if (chunk.data[ch])
					chunk.data[ch] = static_cast<float *>(
						bmemdup(chunk.data[ch], chunk.frames * sizeof(float)));
			da_push_back(job->replay.audio, &chunk);
		}
		job->metadata["source_name"] = name;
		job->metadata["encoder"] = "unknown";
	} else {
		obs_source_t *source = find_capture(capture);
		if (!source)
			return failure(capture, "CAPTURE_NOT_FOUND");
		void *data = obs_obj_get_data(source);
		obs_data_t *h = data ? sr_capture_health(data) : nullptr;
		QJsonObject health = from_obs(h);
		obs_data_release(h);
		QString state = health["status"].toString();
		const char *rejected = state == "OFFLINE" ? "CAMERA_OFFLINE"
				       : state == "ERROR" ? "ENCODER_UNAVAILABLE"
							  : nullptr;
		{
			std::lock_guard<std::mutex> lock(jobs_mutex);
			if (static_cast<double>(reserved_bytes) + health["memory_bytes"].toDouble() >
			    static_cast<double>(max_bytes))
				rejected = "MEMORY_LIMIT";
		}
		bool ok = !rejected && data &&
			  sr_capture_snapshot_at(data, &job->replay, cutoff,
						 static_cast<uint64_t>(duration_ms) * 1000000ULL);
		obs_source_release(source);
		if (!ok)
			return failure(capture, rejected ? rejected : "BUFFER_EMPTY");
		job->metadata["source_name"] = health["source_name"];
		job->metadata["encoder"] = health["encoder_actual"];
	}
	if (!job->replay.video.num)
		return failure(capture, "BUFFER_EMPTY");
	enrich(*job);
	char *dir = sr_config_get_save_dir();
	QString save_dir = QString::fromUtf8(dir);
	bfree(dir);
	if (save_dir.isEmpty())
		return failure(capture, "SAVE_DIR_INVALID");
	save_dir = QFileInfo(save_dir).absoluteFilePath();
	if (!QDir().mkpath(save_dir))
		return failure(capture, "SAVE_DIR_INVALID");
	/* SHA-256 IDs avoid Windows reserved names, truncation collisions and traversal. */
	job->metadata["path"] = QDir(save_dir).filePath("replay_" + key + ".mp4");
	job->metadata["status"] = "ACCEPTED";
	{
		std::lock_guard<std::mutex> lock(jobs_mutex);
		if (stopping)
			return failure(capture, "PLUGIN_SHUTTING_DOWN");
		if (job->bytes > max_bytes - reserved_bytes)
			return failure(capture, "MEMORY_LIMIT");
		if (!persist(job))
			return failure(capture, "JOURNAL_WRITE_FAILED");
		reserved_bytes += job->bytes;
		active[key] = job;
	}
	emit_vendor("ReplayAccepted", job->metadata);
	QJsonObject response = job->metadata;
	{
		std::lock_guard<std::mutex> lock(jobs_mutex);
		job->metadata["status"] = "SAVE_QUEUED";
		queue.push_back(job);
	}
	jobs_cv.notify_one();
	return response;
}

void save_worker()
{
	std::map<QString, QString> previous_health;
	for (;;) {
		std::shared_ptr<Job> job;
		{
			std::unique_lock<std::mutex> lock(jobs_mutex);
			jobs_cv.wait_for(lock, std::chrono::seconds(2), [] { return stopping || !queue.empty(); });
			if (queue.empty() && stopping)
				break;
			if (!queue.empty()) {
				job = queue.front();
				queue.pop_front();
				saving = 1;
				job->metadata["status"] = "SAVING";
			}
		}
		if (!job) {
			for (const QJsonValue &value : capture_list()) {
				QJsonObject health = value.toObject();
				QString id = health["capture_id"].toString(), state = health["status"].toString();
				if (previous_health[id] != state) {
					previous_health[id] = state;
					emit_vendor("CaptureSourceHealthChanged", health);
				}
			}
			continue;
		}
		emit_vendor("ReplaySaveStarted", job->metadata);
		QByteArray path = job->metadata["path"].toString().toUtf8();
		const char *err =
			sr_save_replay_ex(&job->replay, path.constData(), job->metadata["save_audio"].toBool());
		QJsonObject terminal;
		{
			std::lock_guard<std::mutex> lock(jobs_mutex);
			job->metadata["status"] = err ? "FAILED" : "SAVED";
			job->metadata["error_code"] = err ? err : "";
			job->metadata["completed_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
			if (!err) {
				job->metadata["size_bytes"] =
					static_cast<double>(QFileInfo(job->metadata["path"].toString()).size());
				last_save = job->metadata["completed_at"].toString();
			} else
				last_error = err;
			bool durable = persist(job);
			if (!durable) {
				last_error = "JOURNAL_WRITE_FAILED";
				job->metadata["status_durable"] = false;
			} else
				job->metadata["status_durable"] = true;
			terminal = job->metadata;
			reserved_bytes -= job->bytes;
			saving = 0;
			sr_replay_free(&job->replay);
			job->bytes = 0;
			/* If persistence failed, retain terminal status until shutdown. */
			if (durable)
				active.erase(job->key);
		}
		if (!err) {
			if (!durable_json(job->metadata["path"].toString() + ".json", terminal)) {
				std::lock_guard<std::mutex> lock(jobs_mutex);
				last_error = "METADATA_WRITE_FAILED";
			}
			if (job->played)
				sr_dock_mark_played(path.constData());
		}
		blog(err ? LOG_WARNING : LOG_INFO, "[sports-replay] event=%s capture=%s state=%s error=%s path=%s",
		     terminal["event_id"].toString().toUtf8().constData(),
		     terminal["capture_id"].toString().toUtf8().constData(), err ? "FAILED" : "SAVED", err ? err : "",
		     path.constData());
		emit_vendor(err ? "ReplayFailed" : "ReplaySaved", terminal);
	}
}

QJsonObject global_health()
{
	QJsonArray cameras = capture_list();
	int healthy = 0;
	for (const QJsonValue &c : cameras)
		if (c.toObject()["status"] == "OK")
			healthy++;
	char *d = sr_config_get_save_dir();
	QString dir = QString::fromUtf8(d);
	bfree(d);
	QStorageInfo disk(dir);
	QFileInfo info(dir);
	QTemporaryFile probe(QDir(dir).filePath(".sports-replay-probe-XXXXXX"));
	bool writable = info.isDir() && probe.open();
	probe.close();
	QJsonObject result = {{"plugin_version", PLUGIN_VERSION},
			      {"integration_api_version", 1},
			      {"obs_version", obs_get_version_string()},
			      {"session_id", session},
			      {"save_directory", dir},
			      {"save_directory_writable", writable},
			      {"disk_free", static_cast<double>(disk.bytesAvailable())},
			      {"active_capture_sources", cameras.size()},
			      {"healthy_capture_sources", healthy},
			      {"capture_sources", cameras}};
	{
		std::lock_guard<std::mutex> lock(jobs_mutex);
		result["save_queue_length"] = static_cast<double>(queue.size());
		result["jobs_saving"] = static_cast<double>(saving);
		result["snapshot_memory_bytes"] = static_cast<double>(reserved_bytes);
		result["queue_limit"] = static_cast<double>(max_jobs);
		result["memory_limit_bytes"] = static_cast<double>(max_bytes);
		result["last_save_time"] = last_save;
		result["last_error"] = last_error;
		result["shutting_down"] = stopping;
	}
	{
		std::lock_guard<std::mutex> lock(event_mutex);
		result["integration_available"] = vendor != nullptr;
	}
	result["capabilities"] = QJsonObject{{"multi_camera_capture", true},
					     {"event_id", true},
					     {"audio_save", true},
					     {"atomic_save", true},
					     {"persistent_idempotency", true},
					     {"duration_selection", true},
					     {"post_roll", false}};
	return result;
}

void request_callback_impl(obs_data_t *input, obs_data_t *output, void *context)
{
	/* Admission lock also makes shutdown wait for requests and snapshots in progress. */
	std::lock_guard<std::mutex> admission(admission_mutex);
	QJsonObject request = from_obs(input), result;
	const char *type = static_cast<const char *>(context);
	bool shutting_down;
	{
		std::lock_guard<std::mutex> lock(jobs_mutex);
		shutting_down = stopping;
	}
	if (shutting_down && strcmp(type, "GetPluginInfo") && strcmp(type, "GetHealth"))
		result = {{"error_code", "PLUGIN_SHUTTING_DOWN"}};
	else if (!strcmp(type, "GetPluginInfo") || !strcmp(type, "GetHealth"))
		result = global_health();
	else if (!strcmp(type, "ListCaptureSources"))
		result = {{"capture_sources", capture_list()}, {"session_id", session}};
	else if (!strcmp(type, "GetCaptureSource")) {
		QString id = request["capture_id"].toString();
		obs_source_t *source = valid_capture(id) ? find_capture(id) : nullptr;
		if (source) {
			obs_data_t *h = sr_capture_health(obs_obj_get_data(source));
			result = from_obs(h);
			obs_data_release(h);
			obs_source_release(source);
		} else
			result = failure(id, valid_capture(id) ? "CAPTURE_NOT_FOUND" : "INVALID_CAPTURE_ID");
	} else if (!strcmp(type, "GetReplayStatus")) {
		QString event = request["event_id"].toString(), id = request["capture_id"].toString();
		if (!valid_event(event) || !valid_capture(id))
			result = failure(id, "INVALID_REQUEST");
		else {
			std::lock_guard<std::mutex> lock(jobs_mutex);
			result = status_locked(key_for(event, id));
			if (result.isEmpty()) {
				if (QFile::exists(journal(key_for(event, id))))
					result = failure(id, "JOURNAL_INVALID");
				else
					result = {{"event_id", event}, {"capture_id", id}, {"status", "NOT_RECEIVED"}};
			} else if (result["status"] == "SAVED")
				result["file_available"] = QFileInfo(result["path"].toString()).isFile();
		}
	} else if (!strcmp(type, "CaptureEvent")) {
		QString event = request["event_id"].toString();
		int duration = request.value("duration_ms").toInt(30000);
		bool audio = request.value("save_audio").toBool(true);
		QJsonArray cameras = request["cameras"].toArray();
		if (!valid_event(event))
			result = {{"accepted", false}, {"error_code", "INVALID_EVENT_ID"}};
		else if ((request.contains("duration_ms") &&
			  (!request["duration_ms"].isDouble() || request["duration_ms"].toDouble() != duration)) ||
			 (request.contains("save_audio") && !request["save_audio"].isBool()) ||
			 (request.contains("save_only") && !request["save_only"].isBool()) ||
			 request["post_roll_ms"].toDouble() != 0 || duration < 1000 || duration > 120000 ||
			 cameras.isEmpty() || cameras.size() > static_cast<qsizetype>(max_cameras) ||
			 request["save_only"].toBool(true) == false)
			result = {{"accepted", false}, {"error_code", "INVALID_REQUEST"}};
		else {
			uint64_t cutoff = os_gettime_ns();
			QJsonArray responses;
			bool accepted = false;
			for (const QJsonValue &camera : cameras) {
				QString id = camera.toObject()["capture_id"].toString();
				QJsonObject status = valid_capture(id) ? submit(event, id, duration, audio, cutoff,
										nullptr, {}, false)
								       : failure(id, "INVALID_CAPTURE_ID");
				QString state = status["status"].toString();
				if (state != "FAILED")
					accepted = true;
				responses.append(status);
			}
			result = {{"accepted", accepted},
				  {"event_id", event},
				  {"session_id", session},
				  {"cameras", responses}};
		}
	}
	obs_data_t *response = to_obs(result);
	if (response) {
		obs_data_apply(output, response);
		obs_data_release(response);
	}
}
void request_callback(obs_data_t *input, obs_data_t *output, void *context)
{
	try {
		request_callback_impl(input, output, context);
	} catch (...) {
		obs_data_set_string(output, "error_code", "INTERNAL_ERROR");
		obs_data_set_bool(output, "accepted", false);
	}
}
} // namespace

char *sr_integration_register_capture(obs_source_t *source, const char *preferred_id)
{
	QString id = QString::fromUtf8(preferred_id ? preferred_id : "");
	std::lock_guard<std::mutex> lock(registry_mutex);
	if (!valid_capture(id))
		id = uuid();
	for (const Capture &c : registry)
		if (c.id == id && !obs_weak_source_expired(c.weak)) {
			id = uuid();
			break;
		}
	registry.push_back({id, obs_source_get_weak_source(source)});
	return bstrdup(id.toUtf8().constData());
}
void sr_integration_unregister_capture(obs_source_t *source)
{
	std::lock_guard<std::mutex> lock(registry_mutex);
	for (auto it = registry.begin(); it != registry.end(); ++it)
		if (obs_weak_source_references_source(it->weak, source)) {
			obs_weak_source_release(it->weak);
			registry.erase(it);
			break;
		}
}
bool sr_integration_save_manual(const sr_replay *replay, const char *capture_id, const char *source_name, bool audio,
				bool played)
{
	std::lock_guard<std::mutex> admission(admission_mutex);
	QJsonObject result = submit("manual-" + uuid(), QString::fromUtf8(capture_id), 0, audio, os_gettime_ns(),
				    replay, QString::fromUtf8(source_name), played);
	if (result["status"] == "FAILED") {
		blog(LOG_WARNING, "[sports-replay] manual save rejected: %s",
		     result["error_code"].toString().toUtf8().constData());
		return false;
	}
	return true;
}
void sr_integration_init(void)
{
	session = uuid();
	char *path = obs_module_config_path("integration-jobs");
	store = QString::fromUtf8(path ? path : "");
	bfree(path);
	if (store.isEmpty() || !QDir().mkpath(store)) {
		last_error = "JOURNAL_WRITE_FAILED";
		return;
	}
	/* Recover only files described by our private journal. Terminal records stay on
	 * disk, not in an ever-growing RAM map. Accepted snapshots vanish after crash. */
	for (const QString &file : QDir(store).entryList({"*.json"}, QDir::Files)) {
		QJsonObject record = read_json(QDir(store).filePath(file));
		QString state = record["status"].toString();
		if (record.isEmpty() || state == "SAVED" || state == "FAILED")
			continue;
		QString final = record["path"].toString();
		bool exists = false;
		if (!final.isEmpty() && QFileInfo(final).size() > 0) {
			AVFormatContext *format = nullptr;
			QByteArray name = final.toUtf8();
			exists = avformat_open_input(&format, name.constData(), nullptr, nullptr) == 0 &&
				 avformat_find_stream_info(format, nullptr) == 0 &&
				 av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) >= 0;
			avformat_close_input(&format);
		}
		record["status"] = exists ? "SAVED" : "FAILED";
		record["error_code"] = exists ? "" : "INTERRUPTED";
		record["recovered"] = true;
		if (exists) {
			record["size_bytes"] = static_cast<double>(QFileInfo(final).size());
			durable_json(final + ".json", record);
		} else if (!final.isEmpty())
			QFile::remove(final + ".partial");
		if (!durable_json(QDir(store).filePath(file), record))
			last_error = "JOURNAL_WRITE_FAILED";
	}
	{
		std::lock_guard<std::mutex> lock(jobs_mutex);
		stopping = false;
	}
	worker = std::thread([] {
		try {
			save_worker();
		} catch (...) {
			std::lock_guard<std::mutex> lock(jobs_mutex);
			stopping = true;
			last_error = "INTERNAL_ERROR";
			blog(LOG_ERROR,
			     "[sports-replay] save worker stopped after internal error; accepted jobs remain recoverable");
		}
	});
}
void sr_integration_post_load(void)
{
	std::lock_guard<std::mutex> lock(event_mutex);
	vendor = obs_websocket_register_vendor("sports-replay");
	if (vendor)
		for (const char *type : requests)
			if (!obs_websocket_vendor_register_request(vendor, type, request_callback,
								   const_cast<char *>(type)))
				blog(LOG_WARNING, "[sports-replay] cannot register request %s", type);
	blog(LOG_INFO, "[sports-replay] Integration API v1 %s",
	     vendor ? "available" : "unavailable; manual operation retained");
}
void sr_integration_shutdown(void)
{
	{
		std::lock_guard<std::mutex> admission(admission_mutex);
		std::lock_guard<std::mutex> lock(jobs_mutex);
		stopping = true;
	}
	{
		std::lock_guard<std::mutex> lock(event_mutex);
		if (vendor)
			for (const char *type : requests)
				obs_websocket_vendor_unregister_request(vendor, type);
		vendor = nullptr;
	}
	jobs_cv.notify_all();
	if (worker.joinable())
		worker.join();
	{
		std::lock_guard<std::mutex> lock(jobs_mutex);
		queue.clear();
		active.clear();
		reserved_bytes = 0;
	}
	std::lock_guard<std::mutex> lock(registry_mutex);
	for (const Capture &c : registry)
		obs_weak_source_release(c.weak);
	registry.clear();
}

obs_source_t *sr_integration_find_capture(const char *id)
{
	return find_capture(QString::fromUtf8(id ? id : ""));
}
