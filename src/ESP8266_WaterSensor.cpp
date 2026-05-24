#include <time.h>
#include <ESP8266WiFi.h>
#include <ArduinoHA.h>
#include "user_settings.h"
#include <Adafruit_ADS1X15.h>

#define uS_TO_S_FACTOR 1000000ULL // microseconds per second
#define MAX_SLEEP_SECS        3600 // ESP8266 hardware cap; do not raise above ~3600 s
#define NTP_RETRIES              3 // full NTP attempts before giving up
#define NTP_POLLS_PER_ATTEMPT   10 // polls per attempt
#define NTP_WAIT_MS            500 // ms between polls

// RTC user memory survives deep sleep and is used to estimate the current time
// without a WiFi/NTP round-trip on every wake-up.
#define RTC_MAGIC  0xA5C3F1E9  // increment when RtcData layout changes
#define RTC_OFFSET 0  // word offset in the user RTC area

struct RtcData {
    uint32_t magic;
    uint32_t utcEpoch;    // estimated UTC time at the next scheduled wake-up
    uint32_t nextMeasure; // UTC time of the next scheduled measurement
};

bool loadRtcData(RtcData &data) {
    ESP.rtcUserMemoryRead(RTC_OFFSET, (uint32_t *)&data, sizeof(data));
    return data.magic == RTC_MAGIC;
}

void saveRtcData(uint32_t utcEpoch, uint32_t nextMeasure) {
    RtcData data = { RTC_MAGIC, utcEpoch, nextMeasure };
    ESP.rtcUserMemoryWrite(RTC_OFFSET, (uint32_t *)&data, sizeof(data));
}

// UTC+1 (CET) as a conservative Berlin estimate; at most 1 hour early during summer time
bool isNightTime(uint32_t utcEpoch) {
    int hour = (int)(((utcEpoch + 3600UL) / 3600UL) % 24UL);
    return hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR;
}

uint32_t secondsUntilMorning(uint32_t utcEpoch) {
    uint32_t local    = utcEpoch + 3600UL;
    int secsToday     = (int)((local / 3600UL % 24UL) * 3600UL
                            + (local / 60UL  % 60UL) * 60UL
                            +  local % 60UL);
    int target        = NIGHT_END_HOUR * 3600;
    if (secsToday < target)
        return (uint32_t)(target - secsToday);
    return (uint32_t)(86400 - secsToday + target);
}

// Returns seconds until the next measurement: intervalSecs minus time elapsed since the last full hour.
// Example: 13:11 with 3h interval -> 10800 - 660 = 10140s -> wakes at 16:00.
uint32_t secondsUntilNextMeasure(uint32_t intervalSecs) {
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    uint32_t secsFromLastHour = (uint32_t)(local.tm_min * 60 + local.tm_sec);
    return intervalSecs - secsFromLastHour;
}

// ADS1115 analog-to-digital converter
Adafruit_ADS1115 ads;

void Sleep(uint32_t secondsToSleep)
{
	uint32_t toSleep = secondsToSleep < MAX_SLEEP_SECS ? secondsToSleep : MAX_SLEEP_SECS;

	WiFi.disconnect(true);
	WiFi.mode(WIFI_OFF);

	digitalWrite(D5, LOW);
	pinMode(D5, INPUT);

	Serial.printf("Going to sleep for %.1f minutes.\n", toSleep / 60.0);
	delay(1000);

	ESP.deepSleep((uint64_t)toSleep * uS_TO_S_FACTOR);
}

const bool connectToNetwork()
{
	Serial.print("Connecting to WiFi ");
	Serial.print(ssid);

	WiFi.mode(WIFI_STA);
	WiFi.hostname(deviceName);
	WiFi.setAutoConnect(true);
	WiFi.begin(ssid, password);

	int i = 0;
	int j = 0;
	while (WiFi.status() != WL_CONNECTED)
	{
		delay(500);
		if (i > 80)
		{
			Serial.println();
			i = 0;
			if (j++ > 2)
				return false;
		}
		i++;
		Serial.print(".");
	}
	Serial.println();
	Serial.print("WiFi Connected: ");
	Serial.println(WiFi.localIP());

	return WiFi.status() == WL_CONNECTED;
}

