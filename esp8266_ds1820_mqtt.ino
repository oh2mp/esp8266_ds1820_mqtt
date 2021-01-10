/*
    esp8266 ds18x20 MQTT

    See https://github.com/oh2mp/
*/

#include <OneWire.h>
#include <DallasTemperature.h>

#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <FS.h>

/* ------------------------------------------------------------------------------- */
/* These are the pins for all ESP8266 boards */
//      Name   GPIO    Function
#define PIN_D0  16  // WAKE
#define PIN_D1   5  // User purpose
#define PIN_D2   4  // User purpose
#define PIN_D3   0  // Low on boot means enter FLASH mode
#define PIN_D4   2  // TXD1
#define PIN_D5  14  // HSCLK
#define PIN_D6  12  // HMISO
#define PIN_D7  13  // HMOSI  RXD2
#define PIN_D8  15  // HCS    TXD0
#define PIN_D9   3  //        RXD0
#define PIN_D10  1  //        TXD0

#define PIN_MOSI 8  // SD1
#define PIN_MISO 7  // SD0
#define PIN_SCLK 6  // CLK
#define PIN_HWCS 0  // D3

#define PIN_D11  9  // SD2
#define PIN_D12 10  // SD4
/* ------------------------------------------------------------------------------- */

#define LED PIN_D0
#define LED_ON LOW    // Reverse these if you use eg. ESP12E, not NodeMCU dev module. 
#define LED_OFF HIGH

#define ONE_WIRE_BUS PIN_D4
#define APREQUEST PIN_D3
#define APTIMEOUT 120000
#define MAX_SENSORS 8

float sens[MAX_SENSORS];        // sensor values
char sensname[MAX_SENSORS][24]; // sensor names
int sread = 0;                  // Flag if sensors have been read on this iteration
int onewire_wait = 1;           // if we are waiting for 1wire data
unsigned long mytime = 0;       // Used for delaying, see loop function
int scount = 0;                 // sensors amount
int interval = 0;               // interval in minutes
unsigned long portal_timer = 0;

// Default hostname base. Last 3 octets of MAC are added as hex.
// The hostname can be changed explicitly from the portal.
// Maxlen in ESP8266WiFi class is 24
char myhostname[24] = "esp8266-ds1820-";

// placeholder values
char topicbase[256] = "dallastemp";
char mqtt_user[64]  = "foo";
char mqtt_pass[64]  = "bar";
char mqtt_host[64]  = "192.168.36.99";
int  mqtt_port      = 1883;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DeviceAddress sensor[8];

ESP8266WiFiMulti WiFiMulti;
WiFiClient wificlient;
PubSubClient mqttclient(wificlient);

ESP8266WebServer server(80);
IPAddress apIP(192, 168, 4, 1); // portal ip address
File file;

/* ------------------------------------------------------------------------------- */
void loadWifis() {
    if (SPIFFS.exists("/known_wifis.txt")) {
        char ssid[33];
        char pass[65];

        file = SPIFFS.open("/known_wifis.txt", "r");
        while (file.available()) {
            memset(ssid, 0, sizeof(ssid));
            memset(pass, 0, sizeof(pass));
            file.readBytesUntil('\t', ssid, 32);
            file.readBytesUntil('\n', pass, 64);
            WiFiMulti.addAP(ssid, pass);
            Serial.printf("wifi loaded: %s / %s\n", ssid, pass);
        }
        file.close();
    } else {
        Serial.println("no wifis configured");
    }
}

