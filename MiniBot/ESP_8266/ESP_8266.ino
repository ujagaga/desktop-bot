#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <Servo.h>

#define WIFI_AP_SSID "MiniBot"
#define WIFI_AP_PASSWORD "MiniBot"
// ESP-01 RX: the only pin with no boot strapping role and no boot log output.
#define SERVO_PIN 3
// Release the servo once it has reached the angle so it doesn't buzz against the magnet.
#define SERVO_DETACH_MS 1000

ESP8266WebServer server(80);
Servo servo;
unsigned long servoMovedAt;
bool servoAttached;

void handleServo() {
  String arg = server.arg("angle");
  int angle = arg.toInt();
  if (arg.length() == 0 || angle < 0 || angle > 180 || (angle == 0 && arg != "0")) {
    server.send(400, "text/plain", "angle must be 0-180\n");
    return;
  }
  servo.attach(SERVO_PIN);
  servo.write(angle);
  servoAttached = true;
  servoMovedAt = millis();
  server.send(200, "text/plain", String(angle) + "\n");
}

void setup() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
  server.on("/servo", HTTP_GET, handleServo);
  server.begin();
}

void loop() {
  server.handleClient();
  if (servoAttached && millis() - servoMovedAt > SERVO_DETACH_MS) {
    servo.detach();
    servoAttached = false;
  }
}
