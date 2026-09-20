#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ctype.h>
#include <strings.h>
#include "http_server.h"
#include "comms.h"

static WebServer server(80);
static bool running = false;
static bool sleepPending = false;
static unsigned long sleepRequestedAt = 0;
static const unsigned long SLEEP_RESPONSE_GRACE_MS = 500;

static bool isSleepCommand(const String &command) {
  // Match the shared dispatcher's case-insensitive first token, including
  // ignored sleep arguments, without accepting prefixes such as "sleepy".
  const char *text = command.c_str();
  return command.length() >= 5 && strncasecmp(text, "sleep", 5) == 0 &&
         (text[5] == '\0' || isspace(static_cast<unsigned char>(text[5])));
}

// Capture command replies without redirecting either serial port.
class CommandOutput : public Print {
 public:
  String text;
  size_t write(uint8_t value) override {
    return text.concat(static_cast<char>(value)) ? 1 : 0;
  }
};

static const char consolePage[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MiniBot console</title>
<style>
body{max-width:850px;margin:2rem auto;padding:0 1rem;background:#111827;color:#e5e7eb;font:16px system-ui}
pre{height:55vh;overflow:auto;background:#030712;padding:1rem;white-space:pre-wrap;overflow-wrap:anywhere;border-radius:8px}
form{display:flex;gap:.5rem}input{flex:1;min-width:0}input,button{font:inherit;padding:.7rem;border-radius:5px}small{display:block;margin:1rem 0;color:#9ca3af}
</style>
</head>
<body>
<h1>MiniBot console</h1>
<p>Enter a serial command. Use <code>help</code> to list commands.</p>
<pre id="output" role="log" aria-live="polite"></pre>
<form id="console" action="/command" method="get">
<input id="command" name="cmd" aria-label="Command" placeholder="help" maxlength="127" autocomplete="off" required autofocus>
<button id="send" type="submit">Send</button>
</form>
<small>Sleep and Wi-Fi commands may disconnect this console. Long commands delay responses.</small>
<script>
const form=document.getElementById('console');
const input=document.getElementById('command');
const output=document.getElementById('output');
const send=document.getElementById('send');
function append(text){output.textContent=(output.textContent+text).slice(-32768);output.scrollTop=output.scrollHeight;}
form.addEventListener('submit',async event=>{
  event.preventDefault();
  const command=input.value;
  if(!command.trim())return;
  append('> '+command+'\n');
  input.disabled=true;send.disabled=true;
  try{
    const response=await fetch('/command?'+new URLSearchParams({cmd:command}),{cache:'no-store'});
    const text=await response.text();
    append(text+(text.endsWith('\n')?'':'\n'));
    if(!response.ok)append('HTTP '+response.status+'\n');
  }catch(error){append('Connection lost or request failed. Check Wi-Fi; the command may have executed.\n');}
  finally{input.disabled=false;send.disabled=false;input.value='';input.focus();}
});
</script>
</body>
</html>)HTML";

static void handleCommand() {
  server.sendHeader("Cache-Control", "no-store");
  if (!server.hasArg("cmd") || server.arg("cmd").isEmpty()) {
    server.send(400, "text/plain; charset=utf-8", "ERR missing cmd parameter\n");
    return;
  }
  String command = server.arg("cmd");
  // Reject embedded NULs rather than executing a silently truncated command.
  if (command.length() > 127 || command.indexOf('\r') >= 0 ||
      command.indexOf('\n') >= 0 || strlen(command.c_str()) != command.length()) {
    server.send(400, "text/plain; charset=utf-8",
                "ERR expected one command line, maximum 127 bytes\n");
    return;
  }
  if (isSleepCommand(command)) {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain; charset=utf-8", "OK sleep scheduled\n");
    server.client().stop();
    // NetworkClient::flush() is a no-op. Give the network stack time to send
    // the response and close before the shared handler disables Wi-Fi.
    sleepRequestedAt = millis();
    sleepPending = true;
    return;
  }
  CommandOutput output;
  COMMS_Execute(command.c_str(), output);
  server.send(200, "text/plain; charset=utf-8", output.text);
}

void HTTP_SERVER_Init() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", consolePage);
  });
  server.on("/command", HTTP_GET, handleCommand);
  server.onNotFound([]() {
    server.send(404, "text/plain; charset=utf-8", "Not found\n");
  });
}

void HTTP_SERVER_Process() {
  if (sleepPending) {
    if (millis() - sleepRequestedAt < SLEEP_RESPONSE_GRACE_MS) return;
    sleepPending = false;
    server.stop();
    running = false;
    CommandOutput output;
    COMMS_Execute("sleep", output);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (running) server.stop();
    running = false;
    return;
  }
  if (!running) {
    server.begin();
    running = true;
    Serial.print("HTTP console: http://");
    Serial.println(WiFi.localIP());
  }
  server.handleClient();
}
