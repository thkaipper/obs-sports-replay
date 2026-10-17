/* Exercise production playback callbacks; intercept only the OBS outputs and
 * services unrelated to the playback state machine. GPL-2.0-or-later. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <inttypes.h>
#include <math.h>
static uint64_t audio_ts[1024];
static uint32_t audio_frames[1024];
static size_t audio_count;
static int first_y = -1;
static int frame_count;
static int ended_count;
static bool active_source;
static void inspect_audio(obs_source_t *source, const struct obs_source_audio *audio)
{
	(void)source;
	if (audio_count < 1024) {
		audio_ts[audio_count] = audio->timestamp;
		audio_frames[audio_count++] = audio->frames;
	}
}
static void inspect_video(obs_source_t *source, const struct obs_source_frame *frame)
{
	(void)source;
	if (first_y < 0)
		first_y = frame->data[0][0];
	frame_count++;
}
static void inspect_ended(obs_source_t *source)
{
	(void)source;
	ended_count++;
}
static bool inspect_active(const obs_source_t *source)
{
	(void)source;
	return active_source;
}
#define obs_source_output_audio inspect_audio
#define obs_source_output_video inspect_video
#define obs_source_media_ended inspect_ended
#define obs_source_active inspect_active
#include "../src/playback-source.c"

const char *obs_module_text(const char *key)
{
	return key;
}
obs_source_t *sr_integration_find_capture(const char *id)
{
	(void)id;
	return NULL;
}
bool sr_integration_save_manual(const struct sr_replay *r, const char *id, const char *n, bool a, bool p)
{
	(void)r;
	(void)id;
	(void)n;
	(void)a;
	(void)p;
	return false;
}
bool sr_capture_snapshot_at(void *d, struct sr_replay *r, uint64_t c, uint64_t t)
{
	(void)d;
	(void)r;
	(void)c;
	(void)t;
	return false;
}
obs_data_t *sr_capture_health(void *d)
{
	(void)d;
	return obs_data_create();
}
char *sr_config_get_save_dir(void)
{
	return bstrdup("");
}
char *sr_scene_tracker_previous(void)
{
	return NULL;
}
void sr_switch_to_scene_return(const char *s)
{
	(void)s;
}
void sr_switch_to_scene_of_source_return(const char *s)
{
	(void)s;
}
void sr_switch_to_scene(const char *s)
{
	(void)s;
}
char *sr_find_scene_with_source(const char *s)
{
	(void)s;
	return NULL;
}
bool sr_scene_tracker_consume_returning(void)
{
	return false;
}
void sr_scene_tracker_note_replay_launch(void) {}
void sr_scene_tracker_end_replay_guard(void) {}
static void check(bool ok, const char *message)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
	fflush(stdout);
	if (!ok)
		exit(2);
}
static void restart(struct sr_playback *p)
{
	sr_playback_begin_replay_phase(p);
	p->playing = true;
	p->paused = false;
	audio_count = 0;
	frame_count = 0;
	first_y = -1;
}
int main(int argc, char **argv)
{
	check(argc == 2, "media fixture provided");
	struct sr_replay replay = {0};
	check(sr_load_replay(argv[1], &replay), "saved replay reloads");
	struct sr_playback p = {0};
	pthread_mutex_init(&p.mutex, NULL);
	p.speed_percent = 100;
	p.cur_idx = -1;
	p.end_action_override = -1;
	sr_playback_install_replay(&p, &replay, SR_END_FREEZE, false);
	sr_playback_tick(&p, 1.0f / 30.0f);
	check(first_y == 16 && p.cur_idx == 0, "first tick displays frame zero");
	check(audio_count == 1 && p.audio_idx == 1, "audio at the initial timestamp is emitted");
	for (int i = 0; i < 12; i++)
		sr_playback_tick(&p, 1.0f / 30.0f);
	bool ordered = audio_count > 10;
	for (size_t i = 1; i < audio_count; i++)
		if (audio_ts[i] < audio_ts[i - 1] + (audio_frames[i - 1] * 1000000000ULL + 47999) / 48000)
			ordered = false;
	check(ordered, "batched audio timestamps preserve sample spacing without overlap");
	uint64_t before = p.playhead;
	p.paused = true;
	size_t before_audio = audio_count;
	sr_playback_tick(&p, 1);
	check(p.playhead == before && audio_count == before_audio && !p.audio_clock_valid,
	      "pause holds position and resets the audio clock");
	p.paused = false;
	sr_playback_tick(&p, 1.0f / 30.0f);
	check(p.playhead > before && p.audio_clock_valid, "resume advances with a new audio clock");
	p.speed_percent = 50;
	before_audio = audio_count;
	sr_playback_tick(&p, 1.0f / 30.0f);
	check(audio_count == before_audio && !p.audio_clock_valid, "slow motion remains silent");
	p.backward = true;
	before = p.playhead;
	sr_playback_tick(&p, 1.0f / 30.0f);
	check(p.playhead < before && audio_count == before_audio, "reverse moves backward without audio");
	p.backward = false;
	p.speed_percent = 100;
	p.muted = true;
	sr_playback_tick(&p, 1.0f / 30.0f);
	check(audio_count == before_audio, "mute suppresses output");
	p.muted = false;
	restart(&p);
	sr_playback_tick(&p, 1.0f / 30.0f);
	check(p.cur_idx == 0 && audio_count == 1, "restart restores the initial video and audio");
	while (p.playing)
		sr_playback_tick(&p, 1.0f / 30.0f);
	uint64_t samples = 0;
	for (size_t i = 0; i < audio_count; i++)
		samples += audio_frames[i];
	check(samples >= 119990 && samples <= 120010 && ended_count == 1,
	      "complete playback retains the audio tail and ends once");
	p.end_action_override = SR_END_LOOP;
	restart(&p);
	for (int i = 0; i < 90; i++)
		sr_playback_tick(&p, 1.0f / 30.0f);
	check(p.playing && ended_count == 1, "loop restarts without a terminal event");
	p.end_action_override = SR_END_FREEZE;
	p.intro_clip = sr_clip_open(argv[1]);
	p.outro_clip = sr_clip_open(argv[1]);
	sr_playback_begin_sequence(&p);
	check(p.phase == PHASE_INTRO, "intro remains the first sequence phase");
	bool saw_replay = false, saw_outro = false;
	for (int i = 0; i < 400 && p.playing; i++) {
		sr_playback_tick(&p, 1.0f / 30.0f);
		saw_replay |= p.phase == PHASE_REPLAY;
		saw_outro |= p.phase == PHASE_OUTRO;
	}
	check(saw_replay && saw_outro && !p.playing, "intro, replay and outro finish in sequence");
	sr_clip_close(p.intro_clip);
	sr_clip_close(p.outro_clip);
	sr_replay_free(&p.replay);
	gop_cache_clear(&p);
	da_free(p.gop_frames);
	sr_decoder_destroy(p.decoder);
	pthread_mutex_destroy(&p.mutex);

	check(obs_startup("en-US", NULL, NULL), "isolated source context starts");
	obs_register_source(&sr_playback_info);
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "autoplay", false);
	obs_data_set_int(settings, "end_action", SR_END_RETURN);
	obs_source_t *source = obs_source_create(SR_PLAYBACK_ID, "Playback regression", settings, NULL);
	obs_data_release(settings);
	struct sr_playback *live = obs_obj_get_data(source);
	check(live && sr_playback_play_file(source, argv[1]), "file loads on an inactive source");
	check(live->skip_next_autocapture, "pending activation protects a loaded file");
	sr_playback_activate(live);
	check(!live->skip_next_autocapture && live->playing, "activation consumes the file guard once");
	active_source = true;
	check(sr_playback_play_file(source, argv[1]) && !live->skip_next_autocapture,
	      "loading on an already active source leaves no pending guard");
	active_source = false;
	sr_playback_play_file(source, argv[1]);
	sr_playback_deactivate(live);
	check(!live->skip_next_autocapture && !live->playing, "deactivation cancels a pending file activation");
	live->autoplay = true;
	sr_playback_activate(live);
	check(live->bounce_countdown > 0, "next activation attempts capture and handles an empty camera");
	check(!sr_playback_play_file(source, "missing-replay.mp4"), "failed file loading is reported");
	obs_source_release(source);
	obs_wait_for_destroy_queue();
	obs_shutdown();
	return 0;
}
