/* Sports Replay, Copyright (C) 2026 Systec. GPL-2.0-or-later.
 * Video remains compressed; optional captured float audio is encoded as AAC. */
#include "sr-save.h"
#include <QFile>
#include <QString>
#include <algorithm>
#include <cerrno>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

static const AVRational ns_tb = {1, 1000000000};
static const char *io_error(int ret)
{
	return ret == AVERROR(ENOSPC) ? "DISK_FULL" : "WRITE_FAILED";
}

/* Same-volume publication, deliberately never replaces an existing replay. */
static bool publish(const QString &temporary, const QString &final)
{
#ifdef _WIN32
	HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(temporary.utf16()), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
			       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	bool ok = FlushFileBuffers(h) != FALSE;
	CloseHandle(h);
	return ok && MoveFileExW(reinterpret_cast<LPCWSTR>(temporary.utf16()), reinterpret_cast<LPCWSTR>(final.utf16()),
				 MOVEFILE_WRITE_THROUGH);
#else
	QByteArray temp = QFile::encodeName(temporary), dest = QFile::encodeName(final);
	int fd = open(temp.constData(), O_RDWR);
	if (fd < 0)
		return false;
	bool ok = fsync(fd) == 0;
	close(fd);
	if (!ok || link(temp.constData(), dest.constData()) != 0)
		return false;
	unlink(temp.constData());
	return true;
#endif
}