/* ------------------------------------------------------------------------------- */
/* get sensor index from hexstring like "10F78DBA000800D7" */
int getSensorIndex(const char *hexString) {
    char tmp[3];
    char saddrstr[17];
    memset(saddrstr, 0, sizeof(saddrstr));
    for (int i = 0; i < scount; i++) {
        for (uint8_t j = 0; j < 8; j++) {
            sprintf(tmp, "%02X", sensor[i][j]);
            tmp[2] = 0;
            strcat(saddrstr, tmp);
        }
        if (strcmp(saddrstr, hexString) == 0) {
            return i;
        } else {
            memset(saddrstr, 0, sizeof(saddrstr));
        }
    }
    return -1; // no sensor found
}
/* ------------------------------------------------------------------------------- */
void loadSavedSensors() {
    char sname[25];
    char saddrstr[17];

    if (SPIFFS.exists("/known_sensors.txt")) {
        file = SPIFFS.open("/known_sensors.txt", "r");
        while (file.available()) {
            memset(sname, '\0', sizeof(sname));
            memset(saddrstr, '\0', sizeof(saddrstr));

            file.readBytesUntil('\t', saddrstr, 17);
            file.readBytesUntil('\n', sname, 25);
            if (getSensorIndex(saddrstr) > -1) {
                strcpy(sensname[getSensorIndex(saddrstr)], sname);
            }
        }
        file.close();
    }

}
/* ------------------------------------------------------------------------------- */
void loadMQTT() {
    if (SPIFFS.exists("/mqtt.txt")) {
        char tmpstr[8];
        memset(tmpstr, 0, sizeof(tmpstr));
        memset(mqtt_host, 0, sizeof(mqtt_host));
        memset(mqtt_user, 0, sizeof(mqtt_user));
        memset(mqtt_pass, 0, sizeof(mqtt_pass));
        memset(topicbase, 0, sizeof(topicbase));
        memset(myhostname, 0, sizeof(myhostname));

        file = SPIFFS.open("/mqtt.txt", "r");
        while (file.available()) {
            file.readBytesUntil(':', mqtt_host, sizeof(mqtt_host));
            file.readBytesUntil('\n', tmpstr, sizeof(tmpstr));
            mqtt_port = atoi(tmpstr);
            if (mqtt_port < 1 || mqtt_port > 65535) mqtt_port = 1883; // default
            file.readBytesUntil(':', mqtt_user, sizeof(mqtt_user));
            file.readBytesUntil('\n', mqtt_pass, sizeof(mqtt_pass));
            file.readBytesUntil('\n', topicbase, sizeof(topicbase));
            file.readBytesUntil('\n', myhostname, sizeof(myhostname));
            memset(tmpstr, 0, sizeof(tmpstr));
            file.readBytesUntil('\n', tmpstr, sizeof(tmpstr));
            interval = atoi(tmpstr);
        }
        file.close();
        Serial.printf("MQTT broker: %s:%d\nTopic prefix: %s\n", mqtt_host, mqtt_port, topicbase);
        Serial.printf("My hostname: %s\nInterval %d minutes\n", myhostname, interval);
    }
}

/* ------------------------------------------------------------------------------- */
void setup() {
    pinMode(APREQUEST, INPUT_PULLUP);
    pinMode(LED, OUTPUT);
    digitalWrite(LED, LED_OFF);
    
    Serial.begin(115200);
    Serial.println("\n\nESP8266 ds1820 to MQTT\n\n");

    // Append last 3 octets of MAC to the default hostname
    uint8_t mymac[6];
    wifi_get_macaddr(STATION_IF, mymac);
    char mac_end[8];
    sprintf(mac_end, "%02x%02x%02x", mymac[3], mymac[4], mymac[5]);
    strcat(myhostname, mac_end);
    Serial.printf("Default hostname: %s\n", myhostname);

    sensors.begin();
    delay(100);

    scount = sensors.getDeviceCount();
    if (scount > MAX_SENSORS) {
        scount = MAX_SENSORS;
    }
    for (int i = 0 ; i < scount; i++) {
        if (sensors.getAddress(sensor[i], i)) {
            Serial.printf("Found sensor %d: ", i);
            for (uint8_t j = 0; j < 8; j++) {
                Serial.printf("%02X", sensor[i][j]);
            }
            Serial.printf("\n");
        }
    }

    SPIFFS.begin();

    // If there are no configuration files, start portal.
    if (SPIFFS.exists("/mqtt.txt") && SPIFFS.exists("/known_wifis.txt") && SPIFFS.exists("/known_sensors.txt")) {
        loadMQTT();
        loadWifis();
        loadSavedSensors();

        WiFi.mode(WIFI_STA);
        if (strlen(myhostname) > 0) WiFi.hostname(myhostname);
        mqttclient.setServer(mqtt_host, mqtt_port);
    } else {
        startPortal();
    }
}

