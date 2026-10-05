#include "voice.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/stream_buffer.h>
#include <math.h>

#include "audio.h"
#include "config.h"
#include "faces.h"
#include "battery.h"
#include "lcd.h"
#include "wake_word.h"

// Root certificate bundle supplied by the installed ESP32 Arduino core.
extern const uint8_t bundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t bundleEnd[] asm("_binary_x509_crt_bundle_end");

namespace {
constexpr size_t BYTES_PER_SECOND = 16000 * sizeof(int16_t);
constexpr size_t MIC_BUFFER_BYTES = 2 * BYTES_PER_SECOND;    // covers the TLS connect after the wake word
constexpr size_t PLAY_BUFFER_BYTES = 60 * BYTES_PER_SECOND;  // replies arrive faster than real time
constexpr size_t SEND_CHUNK_BYTES = BYTES_PER_SECOND / 10;   // 100 ms frames
constexpr unsigned long CONNECT_TIMEOUT_MS = 10000;
constexpr unsigned long FOLLOW_UP_MS = 8000;
constexpr unsigned long MAX_SESSION_MS = 180000;
// Mic stays muted this long after the last played sample (no echo cancellation).
constexpr unsigned long ECHO_TAIL_MS = 300;
constexpr float CHIME_AMPLITUDE = 3000.0f;  // about -21 dBFS
constexpr unsigned long ERROR_FACE_MS = 2000;

enum class State { Idle, Connecting, Active };
const char *const STATE_NAMES[] = { "idle", "connecting", "active" };

State state = State::Idle;
WebSocketsClient socket;
String url, apiKey, host, path;
uint16_t port = 443;
bool secure = true;

StreamBufferHandle_t micBuffer = nullptr;
StreamBufferHandle_t playBuffer = nullptr;
volatile unsigned long lastPlayMs = 0;
unsigned long startMs = 0;
unsigned long lastActivityMs = 0;
bool audioSinceEnd = false;
const char *lastError = "none";
unsigned long errorFaceMs = 0;
uint32_t sessionCount = 0, sentBytes = 0, receivedBytes = 0, droppedBytes = 0;

bool parseUrl(const String &value) {
  String rest;
  if (value.startsWith("wss://")) {
    secure = true;
    port = 443;
    rest = value.substring(6);
  } else if (value.startsWith("ws://")) {
    secure = false;
    port = 80;
    rest = value.substring(5);
  } else {
    return false;
  }
  int slash = rest.indexOf('/');
  path = slash < 0 ? "/" : rest.substring(slash);
  host = slash < 0 ? rest : rest.substring(0, slash);
  int colon = host.indexOf(':');
  if (colon >= 0) {
    port = host.substring(colon + 1).toInt();
    host = host.substring(0, colon);
  }
  return host.length() > 0 && port > 0;
}

bool speaking() {
  return xStreamBufferBytesAvailable(playBuffer) > 0 || millis() - lastPlayMs < ECHO_TAIL_MS;
}

void drain(StreamBufferHandle_t buffer) {
  uint8_t scratch[256];
  while (xStreamBufferReceive(buffer, scratch, sizeof(scratch), 0)) {}
}

// Three quiet rising tones (C5, E5, G5) with short fades to avoid clicks.
void queueChime() {
  const float notes[] = { 523.25f, 659.25f, 783.99f };
  constexpr size_t TONE_SAMPLES = 16000 * 90 / 1000;
  constexpr size_t GAP_SAMPLES = 16000 * 25 / 1000;
  constexpr size_t FADE_SAMPLES = 16000 * 8 / 1000;
  static int16_t samples[TONE_SAMPLES + GAP_SAMPLES];  // keep 3.7 KB off the loop stack
  for (float note : notes) {
    for (size_t i = 0; i < TONE_SAMPLES + GAP_SAMPLES; ++i) {
      float envelope = 0.0f;
      if (i < TONE_SAMPLES) {
        size_t edge = min(i, TONE_SAMPLES - 1 - i);
        envelope = edge < FADE_SAMPLES ? (float)edge / FADE_SAMPLES : 1.0f;
      }
      samples[i] = (int16_t)(CHIME_AMPLITUDE * envelope * sinf(2.0f * PI * note * i / 16000));
    }
    xStreamBufferSend(playBuffer, samples, sizeof(samples), 0);
  }
}

void playTask(void *) {
  int16_t samples[256];
  bool ampOn = false;
  for (;;) {
    size_t bytes = xStreamBufferReceive(playBuffer, samples, sizeof(samples), pdMS_TO_TICKS(50));
    if (bytes) {
      if (!ampOn) {
        AUDIO_SetAmp(true);
        ampOn = true;
        delay(10);  // amplifier start-up
      }
      AUDIO_Write(samples, bytes / sizeof(samples[0]));
      lastPlayMs = millis();
    } else if (ampOn && millis() - lastPlayMs > ECHO_TAIL_MS) {
      AUDIO_SetAmp(false);
      ampOn = false;
    }
  }
}

// Failures show the sad face briefly so a silent robot is not mistaken for a slow one.
void showResult(const char *error) {
  if (error) {
    lastError = error;
    FACE_Show(2);  // 02_sad
    errorFaceMs = millis() | 1;
  } else {
    LCD_Clear();
    BATT_ShowStatus();
  }
}

void endSession(const char *error) {
  if (state == State::Idle) return;
  bool connected = socket.isConnected();
  state = State::Idle;  // before disconnect(): its event must not end the session twice
  WAKEWORD_SetCapture(nullptr);
  if (connected) socket.sendTXT("{\"type\":\"stop\"}");
  socket.disconnect();
  showResult(error);
}

void handleText(const uint8_t *payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  const char *type = doc["type"] | "";
  if (strcmp(type, "ready") == 0) {
    state = State::Active;
    lastActivityMs = millis();
  } else if (strcmp(type, "transcript") == 0) {
    if (strcmp(doc["role"] | "", "user") == 0) lastActivityMs = millis();
    Serial.printf("VOICE %s: %s\n", (const char *)(doc["role"] | ""), (const char *)(doc["text"] | ""));
  } else if (strcmp(type, "interrupted") == 0) {
    drain(playBuffer);
  } else if (strcmp(type, "error") == 0) {
    Serial.printf("VOICE gateway error: %s\n", (const char *)(doc["error"] | ""));
    endSession("gateway error");
  } else if (strcmp(type, "session_ending") == 0) {
    endSession("gateway ended session");
  }
}

void onEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_TEXT:
      handleText(payload, length);
      break;
    case WStype_BIN: {
      size_t queued = xStreamBufferSend(playBuffer, payload, length, 0);
      receivedBytes += length;
      droppedBytes += length - queued;
      lastActivityMs = millis();
      break;
    }
    case WStype_DISCONNECTED:
      endSession(state == State::Connecting ? "connect failed" : "disconnected");
      break;
    default:
      break;
  }
}
}