static int drain_audio(AVCodecContext *encoder, AVFormatContext *format, AVStream *stream)
{
	AVPacket *packet = av_packet_alloc();
	if (!packet)
		return AVERROR(ENOMEM);
	int ret;
	while ((ret = avcodec_receive_packet(encoder, packet)) == 0) {
		packet->stream_index = stream->index;
		av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
		ret = av_interleaved_write_frame(format, packet);
		av_packet_unref(packet);
		if (ret < 0)
			break;
	}
	av_packet_free(&packet);
	return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

const char *sr_save_replay_ex(const sr_replay *r, const char *path, bool save_audio)
{
	if (!r || !r->video.num)
		return "BUFFER_EMPTY";
	size_t start = 0;
	while (start < r->video.num && !(r->video.array[start].pkt->flags & AV_PKT_FLAG_KEY))
		start++;
	if (start == r->video.num)
		return "MUX_FAILED";
	QString final = QString::fromUtf8(path), temporary = final + QStringLiteral(".partial");
	if (QFile::exists(final) || QFile::exists(temporary))
		return "FILE_EXISTS";
	QByteArray temp_utf8 = temporary.toUtf8();
	AVFormatContext *format = nullptr;
	AVCodecContext *audio = nullptr;
	AVFrame *aframe = nullptr;
	AVPacket *packet = nullptr;
	const char *error = nullptr;
	int ret = avformat_alloc_output_context2(&format, nullptr, "mp4", temp_utf8.constData());
	if (ret < 0 || !format)
		return "MUX_FAILED";
	AVStream *video = avformat_new_stream(format, nullptr);
	AVStream *astream = nullptr;
	int channels = static_cast<int>(get_audio_channels(r->speakers));
	bool with_audio = save_audio && r->audio.num && r->samples_per_sec && channels > 0;
	int64_t first = static_cast<int64_t>(r->video.array[start].ts);
	int64_t last = static_cast<int64_t>(r->video.array[r->video.num - 1].ts);
	int64_t tail = r->video.num > start + 1 ? last - static_cast<int64_t>(r->video.array[r->video.num - 2].ts)
						: 33333333;
	if (tail <= 0)
		tail = 33333333;
	int64_t total_samples =
		av_rescale_q(last - first + tail, ns_tb,
			     AVRational{1, static_cast<int>(r->samples_per_sec ? r->samples_per_sec : 48000)});
	if (!video) {
		error = "MUX_FAILED";
		goto cleanup;
	}
	video->time_base = ns_tb;
	video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	video->codecpar->codec_id = r->codec_id;
	video->codecpar->width = static_cast<int>(r->width);
	video->codecpar->height = static_cast<int>(r->height);
	video->codecpar->format = AV_PIX_FMT_YUV420P;
	if (r->extradata_size > 0) {
		video->codecpar->extradata = static_cast<uint8_t *>(
			av_mallocz(static_cast<size_t>(r->extradata_size) + AV_INPUT_BUFFER_PADDING_SIZE));
		if (!video->codecpar->extradata) {
			error = "MEMORY_LIMIT";
			goto cleanup;
		}
		memcpy(video->codecpar->extradata, r->extradata, static_cast<size_t>(r->extradata_size));
		video->codecpar->extradata_size = r->extradata_size;
	}
	if (with_audio) {
		const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
		if (!codec) {
			error = "AUDIO_UNAVAILABLE";
			goto cleanup;
		}
		audio = avcodec_alloc_context3(codec);
		if (!audio) {
			error = "MEMORY_LIMIT";
			goto cleanup;
		}
		audio->sample_rate = static_cast<int>(r->samples_per_sec);
		audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
		audio->time_base = AVRational{1, audio->sample_rate};
		audio->bit_rate = 96000 * channels;
		av_channel_layout_default(&audio->ch_layout, channels);
		audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		if (avcodec_open2(audio, codec, nullptr) < 0) {
			error = "AUDIO_UNAVAILABLE";
			goto cleanup;
		}
		astream = avformat_new_stream(format, nullptr);
		if (!astream || avcodec_parameters_from_context(astream->codecpar, audio) < 0) {
			error = "MUX_FAILED";
			goto cleanup;
		}
		astream->time_base = audio->time_base;
		aframe = av_frame_alloc();
		if (!aframe) {
			error = "MEMORY_LIMIT";
			goto cleanup;
		}
		aframe->format = audio->sample_fmt;
		aframe->sample_rate = audio->sample_rate;
		aframe->nb_samples = audio->frame_size;
		av_channel_layout_copy(&aframe->ch_layout, &audio->ch_layout);
		if (av_frame_get_buffer(aframe, 0) < 0) {
			error = "MEMORY_LIMIT";
			goto cleanup;
		}
	}
	ret = avio_open(&format->pb, temp_utf8.constData(), AVIO_FLAG_WRITE);
	if (ret < 0) {
		error = ret == AVERROR(ENOSPC) ? "DISK_FULL" : "SAVE_DIR_UNWRITABLE";
		goto cleanup;
	}
	if ((ret = avformat_write_header(format, nullptr)) < 0) {
		error = io_error(ret);
		goto cleanup;
	}
	packet = av_packet_alloc();
	if (!packet) {
		error = "MEMORY_LIMIT";
		goto cleanup;
	}
	for (size_t i = start; i < r->video.num; i++) {
		if (av_packet_ref(packet, r->video.array[i].pkt) < 0) {
			error = "MEMORY_LIMIT";
			goto cleanup;
		}
		int64_t ts = static_cast<int64_t>(r->video.array[i].ts) - first;
		int64_t duration = i + 1 < r->video.num ? static_cast<int64_t>(r->video.array[i + 1].ts) -
								  static_cast<int64_t>(r->video.array[i].ts)
							: tail;
		if (duration <= 0 || ts < 0) {
			error = "MUX_FAILED";
			goto cleanup;
		}
		packet->stream_index = video->index;
		packet->pts = packet->dts = av_rescale_q(ts, ns_tb, video->time_base);
		packet->duration = av_rescale_q(duration, ns_tb, video->time_base);
		ret = av_interleaved_write_frame(format, packet);
		av_packet_unref(packet);
		if (ret < 0) {
			error = io_error(ret);
			goto cleanup;
		}
	}
	if (audio) {
		size_t cursor = 0;
		for (int64_t sample = 0; sample < total_samples; sample += audio->frame_size) {
			if (av_frame_make_writable(aframe) < 0) {
				error = "MEMORY_LIMIT";
				goto cleanup;
			}
			aframe->nb_samples =
				static_cast<int>(std::min<int64_t>(audio->frame_size, total_samples - sample));
			for (int ch = 0; ch < channels; ch++)
				memset(aframe->data[ch], 0, static_cast<size_t>(aframe->nb_samples) * sizeof(float));
			for (size_t j = cursor; j < r->audio.num; j++) {
				const sr_audio_chunk &chunk = r->audio.array[j];
				int64_t offset =
					av_rescale_q(static_cast<int64_t>(chunk.ts) - first, ns_tb, audio->time_base);
				int64_t end = offset + chunk.frames;
				if (end <= sample) {
					cursor = j + 1;
					continue;
				}
				if (offset >= sample + aframe->nb_samples)
					break;
				int64_t lo = std::max(offset, sample), hi = std::min(end, sample + aframe->nb_samples);
				if (hi <= lo)
					continue;
				for (int ch = 0; ch < channels; ch++)
					if (chunk.data[ch])
						memcpy(reinterpret_cast<float *>(aframe->data[ch]) + lo - sample,
						       chunk.data[ch] + lo - offset,
						       static_cast<size_t>(hi - lo) * sizeof(float));
			}
			aframe->pts = sample;
			if ((ret = avcodec_send_frame(audio, aframe)) < 0 ||
			    (ret = drain_audio(audio, format, astream)) < 0) {
				error = io_error(ret);
				goto cleanup;
			}
		}
		if ((ret = avcodec_send_frame(audio, nullptr)) < 0 || (ret = drain_audio(audio, format, astream)) < 0) {
			error = io_error(ret);
			goto cleanup;
		}
	}
	if ((ret = av_write_trailer(format)) < 0) {
		error = io_error(ret);
		goto cleanup;
	}
	avio_flush(format->pb);
	if (format->pb->error < 0) {
		error = io_error(format->pb->error);
		goto cleanup;
	}
cleanup:
	av_packet_free(&packet);
	av_frame_free(&aframe);
	avcodec_free_context(&audio);
	if (format->pb) {
		ret = avio_closep(&format->pb);
		if (ret < 0 && !error)
			error = io_error(ret);
	}
	avformat_free_context(format);
	if (!error && !publish(temporary, final))
		error = "PUBLISH_FAILED";
	if (error)
		QFile::remove(temporary);
	return error;
}

bool sr_save_replay(const sr_replay *r, const char *path)
{
	return sr_save_replay_ex(r, path, true) == nullptr;
}
