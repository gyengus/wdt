/*
 * Generic ESP8266 Module, 80MHz, Flash, ck, 26MHz, 40MHz, DOUT, 1M (64K SPIFFS), 1, v1.4 Compile from source, Disabled, None, Only Sketch, 115200
 */
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

#if defined(MQTT_HOST)
#define SPIFFS_ALIGNED_OBJECT_INDEX_TABLES 1
#include "PubSubClient.h"
PubSubClient client(espClient);
unsigned long lastMQTTReconnectAttempt = 0;
String MQTT_DEVICE_TOPIC_FULL = "";
String MQTT_UPDATE_TOPIC_FULL = "";
boolean requireRestart = false;

bool publishToMQTT(String topic, String payload, bool retain = true);
void mqttDisConnect();
void mqttReConnect();
boolean connectToMQTT();
void receiveFromMQTT(const MQTT::Publish& pub);
#endif

String macAddress = "";
unsigned long lastWiFiReconnectAttempt = 0;
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
		+ "\"commitHash\": \"" + String(COMMIT_HASH) + "\","
		+ "\"sketchSize\": \"" + String(ESP.getSketchSize()) + "\","
		+ "\"freeSketchSize\": \"" + String(ESP.getFreeSketchSpace()) + "\","
		+ "\"flashChipSize\": \"" + String(ESP.getFlashChipSize()) + "\","
		+ "\"realFlashSize\": \"" + String(ESP.getFlashChipRealSize()) + "\","
		+ "\"syslogServer\": \"" + SYSLOG_SERVER + "\","
		+ "\"watchedHost\": \"" + HOST + "\","
		+ "\"lastPingTime\": \"" + String(lastPingTime) + "\","
		+ "\"lastPingResult\": \"" + (lastPingResult ? "success" : "failed") + "\","
#if defined(MQTT_HOST)
		+ "\"mqttState\": \"" + (client.connected() ? "" : "dis") + "connected\","
#endif
#if defined(DEBUG)
		+ "\"debug\": true"
#else
		+ "\"debug\": false"
#endif
		+ "}";
	server.send(200, "application/json", json);
}

void serveReset() {
	String token = server.arg("access-token");
	if (token == ACCESS_TOKEN) {
		resetHost();
		server.send(200, "application/json", "{\"status\": \"OK\"}");
	} else {
		server.send(403, "application/json", "{\"status\": \"ACCESS DENIED\"}");
	}
}

void setup() {
	ESP.wdtDisable();
	pinMode(RELAY, OUTPUT);

	macAddress = WiFi.macAddress();

#if defined(DEBUG)
	Serial.begin(115200);
	delay(1000);
	Serial.println("");
	Serial.print("MAC address: ");
	Serial.println(macAddress);
#else
	pinMode(LED, OUTPUT);
#endif

	NBNS.begin(DEVICE_HOSTNAME);
	connectToWiFi();

	server.on("/", serveJSON);
	server.on("/reset", serveReset);
#if defined(MQTT_HOST)
	server.on("/mqttreconnect", mqttReConnect);
	server.on("/mqttdisconnect", mqttDisConnect);
#endif
	server.begin();

#if defined(MQTT_HOST)
	MQTT_DEVICE_TOPIC_FULL = MQTT_DEVICE_TOPIC + String(DEVICE_HOSTNAME);
	MQTT_UPDATE_TOPIC_FULL = MQTT_DEVICE_TOPIC_FULL + String("/update");
	client.set_server(MQTT_HOST, MQTT_PORT);
	lastMQTTReconnectAttempt = 0;
#endif

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
#if defined(MQTT_HOST)
	if (requireRestart) {
		client.loop();
		requireRestart = false;
#if defined(DEBUG)
		Serial.println("Reboot...");
#endif
		client.disconnect();
		ESP.wdtDisable();
		delay(200);
		ESP.restart();
		while (1) {
			ESP.wdtFeed();
			delay(200);
		}
	}
#endif
	if (WiFi.status() == WL_CONNECTED) {
		server.handleClient();
		if (needCheck) {
			needCheck = false;
			checkHost();
		}
#if defined(MQTT_HOST)
		if (client.connected()) {
			client.loop();
		} else {
			unsigned long now = millis();
			if (now - lastMQTTReconnectAttempt >= 5000) {
				if (connectToMQTT()) {
					lastMQTTReconnectAttempt = 0;
				} else {
					lastMQTTReconnectAttempt = millis();
				}
			}
		}
#endif
	} else {
#if defined(DEBUG)
		Serial.println("Disconnected from WiFi network");
#endif
		unsigned long now = millis();
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
#if defined(DEBUG)
	Serial.println("Relay high");
#endif
	delay(1000);
	digitalWrite(RELAY, LOW);
#if defined(DEBUG)
	Serial.println("Relay low");
#endif
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
#if defined(DEBUG)
			Serial.println(buf);
#endif
			failedPings = 0;
			resetHost();
		}
	}
}
bool pingHost() {
#if !defined(DEBUG)
	digitalWrite(LED, LOW);
#endif
	if (lastPingResult = Ping.ping(HOST, PING_NUM)) {
#if defined(DEBUG)
		Serial.println("Ping OK");
#endif
	} else {
#if defined(DEBUG)
		Serial.println("Ping failed");
#endif
		syslog.logf(LOG_ERR, "Ping failed");
	}
#if !defined(DEBUG)
	digitalWrite(LED, HIGH);
#endif
	lastPingTime = millis();
	return lastPingResult;
}

