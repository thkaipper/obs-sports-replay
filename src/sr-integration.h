/* Sports Replay, Copyright (C) 2026 Systec. GPL-2.0-or-later. */
#pragma once
#include "sr-buffer.h"
#ifdef __cplusplus
extern "C" {
#endif
void sr_integration_init(void);
void sr_integration_post_load(void);
void sr_integration_shutdown(void);
/* Returned ID is owned by caller. Registry keeps only weak source refs. */
char *sr_integration_register_capture(obs_source_t *source, const char *preferred_id);
void sr_integration_unregister_capture(obs_source_t *source);
obs_source_t *sr_integration_find_capture(const char *id); /* strong ref */
/* Copies replay; caller retains its snapshot for immediate playback. */
bool sr_integration_save_manual(const struct sr_replay *replay, const char *capture_id, const char *source_name,
				bool save_audio, bool played);
/* Capture callbacks serialize state/encoder and snapshot access. */
obs_data_t *sr_capture_health(void *data);
bool sr_capture_snapshot_at(void *data, struct sr_replay *out, uint64_t cutoff_ns, uint64_t duration_ns);
#ifdef __cplusplus
}
#endif
