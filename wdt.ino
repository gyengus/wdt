#include "config.h"

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266NetBIOS.h>
#include <ESP8266Ping.h>
#include <WiFiUdp.h>
#include <Syslog.h>
#include <Ticker.h>

ESP8266WebServer server(80);
WiFiClient espClient;
WiFiUDP udpClient;
Ticker ticker;

Syslog syslog(udpClient, SYSLOG_SERVER, SYSLOG_PORT, DEVICE_HOSTNAME, APP_NAME, LOG_INFO);
String macAddress = "";
long lastWiFiReconnectAttempt = 0;
int failedPings = 0;
long lastPingTime = 0;
bool lastPingResult = false;
bool needCheck = true;

void setNeedCheck();
void checkHost();
bool pingHost();
void resetHost();
bool connectToWiFi();
void sendNotification(String message);

void serveJSON() {
  String json = "{\"deviceName\": \"" + String(DEVICE_HOSTNAME) + "\","
              + "\"mac\": \"" + WiFi.macAddress() + "\","
              + "\"sketchSize\": \"" + String(ESP.getSketchSize()) + "\","
              + "\"freeSketchSize\": \"" + String(ESP.getFreeSketchSpace()) + "\","
              + "\"flashChipSize\": \"" + String(ESP.getFlashChipSize()) + "\","
              + "\"realFlashSize\": \"" + String(ESP.getFlashChipRealSize()) + "\","
              + "\"syslogServer\": \"" + SYSLOG_SERVER + "\","
              + "\"watchedHost\": \"" + HOST + "\","
              + "\"lastPingTime\": \"" + String(lastPingTime) + "\","
              + "\"lastPingResult\": \"" + (lastPingResult ? "success" : "failed") + "\""
              + "}";
  server.send(200, "application/json", json);
}

void setup() {
  ESP.wdtDisable();
  pinMode(RELAY, OUTPUT);

  if (DEBUG) {
    Serial.begin(115200);
    delay(1000);
    Serial.println("");
    macAddress = WiFi.macAddress();
    Serial.print("MAC address: ");
    Serial.println(macAddress);
  } else {
    pinMode(LED, OUTPUT);
  }

  NBNS.begin(DEVICE_HOSTNAME);
  connectToWiFi();

  server.on("/", serveJSON);
  server.begin();

  ticker.attach(PING_INTERVALL, setNeedCheck);

  ESP.wdtEnable(5000);

  IPAddress ip = WiFi.localIP();
  String buf = "Connected, IP: " + String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
  syslog.logf(LOG_INFO, buf.c_str());
  buf = "Started, watching host: " + String(HOST);
  syslog.logf(LOG_INFO, buf.c_str());
}

void loop() {
  ESP.wdtFeed();
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
    if (needCheck) {
      needCheck = false;
      checkHost();
    }
  } else {
    if (DEBUG) Serial.println("Disconnected from WiFi network");
    long now = millis();
    if ((now - lastWiFiReconnectAttempt) >= 2000) {
      if (connectToWiFi()) {
        lastWiFiReconnectAttempt = 0;
      } else {
        lastWiFiReconnectAttempt = millis();
      }
    }
  }
}

void resetHost() {
  ESP.wdtFeed();
  ticker.detach();

  digitalWrite(RELAY, HIGH);
  if (DEBUG) Serial.println("Relay high");
  delay(1000);
  digitalWrite(RELAY, LOW);
  if (DEBUG) Serial.println("Relay low");
  String buf = "Host (" + String(HOST) + ") restarted";
  syslog.logf(LOG_INFO, buf.c_str());
  sendNotification(buf);

  ticker.attach(PING_INTERVALL, setNeedCheck);
}

void setNeedCheck() {
  needCheck = true;
}

void checkHost() {
  if (pingHost()) {
    failedPings = 0;
  } else {
    failedPings++;
    if (failedPings >= PING_RETRY_NUM) {
      String buf = "Host (" + String(HOST) + ") unreachable";
      syslog.logf(LOG_ERR, buf.c_str());
      if (DEBUG) Serial.println(buf);
      failedPings = 0;
      resetHost();
    }
  }
}
bool pingHost() {
  if (!DEBUG) digitalWrite(LED, LOW);
  if (lastPingResult = Ping.ping(HOST, PING_NUM)) {
    if (DEBUG) Serial.println("Ping OK");
  } else {
    if (DEBUG) Serial.println("Ping failed");
    syslog.logf(LOG_ERR, "Ping failed");
  }
  if (!DEBUG) digitalWrite(LED, HIGH);
  lastPingTime = millis();
  return lastPingResult;
}

bool connectToWiFi() {
  WiFi.mode(WIFI_STA);
  if (DEBUG) {
    Serial.print("Connecting to WiFi network: ");
    Serial.println(sta_ssid);
  }
  WiFi.hostname(DEVICE_HOSTNAME);
  WiFi.begin(sta_ssid, sta_password);

  int i = 0;
  while (WiFi.status() != WL_CONNECTED) {
    ESP.wdtFeed();
    delay(500);
    if (DEBUG) Serial.print(".");
    i++;
    if (i >= 20) break;
  }
  if (DEBUG) Serial.println("");

  if (WiFi.status() == WL_CONNECTED) {
    if (DEBUG) {
      Serial.print("Success, IP address: ");
      Serial.println(WiFi.localIP());
    }
    return true;
  } else {
    if (DEBUG) Serial.println("Unable to connect");
    return false;
  }
}

void sendNotification(String message) {
  WiFiClient client;

  if (client.connect("maker.ifttt.com", 80)) {
    String body = "{\"value1\": \"[" + String(DEVICE_HOSTNAME) + "] " + message + "\"}";
    if (DEBUG) {
      Serial.println("Url: maker.ifttt.com/trigger/" + String(maker_event) + "/with/key/" + String(maker_api_key));
      Serial.println("Connected to remote host, sending data: " + body + "\n");
    }

    client.println("POST /trigger/" + String(maker_event) + "/with/key/" + String(maker_api_key) + " HTTP/1.1");
    client.println("Host: maker.ifttt.com");
    client.println("Content-Type: application/json");
    if (!DEBUG) client.println("Connection: close"); // Ha ezt küldöm, akkor a szerver válasz nélkül bontja a kapcsolatot.
    client.println("Content-Length: " + String(body.length()));
    client.println();
    client.println(body);
    client.println();
    delay(1000);

    if (DEBUG) {
      Serial.println("IFTTT answer:");
      char c;
      while (client.available()){
        c = client.read();
        Serial.print(c);
      }
      Serial.println();
    }

    client.stop();
  } else {
    if (DEBUG) Serial.println("Error iftttMessage");
  }
}
