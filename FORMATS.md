# ESP8266 DS1820 MQTT configuration file formats

Here is the specification of the configuration files. All files use only newline aka Unix line break. 
Windows line break CRLF will cause problems, so don't use notepad.exe or like that.

## mqtt.txt

row 1: ip:port or host:port. Must be separated by colon.

row 2: username:password for the MQTT broker. They must be separated by colon.

row 3: topic prefix. This is the base for the topic and sensor name is added automatically after this.

row 4: hostname what the unit will tell to DHCP server and MQTT broker

row 5: MQTT publish interval in minutes.

All rows must end in newline.

**Example mqtt.txt file:**

```
192.168.36.99:1883
publisher:password123
home/dallas
esp8266-thermometer-a1
5
```

## known_sensors.txt

One known sensor per row. First the hardware address in uppercase hex, then TAB, 
then name of the sensor and newline.

**Example known_sensors.txt file:**

```
2853B079A2000346	foo

```

## known_wifis.txt

One known WiFi network per row. First the SSID, then TAB, then password and newline.

**Example known_wifis.txt**

```
OH2MP	MyVerySecretPass123
OH2MP-5	AnotherVerySecretPass456
```