const String GetZuluTime()
{
	time_t now;			 // this is the epoch
	tm tm;				 // the structure tm holds time information in a more convient way
	time(&now);			 // read the current time
	gmtime_r(&now, &tm); // update the structure tm with the current time (in GMT)
	char buf[25];
	snprintf(buf, sizeof(buf), "20%02d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year - 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	return String(buf);
}

char* concat(const char *str1, const char *str2)
{
	// Alloziere Speicherplatz für den resultierenden char*
	size_t len1 = strlen(str1);
	size_t len2 = strlen(str2);
	char *result = new char[len1 + len2 + 1]; // +1 für das Nullzeichen am Ende

	// Kopiere den Inhalt von str1 in result
	strcpy(result, str1);

	// Hänge den Inhalt von str2 an result an
	strcat(result, str2);

	return result;
}

String floatToString(float value, int decimalPlaces)
{
	// Berechne den Multiplikationsfaktor für die Rundung
	float multiplier = pow(10.0, decimalPlaces);

	// Runde den Wert entsprechend der Anzahl der Dezimalstellen
	float roundedValue = roundf(value * multiplier) / multiplier;

	// Überprüfe, ob der gerundete Wert Dezimalstellen ungleich null hat
	if (fabs(roundedValue - (int)roundedValue) >= (1.0 / multiplier))
	{
		// Konvertiere den gerundeten Wert mit Dezimalstellen in einen String
		char buffer[20];
		sprintf(buffer, "%.*f", decimalPlaces, roundedValue);
		return String(buffer);
	}
	else
	{
		// Konvertiere den Wert ohne Dezimalstellen in einen String
		return String((int)value);
	}
}

void SenqMqtt(float battVolt, float battPercent, float percent1, float percent2, float percent3, float percent4)
{
	byte mac[WL_MAC_ADDR_LENGTH];
	WiFi.macAddress(mac);

	WiFiClient wifiClient;
	HADevice device;
	device.setUniqueId(mac, sizeof(mac)); // must be set, also for each sensor
	device.setName(deviceName);
	device.setManufacturer("https://github.com/cybdis");
	device.setModel("Plantsensor 4.3 (Rev 2)");
	device.setSoftwareVersion("4.3.0");
	
	HAMqtt mqtt(wifiClient, device, 10); // 10 -> see https://github.com/dawidchyrzynski/arduino-home-assistant/issues/172#issuecomment-1596233347

	HASensor sensorBatteryVoltage(concat("batteryVoltage_", device.getUniqueId()));
	HASensor sensorBatteryPercent(concat("batteryPercent_", device.getUniqueId()));
	HASensor sensorTimestamp(concat("timestamp_", device.getUniqueId()));
	HASensor sensorSoil1(concat("soil1_", device.getUniqueId()));
	HASensor sensorSoil2(concat("soil2_", device.getUniqueId()));
	HASensor sensorSoil3(concat("soil3_", device.getUniqueId()));
	HASensor sensorSoil4(concat("soil4_", device.getUniqueId()));

	// For Icons, see https://pictogrammers.github.io/@mdi/font/7.1.96/

	sensorSoil1.setUnitOfMeasurement("%");
	sensorSoil1.setIcon("mdi:water-percent");
	sensorSoil1.setName(concat(deviceName, " Soil 1"));
	sensorSoil1.setDeviceClass("moisture");
	sensorSoil1.setForceUpdate(true);
	sensorSoil1.setExpireAfter(43200);
	sensorSoil1.setStateClass("measurement");

	sensorSoil2.setUnitOfMeasurement("%");
	sensorSoil2.setIcon("mdi:water-percent");
	sensorSoil2.setName(concat(deviceName, " Soil 2"));
	sensorSoil2.setDeviceClass("moisture");
	sensorSoil2.setForceUpdate(true);
	sensorSoil2.setExpireAfter(43200);
	sensorSoil2.setStateClass("measurement");

	sensorSoil3.setUnitOfMeasurement("%");
	sensorSoil3.setIcon("mdi:water-percent");
	sensorSoil3.setName(concat(deviceName, " Soil 3"));
	sensorSoil3.setDeviceClass("moisture");
	sensorSoil3.setForceUpdate(true);
	sensorSoil3.setExpireAfter(43200);
	sensorSoil3.setStateClass("measurement");

	sensorSoil4.setUnitOfMeasurement("%");
	sensorSoil4.setIcon("mdi:water-percent");
	sensorSoil4.setName(concat(deviceName, " Soil 4"));
	sensorSoil4.setDeviceClass("moisture");
	sensorSoil4.setForceUpdate(true);
	sensorSoil4.setExpireAfter(43200);
	sensorSoil4.setStateClass("measurement");

	sensorBatteryVoltage.setName(concat(deviceName, " Batt Voltage"));
	sensorBatteryVoltage.setIcon("mdi:battery");
	sensorBatteryVoltage.setUnitOfMeasurement("V");
	sensorBatteryVoltage.setDeviceClass("voltage");
	sensorBatteryVoltage.setForceUpdate(true);
	sensorBatteryVoltage.setExpireAfter(43200);
	sensorBatteryVoltage.setStateClass("measurement");

	sensorBatteryPercent.setName(concat(deviceName, " Batt Percent"));
	sensorBatteryPercent.setIcon("mdi:battery");
	sensorBatteryPercent.setUnitOfMeasurement("%");
	sensorBatteryPercent.setDeviceClass("battery");
	sensorBatteryPercent.setForceUpdate(true);
	sensorBatteryPercent.setExpireAfter(43200);
	sensorBatteryPercent.setStateClass("measurement");

	sensorTimestamp.setName(concat(deviceName, " Updated"));
	sensorTimestamp.setIcon("mdi:update");
	sensorTimestamp.setDeviceClass("timestamp");
	sensorTimestamp.setExpireAfter(43200);

	String dateTime = GetZuluTime();
	Serial.print("Datetime: ");
	Serial.println(dateTime);

	Serial.print("Conneting to MQTT broker: ");
	Serial.print(mqttBrokerHost);
	Serial.print("' as '");
	Serial.print(mqttUser);
	Serial.println("' ...");

	bool success = mqtt.begin(mqttBrokerHost, mqttPort, mqttUser, mqttPass);
	if (success)
	{
		mqtt.loop(); // connects to server
		Serial.print("Connected to MQTT Broker. Sending MQTT signal... ");

		sensorSoil1.setValue(floatToString(percent1, 1).c_str());
		sensorSoil2.setValue(floatToString(percent2, 1).c_str());
		sensorSoil3.setValue(floatToString(percent3, 1).c_str());
		sensorSoil4.setValue(floatToString(percent4, 1).c_str());
		sensorBatteryVoltage.setValue(floatToString(battVolt, 2).c_str());
		sensorBatteryPercent.setValue(floatToString(battPercent, 1).c_str());
		sensorTimestamp.setValue(dateTime.c_str());

		for (int i = 0;i<10;i++) {
			mqtt.loop(); // processes messages multiple times, dunno why required - strange impl
			delay(50);
		}
		Serial.println("MQTT values sent.");
	}
	else
	{
		Serial.println("ERROR: Could not connected to MQTT Broker!");
	}
}

float GetBatteryPercent(float currentVoltage, float minimumVoltage, float maximumVoltage)
{
	// Überprüfung, ob die aktuelle Spannung unterhalb des Mindestwerts liegt
	if (currentVoltage <= minimumVoltage)
	{
		return 0.0; // Wenn die aktuelle Spannung kleiner oder gleich dem Mindestwert ist, wird der Ladezustand als 0% angenommen
	}

	// Überprüfung, ob die aktuelle Spannung nicht größer als die maximale Spannung ist
	if (currentVoltage > maximumVoltage)
	{
		return 100.0; // Wenn die aktuelle Spannung größer als die maximale Spannung ist, wird der Ladezustand als 100% angenommen
	}

	// Berechnung des prozentualen Ladezustands relativ zum Mindestwert
	float relativeStateOfCharge = ((currentVoltage - minimumVoltage) / (maximumVoltage - minimumVoltage)) * 100.0;

	return relativeStateOfCharge;
}

float GetPercent(float value, int air, int water)
{
	float percent = map(value, air, water, 0, 100); // integer map intentional: sensor resolution does not warrant float precision
	if (!doCalibration && percent > 100)
		percent = 100;
	if (!doCalibration && percent < 0)
		percent = 0;

	return percent;
}

void WriteCalibrationData(int port, float value, float min, float max, int air, int water)
{
	float percent = GetPercent(value, air, water);

	Serial.print("Analog");
	Serial.print(port);
	Serial.print(" read: ");
	Serial.print(value);
	Serial.print("    [Min (cal. Water): ");
	Serial.print(min);
	Serial.print(" | Max (cal. Air): ");
	Serial.print(max);
	Serial.print("]    [current Air: ");
	Serial.print(air);
	Serial.print(" | current Water: ");
	Serial.print(water);
	Serial.print("]   - Calculated Percent: ");
	Serial.print(percent);

	Serial.println();
}

int updateSensor()
{
	float min0 = 55555, min1 = 55555, min2 = 55555, min3 = 55555;
	float max0 = -1, max1 = -1, max2 = -1, max3 = -1;
	float percent0, percent1, percent2, percent3;

	float adc0, adc1, adc2, adc3;

	int i = doCalibration ? 1000 : 1; // change to do calibration
	while (i-- > 0)
	{
		adc0 = ads.readADC_SingleEnded(0) * 0.125;
		if (adc0 > max0) max0 = adc0;
		if (adc0 < min0) min0 = adc0;

		adc1 = ads.readADC_SingleEnded(1) * 0.125;
		if (adc1 > max1) max1 = adc1;
		if (adc1 < min1) min1 = adc1;

		adc2 = ads.readADC_SingleEnded(2) * 0.125;
		if (adc2 > max2) max2 = adc2;
		if (adc2 < min2) min2 = adc2;

		adc3 = ads.readADC_SingleEnded(3) * 0.125;
		if (adc3 > max3) max3 = adc3;
		if (adc3 < min3) min3 = adc3;

		WriteCalibrationData(0, adc0, min0, max0, air0, water0);
		WriteCalibrationData(1, adc1, min1, max1, air1, water1);
		WriteCalibrationData(2, adc2, min2, max2, air2, water2);
		WriteCalibrationData(3, adc3, min3, max3, air3, water3);

		if (i > 0)
		{
			delay(500);
			Serial.println();
		}
	}
	if (doCalibration)
		return 0;

	percent0 = GetPercent(min0, air0, water0);
	percent1 = GetPercent(min1, air1, water1);
	percent2 = GetPercent(min2, air2, water2);
	percent3 = GetPercent(min3, air3, water3);
	Serial.println();
	
	Serial.println("Reading Battery...");
	pinMode(A0, INPUT);
	unsigned int raw = analogRead(A0);
	Serial.print("A0 read: ");
	Serial.println(raw);
	float volt = raw / 1023.0;
	Serial.print("Volt read: ");
	Serial.println(volt);
	volt = volt * 4.2;
	Serial.print("Volt calculated: ");
	Serial.println(volt);
	int battPercent = GetBatteryPercent(volt, 2.5, 4.2);
	Serial.print("Battery percent: ");
	Serial.println(battPercent);
	Serial.println();

	SenqMqtt(volt, battPercent, percent0, percent1, percent2, percent3);

	return battPercent;
}

void setup()
{
	Serial.begin(115200);
	Serial.println();
	Serial.println("Starting...");

	// RTC checks are only valid after a deep sleep wake-up; on manual reset the data is stale.
	RtcData rtc;
	bool validWake = !doCalibration
	                 && ESP.getResetReason() == "Deep-Sleep Wake"
	                 && loadRtcData(rtc);

	if (validWake && isNightTime(rtc.utcEpoch)) {
		uint32_t toSleep = secondsUntilMorning(rtc.utcEpoch);
		if (toSleep > MAX_SLEEP_SECS) toSleep = MAX_SLEEP_SECS;
		Serial.printf("Night time (%02dh local) - sleeping %us\n",
		              (int)(((rtc.utcEpoch + 3600UL) / 3600UL) % 24UL), toSleep);
		saveRtcData(rtc.utcEpoch + toSleep, rtc.nextMeasure);
		delay(100);
		ESP.deepSleep((uint64_t)toSleep * uS_TO_S_FACTOR);
		return;
	}

	if (validWake && rtc.utcEpoch < rtc.nextMeasure) {
		uint32_t toSleep = rtc.nextMeasure - rtc.utcEpoch;
		if (toSleep > MAX_SLEEP_SECS) toSleep = MAX_SLEEP_SECS;
		Serial.printf("Intermediate wake - next measure in %us, sleeping %us\n",
		              rtc.nextMeasure - rtc.utcEpoch, toSleep);
		saveRtcData(rtc.utcEpoch + toSleep, rtc.nextMeasure);
		delay(100);
		ESP.deepSleep((uint64_t)toSleep * uS_TO_S_FACTOR);
		return;
	}

	// ADS and SOIL sensors powered via D5
	pinMode(D5, OUTPUT);
	digitalWrite(D5, HIGH);

	if (!doCalibration && !connectToNetwork())
	{
		Serial.print("Error connecting to wifi!");
		Sleep(TIME_TO_SLEEP);
		return;
	}
	else if (doCalibration)
		delay(2000); // required for ADS to come up

	ads.setGain(GAIN_ONE); // set to +- 4096 mV
	Serial.println("AnalogDigitalSensor: ADC Range set to: +/- 4096 mV (ADS1115: 1 bit = 0.125 mV)");
	if (!ads.begin())
	{
		Serial.print("Error initializing ADS!");
		Sleep(TIME_TO_SLEEP);
		return;
	}

	Serial.printf("Configuring NTP: server=%s tz=%s\n", MY_NTP_SERVER, MY_TZ);
	configTime(MY_TZ, MY_NTP_SERVER);

	// Wait for NTP sync; retry up to NTP_RETRIES times with NTP_WAIT_MS between polls.
	// Each retry calls configTime again to re-trigger the SNTP request.
	bool ntpOk = false;
	for (int attempt = 1; attempt <= NTP_RETRIES && !ntpOk; attempt++) {
		if (attempt > 1) {
			Serial.printf("NTP attempt %d/%d - re-requesting from %s\n", attempt, NTP_RETRIES, MY_NTP_SERVER);
			configTime(MY_TZ, MY_NTP_SERVER);
		}
		time_t now = 0;
		for (int i = 0; i < NTP_POLLS_PER_ATTEMPT && now < 1000000000; i++) {
			delay(NTP_WAIT_MS);
			time(&now);
			Serial.printf("  NTP poll %d: epoch=%lu\n", i + 1, (unsigned long)now);
		}
		time_t final = time(nullptr);
		if (final >= 1000000000) {
			ntpOk = true;
			Serial.printf("NTP synced on attempt %d: epoch=%lu (%s)\n",
			              attempt, (unsigned long)final, GetZuluTime().c_str());
		} else {
			Serial.printf("NTP attempt %d failed (epoch=%lu)\n", attempt, (unsigned long)final);
		}
	}
	if (!ntpOk) Serial.println("NTP sync failed - using fixed interval sleep.");

	Serial.println("Initialized.");
	Serial.println();

	int battPercent = updateSensor();

	digitalWrite(D5, LOW);

	uint32_t intervalSecs = (battPercent > 80) ? TIME_TO_SLEEP : TIME_TO_SLEEP_LONG;
	uint32_t sleepSecs, cappedSleep, nextMeasureEpoch;
	if (ntpOk) {
		sleepSecs        = secondsUntilNextMeasure(intervalSecs);
		cappedSleep      = sleepSecs < MAX_SLEEP_SECS ? sleepSecs : MAX_SLEEP_SECS;
		nextMeasureEpoch = (uint32_t)time(nullptr) + sleepSecs;
	} else {
		sleepSecs        = intervalSecs;
		cappedSleep      = sleepSecs < MAX_SLEEP_SECS ? sleepSecs : MAX_SLEEP_SECS;
		nextMeasureEpoch = 0; // invalid epoch; force measurement on next wake
	}
	saveRtcData((uint32_t)time(nullptr) + cappedSleep, nextMeasureEpoch);
	Sleep(sleepSecs);
}

void loop()
{
}
