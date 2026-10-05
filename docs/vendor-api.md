# Sports Replay Vendor API v1

Plugin 1.3.0. Target: OBS Studio 32.2.2 Windows x64. Vendor: `sports-replay`.
Use the existing authenticated obs-websocket connection. Subscribe to Vendors events
(`eventSubscriptions` includes 512). No extra server is started by this plugin.

## Requests

All requests are sent inside `CallVendorRequest`:

```json
{
  "requestType": "CallVendorRequest",
  "requestData": {
    "vendorName": "sports-replay",
    "requestType": "CaptureEvent",
    "requestData": {
      "event_id": "SR01-20261004-194215-000127",
      "duration_ms": 30000,
      "save_only": true,
      "save_audio": true,
      "cameras": [
        {"capture_id": "550e8400-e29b-41d4-a716-446655440001"},
        {"capture_id": "550e8400-e29b-41d4-a716-446655440002"}
      ]
    }
  }
}
```

**Use an array of objects for cameras, not an array of strings.** The public
obs-websocket callback uses `obs_data_t`, whose arrays represent objects. The JSON
above was exercised through obs-websocket 5.7.4 in a separate OBS 32.2.2 instance.
When sending a complete raw WebSocket message, wrap this example in `op: 6`,
`d`, and add a unique `requestId` inside `d`.

| Request | requestData | Result |
| --- | --- | --- |
| GetPluginInfo | `{}` | Plugin/API/OBS versions, session, capabilities and health |
| ListCaptureSources | `{}` | `capture_sources` array and session |
| GetCaptureSource | `{"capture_id":"UUID"}` | One filter's identity, names, encoder and health |
| GetHealth | `{}` | Global health plus per-filter information |
| CaptureEvent | Example above | Acceptance/status independently for each camera |
| GetReplayStatus | `{"event_id":"E123","capture_id":"UUID"}` | Durable or active job status; NOT_RECEIVED when absent |

The outer obs-websocket response contains `responseData.responseData`; client
libraries may already unwrap the outer response. Plugin error codes are in the
vendor response body, not inferred from log text.

## Identity and admission

Filters persist `capture_id` and the owning OBS source UUID in their settings.
Renaming preserves the ID. Duplicating settings for a new filter produces a new ID.
Playback-source hotkeys use their own OBS source UUID and migrate legacy bindings.

`event_id` is required: nonempty, at most 128 UTF-16 units / 256 UTF-8 bytes, without
control characters. It is metadata, never interpreted as a path. `capture_id` must
be a lowercase UUID without braces. `duration_ms` defaults to 30000 and accepts
integer values from 1000 to 120000. `save_audio` defaults to true. `save_only` is
true; automation that changes playback and post-roll are not implemented.
Up to 64 camera entries are allowed in one request.

Snapshots use a shared monotonic cutoff. Cameras that fail are reported separately.
`accepted: true` means at least one camera has a nonfailed job; inspect every camera.
Requests before any frame, offline cameras, invalid IDs and full queues are rejected
before acceptance and can be retried. Once a snapshot is durably accepted, never
submit that same `(event_id, capture_id)` expecting a new snapshot.

The queue permits at most 32 active jobs and approximately 512 MiB of owned snapshots.
These bounds apply to saved-job snapshots, not the live ring buffers or playback GOP
caches. One worker writes files; video is remuxed without re-encoding.

## Deduplication and restart

Key: `(event_id, capture_id)`. Duplicate requests return the existing status,
`duplicate: true`, and `ALREADY_SAVED` or `ALREADY_ACCEPTED`. Different duration or
audio options return `REQUEST_CONFLICT`. Each camera remains independent.

Accepted job metadata is committed to the plugin's `integration-jobs` directory
before acceptance is returned. Terminal records stay on disk rather than accumulating
in RAM. The records are not automatically expired: preserve this directory to retain
deduplication. It is part of the plugin's state, not the external application's database.

After restart, `session_id` changes. A published valid MP4 can recover as SAVED;
an accepted job whose in-memory snapshot was lost becomes FAILED / INTERRUPTED.
Neither is automatically recaptured. A client must query `GetReplayStatus` after
disconnecting: VendorEvents are notifications, not a reliable event journal.
SAVED remains deduplicated even if someone deletes its MP4; status queries include
`file_available` so the client can distinguish that case.

## Files and audio

Final filename: `replay_<SHA256(event_id + NUL + capture_id)>.mp4`.
This avoids Windows reserved names, traversal, sanitization collisions and friendly-name
renames. Names, IDs and timing remain in API metadata and `<filename>.mp4.json`.
The dock reads metadata for its labels and routing and supports legacy filenames too.

