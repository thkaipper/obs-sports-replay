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

#include "sr-buffer.h"
#include <util/platform.h>

void sr_buffer_init(struct sr_buffer *b)
{
	memset(b, 0, sizeof(*b));
	pthread_mutex_init(&b->mutex, NULL);
	b->max_bytes = 512ULL * 1024 * 1024;
}

static void free_video_packet(struct sr_packet *p)
{
	av_packet_free(&p->pkt);
}

static void free_audio_chunk(struct sr_audio_chunk *c)
{
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		bfree(c->data[i]);
		c->data[i] = NULL;
	}
}

void sr_buffer_clear(struct sr_buffer *b)
{
	pthread_mutex_lock(&b->mutex);
	while (b->video.size) {
		struct sr_packet p;
		deque_pop_front(&b->video, &p, sizeof(p));
		free_video_packet(&p);
	}
	while (b->audio.size) {
		struct sr_audio_chunk c;
		deque_pop_front(&b->audio, &c, sizeof(c));
		free_audio_chunk(&c);
	}
	b->video_bytes = b->audio_bytes = 0;
	pthread_mutex_unlock(&b->mutex);
}

void sr_buffer_free(struct sr_buffer *b)
{
	sr_buffer_clear(b);
	deque_free(&b->video);
	deque_free(&b->audio);
	bfree(b->extradata);
	b->extradata = NULL;
	pthread_mutex_destroy(&b->mutex);
}

void sr_buffer_set_extradata(struct sr_buffer *b, const uint8_t *data, int size)
{
	pthread_mutex_lock(&b->mutex);
	bfree(b->extradata);
	b->extradata = NULL;
	b->extradata_size = 0;
	if (data && size > 0) {
		b->extradata = bmemdup(data, (size_t)size);
		b->extradata_size = size;
	}
	pthread_mutex_unlock(&b->mutex);
}

static struct sr_packet *video_at(struct sr_buffer *b, size_t idx)
{
	return deque_data(&b->video, idx * sizeof(struct sr_packet));
}

static bool is_keyframe(const struct sr_packet *p)
{
	return (p->pkt->flags & AV_PKT_FLAG_KEY) != 0;
}

static size_t audio_bytes(const struct sr_audio_chunk *chunk)
{
	size_t bytes = sizeof(*chunk);
	for (size_t ch = 0; ch < MAX_AV_PLANES; ch++)
		if (chunk->data[ch])
			bytes += (size_t)chunk->frames * sizeof(float);
	return bytes;
}

static void drop_video(struct sr_buffer *b)
{
	struct sr_packet packet;
	deque_pop_front(&b->video, &packet, sizeof(packet));
	b->video_bytes -= sizeof(packet) + (size_t)packet.pkt->size;
	free_video_packet(&packet);
}

static void drop_audio(struct sr_buffer *b)
{
	struct sr_audio_chunk chunk;
	deque_pop_front(&b->audio, &chunk, sizeof(chunk));
	b->audio_bytes -= audio_bytes(&chunk);
	free_audio_chunk(&chunk);
}

/* Never leave an undecodable partial GOP at the front. If one GOP alone is
 * too large, discard it and wait for the next keyframe. */
static void enforce_memory_limit(struct sr_buffer *b)
{
	while (b->video_bytes + b->audio_bytes > b->max_bytes) {
		struct sr_audio_chunk first_audio = {0};
		if (b->audio.size)
			deque_peek_front(&b->audio, &first_audio, sizeof(first_audio));
		if (b->video.size && (!b->audio.size || video_at(b, 0)->ts <= first_audio.ts)) {
			drop_video(b);
			while (b->video.size && !is_keyframe(video_at(b, 0)))
				drop_video(b);
		} else if (b->audio.size) {
			drop_audio(b);
		} else {
			break;
		}
	}
}

/* Drops expired frames from the front. Unless the stream is all-intra a
 * packet is only decodable together with the keyframe that opens its GOP,
 * so whole GOPs are dropped at once and only once every frame in them has
 * expired - the buffer therefore holds up to one extra GOP. */