/* ------------------------------------------------------------------------------- */
void loop() {
    if (portal_timer == 0) {
        digitalWrite(LED, LED_OFF);

        sread = 0;

        // If it has been more than interval minutes since last temperature read, do it now
        if ((millis() - mytime > 60000 * interval && interval > 0) || interval == 0) {
            if (onewire_wait == 0) {
                mytime = millis();
                sensors.setWaitForConversion(false);
                sensors.requestTemperatures();
                pinMode(ONE_WIRE_BUS, OUTPUT);
                digitalWrite(ONE_WIRE_BUS, HIGH);
                onewire_wait = 1;
            }
        }
        // after 1000ms per sensor the sensors should be read already
        if (millis() - mytime > 1000 * scount && onewire_wait == 1) {
            mytime = millis();
            onewire_wait = 0;
            for (int i = 0; i < scount; i++) {
                if (sensors.isConnected(sensor[i])) {
                    sens[i] = sensors.getTempC(sensor[i]);
                    Serial.printf("sensor %d = %.4f°C\n", i, sens[i]);
                } else {
                    sens[i] = NULL;
                }
            }
            sread = 1;
        }
        if (sread == 1) {
            // send MQTT
            WiFi.mode(WIFI_STA);

            sread = 0;
            if (WiFiMulti.run() == WL_CONNECTED) {
                char json[32];  // This is enough space here
                char topic[128];
                // We send in deciCelsius
                // See: https://github.com/oh2mp/esp32_ble2mqtt/blob/main/DATAFORMATS.md
                //      TAG_DS1820 = 6
                if (mqttclient.connect(myhostname, mqtt_user, mqtt_pass)) {
                    for (int i = 0; i < scount; i++) {
                        if (sens[i] != NULL && sens[i] != 85.0 && sens[i] > -60.0) {
                            sprintf(json, "{\"type\":6,\"t\":%d}", int(sens[i] * 10 + .5));
                            if (strlen(topicbase) > 0) {
                                sprintf(topic, "%s/%s", topicbase, sensname[i]);
                            } else {
                                sprintf(topic, "%s", sensname[i]);
                            }
                            mqttclient.publish(topic, json);
                            Serial.printf("%s %s\n", topic, json);
                        }
                    }
                    mqttclient.disconnect();
                } else {
                    Serial.printf("Failed to connect MQTT broker, state=%d\n", mqttclient.state());
                }

            } else {
                Serial.printf("Failed to connect WiFi, status=%d\n", WiFi.status());
            }
        }
    } else if (portal_timer > 0) { // portal mode

        server.handleClient();

        // blink onboard leds if we are in portal mode
        if (int(millis() % 1000) < 500) {
            digitalWrite(LED, LED_ON);
        } else {
            digitalWrite(LED, LED_OFF);
        }
    }
    if (digitalRead(APREQUEST) == LOW && portal_timer == 0) {
        startPortal();
    }
    if (millis() - portal_timer > APTIMEOUT && portal_timer > 0) {
        Serial.println("Portal timeout. Booting.");
        delay(1000);
        ESP.restart();
    }
}
/* ------------------------------------------------------------------------------- */
// Portal code begins here
/* ------------------------------------------------------------------------------- */

