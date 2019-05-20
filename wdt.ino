/*
 * Generic ESP8266 Module, 80MHz, Flash, ck, 26MHz, 40MHz, dout, 1M (64K SPIFFS), 1, v1.4 Compile from source, Disabled, None, Only Sketch, 115200
 */
#include "config.h"

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266Ping.h>
#include <WiFiUdp.h>
#include <Syslog.h>
#include <Ticker.h>

ESP8266WebServer server(80);
WiFiClient espClient;
Ticker ticker;
#if defined(SYSLOG_SERVER)
WiFiUDP udpClient;
Syslog syslog(udpClient, SYSLOG_SERVER, SYSLOG_PORT, DEVICE_HOSTNAME, APP_NAME, LOG_INFO);
#endif

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
bool reseted = false;
int avg_time_ms = -1;
bool needCheck = true;
char hostname[] = HOST;
IPAddress host_ip;

void setNeedCheck();
void checkHost();
bool pingHost();
void resetHost();
bool connectToWiFi();
void sendNotification(String message);
bool log(uint16_t pri, const char *fmt);
String collectDataForJSON(bool forHTTP);

void serveJSON() {
	server.send(200, "application/json", collectDataForJSON(true));
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

	connectToWiFi();

	IPAddress ip = WiFi.localIP();

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
	client.set_callback(receiveFromMQTT);
	lastMQTTReconnectAttempt = 0;
#endif

	ticker.attach(PING_INTERVALL, setNeedCheck);

	ESP.wdtEnable(5000);

	String buf = "Started " + String(DEVICE_HOSTNAME) + " (commit: " + String(COMMIT_HASH) + "), IP: " + String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]) + ", watching host: " + String(HOST);
	log(LOG_INFO, buf.c_str());
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
	log(LOG_INFO, buf.c_str());
	sendNotification(buf);
	reseted = true;

	ticker.attach(WAIT_AFTER_RESET, setNeedCheck);
}

void setNeedCheck() {
	needCheck = true;
}

void checkHost() {
	if (pingHost()) {
		failedPings = 0;
		reseted = false;
	} else {
		failedPings++;
		if (failedPings >= PING_RETRY_NUM) {
			String buf = "Host (" + String(HOST) + ") unreachable";
			log(LOG_ERR, buf.c_str());
#if defined(DEBUG)
			Serial.println(buf);
#endif
			failedPings = 0;
			if (!reseted) resetHost();
		}
	}
}

bool pingHost() {
#if !defined(DEBUG)
	digitalWrite(LED, LOW);
#endif
	IPAddress tmpip;
	if (WiFi.hostByName(HOST, tmpip, 5000)) {
		host_ip = tmpip;
	}
	if (lastPingResult = Ping.ping(host_ip, PING_NUM)) {
#if defined(DEBUG)
		Serial.println("Ping OK");
#endif
		avg_time_ms = Ping.averageTime();
	} else {
#if defined(DEBUG)
		Serial.println("Ping failed");
#endif
		log(LOG_ERR, "Ping failed");
		avg_time_ms = -1;
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
	Serial.println(STA_SSID);
#endif
	WiFi.hostname(DEVICE_HOSTNAME);
	WiFi.begin(STA_SSID, STA_PASSWORD);

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
#if defined(MAKER_EVENT)
	WiFiClient client;

	if (client.connect("maker.ifttt.com", 80)) {
		String body = "{\"value1\": \"[" + String(DEVICE_HOSTNAME) + "] " + message + "\"}";
#if defined(DEBUG)
		Serial.println("Url: maker.ifttt.com/trigger/" + String(MAKER_EVENT) + "/with/key/" + String(MAKER_API_KEY));
		Serial.println("Connected to remote host, sending data: " + body + "\n");
#endif

		client.println("POST /trigger/" + String(MAKER_EVENT) + "/with/key/" + String(MAKER_API_KEY) + " HTTP/1.1");
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
#endif
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
		publishToMQTT(MQTT_DEVICE_TOPIC_FULL, collectDataForJSON(false), true);
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
			String buf = "Receiving firmware update of " + String(size) + " bytes";
			log(LOG_INFO, buf.c_str());
#if defined(DEBUG)
			Serial.println("Receiving firmware update of " + String(size) + " bytes");
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
				log(LOG_INFO, "Update success, restart...");
				requireRestart = true;
			}
		}
	}
}
#endif

bool log(uint16_t pri, const char *fmt) {
#if defined(SYSLOG_SERVER)
	return syslog.logf(pri, fmt);
#else
	return true;
#endif
}

String collectDataForJSON(bool forHTTP) {
	String json = "{\"deviceName\": \"" + String(DEVICE_HOSTNAME) + "\","
		+ "\"deviceDescription\": \"" + String(DEVICE_DESCRIPTION) + "\","
		+ "\"mac\": \"" + WiFi.macAddress() + "\","
		+ "\"commitHash\": \"" + String(COMMIT_HASH) + "\","
		+ "\"sketchSize\": " + String(ESP.getSketchSize()) + ","
		+ "\"freeSketchSize\": " + String(ESP.getFreeSketchSpace()) + ","
		+ "\"flashChipSize\": " + String(ESP.getFlashChipSize()) + ","
		+ "\"realFlashSize\": " + String(ESP.getFlashChipRealSize()) + ","
#if defined(SYSLOG_SERVER)
		+ "\"syslogServer\": \"" + SYSLOG_SERVER + "\","
#endif
		+ "\"watchedHost\": \"" + HOST + "\",";
	if (forHTTP) {
			json += "\"lastPingTime\": " + String(lastPingTime) + ","
				+ "\"lastPingResult\": \"" + (lastPingResult ? "success" : "failed") + "\","
				+ "\"lastPingAvgTime\": " + String(avg_time_ms) + ","
#if defined(MQTT_HOST)
		+ "\"mqttState\": \"" + (client.connected() ? "" : "dis") + "connected\","
		+ "\"deviceTopic\": \"" + MQTT_DEVICE_TOPIC_FULL + "\","
#endif
		+ "";
} else  {
	json += "\"ip\": \"" + WiFi.localIP().toString() + "\",";
}
#if defined(DEBUG)
		json += "\"debug\": true";
#else
		json += "\"debug\": false";
#endif
		json += "}";
	return json;
}
