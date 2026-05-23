// TODO: rename to user_settings.h 

// *******************************************************************************************************************************
// START userdefined data
// *******************************************************************************************************************************
#include <Arduino.h>

#define MY_NTP_SERVER "pool.ntp.org"
#define MY_TZ "CET-1CEST,M3.5.0/02,M10.5.0/03"

// Off-sets for time, and summertime. each hour is 3.600 seconds.
const long  gmtOffset_sec = 3600;    // Europa/Berlin

#define uS_TO_S_FACTOR 1000000ULL   //Conversion factor for micro seconds to seconds
#define TIME_TO_SLEEP_LONG  10800   // 180 min = 3h 
#define TIME_TO_SLEEP  3600         // 60 min = 1h

// Device name, to appear on WiFi and as MQTT/HA device name
const char* deviceName    = "EspSensorSolar2";

// ############## WiFi ###############
const char* ssid = "myWiFiSSID";
const char* password = "12345678!";
// #####################################


// MQTT Configuration
const char *mqttBrokerHost  = "192.168.1.2";
const char *mqttUser        = "mqtt_username"; // mqtt username, empty if not required
const char *mqttPass        = "password";      // mqtt password, empty if not required
int         mqttPort        = 1883;            // default: 1883

// SOIL CALIBRATION DATA ---------------------------------------------
bool doCalibration = false; // Connect to serial monitor for calibration, set to false if done. 

// Sensors
int air0 = 2418, 	air1 = 3000, 	air2 = 2412, 	air3 = 2421;
int water0 = 1055, 	water1 = 3001, 	water2 = 1233, 	water3 = 1215;

// *******************************************************************************************************************************
// END userdefined data
// *******************************************************************************************************************************