bool VOICE_Init() {
  url = VOICE_DEFAULT_URL;  // also when the namespace does not exist yet
  Preferences prefs;
  if (prefs.begin("miniBotVoice", true)) {
    url = prefs.getString("url", VOICE_DEFAULT_URL);
    apiKey = prefs.getString("key", "");
    prefs.end();
  }
  parseUrl(url);

  static StaticStreamBuffer_t micStruct, playStruct;
  uint8_t *micStorage = (uint8_t *)heap_caps_malloc(MIC_BUFFER_BYTES + 1, MALLOC_CAP_SPIRAM);
  uint8_t *playStorage = (uint8_t *)heap_caps_malloc(PLAY_BUFFER_BYTES + 1, MALLOC_CAP_SPIRAM);
  if (!micStorage || !playStorage) {
    lastError = "PSRAM buffers unavailable";
    return false;
  }
  micBuffer = xStreamBufferCreateStatic(MIC_BUFFER_BYTES, 1, micStorage, &micStruct);
  playBuffer = xStreamBufferCreateStatic(PLAY_BUFFER_BYTES, 1, playStorage, &playStruct);
  socket.onEvent(onEvent);
  return xTaskCreatePinnedToCore(playTask, "voiceplay", 4096, nullptr, 3, nullptr, 1) == pdPASS;
}

