#include "voice.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/stream_buffer.h>
#include <math.h>

#include "audio.h"
#include "comms.h"
#include "config.h"
#include "faces.h"
#include "lcd.h"
#include "screen.h"
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
constexpr unsigned long HEALTH_INTERVAL_MS = 60000;
constexpr unsigned long MAX_DISPLAY_S = 600;

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
// A gateway command's display stays displayHoldMs, or until the session ends when that is 0.
bool displayHeld = false;
unsigned long displayStartMs = 0, displayHoldMs = 0;
unsigned long lastHealthMs = 0;
bool serverOk = true;  // until a health check says otherwise
int healthCode = 0;
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
  } else if (!displayHeld || !displayHoldMs) {
    displayHeld = false;
    SCREEN_Refresh();
  }
}

void showDisplay(const char *clock, int face, const char *text, unsigned long seconds) {
  if (clock) {
    LCD_ShowTime(clock, "");
  } else {
    bool faceShown = face >= 0 && FACE_Show(face);
    if (text) faceShown ? LCD_ShowFooter(text) : LCD_ShowText(text);
  }
  displayHeld = true;
  displayStartMs = millis();
  displayHoldMs = min(seconds, MAX_DISPLAY_S) * 1000;
}

// HTTP(S) request to another gateway endpoint on the same host, authenticated with the device key.
struct GatewayRequest {
  HTTPClient http;
  NetworkClientSecure secureClient;
  NetworkClient plainClient;

  bool begin(const char *endpoint) {
    String target = String(secure ? "https://" : "http://") + host + ":" + port + endpoint;
    if (secure) secureClient.setCACertBundle(bundleStart, bundleEnd - bundleStart);
    if (!(secure ? http.begin(secureClient, target) : http.begin(plainClient, target))) return false;
    http.setTimeout(5000);
    http.addHeader("X-API-Key", apiKey);
    return true;
  }
};

// GET /health on the gateway host; needs the device key and Gemini configured on the Pi.
void checkHealth() {
  lastHealthMs = millis();
  GatewayRequest request;
  if (!request.begin("/health")) return;
  HTTPClient &http = request.http;
  healthCode = http.GET();
  serverOk = healthCode == 200 && http.getString().indexOf("\"gemini_configured\": true") >= 0;
  http.end();
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
    if (doc["session"].is<const char *>()) COMMS_RequestCameraSnapshot(doc["session"]);
  } else if (strcmp(type, "transcript") == 0) {
    if (strcmp(doc["role"] | "", "user") == 0) lastActivityMs = millis();
    Serial.printf("VOICE %s: %s\n", (const char *)(doc["role"] | ""), (const char *)(doc["text"] | ""));
  } else if (strcmp(type, "interrupted") == 0) {
    drain(playBuffer);
  } else if (strcmp(type, "error") == 0) {
    Serial.printf("VOICE gateway error: %s\n", (const char *)(doc["error"] | ""));
    endSession("gateway error");
  } else if (strcmp(type, "display") == 0) {
    showDisplay(doc["clock"] | (const char *)nullptr, doc["face"] | -1, doc["text"] | (const char *)nullptr, doc["seconds"] | 0UL);
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
  if (displayHeld && displayHoldMs && millis() - displayStartMs >= displayHoldMs) {
    displayHeld = false;
    if (!errorFaceMs) SCREEN_Refresh();
  }
  if (errorFaceMs && millis() - errorFaceMs > ERROR_FACE_MS) {
    errorFaceMs = 0;
    if (state == State::Idle) showResult(nullptr);
  }
  if (state == State::Idle) {
    if (host.length() && WiFi.status() == WL_CONNECTED &&
        (!lastHealthMs || millis() - lastHealthMs > HEALTH_INTERVAL_MS)) {
      checkHealth();
    }
    return;
  }
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
  if (ok) {
    url = candidate;
    lastHealthMs = 0;
  }
  return ok;
}

bool VOICE_SetKey(const char *value) {
  String candidate = value;
  if (!candidate.length()) return false;
  Preferences prefs;
  if (!prefs.begin("miniBotVoice", false)) return false;
  bool ok = prefs.putString("key", candidate) == candidate.length();
  prefs.end();
  if (ok) {
    apiKey = candidate;
    lastHealthMs = 0;
  }
  return ok;
}

bool VOICE_ServerVoice(Print &output, const char *name) {
  if (!host.length() || !apiKey.length()) {
    output.println("ERR voice url/key not set");
    return false;
  }
  String body;
  if (name) {
    String chosen = strcasecmp(name, "default") == 0 ? "" : name;
    for (char c : chosen) {
      if (!isalpha((unsigned char)c)) {
        output.println("ERR voice name must be letters only");
        return false;
      }
    }
    // Gateway names are capitalized (Kore, Puck, ...); accept any case here.
    chosen.toLowerCase();
    if (chosen.length()) chosen[0] = toupper(chosen[0]);
    body = "{\"voice\":\"" + chosen + "\"}";
  }
  GatewayRequest request;
  if (!request.begin("/voice")) {
    output.println("ERR cannot reach gateway");
    return false;
  }
  int code;
  if (name) {
    request.http.addHeader("Content-Type", "application/json");
    code = request.http.POST(body);
  } else {
    code = request.http.GET();
  }
  JsonDocument doc;
  bool parsed = code > 0 && !deserializeJson(doc, request.http.getString());
  request.http.end();
  if (!parsed || (code != 200 && code != 400)) {
    output.printf("ERR gateway voice request failed (%d)\n", code);
    return false;
  }
  if (code == 400) output.print("ERR unknown voice. ");
  else output.printf("Server voice: %s\n", strlen(doc["voice"] | "") ? (const char *)doc["voice"] : "default");
  output.print("Voices: default");
  for (JsonVariant voice : doc["voices"].as<JsonArray>()) {
    output.print(' ');
    output.print(voice.as<const char *>());
  }
  output.println();
  return code == 200;
}

bool VOICE_IsBusy() {
  return state != State::Idle || errorFaceMs || displayHeld;
}

bool VOICE_ServerOk() {
  return serverOk;
}

void VOICE_PrintStatus(Print &output) {
  output.printf("VOICE state=%s url=%s key=%s server=%s(%d) sessions=%lu sent=%lus received=%lus dropped=%lu "
                "last_error=%s\n",
                STATE_NAMES[(int)state], url.length() ? url.c_str() : "(unset)", apiKey.length() ? "set" : "(unset)",
                serverOk ? "ok" : "fail", healthCode, (unsigned long)sessionCount,
                (unsigned long)(sentBytes / BYTES_PER_SECOND), (unsigned long)(receivedBytes / BYTES_PER_SECOND),
                (unsigned long)droppedBytes, lastError);
}