bool connectToWiFi() {
	WiFi.mode(WIFI_STA);
#if defined(DEBUG)
	Serial.print("Connecting to WiFi network: ");
	Serial.println(sta_ssid);
#endif
	WiFi.hostname(DEVICE_HOSTNAME);
	WiFi.begin(sta_ssid, sta_password);

	int i = 0;
	while (WiFi.status() != WL_CONNECTED) {
		ESP.wdtFeed();
		delay(500);
#if defined(DEBUG)
		Serial.print(".");
#endif
		i++;
		if (i >= 20) break;
	}
#if defined(DEBUG)
	Serial.println("");
#endif

	if (WiFi.status() == WL_CONNECTED) {
#if defined(DEBUG)
		Serial.print("Success, IP address: ");
		Serial.println(WiFi.localIP());
#endif
		return true;
	} else {
#if defined(DEBUG)
		Serial.println("Unable to connect");
#endif
		return false;
	}
}

void sendNotification(String message) {
	WiFiClient client;

	if (client.connect("maker.ifttt.com", 80)) {
		String body = "{\"value1\": \"[" + String(DEVICE_HOSTNAME) + "] " + message + "\"}";
#if defined(DEBUG)
		Serial.println("Url: maker.ifttt.com/trigger/" + String(maker_event) + "/with/key/" + String(maker_api_key));
		Serial.println("Connected to remote host, sending data: " + body + "\n");
#endif

		client.println("POST /trigger/" + String(maker_event) + "/with/key/" + String(maker_api_key) + " HTTP/1.1");
		client.println("Host: maker.ifttt.com");
		client.println("Content-Type: application/json");
#if !defined(DEBUG)
		client.println("Connection: close"); // Ha ezt küldöm, akkor a szerver válasz nélkül bontja a kapcsolatot.
#endif
		client.println("Content-Length: " + String(body.length()));
		client.println();
		client.println(body);
		client.println();
		delay(1000);

#if defined(DEBUG)
		Serial.println("IFTTT answer:");
		char c;
		while (client.available()){
			c = client.read();
			Serial.print(c);
		}
		Serial.println();
#endif

		client.stop();
	} else {
#if defined(DEBUG)
		Serial.println("Error iftttMessage");
#endif
	}
}

#if defined(MQTT_HOST)
void mqttDisConnect() {
	if (client.connected()) {
		client.disconnect();
		delay(10);
	}
	serveJSON();
}
void mqttReConnect() {
	if (client.connected()) {
		client.disconnect();
		delay(50);
	}
	connectToMQTT();
	lastMQTTReconnectAttempt = 0;
	delay(10);
	serveJSON();
}

boolean connectToMQTT() {
#if defined(DEBUG)
	Serial.print("Trying to connect MQTT broker...");
#endif
	MQTT::Connect con(DEVICE_HOSTNAME);
	con.set_clean_session();
	con.set_will(MQTT_DEVICE_TOPIC_FULL, "");
	con.set_keepalive(30);
	if (MQTT_USER) {
		con.set_auth(MQTT_USER, MQTT_PASSWORD);
	}
	if (client.connect(con)) {
#if defined(DEBUG)
		Serial.println(" success");
#endif
		String deviceData = "{\"name\": \"" + String(DEVICE_HOSTNAME) + "\", \"deviceDescription\": \"" + String(DEVICE_DESCRIPTION) + "\", \"commitHash\": \"" + String(COMMIT_HASH) + "\", \"ip\": \"" + WiFi.localIP().toString() + "\", \"mac\": \"" + macAddress + "\"}";
		publishToMQTT(MQTT_DEVICE_TOPIC_FULL, deviceData, true);
    client.subscribe(MQTT_UPDATE_TOPIC_FULL);
	} else {
#if defined(DEBUG)
		Serial.println(" failed");
#endif
	}
	return client.connected();
}

bool publishToMQTT(String topic, String payload, bool retain) {
	if (!client.connected()) {
		connectToMQTT();
	}
	if (retain) {
		client.publish(MQTT::Publish(topic, payload).set_retain());
	} else {
		client.publish(topic, payload);
	}
	client.loop();
	return true;
}

void receiveFromMQTT(const MQTT::Publish& pub) {
#if defined(DEBUG)
	Serial.print("Message arrived [");
	Serial.print(pub.topic());
	Serial.print("] Size: " + String(pub.payload_len()) + " B");
	Serial.println();
#endif
	if (pub.topic() == MQTT_UPDATE_TOPIC_FULL) {
		ESP.wdtFeed();
		uint32_t size = pub.payload_len();
		if (size == 0) {
#if defined(DEBUG)
			Serial.println("Error, sketch size is 0 B");
#endif
		} else {
#if defined(DEBUG)
			Serial.println("Receiving firmware update of " + String(size) + " bytes...");
			Serial.setDebugOutput(false);
#endif
			ESP.wdtFeed();
			if (ESP.updateSketch(*pub.payload_stream(), size, false, false)) {
				ESP.wdtFeed();
#if defined(DEBUG)
				Serial.println("Clearing retained message.");
#endif
				publishToMQTT(MQTT_UPDATE_TOPIC_FULL, "", true);
				ESP.wdtFeed();
#if defined(DEBUG)
				Serial.println("Update success");
#endif
				requireRestart = true;
			}
		}
	}
}
#endif
