"""Exercise a separate portable OBS instance; never contacts production OBS.
Usage: python tests/live-websocket.py CONFIG_JSON VIDEO_FILE REPORT_JSON
Requires websockets. Config contains the test instance's port and password.
"""
import asyncio
import base64
import hashlib
import json
import pathlib
import sys
import time
import uuid
import websockets


async def main():
    config = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
    events = []
    async with websockets.connect(f"ws://127.0.0.1:{config['server_port']}") as ws:
        hello = json.loads(await ws.recv())["d"]
        identify = {"rpcVersion": 1, "eventSubscriptions": 512}
        if "authentication" in hello:
            auth = hello["authentication"]
            secret = base64.b64encode(hashlib.sha256((config["server_password"] + auth["salt"]).encode()).digest()).decode()
            identify["authentication"] = base64.b64encode(hashlib.sha256((secret + auth["challenge"]).encode()).digest()).decode()
        await ws.send(json.dumps({"op": 1, "d": identify}))
        assert json.loads(await ws.recv())["op"] == 2

        async def request(kind, data=None):
            request_id = str(uuid.uuid4())
            await ws.send(json.dumps({"op": 6, "d": {"requestType": kind, "requestId": request_id, "requestData": data or {}}}))
            while True:
                message = json.loads(await asyncio.wait_for(ws.recv(), 15))
                if message["op"] == 5:
                    events.append(message["d"])
                if message["op"] == 7 and message["d"]["requestId"] == request_id:
                    result = message["d"]
                    assert result["requestStatus"]["result"], result["requestStatus"]
                    return result.get("responseData", {})

        async def vendor(kind, data=None):
            return (await request("CallVendorRequest", {"vendorName": "sports-replay", "requestType": kind, "requestData": data or {}}))["responseData"]

        info = await vendor("GetPluginInfo")
        assert info["integration_api_version"] == 1
        assert info["plugin_version"] == "1.3.0"
        print("PASS: real authenticated WebSocket + Vendor API", flush=True)
        scenes = (await request("GetSceneList"))["scenes"]
        if not any(s["sceneName"] == "Integration Scene" for s in scenes):
            await request("CreateScene", {"sceneName": "Integration Scene"})
        await request("SetCurrentProgramScene", {"sceneName": "Integration Scene"})
        for old_input in (await request("GetInputList"))["inputs"]:
            await request("RemoveInput", {"inputName": old_input["inputName"]})
        names = ["Integration Camera A", "Integration Camera B"]
        for name in names:
            await request("CreateInput", {"sceneName": "Integration Scene", "inputName": name, "inputKind": "ffmpeg_source", "inputSettings": {
                "is_local_file": True, "local_file": str(pathlib.Path(sys.argv[2]).resolve()), "looping": True,
                "restart_on_activate": True, "close_when_inactive": False}, "sceneItemEnabled": True})
            await request("CreateSourceFilter", {"sourceName": name, "filterName": "Sports Capture", "filterKind": "sports_replay_capture",
                                                 "filterSettings": {"duration_ms": 60000, "capture_fps": 30, "encoder": 4, "keyint": 15}})
        captures = []
        for _ in range(100):
            captures = (await vendor("ListCaptureSources"))["capture_sources"]
            if len(captures) == 2 and all(c["buffer_frames"] >= 30 and c["status"] in ("OK", "DEGRADED") for c in captures):
                break
            await asyncio.sleep(0.1)
        assert len(captures) == 2 and all(c["buffer_frames"] >= 30 for c in captures), captures
        ids = [c["capture_id"] for c in captures]
        event = "live-" + str(uuid.uuid4())
        data = {"event_id": event, "duration_ms": 1000, "save_audio": True, "cameras": [{"capture_id": i} for i in ids]}
        accepted = await vendor("CaptureEvent", data)
        assert accepted["accepted"], accepted
        duplicate = await vendor("CaptureEvent", data)
        assert all(c["duplicate"] for c in duplicate["cameras"]), duplicate
        saved = []
        for _ in range(100):
            saved = [await vendor("GetReplayStatus", {"event_id": event, "capture_id": i}) for i in ids]
            if all(c["status"] == "SAVED" for c in saved):
                break
            assert all(c["status"] != "FAILED" for c in saved), saved
            await asyncio.sleep(0.1)
        assert all(c["status"] == "SAVED" and pathlib.Path(c["path"]).is_file() for c in saved), saved
        # Request a final status to drain any events preceding that response.
        health = await vendor("GetHealth")
        saved_events = [e for e in events if e.get("eventType") == "VendorEvent" and e.get("eventData", {}).get("eventType") == "ReplaySaved"]
        assert len(saved_events) >= 2, events
        assert not any(pathlib.Path(c["path"] + ".partial").exists() for c in saved)
        report = {"hello": {"obsStudioVersion": hello["obsStudioVersion"], "obsWebSocketVersion": hello["obsWebSocketVersion"]},
                  "plugin_info": info, "accepted": accepted, "duplicate": duplicate, "saved": saved, "health": health, "events": events}
        pathlib.Path(sys.argv[3]).write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
        print("PASS: two real Media Sources captured through filters, deduplication and ReplaySaved events", flush=True)
        # Delete only the sources created above, in this test instance.
        for name in names:
            await request("RemoveInput", {"inputName": name})


if __name__ == "__main__":
    asyncio.run(main())
