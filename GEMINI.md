# Gemini conversation gateway

`server/conversation.py` is an asynchronous service on port **8001**, separate from the
Flask/OpenCV server on port 8000. The ESP32 connects to the Pi; the Pi connects to
[Gemini Live](https://ai.google.dev/gemini-api/docs/live-api). Google browser OAuth
credentials (`client_secret.json`) are unrelated to the Gemini API key.

## Setup on the Pi

Add to the existing private `server/appsettings.py`:

```python
GEMINI_API_KEY = "your-gemini-api-key"
GEMINI_MODEL = "gemini-3.1-flash-live-preview"
GEMINI_MAX_SESSIONS = 2
RECOGNIZE_URL = "http://127.0.0.1:8000/recognize"
```

The last three settings have those defaults. `GEMINI_SYSTEM_INSTRUCTION` can
optionally replace the assistant's default instructions. `GEMINI_API_KEY` can
also be supplied through the service environment and overrides the settings file.
Keep your existing `API_KEY`: it authenticates the device to both Pi services.

Run `bash install.sh`. It installs and starts both `face-recognition.service` and
`face-conversation.service`, preserving existing settings. Without a Gemini key,
the gateway starts but rejects conversations with HTTP 503. After changing settings:

```bash
sudo systemctl restart face-conversation.service
sudo journalctl -u face-conversation.service -f
```

For manual startup, install `server/requirements.txt` and run `python server/conversation.py`.
The gateway itself does not load OpenCV models. The recognition server must be
running for snapshot recognition, but audio/text conversations work independently.

Use WSS through your HTTPS reverse proxy outside a trusted local network. Route
`/conversation` and `/health` to port 8001, forward `X-API-Key`, enable WebSocket
Upgrade forwarding, and use an idle timeout longer than 60 seconds. Keep the
existing browser and recognition routes on port 8000.

## Endpoints

Both endpoints require `X-API-Key: <your existing device API key>` as a request
header. Keys in query strings and browser login sessions are not accepted.

| Endpoint | Purpose |
| --- | --- |
| `GET /health` | Returns `{"status":"ok","gemini_configured":true}`. Configuration status is not a live provider connectivity check. |
| `GET /conversation` with WebSocket upgrade | Opens one isolated Gemini session for this connection. |

Before upgrade: HTTP 401 means missing/invalid device key, 503 means missing
Gemini configuration, and 429 means the simultaneous-session limit was reached.
After upgrade, setup failures produce an `error` event and close the connection.

## ESP32 WebSocket protocol

Connect to `ws://<pi-address>:8001/conversation` on a trusted LAN (WSS through the
proxy). Wait for this JSON text frame before sending input:

```json
{"type":"ready","input_audio":"pcm_s16le","input_sample_rate":16000,"output_sample_rate":24000,"channels":1}
```

Send microphone samples as **binary WebSocket frames**, mono signed 16-bit
little-endian PCM at **16 kHz**, without WAV headers. Prefer 20–100 ms chunks
(640–3200 bytes); the maximum chunk is 32000 bytes. Gemini's automatic speech
activity detection determines when to respond. When you stop transmitting audio,
send `audio_end`; a later binary frame restarts the input stream.

Send controls as JSON text frames:

| Message | Meaning |
| --- | --- |
| `{"type":"text","text":"Hello"}` | Send a complete text turn, up to 4000 characters; Gemini responds with audio. |
| `{"type":"audio_end"}` | Flush input when the microphone stops. |
| `{"type":"snapshot","jpeg":"<base64 JPEG>"}` | Recognize an image locally and add the result to this conversation. Maximum decoded size: 1 MB. |
| `{"type":"stop"}` | Close the conversation and release the provider connection. |

Snapshots are sent to the existing `/recognize` endpoint with the API key. The
returned names and server observation time are sent to Gemini as uncertain context.
The JPEG itself is **not** sent to Gemini. This version provides face context, not
cloud scene/video understanding. A standalone call to `/recognize` does not update
a conversation: send a `snapshot` message over that conversation's socket instead.
Send snapshots sparingly, ideally before speech; recognition is awaited before
processing the next device message on that connection. An observation can prompt a
spoken response. Face matches are not speaker identification or authorization.

Incoming **binary frames** contain mono signed 16-bit little-endian PCM at **24 kHz**.
Buffer and play these through the speaker. JSON text events are:

| Event type | Fields / client action |
| --- | --- |
| `ready` | Audio format negotiation shown above. |
| `transcript` | `role` (`user` or `assistant`) and `text`; incremental transcription chunks. |
| `recognition` | `faces` and `names`, as returned by the recognition API. |
| `interrupted` | Stop playback immediately and clear queued audio. |
| `turn_complete` | The model turn finished; already buffered audio may still need playback. |
| `session_ending` | `reason`; the provider is disconnecting. Reconnect with backoff for a new conversation. |
| `error` | `error` contains a user-safe message. Input validation errors allow retry on the same socket; provider failures close it. |

The device must answer WebSocket pings and handle connection loss. This initial
version does **not** preserve history across reconnects or execute model-requested
hardware tools. Context compression is enabled within a connection. Provider
connection limits still apply; sessions are not intended to remain open all day.
Close when idle to release capacity. Handle microphone/speaker echo on the device
or use push-to-talk for the first hardware prototype.

## Smoke test without a microphone

From a computer with `aiohttp` installed and `clientsettings.py` configured:

```python
import asyncio
import aiohttp
import clientsettings

async def main():
    async with aiohttp.ClientSession() as http:
        async with http.ws_connect(
            "ws://<pi-address>:8001/conversation",
            headers={"X-API-Key": clientsettings.API_KEY},
        ) as ws:
            print(await ws.receive_json())  # ready (or configuration error)
            await ws.send_json({"type": "text", "text": "Say hello briefly."})
            async for message in ws:
                if message.type == aiohttp.WSMsgType.BINARY:
                    print("Audio bytes:", len(message.data))
                elif message.type == aiohttp.WSMsgType.TEXT:
                    event = message.json()
                    print(event)
                    if event["type"] in {"turn_complete", "error", "session_ending"}:
                        break

asyncio.run(main())
```

## Automated checks

```bash
python -m unittest discover -s server -p test_conversation.py -v
python -m unittest discover -s tests -v
```

The gateway tests run actual local WebSocket connections against fake Gemini and
recognition services; they require no cloud API key and incur no API charges.
They verify authentication, configuration handling, audio relay, text turns,
transcripts, interruption, snapshot context, malformed input, capacity, and shutdown.
A live Gemini smoke test is still needed with your key and model access.
