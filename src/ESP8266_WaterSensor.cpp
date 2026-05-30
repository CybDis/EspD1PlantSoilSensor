#include <time.h>
#include <ESP8266WiFi.h>
#include <ArduinoHA.h>
#include "user_settings.h"
#include <Adafruit_ADS1X15.h>

#define uS_TO_S_FACTOR 1000000ULL // microseconds per second
#define MAX_SLEEP_SECS        3600 // ESP8266 hardware cap; do not raise above ~3600 s
#define NTP_MIN_VALID_EPOCH 1000000000UL // 2001-09-09; a real NTP sync produces a value above this
#define NTP_RETRIES              3 // full NTP attempts before giving up
#define NTP_POLLS_PER_ATTEMPT   10 // polls per attempt
#define NTP_WAIT_MS            500 // ms between polls

// We deliberately do NOT estimate time across deep sleep. The board has no real-time clock,
// and dead-reckoning the epoch from sleep durations proved unreliable. Instead, on every wake
// the device fetches the real UTC time via NTP first and decides everything from that.
// The only value kept in RTC user memory is the scheduled epoch of the next measurement, and
// it is always compared against the freshly fetched real time, never used as a clock itself.
#define RTC_MAGIC  0xA5C3F1ED  // increment when RtcData layout changes
#define RTC_OFFSET 0           // word offset in the user RTC area
#define DRD_ARMED  0xBEEFCAFEUL // double-reset-detector sentinel; see setup()

struct RtcData {
    uint32_t magic;
    uint32_t nextMeasure; // real UTC epoch of the next scheduled measurement
    uint32_t drd;         // DRD_ARMED while a wake cycle is in progress; see double-reset logic
};

// In-memory copy of the RTC user-memory block, persisted via rtcPersist().
RtcData g_rtc;
bool    g_rtcValid = false;

void rtcLoad() {
    ESP.rtcUserMemoryRead(RTC_OFFSET, (uint32_t *)&g_rtc, sizeof(g_rtc));
    g_rtcValid = (g_rtc.magic == RTC_MAGIC);
    if (!g_rtcValid) {
        g_rtc.nextMeasure = 0;
        g_rtc.drd = 0;
    }
}

void rtcPersist() {
    g_rtc.magic = RTC_MAGIC;
    ESP.rtcUserMemoryWrite(RTC_OFFSET, (uint32_t *)&g_rtc, sizeof(g_rtc));
}

// All checks below run on the real local time from NTP + MY_TZ, so they are
// daylight-saving correct.
bool isNightTime(const struct tm &local) {
    return local.tm_hour >= NIGHT_START_HOUR || local.tm_hour < NIGHT_END_HOUR;
}

uint32_t secondsUntilMorning(const struct tm &local) {
    int secsToday = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    int target    = NIGHT_END_HOUR * 3600;
    if (secsToday < target)
        return (uint32_t)(target - secsToday);
    return (uint32_t)(86400 - secsToday + target);
}

// Seconds until the next measurement, aligned to the top of the hour:
// intervalSecs minus the time already elapsed since the last full hour.
// Example: 13:11 with a 3h interval -> 10800 - 660 = 10140s -> next measure at 16:00.
uint32_t secondsUntilNextMeasure(const struct tm &local, uint32_t intervalSecs) {
    uint32_t secsFromLastHour = (uint32_t)(local.tm_min * 60 + local.tm_sec);
    return intervalSecs - secsFromLastHour;
}

// ADS1115 analog-to-digital converter
Adafruit_ADS1115 ads;