static void evict_expired_video(struct sr_buffer *b, uint64_t now)
{
	const size_t count = b->video.size / sizeof(struct sr_packet);
	size_t next_gop = 0;

	for (size_t i = 1; i < count; i++) {
		if (!is_keyframe(video_at(b, i)))
			continue;
		/* the GOP ending right before this keyframe is only expendable
		 * once its last frame has fallen out of the window */
		const struct sr_packet *last = video_at(b, i - 1);
		if (now <= last->ts || now - last->ts <= b->duration_ns)
			break;
		next_gop = i;
	}

	for (size_t i = 0; i < next_gop; i++)
		drop_video(b);
}

void sr_buffer_push_video(struct sr_buffer *b, AVPacket *pkt, uint64_t ts)
{
	struct sr_packet entry = {.pkt = pkt, .ts = ts, .arrival_ns = os_gettime_ns()};

	pthread_mutex_lock(&b->mutex);
	if ((b->video.size && ts <= video_at(b, b->video.size / sizeof(entry) - 1)->ts) ||
	    (!b->video.size && !is_keyframe(&entry))) {
		pthread_mutex_unlock(&b->mutex);
		av_packet_free(&pkt);
		return;
	}
	deque_push_back(&b->video, &entry, sizeof(entry));
	b->video_bytes += sizeof(entry) + (size_t)pkt->size;
	evict_expired_video(b, ts);
	enforce_memory_limit(b);
	pthread_mutex_unlock(&b->mutex);
}

void sr_buffer_push_audio(struct sr_buffer *b, const struct obs_audio_data *audio, uint32_t channels)
{
	struct sr_audio_chunk chunk = {0};
	chunk.frames = audio->frames;
	chunk.ts = audio->timestamp;
	for (uint32_t i = 0; i < channels && i < MAX_AV_PLANES; i++) {
		if (!audio->data[i])
			break;
		chunk.data[i] = bmemdup(audio->data[i], audio->frames * sizeof(float));
	}

	pthread_mutex_lock(&b->mutex);

	/* the source restarted and its clock went backwards: the buffered audio
	 * would never expire against the new timeline */
	if (b->audio.size) {
		struct sr_audio_chunk back;
		deque_peek_back(&b->audio, &back, sizeof(back));
		if (chunk.ts == back.ts) {
			pthread_mutex_unlock(&b->mutex);
			free_audio_chunk(&chunk);
			return;
		}
		if (chunk.ts < back.ts) {
			while (b->audio.size)
				drop_audio(b);
		}
	}

	deque_push_back(&b->audio, &chunk, sizeof(chunk));
	b->audio_bytes += audio_bytes(&chunk);

	struct sr_audio_chunk front;
	deque_peek_front(&b->audio, &front, sizeof(front));
	while (b->audio.size > sizeof(chunk) && chunk.ts > front.ts && chunk.ts - front.ts > b->duration_ns) {
		drop_audio(b);
		deque_peek_front(&b->audio, &front, sizeof(front));
	}
	enforce_memory_limit(b);
	pthread_mutex_unlock(&b->mutex);
}