The worker explicitly selects the MP4 muxer, writes `<filename>.mp4.partial`, checks
trailer, flush and close, and uses same-directory publication without overwriting.
Only then is SAVED reported. Windows publication uses FlushFileBuffers and
MoveFileExW with WRITE_THROUGH. Abandoned partials associated with interrupted
accepted jobs are removed at startup. Arbitrary files in the save directory are
not deleted. Network filesystems need separate validation of their durability semantics.

AAC uses the captured sample rate and float planar audio; chunks are aligned to the
video interval, gaps are padded with silence. If a source has no captured audio,
the file contains video only and `audio_present: false`. Unsupported AAC settings
fail with AUDIO_UNAVAILABLE; requested audio is never silently disabled due to an
encoder error. Playback from saved files now loads audio too. Slow motion/reverse
continue to mute replay audio, as in the original plugin. Bumper files remain video-only.

Selecting a duration retains the previous keyframe as necessary; actual duration
can exceed the request by approximately one GOP. It is not an exact arbitrary-frame
cut. Return fields include `requested_duration_ms`, `actual_duration_ms` and `duration_ms`.
A common cutoff does not remove physical RTSP latency differences between cameras.

## Events and status

Events arrive as obs-websocket `VendorEvent`, containing `vendorName`, `eventType`
and `eventData`. Plugin event types:

- ReplayAccepted: snapshot accepted and journal committed.
- ReplaySaveStarted: worker began the job.
- ReplaySaved: finalized MP4 published.
- ReplayFailed: an accepted save failed.
- CaptureSourceHealthChanged: periodic change in a filter's health state.

Immediate preacceptance rejections are in the request response and do not promise
an asynchronous ReplayFailed. Health events are checked when the worker is idle;
poll GetHealth during a long save rather than treating events as a heartbeat.

```json
{
  "event_id": "SR01-20261004-194215-000127",
  "capture_id": "550e8400-e29b-41d4-a716-446655440001",
  "source_name": "CAM GOL A",
  "status": "SAVED",
  "path": "C:/Replays/replay_<hash>.mp4",
  "requested_duration_ms": 30000,
  "actual_duration_ms": 30433.33,
  "size_bytes": 18273645,
  "frames": 913,
  "width": 1920,
  "height": 1080,
  "fps": 30,
  "codec": "h264",
  "encoder": "h264_nvenc",
  "save_audio": true,
  "audio_present": true
}
```

Job states: ACCEPTED, SAVE_QUEUED, SAVING, SAVED, FAILED. Missing keys return
NOT_RECEIVED. Metadata includes session, plugin/API versions, creation/completion
times and arrival/cutoff nanoseconds encoded as strings to avoid JSON precision loss.

## Errors and health

Stable codes: INVALID_EVENT_ID, INVALID_CAPTURE_ID, INVALID_REQUEST,
CAPTURE_NOT_FOUND, CAMERA_OFFLINE, ENCODER_UNAVAILABLE, BUFFER_EMPTY,
QUEUE_FULL, MEMORY_LIMIT, REQUEST_CONFLICT, JOURNAL_INVALID,
JOURNAL_WRITE_FAILED, SAVE_DIR_INVALID, SAVE_DIR_UNWRITABLE, DISK_FULL,
MUX_FAILED, WRITE_FAILED, PUBLISH_FAILED, FILE_EXISTS, AUDIO_UNAVAILABLE,
INTERRUPTED, PLUGIN_SHUTTING_DOWN, INTERNAL_ERROR. METADATA_WRITE_FAILED
can appear in global last_error when a ready file's sidecar could not be written.

Health states: OK, DEGRADED, OFFLINE, ERROR. OFFLINE means no frames or last arrival
older than five seconds. ERROR includes unavailable encoder or frames arriving without
encoded packets. An explicit hardware selection that falls back to x264 is DEGRADED.
Automatic backend selection reports the actual encoder without marking software
fallback as degraded. Initial encoder-open failures retry no more often than every
ten seconds while frames keep arriving.

GetHealth reports buffer frame count/duration/memory, received/encoded frame ages,
encoder requested/actual/error, reset count, queue limits, disk free and last error/save.
The directory write check is a temporary probe; free space is observational.
An OBS response indicating missing vendor means this integration is unavailable;
a timeout alone can also be a broken connection. In Safe Mode the plugin may not load.

At shutdown new jobs are rejected and the worker finishes accepted jobs before it
joins. Shutdown can therefore wait for disk I/O; abrupt termination requires restart
recovery. Sidecar and MP4 are separate publications; the durable private journal is
the authority for job identity and state.