void deepSleepSeconds(uint32_t secondsToSleep)
{
	// Cap to the hardware limit. Longer waits (night, multi-hour intervals) are handled by
	// re-checking the real time on the next wake, so a capped sleep is always safe.
	uint32_t toSleep = secondsToSleep < MAX_SLEEP_SECS ? secondsToSleep : MAX_SLEEP_SECS;

	// Disarm the double-reset detector: a normal timer wake must not look like a manual
	// double reset. If a second reset arrives before we reach this point, the flag is still
	// armed on the next boot and a measurement is forced.
	g_rtc.drd = 0;
	rtcPersist();

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

String EpochToZuluTime(uint32_t epoch)
{
	time_t t = (time_t)epoch;
	struct tm tm;
	gmtime_r(&t, &tm);
	char buf[25];
	snprintf(buf, sizeof(buf), "20%02d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year - 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	return String(buf);
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

void SenqMqtt(float battVolt, float battPercent, float percent1, float percent2, float percent3, float percent4, uint32_t nextMeasure)
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
	HASensor sensorNextUpdate(concat("nextUpdate_", device.getUniqueId()));

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

	sensorNextUpdate.setName(concat(deviceName, " Next Update"));
	sensorNextUpdate.setIcon("mdi:calendar-clock");
	sensorNextUpdate.setDeviceClass("timestamp");
	sensorNextUpdate.setForceUpdate(true);
	sensorNextUpdate.setExpireAfter(43200);

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
		sensorNextUpdate.setValue(EpochToZuluTime(nextMeasure).c_str());

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

int updateSensor(uint32_t nextMeasure, bool adsOk)
{
	float percent0, percent1, percent2, percent3;

	if (adsOk) {
		float min0 = 55555, min1 = 55555, min2 = 55555, min3 = 55555;
		float max0 = -1, max1 = -1, max2 = -1, max3 = -1;
		float adc0, adc1, adc2, adc3;

		int i = doCalibration ? 1000 : 1;
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
	} else {
		percent0 = percent1 = percent2 = percent3 = -1;
	}
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

	SenqMqtt(volt, battPercent, percent0, percent1, percent2, percent3, nextMeasure);

	return battPercent;
}

// Fetch the real UTC time via NTP. Returns true once a plausible epoch is received.
bool syncTime()
{
	Serial.printf("Configuring NTP: server=%s tz=%s\n", MY_NTP_SERVER, MY_TZ);
	configTime(MY_TZ, MY_NTP_SERVER);

	for (int attempt = 1; attempt <= NTP_RETRIES; attempt++) {
		if (attempt > 1) {
			Serial.printf("NTP attempt %d/%d - re-requesting from %s\n", attempt, NTP_RETRIES, MY_NTP_SERVER);
			configTime(MY_TZ, MY_NTP_SERVER);
		}
		time_t now = 0;
		for (int i = 0; i < NTP_POLLS_PER_ATTEMPT && now < NTP_MIN_VALID_EPOCH; i++) {
			delay(NTP_WAIT_MS);
			time(&now);
			Serial.printf("  NTP poll %d: epoch=%lu\n", i + 1, (unsigned long)now);
		}
		if (now >= NTP_MIN_VALID_EPOCH) {
			Serial.printf("NTP synced on attempt %d: epoch=%lu (%s)\n",
			              attempt, (unsigned long)now, GetZuluTime().c_str());
			return true;
		}
		Serial.printf("NTP attempt %d failed (epoch=%lu)\n", attempt, (unsigned long)now);
	}
	Serial.println("NTP sync failed.");
	return false;
}

void setup()
{
	Serial.begin(115200);
	Serial.println();
	Serial.printf("Starting... (reset reason: %s)\n", ESP.getResetReason().c_str());

	rtcLoad();

	// Double-reset detector: pressing RESET twice within one wake cycle forces an immediate
	// measurement, even during night time or before the interval is due. The flag is armed
	// here and disarmed in deepSleepSeconds(). If a second reset arrives before the device
	// sleeps, the flag is still armed on this boot and we force a measurement.
	bool forceMeasure = g_rtcValid && g_rtc.drd == DRD_ARMED;
	g_rtc.drd = DRD_ARMED;
	rtcPersist();
	if (forceMeasure)
		Serial.println("Double reset detected - forcing measurement.");

	// Calibration mode runs without WiFi/NTP: just power the sensors and stream readings.
	if (doCalibration) {
		Serial.print("Starting ADS via D5... ");
		pinMode(D5, OUTPUT);
		digitalWrite(D5, HIGH);
		Serial.println("Waiting (10s) for ADS to come up... ");
		delay(10000); // required for ADS to come up
		ads.setGain(GAIN_ONE); // set to +- 4096 mV
		bool adsOk = ads.begin();
		if (adsOk)
			Serial.println("Initialized.");
		else
			Serial.println("Error initializing ADS!");
		updateSensor(0, adsOk);
		deepSleepSeconds(TIME_TO_SLEEP);
		return;
	}

	// 1. Bring up WiFi as early as possible. Without WiFi we cannot reach the MQTT broker,
	//    so retry on the next wake.
	if (!connectToNetwork()) {
		Serial.println("WiFi failed, retrying once...");
		WiFi.disconnect(true);
		delay(1000);
		if (!connectToNetwork()) {
			Serial.println("Error connecting to WiFi - sleeping.");
			deepSleepSeconds(TIME_TO_SLEEP);
			return;
		}
	}

	// 2. Fetch the real UTC time. If NTP fails (internet disturbed but local broker still
	//    reachable), we deliberately skip the night/interval gating below and measure anyway,
	//    so the device keeps sending instead of going silent.
	bool haveTime = syncTime();

	time_t now = time(nullptr);
	struct tm local;
	// A forced measurement (double reset) skips both the night and the interval gate.
	if (haveTime && !forceMeasure) {
		localtime_r(&now, &local);
		Serial.printf("Real local time: %02d:%02d:%02d\n", local.tm_hour, local.tm_min, local.tm_sec);

		// 3a. Night time: go back to sleep as quickly as possible. The sleep is capped to the
		//     hardware limit and re-checked against fresh NTP time on the next wake.
		if (isNightTime(local)) {
			uint32_t toSleep = secondsUntilMorning(local);
			Serial.printf("Night time (%02dh local) - sleeping up to %us\n", local.tm_hour, toSleep);
			deepSleepSeconds(toSleep);
			return;
		}

		// 3b. Not yet due for the next measurement: sleep until it is (capped, re-checked next wake).
		if ((uint32_t)now < g_rtc.nextMeasure) {
			uint32_t toSleep = g_rtc.nextMeasure - (uint32_t)now;
			Serial.printf("Next measurement in %us - sleeping.\n", toSleep);
			deepSleepSeconds(toSleep);
			return;
		}
	} else if (!haveTime) {
		Serial.println("No time available - measuring and sending anyway.");
	}

	// 4. Time to measure: power the sensors, read them and publish via MQTT.
	pinMode(D5, OUTPUT);
	digitalWrite(D5, HIGH);
	delay(2000); // required for ADS to come up

	ads.setGain(GAIN_ONE); // set to +- 4096 mV
	Serial.println("AnalogDigitalSensor: ADC Range set to: +/- 4096 mV (ADS1115: 1 bit = 0.125 mV)");
	bool adsOk = ads.begin();
	if (!adsOk)
		Serial.println("Error initializing ADS!");
	else
		Serial.println("Initialized.");
	Serial.println();

	// Estimate next measure before reading (uses TIME_TO_SLEEP; accurate when battery > 80%)
	uint32_t nextMeasureEst = 0;
	if (haveTime) {
		time_t t = time(nullptr);
		struct tm loc;
		localtime_r(&t, &loc);
		uint32_t secs = secondsUntilNextMeasure(loc, TIME_TO_SLEEP);
		nextMeasureEst = (uint32_t)t + secs;
	}
	int battPercent = updateSensor(nextMeasureEst, adsOk);

	digitalWrite(D5, LOW);

	// Schedule the next measurement. The interval depends on the battery level we just read.
	uint32_t intervalSecs = (battPercent > 80) ? TIME_TO_SLEEP : TIME_TO_SLEEP_LONG;
	uint32_t toSleep;
	if (haveTime) {
		// Align to the top of the hour. Re-read the real time so the schedule is accurate.
		now = time(nullptr);
		localtime_r(&now, &local);
		toSleep = secondsUntilNextMeasure(local, intervalSecs);
		g_rtc.nextMeasure = (uint32_t)now + toSleep;
	} else {
		// No real time: sleep a fixed interval and retry NTP next wake. Store 0 so the
		// interval gate never blocks a future wake based on a bogus timestamp.
		toSleep = intervalSecs;
		g_rtc.nextMeasure = 0;
	}
	rtcPersist();
	deepSleepSeconds(toSleep);
}

void loop()
{
}