void VOICE_Start() {
  if (state != State::Idle || !playBuffer) return;
  sessionCount++;
  lastError = "none";
  errorFaceMs = 0;
  startMs = lastActivityMs = millis();
  audioSinceEnd = false;
  drain(micBuffer);
  queueChime();
  if (!host.length() || !apiKey.length()) {
    showResult("not configured");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    showResult("no Wi-Fi");
    return;
  }
  WAKEWORD_SetCapture(micBuffer);
  socket.setExtraHeaders(("X-API-Key: " + apiKey).c_str());
  if (secure) {
    socket.beginSslWithBundle(host.c_str(), port, path.c_str(), bundleStart, bundleEnd - bundleStart, "");
  } else {
    socket.begin(host.c_str(), port, path.c_str(), "");
  }
  state = State::Connecting;
}

void VOICE_Stop() {
  endSession(nullptr);
}

void VOICE_Process() {
  if (errorFaceMs && millis() - errorFaceMs > ERROR_FACE_MS) {
    errorFaceMs = 0;
    if (state == State::Idle) showResult(nullptr);
  }
  if (state == State::Idle) return;
  socket.loop();
  if (state == State::Idle) return;

  unsigned long now = millis();
  if (speaking()) {
    // Never send our own voice or the chime to Gemini.
    lastActivityMs = now;
    drain(micBuffer);
    if (audioSinceEnd && socket.isConnected()) {
      socket.sendTXT("{\"type\":\"audio_end\"}");
      audioSinceEnd = false;
    }
  } else if (state == State::Active) {
    static uint8_t chunk[SEND_CHUNK_BYTES];
    while (xStreamBufferBytesAvailable(micBuffer) >= SEND_CHUNK_BYTES) {
      size_t bytes = xStreamBufferReceive(micBuffer, chunk, SEND_CHUNK_BYTES, 0);
      if (!socket.sendBIN(chunk, bytes)) break;
      sentBytes += bytes;
      audioSinceEnd = true;
    }
  }

  if (state == State::Connecting && now - startMs > CONNECT_TIMEOUT_MS) {
    endSession("connect timeout");
  } else if (state == State::Active && now - lastActivityMs > FOLLOW_UP_MS) {
    endSession(nullptr);  // follow-up window over
  } else if (now - startMs > MAX_SESSION_MS) {
    endSession("max session length");
  }
}

bool VOICE_SetUrl(const char *value) {
  String candidate = value;
  String oldHost = host, oldPath = path;
  uint16_t oldPort = port;
  bool oldSecure = secure;
  if (!parseUrl(candidate)) {
    host = oldHost;
    path = oldPath;
    port = oldPort;
    secure = oldSecure;
    return false;
  }
  Preferences prefs;
  if (!prefs.begin("miniBotVoice", false)) return false;
  bool ok = prefs.putString("url", candidate) == candidate.length();
  prefs.end();
  if (ok) url = candidate;
  return ok;
}

bool VOICE_SetKey(const char *value) {
  String candidate = value;
  if (!candidate.length()) return false;
  Preferences prefs;
  if (!prefs.begin("miniBotVoice", false)) return false;
  bool ok = prefs.putString("key", candidate) == candidate.length();
  prefs.end();
  if (ok) apiKey = candidate;
  return ok;
}

void VOICE_PrintStatus(Print &output) {
  output.printf("VOICE state=%s url=%s key=%s sessions=%lu sent=%lus received=%lus dropped=%lu last_error=%s\n",
                STATE_NAMES[(int)state], url.length() ? url.c_str() : "(unset)", apiKey.length() ? "set" : "(unset)",
                (unsigned long)sessionCount, (unsigned long)(sentBytes / BYTES_PER_SECOND),
                (unsigned long)(receivedBytes / BYTES_PER_SECOND), (unsigned long)droppedBytes, lastError);
}