void startPortal() {
    portal_timer = millis();
    WiFi.disconnect();
    delay(100);

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("ESP8266 DS1820 MQTT");

    server.on("/", httpRoot);
    server.on("/style.css", httpStyle);
    server.on("/wifis.html", httpWifis);
    server.on("/savewifi", httpSaveWifi);
    server.on("/sensors.html", httpSensors);
    server.on("/savesens", httpSaveSensors);
    server.on("/mqtt.html", httpMQTT);
    server.on("/savemqtt", httpSaveMQTT);

    server.on("/boot", httpBoot);

    server.onNotFound([]() {
        server.sendHeader("Refresh", "1;url=/");
        server.send(404, "text/plain", "QSD QSY");
    });
    server.begin();
    Serial.println("Started portal");
}
/* ------------------------------------------------------------------------------- */

void httpRoot() {
    portal_timer = millis();
    String html;

    file = SPIFFS.open("/index.html", "r");
    html = file.readString();
    file.close();

    server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */

void httpWifis() {
    String html;
    char tablerows[1024];
    char rowbuf[256];
    char ssid[33];
    char pass[33];
    int counter = 0;

    portal_timer = millis();
    memset(tablerows, '\0', sizeof(tablerows));

    file = SPIFFS.open("/wifis.html", "r");
    html = file.readString();
    file.close();

    if (SPIFFS.exists("/known_wifis.txt")) {
        file = SPIFFS.open("/known_wifis.txt", "r");
        while (file.available()) {
            memset(rowbuf, '\0', sizeof(rowbuf));
            memset(ssid, '\0', sizeof(ssid));
            memset(pass, '\0', sizeof(pass));
            file.readBytesUntil('\t', ssid, 33);
            file.readBytesUntil('\n', pass, 33);
            sprintf(rowbuf, "<tr><td>SSID</td><td><input type=\"text\" name=\"ssid%d\" maxlength=\"32\" value=\"%s\"></td></tr>", counter, ssid);
            strcat(tablerows, rowbuf);
            sprintf(rowbuf, "<tr><td>PASS</td><td><input type=\"text\" name=\"pass%d\" maxlength=\"32\" value=\"%s\"></td></tr>", counter, pass);
            strcat(tablerows, rowbuf);
            counter++;
        }
        file.close();
    }
    html.replace("###TABLEROWS###", tablerows);
    html.replace("###COUNTER###", String(counter));

    if (counter > 3) {
        html.replace("table-row", "none");
    }

    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSaveWifi() {
    portal_timer = millis();
    String html;

    file = SPIFFS.open("/known_wifis.txt", "w");

    for (int i = 0; i < server.arg("counter").toInt(); i++) {
        if (server.arg("ssid" + String(i)).length() > 0) {
            file.print(server.arg("ssid" + String(i)));
            file.print("\t");
            file.print(server.arg("pass" + String(i)));
            file.print("\n");
        }
    }
    // Add new
    if (server.arg("ssid").length() > 0) {
        file.print(server.arg("ssid"));
        file.print("\t");
        file.print(server.arg("pass"));
        file.print("\n");
    }
    file.close();

    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "3;url=/");
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSensors() {
    String html;
    char tablerows[1024];
    char rowbuf[256];
    char sname[25];
    char saddrstr[17];
    char tmp[4];
    int counter = 0;
    uint8_t unexistents = 0;
    DeviceAddress saddr;

    portal_timer = millis();
    memset(tablerows, '\0', sizeof(tablerows));
    memset(tmp, '\0', sizeof(tmp));

    file = SPIFFS.open("/sensors.html", "r");
    html = file.readString();
    file.close();

    for (int i = 0 ; i < scount; i++) {
        memset(saddrstr, '\0', sizeof(saddrstr));
        memset(sname, '\0', sizeof(sname));
        for (uint8_t j = 0; j < 8; j++) {
            sprintf(tmp, "%02X", sensor[i][j]);
            strcat(saddrstr, tmp); tmp[2] = 0;
            sprintf(tmp, "%02X:", sensor[i][j]);
            strcat(sname, tmp); tmp[3] = 0;
        }
        if (saddrstr[0] != 0) {
            sname[strlen(sname) - 1] = 0;
            sprintf(rowbuf, "<tr><td>%s<br /><input type=\"text\" name=\"sname%d\" maxlength=\"24\" value=\"%s\">", sname, i, sensname[i]);
            strcat(tablerows, rowbuf);
            sprintf(rowbuf, "<input type=\"hidden\" name=\"saddr%d\" value=\"%s\"></td></tr>", i, saddrstr);
            strcat(tablerows, rowbuf);
            counter++;
        }
    }
    if (SPIFFS.exists("/known_sensors.txt")) {
        file = SPIFFS.open("/known_sensors.txt", "r");
        while (file.available()) {
            memset(sname, '\0', sizeof(sname));
            memset(saddrstr, '\0', sizeof(saddrstr));

            file.readBytesUntil('\t', saddrstr, 17);
            file.readBytesUntil('\n', sname, 25);
            if (getSensorIndex(saddrstr) == -1) {
                if (saddrstr[0] != 0) {
                    if (unexistents == 0) {
                        unexistents = 1;
                        strcat(tablerows, "<tr><td><hr /><b>Unexistent but saved sensors</b></td></tr>");
                    }
                    if (sname[strlen(sname) - 1] == 13) {
                        sname[strlen(sname) - 1] = 0;
                    }
                    sprintf(rowbuf, "<tr><td>%s<br /><input type=\"text\" name=\"sname%d\" maxlength=\"24\" value=\"%s\">", saddrstr, counter, sname);
                    strcat(tablerows, rowbuf);
                    sprintf(rowbuf, "<input type=\"hidden\" name=\"saddr%d\" value=\"%s\"></td></tr>", counter, saddrstr);
                    strcat(tablerows, rowbuf);
                    counter++;
                }
            }
        }
        file.close();
    }

    html.replace("###TABLEROWS###", tablerows);
    html.replace("###COUNTER###", String(counter));

    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSaveSensors() {
    portal_timer = millis();
    String html;

    file = SPIFFS.open("/known_sensors.txt", "w");

    for (int i = 0; i < server.arg("counter").toInt(); i++) {
        if (server.arg("sname" + String(i)).length() > 0) {
            file.print(server.arg("saddr" + String(i)));
            file.print("\t");
            file.print(server.arg("sname" + String(i)));
            file.print("\n");
        }
    }
    file.close();
    loadSavedSensors(); // reread

    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "3;url=/");
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpMQTT() {
    portal_timer = millis();
    String html;

    file = SPIFFS.open("/mqtt.html", "r");
    html = file.readString();
    file.close();

    html.replace("###HOSTPORT###", String(mqtt_host) + ":" + String(mqtt_port));
    html.replace("###USERPASS###", String(mqtt_user) + ":" + String(mqtt_pass));
    html.replace("###TOPICBASE###", String(topicbase));
    html.replace("###MYHOSTNAME###", String(myhostname));
    html.replace("###INTERVAL###", String(interval));

    server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */
void httpSaveMQTT() {
    portal_timer = millis();
    String html;

    file = SPIFFS.open("/mqtt.txt", "w");
    file.printf("%s\n", server.arg("hostport").c_str());
    file.printf("%s\n", server.arg("userpass").c_str());
    file.printf("%s\n", server.arg("topicbase").c_str());
    file.printf("%s\n", server.arg("myhostname").c_str());
    file.printf("%s\n", server.arg("interval").c_str());
    file.close();
    loadMQTT(); // reread

    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "2;url=/");
    server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpStyle() {
    portal_timer = millis();
    String css;

    file = SPIFFS.open("/style.css", "r");
    css = file.readString();
    file.close();
    server.send(200, "text/css", css);
}
/* ------------------------------------------------------------------------------- */

void httpBoot() {
    portal_timer = millis();
    String html;
    file = SPIFFS.open("/ok.html", "r");
    html = file.readString();
    file.close();

    server.sendHeader("Refresh", "2;url=about:blank");
    server.send(200, "text/html; charset=UTF-8", html);
    delay(1000);
    ESP.restart();
}
/* ------------------------------------------------------------------------------- */