const char *sr_buffer_snapshot_bounded(struct sr_buffer *b, struct sr_replay *out, uint64_t cutoff_ns,
				       uint64_t duration_ns, bool audio, size_t max_bytes)
{
	memset(out, 0, sizeof(*out));

	pthread_mutex_lock(&b->mutex);

	size_t video_count = b->video.size / sizeof(struct sr_packet);
	while (video_count && cutoff_ns && video_at(b, video_count - 1)->arrival_ns > cutoff_ns)
		video_count--;
	if (!video_count) {
		pthread_mutex_unlock(&b->mutex);
		return "BUFFER_EMPTY";
	}

	size_t begin = 0;
	if (duration_ns) {
		uint64_t end = video_at(b, video_count - 1)->ts;
		uint64_t first = end > duration_ns ? end - duration_ns : 0;
		for (size_t i = 0; i < video_count; i++) {
			if (video_at(b, i)->ts > first)
				break;
			if (is_keyframe(video_at(b, i)))
				begin = i;
		}
	}
	const uint64_t first_ts = video_at(b, begin)->ts;
	const uint64_t last_ts = video_at(b, video_count - 1)->ts;
	const uint64_t tail = video_count > begin + 1 ? last_ts - video_at(b, video_count - 2)->ts : 33333333;
	const uint64_t end_ts = last_ts + tail;
	const size_t audio_count = audio ? b->audio.size / sizeof(struct sr_audio_chunk) : 0;
	size_t bytes = (size_t)b->extradata_size;
	for (size_t i = begin; i < video_count; i++)
		bytes += sizeof(struct sr_packet) + (size_t)video_at(b, i)->pkt->size;
	for (size_t i = 0; i < audio_count; i++) {
		const struct sr_audio_chunk *src = deque_data(&b->audio, i * sizeof(*src));
		uint64_t end =
			src->ts + (b->samples_per_sec ? (uint64_t)src->frames * 1000000000ULL / b->samples_per_sec : 0);
		if (src->ts < end_ts && end > first_ts)
			bytes += audio_bytes(src);
	}
	if (bytes > max_bytes) {
		pthread_mutex_unlock(&b->mutex);
		return "MEMORY_LIMIT";
	}
	da_reserve(out->video, video_count - begin);
	for (size_t i = begin; i < video_count; i++) {
		struct sr_packet *src = deque_data(&b->video, i * sizeof(struct sr_packet));
		struct sr_packet copy = {.pkt = av_packet_clone(src->pkt),
					 .ts = src->ts,
					 .arrival_ns = src->arrival_ns};
		if (!copy.pkt) {
			pthread_mutex_unlock(&b->mutex);
			sr_replay_free(out);
			return "MEMORY_LIMIT";
		}
		da_push_back(out->video, &copy);
	}

	const uint32_t channels = get_audio_channels(b->speakers);
	for (size_t i = 0; i < audio_count; i++) {
		struct sr_audio_chunk *src = deque_data(&b->audio, i * sizeof(struct sr_audio_chunk));
		uint64_t end =
			src->ts + (b->samples_per_sec ? (uint64_t)src->frames * 1000000000ULL / b->samples_per_sec : 0);
		if (src->ts >= end_ts || end <= first_ts)
			continue;
		struct sr_audio_chunk copy = {0};
		copy.frames = src->frames;
		copy.ts = src->ts;
		for (uint32_t p = 0; p < channels && p < MAX_AV_PLANES; p++) {
			if (!src->data[p])
				break;
			copy.data[p] = bmemdup(src->data[p], src->frames * sizeof(float));
		}
		da_push_back(out->audio, &copy);
	}

	out->codec_id = b->codec_id;
	out->width = b->width;
	out->height = b->height;
	if (b->extradata && b->extradata_size > 0) {
		out->extradata = bmemdup(b->extradata, (size_t)b->extradata_size);
		out->extradata_size = b->extradata_size;
	}
	out->samples_per_sec = b->samples_per_sec;
	out->speakers = b->speakers;

	pthread_mutex_unlock(&b->mutex);

	if (!out->video.num) {
		sr_replay_free(out);
		return "BUFFER_EMPTY";
	}

	out->first_ts = out->video.array[0].ts;
	out->last_ts = out->video.array[out->video.num - 1].ts;
	return NULL;
}

bool sr_buffer_snapshot_at(struct sr_buffer *b, struct sr_replay *out, uint64_t cutoff_ns, uint64_t duration_ns)
{
	return sr_buffer_snapshot_bounded(b, out, cutoff_ns, duration_ns, true, SIZE_MAX) == NULL;
}

bool sr_buffer_snapshot(struct sr_buffer *b, struct sr_replay *out)
{
	return sr_buffer_snapshot_at(b, out, 0, 0);
}

void sr_replay_free(struct sr_replay *r)
{
	for (size_t i = 0; i < r->video.num; i++)
		free_video_packet(&r->video.array[i]);
	da_free(r->video);

	for (size_t i = 0; i < r->audio.num; i++)
		free_audio_chunk(&r->audio.array[i]);
	da_free(r->audio);

	bfree(r->extradata);

	memset(r, 0, sizeof(*r));
}

size_t sr_buffer_video_bytes(struct sr_buffer *b)
{
	size_t total = 0;
	pthread_mutex_lock(&b->mutex);
	const size_t count = b->video.size / sizeof(struct sr_packet);
	for (size_t i = 0; i < count; i++) {
		struct sr_packet *p = deque_data(&b->video, i * sizeof(struct sr_packet));
		total += (size_t)p->pkt->size;
	}
	pthread_mutex_unlock(&b->mutex);
	return total;
}
