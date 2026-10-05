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

#include "sr-load.h"

#include <plugin-support.h>
#include <libavformat/avformat.h>

#define NS_TB \
	(AVRational) { 1, 1000000000 }

static enum speaker_layout speakers_for_channels(int channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

static void receive_audio(AVCodecContext *decoder, AVFrame *frame, AVRational tb, struct sr_replay *out)
{
	while (avcodec_receive_frame(decoder, frame) == 0) {
		int channels = frame->ch_layout.nb_channels;
		enum speaker_layout speakers = speakers_for_channels(channels);
		if (speakers == SPEAKERS_UNKNOWN || frame->sample_rate <= 0)
			continue;
		enum AVSampleFormat format = av_get_packed_sample_fmt((enum AVSampleFormat)frame->format);
		if (format != AV_SAMPLE_FMT_FLT && format != AV_SAMPLE_FMT_DBL && format != AV_SAMPLE_FMT_S16 &&
		    format != AV_SAMPLE_FMT_S32 && format != AV_SAMPLE_FMT_U8)
			continue;
		bool planar = av_sample_fmt_is_planar((enum AVSampleFormat)frame->format) != 0;
		int64_t ts = frame->best_effort_timestamp;
		if (ts == AV_NOPTS_VALUE)
			continue;
		ts = av_rescale_q(ts, tb, NS_TB);
		int skip = ts < 0 ? (int)av_rescale_q(-ts, NS_TB, (AVRational){1, frame->sample_rate}) : 0;
		if (skip >= frame->nb_samples)
			continue;
		struct sr_audio_chunk chunk = {0};
		chunk.frames = (uint32_t)(frame->nb_samples - skip);
		chunk.ts = ts < 0 ? 0 : (uint64_t)ts;
		for (int ch = 0; ch < channels && ch < MAX_AV_PLANES; ch++) {
			chunk.data[ch] = bmalloc(chunk.frames * sizeof(float));
			const uint8_t *src = frame->extended_data[planar ? ch : 0];
			for (uint32_t sample = 0; sample < chunk.frames; sample++) {
				size_t index = planar ? sample + (size_t)skip
						      : (sample + (size_t)skip) * (size_t)channels + (size_t)ch;
				float value = 0;
				switch (format) {
				case AV_SAMPLE_FMT_FLT:
					value = ((const float *)src)[index];
					break;
				case AV_SAMPLE_FMT_DBL:
					value = (float)((const double *)src)[index];
					break;
				case AV_SAMPLE_FMT_S16:
					value = ((const int16_t *)src)[index] / 32768.0f;
					break;
				case AV_SAMPLE_FMT_S32:
					value = (float)(((const int32_t *)src)[index] / 2147483648.0);
					break;
				case AV_SAMPLE_FMT_U8:
					value = (((const uint8_t *)src)[index] - 128) / 128.0f;
					break;
				default:
					break;
				}
				chunk.data[ch][sample] = value;
			}
		}
		out->samples_per_sec = (uint32_t)frame->sample_rate;
		out->speakers = speakers;
		da_push_back(out->audio, &chunk);
	}
}

bool sr_load_replay(const char *path, struct sr_replay *out)
{
	memset(out, 0, sizeof(*out));

	AVFormatContext *fmt = NULL;
	if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
		obs_log(LOG_WARNING, "sr_load: could not open '%s'", path);
		return false;
	}
	if (avformat_find_stream_info(fmt, NULL) < 0) {
		avformat_close_input(&fmt);
		return false;
	}

	const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (vs < 0) {
		avformat_close_input(&fmt);
		return false;
	}

	AVStream *st = fmt->streams[vs];
	out->codec_id = st->codecpar->codec_id;
	out->width = (uint32_t)st->codecpar->width;
	out->height = (uint32_t)st->codecpar->height;
	if (st->codecpar->extradata && st->codecpar->extradata_size > 0) {
		out->extradata = bmemdup(st->codecpar->extradata, (size_t)st->codecpar->extradata_size);
		out->extradata_size = st->codecpar->extradata_size;
	}

	int as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	AVCodecContext *audio = NULL;
	AVFrame *audio_frame = NULL;
	if (as >= 0) {
		const AVCodec *codec = avcodec_find_decoder(fmt->streams[as]->codecpar->codec_id);
		if (codec)
			audio = avcodec_alloc_context3(codec);
		if (audio && (avcodec_parameters_to_context(audio, fmt->streams[as]->codecpar) < 0 ||
			      avcodec_open2(audio, codec, NULL) < 0))
			avcodec_free_context(&audio);
		if (audio)
			audio_frame = av_frame_alloc();
		if (!audio_frame)
			avcodec_free_context(&audio);
	}
	AVPacket *pkt = av_packet_alloc();
	if (!pkt) {
		av_frame_free(&audio_frame);
		avcodec_free_context(&audio);
		avformat_close_input(&fmt);
		sr_replay_free(out);
		return false;
	}
	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == vs) {
			const int64_t src_ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
			if (src_ts == AV_NOPTS_VALUE || src_ts < 0) {
				av_packet_unref(pkt);
				continue;
			}
			struct sr_packet e = {
				.pkt = av_packet_clone(pkt),
				.ts = (uint64_t)av_rescale_q(src_ts, st->time_base, NS_TB),
			};
			if (e.pkt)
				da_push_back(out->video, &e);
		} else if (audio && pkt->stream_index == as) {
			if (avcodec_send_packet(audio, pkt) == 0)
				receive_audio(audio, audio_frame, fmt->streams[as]->time_base, out);
		}
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);
	if (audio) {
		avcodec_send_packet(audio, NULL);
		receive_audio(audio, audio_frame, fmt->streams[as]->time_base, out);
	}
	av_frame_free(&audio_frame);
	avcodec_free_context(&audio);
	avformat_close_input(&fmt);

	if (!out->video.num) {
		sr_replay_free(out);
		return false;
	}

	out->first_ts = out->video.array[0].ts;
	out->last_ts = out->video.array[out->video.num - 1].ts;
	obs_log(LOG_INFO, "sr_load: loaded '%s' (%zu frames)", path, out->video.num);
	return true;
}
