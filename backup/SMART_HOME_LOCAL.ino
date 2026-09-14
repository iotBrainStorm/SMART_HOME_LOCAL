#include "esp_sntp.h"
#include "time.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <new>

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
#include <FirebaseClient.h> // from mobizt — must precede ESPAsyncWebServer to avoid HTTP_GET ambiguity

#include <Adafruit_AHT10.h>
#include <ArduinoJson.h>
#include <Dusk2Dawn.h>
#include <EEPROM.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <Wire.h>

#include "time.h"

//  PIN DEFINITIONS
#define INPUT_SWITCH_1 34
#define INPUT_SWITCH_2 35
#define INPUT_SWITCH_3 36
#define INPUT_SWITCH_4 39

#define OUTPUT_SWITCH_1 18
#define OUTPUT_SWITCH_2 19
#define OUTPUT_SWITCH_3 23
#define OUTPUT_SWITCH_4 5

#define LED_PIN 16
#define BUZZER_PIN 17
#define BOOT_BUTTON 0

#define NUM_SWITCHES 4
#define AUTOMATION_SOURCE_COUNT 6

enum AutomationSourceType : uint8_t {
  AUTOMATION_TIMER = 0,
  AUTOMATION_SCHEDULE = 1,
  AUTOMATION_FUTURE_SCHEDULE = 2,
  AUTOMATION_TEMPERATURE = 3,
  AUTOMATION_HUMIDITY = 4,
  AUTOMATION_SUN = 5
};

const int outPin[NUM_SWITCHES] = {OUTPUT_SWITCH_1, OUTPUT_SWITCH_2,
                                  OUTPUT_SWITCH_3, OUTPUT_SWITCH_4};
const int inPin[NUM_SWITCHES] = {INPUT_SWITCH_1, INPUT_SWITCH_2, INPUT_SWITCH_3,
                                 INPUT_SWITCH_4};
const char *DEFAULT_NTP_SERVER = "pool.ntp.org";
const char *DEFAULT_TIME_ZONE = "+00:00";
const float DEFAULT_LATITUDE = 51.5074f;
const float DEFAULT_LONGITUDE = -0.1278f;
const char *DEFAULT_SWITCH_NAMES[NUM_SWITCHES] = {"DEVICE-1", "DEVICE-2",
                                                  "DEVICE-3", "DEVICE-4"};
const char *DEFAULT_SWITCH_ICONS[NUM_SWITCHES] = {"home", "home", "home",
                                                  "home"};

//  GLOBAL OBJECTS
AsyncWebServer server(80);
Preferences prefs;
Adafruit_AHT10 aht;

//  STATE VARIABLES
bool swState[NUM_SWITCHES] = {false, false, false, false};
String swName[NUM_SWITCHES] = {"DEVICE-1", "DEVICE-2", "DEVICE-3", "DEVICE-4"};
String swIcon[NUM_SWITCHES] = {"home", "home", "home", "home"};
enum RelayModeType : uint8_t {
  RELAY_MODE_OFF = 0,
  RELAY_MODE_ON = 1,
  RELAY_MODE_REMEMBER = 2
};
RelayModeType relayMode[NUM_SWITCHES] = {RELAY_MODE_OFF, RELAY_MODE_OFF, RELAY_MODE_OFF, RELAY_MODE_OFF};

bool pendingWebServerRestart = false;
bool nvsRelayDirty[NUM_SWITCHES] = {false, false, false, false};

// Timers - volatile (RAM only, lost on power cut)
struct SwTimer {
  bool active = false;
  bool paused = false;
  unsigned long endMs = 0;
  unsigned long remainingMs = 0;
  bool targetState = false;
};
SwTimer swTimers[NUM_SWITCHES];

// WiFi
bool dhcpOn = true;
String sIp, sMask = "255.255.255.0", sGw, sDns = "8.8.8.8";
bool portalFlag = false;

// Firebase Database
bool fbOn = false;
String fbUrl, fbToken;

String firebaseAuthUid = "";
String firebaseStreamPath = "";
bool firebaseRuntimeInitialized = false;
bool firebaseStreamRunning = false;
bool firebaseRuntimeDirty = true;
unsigned long firebaseLastInitAttemptMs = 0;
unsigned long firebaseLastStreamAttemptMs = 0;
unsigned long firebaseLastStreamEventMs = 0;
unsigned long firebaseLastWriteMs = 0;
const unsigned long FIREBASE_INIT_RETRY_MS = 5000;
const unsigned long FIREBASE_STREAM_RETRY_MS = 5000;
const unsigned long FIREBASE_STREAM_STALE_MS = 45000;
const unsigned long FIREBASE_WRITE_INTERVAL_MS = 200;
const unsigned long FIREBASE_WRITE_FAIL_RETRY_MS = 1000;
const unsigned long FIREBASE_WRITE_LONG_OP_MS = 150;
const unsigned long FIREBASE_INITIAL_SYNC_WAIT_MS = 3000;
const uint8_t FIREBASE_WRITE_QUEUE_LENGTH = 16;
const char *FIREBASE_AUTH_TASK_UID = "firebaseAuthTask";
const char *FIREBASE_STREAM_TASK_UID = "firebaseStreamTask";

WiFiClientSecure fbSslClient, fbStreamSslClient;
using FirebaseAsyncClient = AsyncClientClass;
FirebaseAsyncClient fbClient(fbSslClient), fbStreamClient(fbStreamSslClient);
FirebaseApp fbApp;
RealtimeDatabase fbDatabase;
UserAuth *fbUserAuth = nullptr;

struct FirebaseWriteTask {
  uint8_t index;
  bool state;
};

QueueHandle_t firebaseWriteQueue = nullptr;
FirebaseWriteTask firebaseDeferredWriteTask = {0, false};
bool firebaseHasDeferredWriteTask = false;
unsigned long firebaseDeferredWriteAtMs = 0;
bool firebaseAwaitingInitialStreamData = false;
unsigned long firebaseStreamStartedAtMs = 0;
String firebaseLastSyncedDeviceName = "";
bool firebaseDeviceNameSyncDirty = true;
String cachedDeviceId = "";
TaskHandle_t firebaseTaskHandle = nullptr;
SemaphoreHandle_t swStateMutex = nullptr;

const char *PRIMARY_ADMIN_ID = "esp";
const char *PRIMARY_ADMIN_PASS = "456456";
String configuredPrimaryAdminId = String(PRIMARY_ADMIN_ID);
String configuredPrimaryAdminPass = String(PRIMARY_ADMIN_PASS);
const char *DEFAULT_FIREBASE_RULES_JSON = R"RULES({
  "rules": {
    "espHome": {
      "$uid": {
        "$deviceId": {
          ".read": "auth != null && auth.uid == $uid",
          ".write": "auth != null && auth.uid == $uid"
        }
      }
    }
  }
})RULES";

// Admin / Time
String ntpSrv = DEFAULT_NTP_SERVER;
String tzStr = DEFAULT_TIME_ZONE;
long gmtOff = 0;
float geoLat = DEFAULT_LATITUDE, geoLon = DEFAULT_LONGITUDE;
String deviceName = "";
bool timeSynced = false;
bool ahtOk = false;
bool locOk = true;

uint8_t automationPriorityOrder[AUTOMATION_SOURCE_COUNT] = {
    AUTOMATION_TIMER, AUTOMATION_SCHEDULE, AUTOMATION_FUTURE_SCHEDULE,
    AUTOMATION_TEMPERATURE, AUTOMATION_HUMIDITY, AUTOMATION_SUN};

bool pendingAutomationAction[NUM_SWITCHES] = {false, false, false, false};
bool pendingAutomationState[NUM_SWITCHES] = {false, false, false, false};
uint8_t pendingAutomationSource[NUM_SWITCHES] = {
    AUTOMATION_TIMER, AUTOMATION_TIMER, AUTOMATION_TIMER, AUTOMATION_TIMER};

// Sunrise / Sunset
int srMin = 0, ssMin = 0, lastCalcDay = -1;

// Input debounce
bool lastInState[NUM_SWITCHES];
unsigned long lastDbMs[NUM_SWITCHES] = {0};
const unsigned long DB_DELAY = 50;

// Loop intervals
unsigned long lastSchedCheck = 0;
unsigned long lastSensorCheck = 0;
unsigned long lastHeapLog = 0;
int lastCheckedMinute = -1;

// Restart flag
bool restartFlag = false;
unsigned long restartAt = 0;
bool restartScheduleEnabled = false;
int restartScheduleMinute = -1;
uint8_t restartScheduleDayMask = 0;
long lastRestartScheduleStamp = -1;
String restartScheduleLastRunKey = "";
bool factoryResetWifiClearPending = false;
unsigned long factoryResetWifiClearAt = 0;
bool forgetWifiFlag = false;
unsigned long forgetWifiAt = 0;
bool coreRoutesOnly = false;

const uint32_t MIN_FREE_HEAP_FOR_EXTENDED_ROUTES = 38000;
const uint32_t MIN_FREE_HEAP_FOR_FIREBASE_WRITE = 45000;
const uint32_t MIN_FREE_HEAP_FOR_FIREBASE_LOOP = 38000;
const uint32_t CRITICAL_LOW_HEAP_RESTART_BYTES = 32000;
const int WIFI_CONNECT_MAX_ATTEMPTS = 5;
const unsigned long WIFI_CONNECT_RETRY_MS = 2000;
const unsigned long WIFI_PORTAL_RECOVERY_RESTART_MS = 200;
const unsigned long HEAP_LOG_INTERVAL_MS = 30000;
const unsigned long BOOT_HOLD_RESET_MS = 5000;
const unsigned long BOOT_HOLD_READY_WINDOW_MS = 15000;
const unsigned long RESET_CHECKPOINT_GAP_MS = 700;
const unsigned long RESET_ALL_DONE_GAP_MS = 2000;
const unsigned long RESET_RESTART_BUFFER_MS = 300;
const unsigned long FACTORY_RESET_WIFI_CLEAR_DELAY_MS = 8300;
const unsigned long FACTORY_RESET_RESTART_DELAY_MS = 9300;
const TickType_t FIREBASE_TASK_DELAY_TICKS = pdMS_TO_TICKS(50);

unsigned long notifyStorageOffAt = 0;
unsigned long bootButtonPressedAt = 0;
unsigned long bootHoldSatisfiedAt = 0;
bool bootHoldSatisfied = false;

void setRelayState(int index, bool state, const String &source);
void setSwitch(int i, bool st);
void calcSunriseSunset();
void handleFirebaseRuntime();
void firebaseAsyncCallback(AsyncResult &aResult);
void initFirebaseWriteQueue();
void clearFirebaseWriteQueue();
bool enqueueFirebaseWrite(int index, bool state);
void processFirebaseWriteQueue();
void firebaseTask(void *param);
void startFirebaseTask();
bool lockSwState(TickType_t waitTicks = portMAX_DELAY);
void unlockSwState();
bool readSwState(int index);
void writeSwState(int index, bool value);
void initDeviceId();
String firebaseDeviceId();
String firebaseDeviceNameValue();
String firebaseBasePathForUid(const String &uid);
String firebaseRelayPath(const String &uid, int index);
String firebaseDeviceNamePath(const String &uid);
void queueAllRelayStatesForFirebaseSync();
void onDeviceNameUpdated(const String &previousName);
bool firebaseSetStringAtPath(const String &path, const String &value);
void restartFirebaseStream(const char *reason);
void syncRelayStateToWebView(int index, bool state);

// Restart cleanly on heap exhaustion instead of crashing via std::terminate()
static void oomNewHandler() {
  ets_printf("[OOM] operator new failed — restarting\n");
  esp_restart();
}

//  BEEP + LED ON NVS CHANGE
void serviceNotifyStorage() {
  if (notifyStorageOffAt && millis() >= notifyStorageOffAt) {
    notifyStorageOffAt = 0;
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
  }
}

void notifyStorage() {
  digitalWrite(LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  notifyStorageOffAt = millis() + 100;
}

//  TIMEZONE PARSING  "+00:00" -> seconds
void parseTZ() {
  int sign = 1;
  if (tzStr.startsWith("-"))
    sign = -1;
  int c = tzStr.indexOf(':');
  int h = 0, m = 0;
  if (c > 0) {
    h = tzStr.substring(1, c).toInt();
    m = tzStr.substring(c + 1).toInt();
  }
  gmtOff = sign * (h * 3600L + m * 60L);
}

void loadTimeSettingsFromAdminStorage() {
  prefs.begin("admin", true);
  String storedNtp = prefs.getString("ntp", ntpSrv);
  String storedTz = prefs.getString("tz", tzStr);
  prefs.end();

  storedNtp.trim();
  if (!storedNtp.isEmpty()) {
    ntpSrv = storedNtp;
  }

  storedTz.trim();
  if (!storedTz.isEmpty()) {
    tzStr = storedTz;
  }

  parseTZ();
}

//  LOAD ALL SETTINGS FROM NVS
void loadSwitchSettings() {
  prefs.begin("sw", true);
  for (int i = 0; i < NUM_SWITCHES; i++) {
    swName[i] = prefs.getString(("n" + String(i)).c_str(), swName[i]);
    swIcon[i] = prefs.getString(("i" + String(i)).c_str(), swIcon[i]);

    // 1. Read the saved setting as a string
    String modeStr = prefs.getString(("r" + String(i)).c_str(), "off");

    // 2. Convert the string to our new Enum
    if (modeStr == "on") {
      relayMode[i] = RELAY_MODE_ON;
    } else if (modeStr == "remember") {
      relayMode[i] = RELAY_MODE_REMEMBER;
    } else {
      relayMode[i] = RELAY_MODE_OFF;
    }

    // 3. Set the initial state using the Enum
    bool initialState = false;
    if (relayMode[i] == RELAY_MODE_ON) {
      initialState = true;
    } else if (relayMode[i] == RELAY_MODE_REMEMBER) {
      initialState = prefs.getBool(("l" + String(i)).c_str(), false);
    }

    writeSwState(i, initialState);
    digitalWrite(outPin[i], initialState ? HIGH : LOW);
  }
  prefs.end();
}

void loadWifiSettings() {
  prefs.begin("wfcfg", true);
  dhcpOn = prefs.getBool("dhcp", true);
  sIp = prefs.getString("sip", "");
  sMask = prefs.getString("mask", "255.255.255.0");
  sGw = prefs.getString("gw", "");
  sDns = prefs.getString("dns", "8.8.8.8");
  prefs.end();
}

bool getSavedWifiCredentials(String &ssid, String &pass) {
  prefs.begin("wfcfg", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return !ssid.isEmpty();
}

bool hasValidStaticConfig(IPAddress &ip, IPAddress &gateway, IPAddress &subnet,
                          IPAddress &dns) {
  return ip.fromString(sIp) && gateway.fromString(sGw) &&
         subnet.fromString(sMask) && dns.fromString(sDns);
}

void applyStationIpConfig() {
  WiFi.mode(WIFI_STA);
  if (!deviceName.isEmpty()) {
    WiFi.setHostname(deviceName.c_str());
  }

  if (dhcpOn) {
    IPAddress zero(0, 0, 0, 0);
    if (WiFi.config(zero, zero, zero, zero)) {
      Serial.println("[WIFI] DHCP mode enabled");
    } else {
      Serial.println("[WIFI] Failed to enable DHCP mode");
    }
    return;
  }

  IPAddress ip, gateway, subnet, dns;
  if (!hasValidStaticConfig(ip, gateway, subnet, dns)) {
    Serial.println("[WIFI] Invalid static IP settings, falling back to DHCP");
    IPAddress zero(0, 0, 0, 0);
    WiFi.config(zero, zero, zero, zero);
    return;
  }

  if (WiFi.config(ip, gateway, subnet, dns)) {
    Serial.printf("[WIFI] Static IP configured: %s\n", ip.toString().c_str());
  } else {
    Serial.println("[WIFI] Failed to apply static IP settings");
  }
}

void saveCurrentWifiCredentials(bool notifyChange = false) {
  String currentSsid = WiFi.SSID();
  if (currentSsid.isEmpty()) {
    return;
  }

  prefs.begin("wfcfg", false);
  prefs.putString("ssid", currentSsid);
  prefs.putString("pass", WiFi.psk());
  prefs.end();

  if (notifyChange) {
    notifyStorage();
  }
}

void prepareWifiForConfigPortal() {
  WiFi.disconnect(false, true);
  delay(200);
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP_STA);
  delay(100);
}

bool runWifiConfigPortal(bool notifyChange = false,
                         bool restartOnSuccess = true) {
  WiFiManager wm;
  bool success = false;

  wm.setConfigPortalTimeout(180); // 3 minutes
  wm.setAPCallback([](WiFiManager *myWiFiManager) {
    Serial.println(
        "[WIFI] Config portal ready - connect to ESP HOME and open "
        "192.168.4.1");
  });
  prepareWifiForConfigPortal();
  Serial.println("[WIFI] Starting forced config portal...");
  Serial.println("[WIFI] SSID: 'ESP HOME' | IP: 192.168.4.1");
  success = wm.startConfigPortal("ESP HOME");

  if (success) {
    Serial.println("\n[SUCCESS] WiFi Connected via Config Portal");
    Serial.printf("SSID : %s\n", WiFi.SSID().c_str());
    Serial.printf("IP Address : %s\n", WiFi.localIP().toString().c_str());
    WiFi.setSleep(false);
    Serial.println("[WIFI] Power save disabled after config portal connection");
    Serial.println("==============================\n");
    saveCurrentWifiCredentials(notifyChange);
    if (restartOnSuccess) {
      Serial.println(
          "[WIFI] Restarting to bring the web server up on the saved "
          "network...");
      delay(500);
      ESP.restart();
    }
    return true;
  }

  Serial.println("\n[ERROR] Config Portal Timeout!");
  Serial.println("Device not connected to WiFi.");
  Serial.println("==============================\n");
  delay(1000);
  return false;
}

String getDefaultDeviceName() {
  uint64_t chipId = ESP.getEfuseMac();
  char buf[13];
  // ESP.getEfuseMac() stores the 48-bit MAC least-significant byte first.
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", (uint8_t)(chipId),
           (uint8_t)(chipId >> 8), (uint8_t)(chipId >> 16),
           (uint8_t)(chipId >> 24), (uint8_t)(chipId >> 32),
           (uint8_t)(chipId >> 40));
  return String(buf);
}

String getLegacyReversedDefaultDeviceName() {
  uint64_t chipId = ESP.getEfuseMac();
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X",
           (uint8_t)(chipId >> 40), (uint8_t)(chipId >> 32),
           (uint8_t)(chipId >> 24), (uint8_t)(chipId >> 16),
           (uint8_t)(chipId >> 8), (uint8_t)(chipId));
  return String(buf);
}

void setDefaultAutomationPriorityOrder() {
  automationPriorityOrder[0] = AUTOMATION_TIMER;
  automationPriorityOrder[1] = AUTOMATION_SCHEDULE;
  automationPriorityOrder[2] = AUTOMATION_FUTURE_SCHEDULE;
  automationPriorityOrder[3] = AUTOMATION_TEMPERATURE;
  automationPriorityOrder[4] = AUTOMATION_HUMIDITY;
  automationPriorityOrder[5] = AUTOMATION_SUN;
}

bool validateAutomationPriorityOrder(const uint8_t *order) {
  bool seen[AUTOMATION_SOURCE_COUNT] = {false, false, false,
                                        false, false, false};
  for (int i = 0; i < AUTOMATION_SOURCE_COUNT; i++) {
    uint8_t src = order[i];
    if (src >= AUTOMATION_SOURCE_COUNT || seen[src])
      return false;
    seen[src] = true;
  }
  return true;
}

int getAutomationPriorityRank(uint8_t source) {
  for (int i = 0; i < AUTOMATION_SOURCE_COUNT; i++) {
    if (automationPriorityOrder[i] == source)
      return i;
  }
  return AUTOMATION_SOURCE_COUNT;
}

const char *automationSourceKey(uint8_t source) {
  switch (source) {
  case AUTOMATION_TIMER:
    return "timer";
  case AUTOMATION_SCHEDULE:
    return "schedule";
  case AUTOMATION_FUTURE_SCHEDULE:
    return "future";
  case AUTOMATION_TEMPERATURE:
    return "temperature";
  case AUTOMATION_HUMIDITY:
    return "humidity";
  case AUTOMATION_SUN:
    return "sun";
  default:
    return "timer";
  }
}

const char *automationSourceLabel(uint8_t source) {
  switch (source) {
  case AUTOMATION_TIMER:
    return "Timer";
  case AUTOMATION_SCHEDULE:
    return "Schedule";
  case AUTOMATION_FUTURE_SCHEDULE:
    return "Future Schedule";
  case AUTOMATION_TEMPERATURE:
    return "Temperature";
  case AUTOMATION_HUMIDITY:
    return "Humidity";
  case AUTOMATION_SUN:
    return "Sunrise & Sunset";
  default:
    return "Timer";
  }
}

int automationSourceFromKey(String key) {
  key.trim();
  key.toLowerCase();
  if (key == "timer")
    return AUTOMATION_TIMER;
  if (key == "schedule")
    return AUTOMATION_SCHEDULE;
  if (key == "future")
    return AUTOMATION_FUTURE_SCHEDULE;
  if (key == "temperature")
    return AUTOMATION_TEMPERATURE;
  if (key == "humidity")
    return AUTOMATION_HUMIDITY;
  if (key == "sun")
    return AUTOMATION_SUN;
  return -1;
}

void clearPendingAutomationActions() {
  for (int i = 0; i < NUM_SWITCHES; i++) {
    pendingAutomationAction[i] = false;
  }
}

void queueAutomationAction(int sw, bool targetState, uint8_t source) {
  if (sw < 0 || sw >= NUM_SWITCHES)
    return;

  int newRank = getAutomationPriorityRank(source);
  if (newRank >= AUTOMATION_SOURCE_COUNT)
    return;

  if (!pendingAutomationAction[sw]) {
    pendingAutomationAction[sw] = true;
    pendingAutomationState[sw] = targetState;
    pendingAutomationSource[sw] = source;
    return;
  }

  int currentRank = getAutomationPriorityRank(pendingAutomationSource[sw]);
  if (newRank <= currentRank) {
    pendingAutomationState[sw] = targetState;
    pendingAutomationSource[sw] = source;
  }
}

void applyPendingAutomationActions() {
  for (int i = 0; i < NUM_SWITCHES; i++) {
    if (!pendingAutomationAction[i])
      continue;
    setRelayState(i, pendingAutomationState[i], "automation");
    Serial.printf("[AUTO] SW%d -> %s (source=%s, rank=%d)\n", i,
                  pendingAutomationState[i] ? "ON" : "OFF",
                  automationSourceLabel(pendingAutomationSource[i]),
                  getAutomationPriorityRank(pendingAutomationSource[i]) + 1);
  }
}

void loadFbSettings() {
  prefs.begin("fb", true);
  fbOn = prefs.getBool("en", false);
  fbUrl = prefs.getString("url", "");
  fbToken = prefs.getString("tok", "");
  prefs.end();
  firebaseRuntimeDirty = true;
}

void loadAdminSettings() {
  String storedDeviceName;
  String storedPrimaryAdminId;
  String storedPrimaryAdminPass;
  String loadedRestartScheduleLastRunKey;
  bool loadedRestartScheduleEnabled = false;
  int loadedRestartScheduleMinute = -1;
  uint8_t loadedRestartScheduleDayMask = 0;
  uint8_t loadedPriority[AUTOMATION_SOURCE_COUNT];
  prefs.begin("admin", true);
  ntpSrv = prefs.getString("ntp", DEFAULT_NTP_SERVER);
  tzStr = prefs.getString("tz", DEFAULT_TIME_ZONE);
  geoLat = prefs.getFloat("lat", DEFAULT_LATITUDE);
  geoLon = prefs.getFloat("lon", DEFAULT_LONGITUDE);
  storedDeviceName = prefs.getString("devname", "");
  storedPrimaryAdminId = prefs.getString("puid", String(PRIMARY_ADMIN_ID));
  storedPrimaryAdminPass = prefs.getString("ppass", String(PRIMARY_ADMIN_PASS));
  loadedRestartScheduleLastRunKey = prefs.getString("rsLast", "");
  loadedRestartScheduleEnabled = prefs.getBool("rsEn", false);
  loadedRestartScheduleMinute = prefs.getInt("rsMin", -1);
  loadedRestartScheduleDayMask = (uint8_t)prefs.getUInt("rsMask", 0);
  for (int i = 0; i < AUTOMATION_SOURCE_COUNT; i++) {
    loadedPriority[i] = (uint8_t)prefs.getInt(("pr" + String(i)).c_str(), i);
  }
  prefs.end();

  storedDeviceName.trim();
  String defaultDeviceName = getDefaultDeviceName();
  String legacyReversedDefaultDeviceName = getLegacyReversedDefaultDeviceName();
  bool migrateLegacyReversedDeviceName = false;
  if (storedDeviceName.isEmpty()) {
    deviceName = defaultDeviceName;
  } else {
    if (storedDeviceName == legacyReversedDefaultDeviceName &&
        legacyReversedDefaultDeviceName != defaultDeviceName) {
      deviceName = defaultDeviceName;
      migrateLegacyReversedDeviceName = true;
    } else {
      deviceName = storedDeviceName;
    }
  }

  storedPrimaryAdminId = normalizeUserId(storedPrimaryAdminId);
  storedPrimaryAdminPass = normalizeUserPass(storedPrimaryAdminPass);
  bool missingPrimaryAdminProfile = false;
  if (storedPrimaryAdminId.isEmpty()) {
    storedPrimaryAdminId = normalizeUserId(String(PRIMARY_ADMIN_ID));
    missingPrimaryAdminProfile = true;
  }
  if (storedPrimaryAdminPass.isEmpty()) {
    storedPrimaryAdminPass = normalizeUserPass(String(PRIMARY_ADMIN_PASS));
    missingPrimaryAdminProfile = true;
  }
  configuredPrimaryAdminId = storedPrimaryAdminId;
  configuredPrimaryAdminPass = storedPrimaryAdminPass;

  bool orderValid = validateAutomationPriorityOrder(loadedPriority);
  if (orderValid) {
    for (int i = 0; i < AUTOMATION_SOURCE_COUNT; i++) {
      automationPriorityOrder[i] = loadedPriority[i];
    }
  } else {
    setDefaultAutomationPriorityOrder();
  }

  if (storedDeviceName.isEmpty() || migrateLegacyReversedDeviceName ||
      !orderValid || missingPrimaryAdminProfile) {
    prefs.begin("admin", false);
    if (storedDeviceName.isEmpty() || migrateLegacyReversedDeviceName) {
      prefs.putString("devname", deviceName);
    }
    if (!orderValid) {
      for (int i = 0; i < AUTOMATION_SOURCE_COUNT; i++) {
        prefs.putInt(("pr" + String(i)).c_str(), automationPriorityOrder[i]);
      }
    }
    if (missingPrimaryAdminProfile) {
      prefs.putString("puid", configuredPrimaryAdminId);
      prefs.putString("ppass", configuredPrimaryAdminPass);
    }
    prefs.end();
  }

  if (loadedRestartScheduleEnabled && loadedRestartScheduleMinute >= 0 &&
      loadedRestartScheduleMinute <= 1439 &&
      loadedRestartScheduleDayMask != 0) {
    restartScheduleEnabled = true;
    restartScheduleMinute = loadedRestartScheduleMinute;
    restartScheduleDayMask = loadedRestartScheduleDayMask;
  } else {
    restartScheduleEnabled = false;
    restartScheduleMinute = -1;
    restartScheduleDayMask = 0;
  }
  loadedRestartScheduleLastRunKey.trim();
  restartScheduleLastRunKey = loadedRestartScheduleLastRunKey;
  lastRestartScheduleStamp = -1;

  locOk = (geoLat >= -90.0f && geoLat <= 90.0f && geoLon >= -180.0f &&
           geoLon <= 180.0f);
  parseTZ();
}

String normalizeUserRole(String role) {
  role.trim();
  role.toLowerCase();
  if (role == "admin")
    return "admin";
  return "user";
}

String normalizeUserId(String id) {
  id.trim();
  id.toLowerCase();
  return id;
}

String normalizeUserPass(String pass) {
  pass.trim();
  return pass;
}

void persistConfiguredPrimaryAdmin(const String &adminId,
                                   const String &adminPass) {
  String normalizedId = normalizeUserId(adminId);
  String normalizedPass = normalizeUserPass(adminPass);

  if (normalizedId.isEmpty())
    normalizedId = normalizeUserId(String(PRIMARY_ADMIN_ID));
  if (normalizedPass.isEmpty())
    normalizedPass = normalizeUserPass(String(PRIMARY_ADMIN_PASS));

  configuredPrimaryAdminId = normalizedId;
  configuredPrimaryAdminPass = normalizedPass;

  prefs.begin("admin", false);
  prefs.putString("puid", configuredPrimaryAdminId);
  prefs.putString("ppass", configuredPrimaryAdminPass);
  prefs.end();
}

void replaceUsersWithSingleAdmin(const String &adminId,
                                 const String &adminPass) {
  String targetAdminId = normalizeUserId(adminId);
  String targetAdminPass = normalizeUserPass(adminPass);

  prefs.begin("users", false);
  int count = prefs.getInt("cnt", 0);

  int preferredIndex = -1;
  int firstAdminIndex = -1;
  for (int i = 0; i < count; i++) {
    String js = prefs.getString(("u" + String(i)).c_str(), "");
    if (js.isEmpty())
      continue;

    JsonDocument d;
    if (deserializeJson(d, js))
      continue;

    String id = normalizeUserId(d["id"].as<String>());
    String role = normalizeUserRole(d["role"].as<String>());

    if (preferredIndex < 0 && id == targetAdminId)
      preferredIndex = i;
    if (firstAdminIndex < 0 && role == "admin")
      firstAdminIndex = i;
  }

  int targetAdminIndex = preferredIndex >= 0 ? preferredIndex : firstAdminIndex;
  if (targetAdminIndex < 0) {
    JsonDocument d;
    d["id"] = targetAdminId;
    d["pass"] = targetAdminPass;
    d["role"] = "admin";
    String serialized;
    serializeJson(d, serialized);
    prefs.putString(("u" + String(count)).c_str(), serialized);
    prefs.putInt("cnt", count + 1);
    prefs.end();
    return;
  }

  for (int i = 0; i < count; i++) {
    String js = prefs.getString(("u" + String(i)).c_str(), "");
    if (js.isEmpty())
      continue;

    JsonDocument d;
    if (deserializeJson(d, js))
      continue;

    bool changed = false;
    String role = normalizeUserRole(d["role"].as<String>());

    if (i == targetAdminIndex) {
      d["id"] = targetAdminId;
      d["pass"] = targetAdminPass;
      d["role"] = "admin";
      changed = true;
    } else if (role == "admin") {
      d["role"] = "user";
      changed = true;
    }

    if (changed) {
      String updated;
      serializeJson(d, updated);
      prefs.putString(("u" + String(i)).c_str(), updated);
    }
  }

  prefs.end();
}

void resetAdminToFactoryDefault() {
  persistConfiguredPrimaryAdmin(String(PRIMARY_ADMIN_ID),
                                String(PRIMARY_ADMIN_PASS));
  replaceUsersWithSingleAdmin(configuredPrimaryAdminId,
                              configuredPrimaryAdminPass);
  notifyStorage();
}

void resetSwitchNamesToDefault() {
  prefs.begin("sw", false);
  for (int i = 0; i < NUM_SWITCHES; i++) {
    swName[i] = DEFAULT_SWITCH_NAMES[i];
    prefs.putString(("n" + String(i)).c_str(), swName[i]);
  }
  prefs.end();
}

void resetSwitchIconsToDefault() {
  prefs.begin("sw", false);
  for (int i = 0; i < NUM_SWITCHES; i++) {
    swIcon[i] = DEFAULT_SWITCH_ICONS[i];
    prefs.putString(("i" + String(i)).c_str(), swIcon[i]);
  }
  prefs.end();
}

void clearRecurringSchedulesStorage() {
  prefs.begin("sched", false);
  prefs.clear();
  prefs.end();
  for (int i = 0; i < NUM_SWITCHES; i++) {
    swTimers[i].active = false;
    swTimers[i].paused = false;
    swTimers[i].endMs = 0;
    swTimers[i].remainingMs = 0;
  }
}

void clearFutureSchedulesStorage() {
  prefs.begin("fsched", false);
  prefs.clear();
  prefs.end();
}

void clearSensorControlStorage() {
  prefs.begin("sensor", false);
  prefs.clear();
  prefs.end();
}

void clearRestartScheduleOnly(bool notifyChange = false) {
  restartScheduleEnabled = false;
  restartScheduleMinute = -1;
  restartScheduleDayMask = 0;
  lastRestartScheduleStamp = -1;
  restartScheduleLastRunKey = "";

  prefs.begin("admin", false);
  prefs.remove("rsEn");
  prefs.remove("rsMin");
  prefs.remove("rsMask");
  prefs.remove("rsLast");
  prefs.end();

  if (notifyChange) {
    notifyStorage();
  }
}

void clearStaticIpConfiguration() {
  dhcpOn = true;
  sIp = "";
  sMask = "255.255.255.0";
  sGw = "";
  sDns = "8.8.8.8";

  prefs.begin("wfcfg", false);
  prefs.putBool("dhcp", true);
  prefs.remove("sip");
  prefs.remove("mask");
  prefs.remove("gw");
  prefs.remove("dns");
  prefs.end();
}

void resetSchedulePriorityToDefault() {
  setDefaultAutomationPriorityOrder();
  prefs.begin("admin", false);
  for (int i = 0; i < AUTOMATION_SOURCE_COUNT; i++) {
    prefs.putInt(("pr" + String(i)).c_str(), automationPriorityOrder[i]);
  }
  prefs.end();
}

void resetLocationToDefault() {
  geoLat = DEFAULT_LATITUDE;
  geoLon = DEFAULT_LONGITUDE;
  locOk = true;
  lastCalcDay = -1;

  prefs.begin("admin", false);
  prefs.putFloat("lat", geoLat);
  prefs.putFloat("lon", geoLon);
  prefs.end();

  calcSunriseSunset();
}

void resetRelayStatesToDefault() {
  prefs.begin("sw", false);
  for (int i = 0; i < NUM_SWITCHES; i++) {
    // 1. Set the active RAM state using the Enum
    relayMode[i] = RELAY_MODE_OFF;

    writeSwState(i, false);
    digitalWrite(outPin[i], LOW);

    // 2. Save the string "off" to NVS memory
    prefs.putString(("r" + String(i)).c_str(), "off");
    prefs.remove(("l" + String(i)).c_str());
  }
  prefs.end();
}

void disableFirebaseOnly() {
  fbOn = false;
  prefs.begin("fb", false);
  prefs.putBool("en", false);
  prefs.end();
  firebaseRuntimeDirty = true;
}

void resetDeviceNameToDefault() {
  String previousName = firebaseDeviceNameValue();
  deviceName = getDefaultDeviceName();
  prefs.begin("admin", false);
  prefs.putString("devname", deviceName);
  prefs.end();
  WiFi.setHostname(deviceName.c_str());
  onDeviceNameUpdated(previousName);
}

void resetNtpServerToDefault() {
  ntpSrv = DEFAULT_NTP_SERVER;
  prefs.begin("admin", false);
  prefs.putString("ntp", ntpSrv);
  prefs.end();
}

void resetTimezoneToDefault() {
  tzStr = DEFAULT_TIME_ZONE;
  parseTZ();
  prefs.begin("admin", false);
  prefs.putString("tz", tzStr);
  prefs.end();
}

void clearWifiCredentialsOnly() {
  prefs.begin("wfcfg", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

void clearFirebaseCredentialsOnly() {
  fbOn = false;
  fbUrl = "";
  fbToken = "";
  prefs.begin("fb", false);
  prefs.putBool("en", false);
  prefs.remove("url");
  prefs.remove("tok");
  prefs.end();
  firebaseRuntimeDirty = true;
}

void resetFirebaseRulesToDefault() {
  prefs.begin("fb", false);
  prefs.putString("rules", DEFAULT_FIREBASE_RULES_JSON);
  prefs.end();
}

void clearFirebaseUsersOnly() {
  prefs.begin("fb", false);
  prefs.putString("authUsers", "[]");
  prefs.end();
}

void resetUsersToSingleDefaultAdmin() {
  prefs.begin("users", false);
  prefs.clear();
  prefs.end();

  persistConfiguredPrimaryAdmin(String(PRIMARY_ADMIN_ID),
                                String(PRIMARY_ADMIN_PASS));
  replaceUsersWithSingleAdmin(configuredPrimaryAdminId,
                              configuredPrimaryAdminPass);
}

bool requireBootHoldResetGesture(AsyncWebServerRequest *req) {
  if (consumeBootButtonHoldForReset()) {
    return true;
  }

  req->send(400, "application/json",
            "{\"ok\":false,\"bootRequired\":true,\"error\":\"Press and hold "
            "BOOT for 5 seconds, then retry reset.\"}");
  return false;
}

void addResetStep(JsonArray steps, const char *key, const char *label,
                  uint16_t durationMs = 420) {
  uint16_t normalizedDuration = durationMs;
  if (normalizedDuration < 180) {
    normalizedDuration = (uint16_t)RESET_CHECKPOINT_GAP_MS;
  }
  JsonObject item = steps.createNestedObject();
  item["key"] = key;
  item["label"] = label;
  item["ok"] = true;
  item["ms"] = normalizedDuration;
}

void updateBootButtonHoldState() {
  unsigned long now = millis();

  if (digitalRead(BOOT_BUTTON) == LOW) {
    if (bootButtonPressedAt == 0) {
      bootButtonPressedAt = now;
    }

    if (!bootHoldSatisfied &&
        (now - bootButtonPressedAt >= BOOT_HOLD_RESET_MS)) {
      bootHoldSatisfied = true;
      bootHoldSatisfiedAt = now;
      Serial.println("[BOOT] Hold detected for admin reset");
    }

    return;
  }

  bootButtonPressedAt = 0;
  if (bootHoldSatisfied &&
      (now - bootHoldSatisfiedAt > BOOT_HOLD_READY_WINDOW_MS)) {
    bootHoldSatisfied = false;
    bootHoldSatisfiedAt = 0;
  }
}

bool consumeBootButtonHoldForReset() {
  unsigned long now = millis();
  if (!bootHoldSatisfied)
    return false;

  if (now - bootHoldSatisfiedAt > BOOT_HOLD_READY_WINDOW_MS) {
    bootHoldSatisfied = false;
    bootHoldSatisfiedAt = 0;
    return false;
  }

  bootHoldSatisfied = false;
  bootHoldSatisfiedAt = 0;
  bootButtonPressedAt = now;
  return true;
}

//  USER MANAGEMENT HELPERS
void initDefaultUser() {
  prefs.begin("users", false);
  int count = prefs.getInt("cnt", 0);
  if (count <= 0) {
    JsonDocument d;
    d["id"] = configuredPrimaryAdminId;
    d["pass"] = configuredPrimaryAdminPass;
    d["role"] = "admin";
    String j;
    serializeJson(d, j);
    prefs.putString("u0", j);
    prefs.putInt("cnt", 1);
    prefs.end();
    return;
  }

  bool hasEspAdmin = false;
  int espIndex = -1;
  int legacyDefaultIndex = -1;
  int totalAdminCount = 0;
  for (int i = 0; i < count; i++) {
    String js = prefs.getString(("u" + String(i)).c_str(), "");
    if (js.isEmpty())
      continue;

    JsonDocument d;
    if (deserializeJson(d, js))
      continue;

    String id = normalizeUserId(d["id"].as<String>());
    String pass = normalizeUserPass(d["pass"].as<String>());
    String role = normalizeUserRole(d["role"].as<String>());

    if (role == "admin")
      totalAdminCount++;

    if (id == normalizeUserId(configuredPrimaryAdminId)) {
      if (espIndex < 0)
        espIndex = i;
      if (role == "admin") {
        hasEspAdmin = true;
      }
    }

    if (legacyDefaultIndex < 0 && id == "mrinal" && pass == "1234" &&
        role == "admin") {
      legacyDefaultIndex = i;
    }
  }

  if (!hasEspAdmin) {
    JsonDocument d;
    d["id"] = configuredPrimaryAdminId;
    d["pass"] = configuredPrimaryAdminPass;
    d["role"] = "admin";
    String migrated;
    serializeJson(d, migrated);

    if (legacyDefaultIndex >= 0) {
      // Migrate legacy mrinal/1234 admin to the default esp admin.
      prefs.putString(("u" + String(legacyDefaultIndex)).c_str(), migrated);
    } else if (espIndex >= 0) {
      // esp user exists but without admin role - promote it.
      prefs.putString(("u" + String(espIndex)).c_str(), migrated);
    } else if (totalAdminCount == 0) {
      // No admin of any kind exists - add esp as the last-resort fallback.
      prefs.putString(("u" + String(count)).c_str(), migrated);
      prefs.putInt("cnt", count + 1);
    }
    // else: other admins already exist; esp was intentionally removed - do
    // nothing.
  }

  // Enforce single-admin policy: keep the first admin, demote any extra admins.
  int keepAdminIndex = -1;
  for (int i = 0; i < count; i++) {
    String js = prefs.getString(("u" + String(i)).c_str(), "");
    if (js.isEmpty())
      continue;

    JsonDocument d;
    if (deserializeJson(d, js))
      continue;

    if (normalizeUserRole(d["role"].as<String>()) != "admin")
      continue;

    if (keepAdminIndex < 0) {
      keepAdminIndex = i;
      continue;
    }

    d["role"] = "user";
    String updated;
    serializeJson(d, updated);
    prefs.putString(("u" + String(i)).c_str(), updated);
  }

  prefs.end();
}

bool verifyAdmin(const String &u, const String &p) {
  String targetUser = normalizeUserId(u);
  String targetPass = normalizeUserPass(p);
  prefs.begin("users", true);
  int n = prefs.getInt("cnt", 0);
  for (int i = 0; i < n; i++) {
    String js = prefs.getString(("u" + String(i)).c_str(), "");
    if (js.isEmpty())
      continue;
    JsonDocument d;
    if (deserializeJson(d, js))
      continue;
    String storedPass = normalizeUserPass(d["pass"].as<String>());
    if (normalizeUserId(d["id"].as<String>()) == targetUser &&
        storedPass == targetPass &&
        normalizeUserRole(d["role"].as<String>()) == "admin") {
      prefs.end();
      return true;
    }
  }
  prefs.end();
  return false;
}

bool requireAdminVerification(AsyncWebServerRequest *req) {
  if (!req->hasParam("adminUser", true) || !req->hasParam("adminPass", true)) {
    req->send(400, "application/json",
              "{\"error\":\"Admin credentials required\"}");
    return false;
  }

  String adminUser = req->getParam("adminUser", true)->value();
  String adminPass = req->getParam("adminPass", true)->value();

  if (!verifyAdmin(adminUser, adminPass)) {
    // Try to recover primary admin in case storage was previously corrupted.
    initDefaultUser();
    if (!verifyAdmin(adminUser, adminPass)) {
      req->send(403, "application/json",
                "{\"error\":\"Admin verification failed\"}");
      return false;
    }
  }

  return true;
}

bool verifyLogin(const String &u, const String &p, String &role) {
  String targetUser = normalizeUserId(u);
  String targetPass = normalizeUserPass(p);
  prefs.begin("users", true);
  int n = prefs.getInt("cnt", 0);
  for (int i = 0; i < n; i++) {
    String js = prefs.getString(("u" + String(i)).c_str(), "");
    if (js.isEmpty())
      continue;
    JsonDocument d;
    if (deserializeJson(d, js))
      continue;
    String storedPass = normalizeUserPass(d["pass"].as<String>());
    if (normalizeUserId(d["id"].as<String>()) == targetUser &&
        storedPass == targetPass) {
      role = normalizeUserRole(d["role"].as<String>());
      prefs.end();
      return true;
    }
  }
  prefs.end();
  return false;
}

bool hasAdmin() {
  prefs.begin("users", true);
  int n = prefs.getInt("cnt", 0);
  for (int i = 0; i < n; i++) {
    String js = prefs.getString(("u" + String(i)).c_str(), "");
    if (js.isEmpty())
      continue;
    JsonDocument d;
    deserializeJson(d, js);
    if (normalizeUserRole(d["role"].as<String>()) == "admin") {
      prefs.end();
      return true;
    }
  }
  prefs.end();
  return false;
}

int countAdmins() {
  prefs.begin("users", true);
  int n = prefs.getInt("cnt", 0), a = 0;
  for (int i = 0; i < n; i++) {
    String js = prefs.getString(("u" + String(i)).c_str(), "");
    if (js.isEmpty())
      continue;
    JsonDocument d;
    deserializeJson(d, js);
    if (normalizeUserRole(d["role"].as<String>()) == "admin")
      a++;
  }
  prefs.end();
  return a;
}

String normalizeEmail(String email) {
  email.trim();
  email.toLowerCase();
  return email;
}

bool isValidEmailAddress(const String &email) {
  int atPos = email.indexOf('@');
  int dotPos = email.lastIndexOf('.');
  return !email.isEmpty() && atPos > 0 && dotPos > atPos + 1 &&
         dotPos < email.length() - 1 && email.indexOf(' ') < 0;
}

void loadFirebaseAuthUsersDoc(JsonDocument &doc) {
  doc.clear();
  prefs.begin("fb", true);
  String raw = prefs.getString("authUsers", "[]");
  prefs.end();

  DeserializationError err = deserializeJson(doc, raw);
  if (err || !doc.is<JsonArray>()) {
    doc.clear();
    doc.to<JsonArray>();
  }
}

void saveFirebaseAuthUsersDoc(JsonDocument &doc) {
  String raw;
  serializeJson(doc, raw);
  prefs.begin("fb", false);
  prefs.putString("authUsers", raw);
  prefs.end();
}

int findFirebaseAuthUserIndex(JsonArray users, const String &email) {
  String targetEmail = normalizeEmail(email);
  int index = 0;
  for (JsonObject user : users) {
    if (normalizeEmail(user["email"].as<String>()) == targetEmail) {
      return index;
    }
    index++;
  }
  return -1;
}

void configureFirebaseSslClient(WiFiClientSecure &client) {
  client.setInsecure();
#if defined(ESP32)
  client.setConnectionTimeout(1000);
  client.setHandshakeTimeout(5);
#endif
#if defined(ESP8266)
  client.setBufferSizes(4096, 1024);
#endif
}

void initFirebaseWriteQueue() {
  if (firebaseWriteQueue)
    return;

  firebaseWriteQueue =
      xQueueCreate(FIREBASE_WRITE_QUEUE_LENGTH, sizeof(FirebaseWriteTask));
  if (!firebaseWriteQueue) {
    Serial.println("[FB] Failed to create write queue");
  }
}

void clearFirebaseWriteQueue() {
  if (!firebaseWriteQueue)
    return;

  FirebaseWriteTask discarded;
  while (xQueueReceive(firebaseWriteQueue, &discarded, 0) == pdTRUE) {
  }
}

bool lockSwState(TickType_t waitTicks) {
  if (!swStateMutex)
    return true;

  return xSemaphoreTake(swStateMutex, waitTicks) == pdTRUE;
}

void unlockSwState() {
  if (swStateMutex) {
    xSemaphoreGive(swStateMutex);
  }
}

bool readSwState(int index) {
  if (index < 0 || index >= NUM_SWITCHES)
    return false;

  bool value = false;
  if (lockSwState()) {
    value = swState[index];
    unlockSwState();
  }
  return value;
}

void writeSwState(int index, bool value) {
  if (index < 0 || index >= NUM_SWITCHES)
    return;

  if (lockSwState()) {
    swState[index] = value;
    unlockSwState();
  }
}

void syncRelayStateToWebView(int index, bool state) {
  (void)index;
  (void)state;
  // Dashboard state comes from /api/switches, so updating swState[] is enough.
}

String firebaseDeviceNameValue() {
  String label = deviceName;
  label.trim();
  if (label.isEmpty()) {
    label = getDefaultDeviceName();
  }

  return label;
}

void initDeviceId() {
  String mac = WiFi.macAddress();
  mac.trim();
  mac.replace(":", "");
  mac.toUpperCase();

  if (mac.length() != 12 || mac == "000000000000") {
    mac = getDefaultDeviceName();
    mac.trim();
    mac.toUpperCase();
  }

  cachedDeviceId = mac;
}

String firebaseDeviceId() {
  if (cachedDeviceId.isEmpty()) {
    initDeviceId();
  }

  if (cachedDeviceId.isEmpty())
    return getDefaultDeviceName();

  return cachedDeviceId;
}

String firebaseBasePathForUid(const String &uid) {
  String normalizedUid = uid;
  normalizedUid.trim();
  if (normalizedUid.isEmpty())
    return "";

  return "/espHome/" + normalizedUid + "/" + firebaseDeviceId();
}

String firebaseRelayPath(const String &uid, int index) {
  if (index < 0 || index >= NUM_SWITCHES)
    return "";

  String basePath = firebaseBasePathForUid(uid);
  if (basePath.isEmpty())
    return "";

  return basePath + "/switch" + String(index + 1);
}

String firebaseDeviceNamePath(const String &uid) {
  String basePath = firebaseBasePathForUid(uid);
  if (basePath.isEmpty())
    return "";

  return basePath + "/deviceName";
}

void queueAllRelayStatesForFirebaseSync() {
  for (int i = 0; i < NUM_SWITCHES; i++) {
    enqueueFirebaseWrite(i, readSwState(i));
  }
}

void onDeviceNameUpdated(const String &previousName) {
  String oldName = previousName;
  oldName.trim();
  if (oldName.isEmpty()) {
    oldName = getDefaultDeviceName();
  }

  String newName = firebaseDeviceNameValue();
  if (oldName == newName) {
    return;
  }

  firebaseDeviceNameSyncDirty = true;
  firebaseLastSyncedDeviceName = "";

  // Keep Firebase node aligned with updated name by replaying the current relay
  // state.
  if (fbOn) {
    clearFirebaseWriteQueue();
    queueAllRelayStatesForFirebaseSync();
  }
}

bool enqueueFirebaseWrite(int index, bool state) {
  if (index < 0 || index >= NUM_SWITCHES)
    return false;

  if (!fbOn || !firebaseWriteQueue)
    return false;

  FirebaseWriteTask task = {(uint8_t)index, state};
  if (xQueueSend(firebaseWriteQueue, &task, 0) == pdTRUE)
    return true;

  // Queue full: drop oldest to keep the most recent command.
  FirebaseWriteTask dropped;
  (void)xQueueReceive(firebaseWriteQueue, &dropped, 0);
  return xQueueSend(firebaseWriteQueue, &task, 0) == pdTRUE;
}

void restartFirebaseStream(const char *reason) {
  if (firebaseStreamRunning) {
    fbStreamClient.stopAsync(FIREBASE_STREAM_TASK_UID);
  }

  firebaseStreamRunning = false;
  firebaseLastStreamAttemptMs = 0;
  firebaseLastStreamEventMs = 0;
  firebaseAwaitingInitialStreamData = false;
  firebaseStreamStartedAtMs = 0;

  if (reason && reason[0] != '\0') {
    Firebase.printf("[FB] Stream restart requested: %s\n", reason);
  }
}

bool tryParseBoolText(String text, bool &out) {
  text.trim();
  text.toLowerCase();
  if (text == "true" || text == "1") {
    out = true;
    return true;
  }
  if (text == "false" || text == "0") {
    out = false;
    return true;
  }
  return false;
}

bool tryParseBoolVariant(const JsonVariantConst &value, bool &out) {
  if (value.is<bool>()) {
    out = value.as<bool>();
    return true;
  }

  if (value.is<int>()) {
    out = value.as<int>() != 0;
    return true;
  }

  if (value.is<long>()) {
    out = value.as<long>() != 0;
    return true;
  }

  if (value.is<double>()) {
    out = value.as<double>() != 0.0;
    return true;
  }

  if (value.is<const char *>()) {
    String text = value.as<String>();
    return tryParseBoolText(text, out);
  }

  return false;
}

bool getPrimaryFirebaseAuthCredential(String &email, String &password) {
  JsonDocument doc;
  loadFirebaseAuthUsersDoc(doc);
  JsonArray users = doc.as<JsonArray>();
  if (users.isNull() || users.size() == 0)
    return false;

  for (JsonObject user : users) {
    String candidateEmail = normalizeEmail(user["email"].as<String>());
    String candidatePassword = user["password"].as<String>();
    candidatePassword.trim();
    if (candidateEmail.isEmpty() || candidatePassword.isEmpty())
      continue;

    email = candidateEmail;
    password = candidatePassword;
    return true;
  }

  return false;
}

void stopFirebaseRuntime() {
  fbStreamClient.stopAsync(FIREBASE_STREAM_TASK_UID);
  fbClient.stopAsync(true);
  fbDatabase.resetApp();
  deinitializeApp(fbApp);

  if (fbUserAuth) {
    delete fbUserAuth;
    fbUserAuth = nullptr;
  }

  firebaseRuntimeInitialized = false;
  firebaseStreamRunning = false;
  firebaseAuthUid = "";
  firebaseStreamPath = "";
  firebaseLastStreamEventMs = 0;
  firebaseLastWriteMs = 0;
  firebaseHasDeferredWriteTask = false;
  firebaseDeferredWriteAtMs = 0;
  firebaseAwaitingInitialStreamData = false;
  firebaseStreamStartedAtMs = 0;
  firebaseLastSyncedDeviceName = "";
  firebaseDeviceNameSyncDirty = true;
  clearFirebaseWriteQueue();
}

bool firebaseSetBoolAtPath(const String &path, bool value) {
  if (!firebaseRuntimeInitialized || !fbApp.ready())
    return false;

  if (firebaseAuthUid.isEmpty())
    return false;

  // Equivalent behavior to Firebase.RTDB.setBool() for FirebaseClient.h.
  bool ok = fbDatabase.set<bool>(fbClient, path, value);
  if (!ok) {
    Firebase.printf("[FB] set<bool> failed path=%s code=%d msg=%s\n",
                    path.c_str(), fbClient.lastError().code(),
                    fbClient.lastError().message().c_str());
  }
  return ok;
}

bool firebaseSetStringAtPath(const String &path, const String &value) {
  if (!firebaseRuntimeInitialized || !fbApp.ready())
    return false;

  if (firebaseAuthUid.isEmpty())
    return false;

  bool ok = fbDatabase.set<String>(fbClient, path, value);
  if (!ok) {
    Firebase.printf("[FB] set<String> failed path=%s code=%d msg=%s\n",
                    path.c_str(), fbClient.lastError().code(),
                    fbClient.lastError().message().c_str());
  }
  return ok;
}

void processFirebaseWriteQueue() {
  if (!firebaseWriteQueue)
    return;

  if (!fbOn || !firebaseRuntimeInitialized || !fbApp.ready() ||
      firebaseAuthUid.isEmpty())
    return;

  if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_FIREBASE_WRITE) {
    return; // skip TLS write under memory pressure; retry next cycle
  }

  unsigned long now = millis();

  if (firebaseHasDeferredWriteTask) {
    if (now - firebaseDeferredWriteAtMs < FIREBASE_WRITE_FAIL_RETRY_MS) {
      return;
    }

    // Yield before retrying a previously failed write so Core 0 can service
    // background work.
    yield();

    if (xQueueSend(firebaseWriteQueue, &firebaseDeferredWriteTask, 0) !=
        pdTRUE) {
      FirebaseWriteTask dropped;
      (void)xQueueReceive(firebaseWriteQueue, &dropped, 0);
      (void)xQueueSend(firebaseWriteQueue, &firebaseDeferredWriteTask, 0);
    }

    firebaseHasDeferredWriteTask = false;
    firebaseDeferredWriteAtMs = now;
    return;
  }

  if (now - firebaseLastWriteMs < FIREBASE_WRITE_INTERVAL_MS)
    return;

  FirebaseWriteTask task;
  if (xQueueReceive(firebaseWriteQueue, &task, 0) != pdTRUE)
    return;

  String path = firebaseRelayPath(firebaseAuthUid, task.index);
  if (path.isEmpty())
    return;

  Serial.println("[FB] WRITE PATH: " + path);
  Serial.println("[FB] UID: " + firebaseAuthUid);
  Serial.println("[FB] DEVICE ID: " + firebaseDeviceId());

  unsigned long writeStartedAt = millis();
  bool ok = firebaseSetBoolAtPath(path, task.state);
  unsigned long writeDurationMs = millis() - writeStartedAt;
  firebaseLastWriteMs = millis();

  if (writeDurationMs >= FIREBASE_WRITE_LONG_OP_MS) {
    yield();
  }

  if (!ok) {
    firebaseDeferredWriteTask = task;
    firebaseHasDeferredWriteTask = true;
    firebaseDeferredWriteAtMs = now;
    Serial.println("[FB] Write failed; deferred retry scheduled");
  }
}

bool writeRelayStateToFirebase(int index, bool state) {
  return enqueueFirebaseWrite(index, state);
}

void setRelayState(int index, bool state, const String &source) {
  if (index < 0 || index >= NUM_SWITCHES)
    return;

  String normalizedSource = source;
  normalizedSource.trim();
  normalizedSource.toLowerCase();

  bool changed = false;
  if (lockSwState()) {
    if (swState[index] != state) {
      swState[index] = state;
      changed = true;
    }
    unlockSwState();
  }

  if (!changed)
    return;

  digitalWrite(outPin[index], state ? HIGH : LOW);

  // DEFERRED FLASH WRITE: Just set the flag. The loop() will safely write it to flash.
  if (relayMode[index] == RELAY_MODE_REMEMBER) {
    nvsRelayDirty[index] = true;
  }

  bool firebaseSource = (normalizedSource == "firebase");
  bool webSource = (normalizedSource == "web" || normalizedSource == "api");
  bool manualSource = (normalizedSource == "manual" || normalizedSource == "physical" ||
                       normalizedSource == "local" || normalizedSource == "automation");

  if (firebaseSource) {
    syncRelayStateToWebView(index, state);
    return;
  }
  if (webSource) {
    enqueueFirebaseWrite(index, state);
    return;
  }
  if (manualSource) {
    syncRelayStateToWebView(index, state);
    enqueueFirebaseWrite(index, state);
    return;
  }

  enqueueFirebaseWrite(index, state);
}

void setSwitch(int i, bool st) { setRelayState(i, st, "local"); }

int relayIndexFromFirebasePath(const String &dataPath) {
  if (!dataPath.startsWith("/switch"))
    return -1;

  String idxText = dataPath.substring(7);
  if (idxText.isEmpty())
    return -1;

  for (size_t i = 0; i < idxText.length(); i++) {
    char c = idxText.charAt(i);
    if (c < '0' || c > '9')
      return -1;
  }

  int oneBased = idxText.toInt();
  if (oneBased < 1 || oneBased > NUM_SWITCHES)
    return -1;

  return oneBased - 1;
}

void applyFirebaseSnapshotPayload(const String &payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    doc.clear();
    return;
  }

  JsonObject obj = doc.as<JsonObject>();
  if (obj.isNull()) {
    doc.clear();
    return;
  }

  for (int i = 0; i < NUM_SWITCHES; i++) {
    String key = "switch" + String(i + 1);
    if (!obj.containsKey(key))
      continue;

    bool state = false;
    if (tryParseBoolVariant(obj[key], state)) {
      setRelayState(i, state, "firebase");
    }
  }

  doc.clear();
}

void applyFirebasePathPayload(const String &dataPath, const String &payload) {
  if (dataPath.length() == 0) {
    return;
  }

  if (dataPath == "/") {
    applyFirebaseSnapshotPayload(payload);
    return;
  }

  int index = relayIndexFromFirebasePath(dataPath);
  if (index < 0)
    return;

  bool state = false;
  if (tryParseBoolText(payload, state)) {
    setRelayState(index, state, "firebase");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    doc.clear();
    return;
  }

  JsonVariant root = doc.as<JsonVariant>();
  if (tryParseBoolVariant(root, state)) {
    setRelayState(index, state, "firebase");
    doc.clear();
    return;
  }

  JsonObject obj = root.as<JsonObject>();
  if (obj.isNull()) {
    doc.clear();
    return;
  }

  if (obj.containsKey("state") && tryParseBoolVariant(obj["state"], state)) {
    setRelayState(index, state, "firebase");
    doc.clear();
    return;
  }

  String directKey = "switch" + String(index + 1);
  if (obj.containsKey(directKey) &&
      tryParseBoolVariant(obj[directKey], state)) {
    setRelayState(index, state, "firebase");
  }

  doc.clear();
}

bool beginFirebaseRuntime() {
  String databaseUrl = fbUrl;
  String apiKey = fbToken;
  databaseUrl.trim();
  apiKey.trim();

  if (databaseUrl.isEmpty() || apiKey.isEmpty())
    return false;

  String authEmail;
  String authPassword;
  if (!getPrimaryFirebaseAuthCredential(authEmail, authPassword))
    return false;

  if (fbUserAuth) {
    delete fbUserAuth;
    fbUserAuth = nullptr;
  }
  fbUserAuth = new UserAuth(apiKey, authEmail, authPassword, 3000);

  initializeApp(fbClient, fbApp, getAuth(*fbUserAuth), firebaseAsyncCallback,
                FIREBASE_AUTH_TASK_UID);
  fbApp.getApp<RealtimeDatabase>(fbDatabase);
  fbDatabase.url(databaseUrl);

  firebaseRuntimeInitialized = true;
  firebaseStreamRunning = false;
  firebaseAuthUid = "";
  firebaseStreamPath = "";
  return true;
}

void ensureFirebaseStreamConnected() {
  if (!firebaseRuntimeInitialized || !fbApp.ready())
    return;

  if (firebaseAuthUid.isEmpty() || firebaseStreamPath.isEmpty())
    return;

  if (firebaseStreamRunning)
    return;

  if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_FIREBASE_WRITE)
    return; // defer stream reconnect until heap recovers

  unsigned long now = millis();
  if (now - firebaseLastStreamAttemptMs < FIREBASE_STREAM_RETRY_MS)
    return;

  firebaseLastStreamAttemptMs = now;
  fbStreamClient.setSSEFilters("get,put,patch,keep-alive,cancel,auth_revoked");
  fbDatabase.get(fbStreamClient, firebaseStreamPath, firebaseAsyncCallback,
                 true, FIREBASE_STREAM_TASK_UID);
  firebaseStreamRunning = true;
  firebaseLastStreamEventMs = now;
  firebaseAwaitingInitialStreamData = true;
  firebaseStreamStartedAtMs = now;
  Serial.println("[FB] Stream start requested");
  Serial.println("[FB] UID: " + firebaseAuthUid);
  Serial.println("[FB] DEVICE ID: " + firebaseDeviceId());
  Serial.println("[FB] PATH: " + firebaseStreamPath);
}

void firebaseAsyncCallback(AsyncResult &aResult) {
  if (!aResult.isResult())
    return;

  String taskId = aResult.uid();
  if (aResult.isError()) {
    Firebase.printf("[FB] Error task=%s msg=%s code=%d\n", taskId.c_str(),
                    aResult.error().message().c_str(), aResult.error().code());
    if (taskId == FIREBASE_AUTH_TASK_UID) {
      firebaseRuntimeInitialized = false;
      firebaseRuntimeDirty = true;
      firebaseLastInitAttemptMs = millis();
      return;
    }
    if (taskId == FIREBASE_STREAM_TASK_UID) {
      restartFirebaseStream("stream task error");
      firebaseLastStreamAttemptMs = millis();
      return;
    }
  }

  if (!aResult.available())
    return;

  RealtimeDatabaseResult &stream = aResult.to<RealtimeDatabaseResult>();
  if (!stream.isStream())
    return;

  firebaseLastStreamEventMs = millis();

  String eventType = stream.event().c_str();
  eventType.toLowerCase();
  if (eventType == "cancel" || eventType == "auth_revoked") {
    restartFirebaseStream(eventType.c_str());
    firebaseLastStreamAttemptMs = millis();
    return;
  }

  if (eventType == "keep-alive") {
    firebaseStreamRunning = true;
    return;
  }

  if (eventType != "get" && eventType != "put" && eventType != "patch") {
    return;
  }

  firebaseStreamRunning = true;

  String dataPath = stream.dataPath().c_str();
  dataPath.trim();
  if (dataPath.isEmpty()) {
    return;
  }

  String payload = stream.to<String>();
  if (payload.isEmpty()) {
    Serial.println("[FB] Empty payload received at: " + dataPath);
  }

  if (dataPath == "/" &&
      (payload.isEmpty() || payload == "null" || payload == "{}")) {
    firebaseAwaitingInitialStreamData = false;
    Serial.println("[FB] No remote relay snapshot found, pushing local state");
    queueAllRelayStatesForFirebaseSync();
    return;
  }

  firebaseAwaitingInitialStreamData = false;
  applyFirebasePathPayload(dataPath, payload);
}

void handleFirebaseRuntime() {
  if (!fbOn) {
    if (firebaseRuntimeInitialized || firebaseStreamRunning ||
        !firebaseAuthUid.isEmpty()) {
      stopFirebaseRuntime();
    } else {
      clearFirebaseWriteQueue();
    }
    return;
  }

  if (firebaseRuntimeDirty) {
    stopFirebaseRuntime();
    firebaseRuntimeDirty = false;
    firebaseLastInitAttemptMs = 0;
    firebaseLastStreamAttemptMs = 0;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (firebaseStreamRunning) {
      restartFirebaseStream("wifi disconnected");
    }
    return;
  }

  if (!firebaseRuntimeInitialized) {
    unsigned long now = millis();
    if (now - firebaseLastInitAttemptMs >= FIREBASE_INIT_RETRY_MS) {
      firebaseLastInitAttemptMs = now;
      if (ESP.getFreeHeap() >= MIN_FREE_HEAP_FOR_FIREBASE_WRITE)
        beginFirebaseRuntime();
    }
    return;
  }

  if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_FIREBASE_LOOP)
    return; // skip fbApp.loop() — ResponseContext::newBuf() would malloc(512) → NULL crash

  fbApp.loop();

  if (!fbApp.ready())
    return;

  String uid = fbApp.getUid();
  uid.trim();
  if (uid.isEmpty())
    return;

  if (uid != firebaseAuthUid) {
    firebaseAuthUid = uid;
    firebaseStreamPath = firebaseBasePathForUid(firebaseAuthUid);

    restartFirebaseStream("UID updated");

    clearFirebaseWriteQueue();
    queueAllRelayStatesForFirebaseSync();
    firebaseDeviceNameSyncDirty = true;

    Serial.println("[FB] UID READY: " + firebaseAuthUid);
    Serial.println("[FB] DEVICE ID: " + firebaseDeviceId());
    Serial.println("[FB] PATH: " + firebaseStreamPath);
  }

  String expectedStreamPath = firebaseBasePathForUid(firebaseAuthUid);
  if (!expectedStreamPath.isEmpty() &&
      expectedStreamPath != firebaseStreamPath) {
    firebaseStreamPath = expectedStreamPath;
    restartFirebaseStream("base path changed");
    clearFirebaseWriteQueue();
    firebaseDeviceNameSyncDirty = true;
    queueAllRelayStatesForFirebaseSync();
  }

  String currentDeviceName = firebaseDeviceNameValue();
  if (firebaseDeviceNameSyncDirty ||
      currentDeviceName != firebaseLastSyncedDeviceName) {
    String namePath = firebaseDeviceNamePath(firebaseAuthUid);
    if (!namePath.isEmpty() &&
        firebaseSetStringAtPath(namePath, currentDeviceName)) {
      firebaseLastSyncedDeviceName = currentDeviceName;
      firebaseDeviceNameSyncDirty = false;
    }
  }

  if (firebaseStreamRunning &&
      (millis() - firebaseLastStreamEventMs > FIREBASE_STREAM_STALE_MS)) {
    restartFirebaseStream("stream stale");
  }

  if (firebaseAwaitingInitialStreamData && firebaseStreamStartedAtMs > 0 &&
      (millis() - firebaseStreamStartedAtMs > FIREBASE_INITIAL_SYNC_WAIT_MS)) {
    firebaseAwaitingInitialStreamData = false;
    Serial.println("[FB] Initial stream wait timed out, pushing local state");
    queueAllRelayStatesForFirebaseSync();
  }

  ensureFirebaseStreamConnected();
}

void firebaseTask(void *param) {
  (void)param;
  for (;;) {
    handleFirebaseRuntime();
    yield();
    processFirebaseWriteQueue();
    vTaskDelay(FIREBASE_TASK_DELAY_TICKS);
  }
}

void startFirebaseTask() {
  if (firebaseTaskHandle)
    return;

  BaseType_t created = xTaskCreatePinnedToCore(
      firebaseTask, "FirebaseTask", 16384, nullptr, 1, &firebaseTaskHandle, 0);

  if (created == pdPASS) {
    Serial.println("[FB] Firebase task started on Core 0");
  } else {
    firebaseTaskHandle = nullptr;
    Serial.println("[FB] Failed to start Firebase task");
  }
}

bool readCurrentLocalTime(struct tm *ti) {
  if (!ti)
    return false;

  time_t nowEpoch = time(nullptr);
  if (nowEpoch <= 0)
    return false;

  localtime_r(&nowEpoch, ti);
  int year = ti->tm_year + 1900;
  if (year < 2020)
    return false;

  return true;
}

void sntpTimeAvailableCallback(struct timeval *t) {
  Serial.println("[TIME] SNTP sync completed successfully in background!");
  timeSynced = true;
  calcSunriseSunset(); // Auto-update sun times when time arrives
}

// SYNC TIME
bool syncTime(struct tm *syncedTime = nullptr) {
  Serial.printf("[TIME] Requesting SNTP sync. WiFi=%s\n",
                WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");

  if (WiFi.status() != WL_CONNECTED)
    return false;

  loadTimeSettingsFromAdminStorage();
  ntpSrv.trim();
  if (ntpSrv.isEmpty())
    ntpSrv = "pool.ntp.org";

  // Bind the background callback
  sntp_set_time_sync_notification_cb(sntpTimeAvailableCallback);

  // This triggers the ESP32 to fetch time asynchronously
  configTime(gmtOff, 0, ntpSrv.c_str(), "pool.ntp.org", "time.nist.gov");

  Serial.println("[TIME] SNTP configured. Syncing in background...");
  return true;
}

//  SUNRISE & SUNSET
void calcSunriseSunset() {
  if (!locOk || !timeSynced)
    return;

  struct tm ti;
  if (!readCurrentLocalTime(&ti))
    return;

  int day = ti.tm_mday;
  if (day == lastCalcDay && srMin >= 0 && srMin <= 1439 && ssMin >= 0 &&
      ssMin <= 1439)
    return;

  int tzH = gmtOff / 3600;
  int tzM = (abs(gmtOff) % 3600) / 60;
  Dusk2Dawn loc(geoLat, geoLon, tzH);

  int sunriseMinute = loc.sunrise(ti.tm_year + 1900, ti.tm_mon + 1, day, false);
  int sunsetMinute = loc.sunset(ti.tm_year + 1900, ti.tm_mon + 1, day, false);

  if (sunriseMinute < 0 || sunsetMinute < 0)
    return;

  sunriseMinute += tzM;
  sunsetMinute += tzM;

  while (sunriseMinute < 0)
    sunriseMinute += 1440;
  while (sunriseMinute > 1439)
    sunriseMinute -= 1440;
  while (sunsetMinute < 0)
    sunsetMinute += 1440;
  while (sunsetMinute > 1439)
    sunsetMinute -= 1440;

  srMin = sunriseMinute;
  ssMin = sunsetMinute;
  lastCalcDay = day;
  Serial.printf("[SUN] Rise=%02d:%02d  Set=%02d:%02d\n", srMin / 60, srMin % 60,
                ssMin / 60, ssMin % 60);
}

//  TIMER CHECK (loop)
void checkTimers() {
  unsigned long now = millis();
  for (int i = 0; i < NUM_SWITCHES; i++) {
    if (swTimers[i].active && !swTimers[i].paused && now >= swTimers[i].endMs) {
      queueAutomationAction(i, swTimers[i].targetState, AUTOMATION_TIMER);
      swTimers[i].active = false;
      swTimers[i].paused = false;
      swTimers[i].remainingMs = 0;
      Serial.printf("[TIMER] SW%d -> %s\n", i,
                    swTimers[i].targetState ? "ON" : "OFF");
    }
  }
}

int parseClockMinutes(const String &timeText) {
  if (timeText.length() < 5 || timeText.charAt(2) != ':')
    return -1;

  int hours = timeText.substring(0, 2).toInt();
  int mins = timeText.substring(3, 5).toInt();
  if (hours < 0 || hours > 23 || mins < 0 || mins > 59)
    return -1;

  return hours * 60 + mins;
}

String formatClockFromMinutes(int totalMinutes) {
  if (totalMinutes < 0 || totalMinutes > 1439)
    return "";

  char buf[6];
  sprintf(buf, "%02d:%02d", totalMinutes / 60, totalMinutes % 60);
  return String(buf);
}

int weekdayIndexFromText(String dayText) {
  dayText.trim();
  dayText.toLowerCase();
  if (dayText.length() >= 3) {
    dayText = dayText.substring(0, 3);
  }

  const char *days[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
  for (int i = 0; i < 7; i++) {
    if (dayText == days[i])
      return i;
  }

  return -1;
}

String getScheduleActionValue(JsonObject obj, const char *defaultValue = "on") {
  String action = obj["action"].as<String>();
  if (action != "on" && action != "off")
    action = defaultValue;
  return action;
}

String getRecurringStartTime(JsonObject obj) {
  String timeText = obj["fromTime"].as<String>();
  if (timeText.isEmpty())
    timeText = obj["onTime"].as<String>();
  return timeText;
}

String getRecurringEndTime(JsonObject obj) {
  String timeText = obj["toTime"].as<String>();
  if (timeText.isEmpty())
    timeText = obj["offTime"].as<String>();
  return timeText;
}

String getFutureStartTime(JsonObject obj) {
  String timeText = obj["fromTime"].as<String>();
  if (timeText.isEmpty())
    timeText = obj["time"].as<String>();
  return timeText;
}

String getFutureEndTime(JsonObject obj) { return obj["toTime"].as<String>(); }

bool rangesOverlapMinutes(int startA, int endA, int startB, int endB) {
  return startA < endB && endA > startB;
}

bool scheduleDaysOverlap(JsonArray daysA, JsonArray daysB) {
  for (JsonVariant dayA : daysA) {
    String dayText = dayA.as<String>();
    for (JsonVariant dayB : daysB) {
      if (dayText == dayB.as<String>())
        return true;
    }
  }
  return false;
}

bool validateRecurringSchedulesData(const String &data, String &error) {
  JsonDocument doc;
  if (deserializeJson(doc, data)) {
    error = "Invalid schedule data";
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) {
    error = "Schedule data must be an array";
    return false;
  }

  for (int i = 0; i < arr.size(); i++) {
    JsonObject entry = arr[i].as<JsonObject>();
    if (!entry["enabled"].as<bool>())
      continue;

    String fromTime = getRecurringStartTime(entry);
    String toTime = getRecurringEndTime(entry);
    int fromMin = parseClockMinutes(fromTime);
    int toMin = parseClockMinutes(toTime);
    JsonArray days = entry["days"].as<JsonArray>();

    if (fromMin < 0 || toMin < 0) {
      error = "Each enabled schedule needs valid From and To times";
      return false;
    }
    if (toMin <= fromMin) {
      error = "Each enabled schedule needs a Time Range where To is after From";
      return false;
    }
    if (days.isNull() || days.size() == 0) {
      error = "Each enabled schedule needs at least one repeat day";
      return false;
    }
  }

  for (int i = 0; i < arr.size(); i++) {
    JsonObject entryA = arr[i].as<JsonObject>();
    if (!entryA["enabled"].as<bool>())
      continue;

    int startA = parseClockMinutes(getRecurringStartTime(entryA));
    int endA = parseClockMinutes(getRecurringEndTime(entryA));
    JsonArray daysA = entryA["days"].as<JsonArray>();

    for (int j = i + 1; j < arr.size(); j++) {
      JsonObject entryB = arr[j].as<JsonObject>();
      if (!entryB["enabled"].as<bool>())
        continue;

      int startB = parseClockMinutes(getRecurringStartTime(entryB));
      int endB = parseClockMinutes(getRecurringEndTime(entryB));
      JsonArray daysB = entryB["days"].as<JsonArray>();

      if (getScheduleActionValue(entryA, "on") ==
              getScheduleActionValue(entryB, "on") &&
          scheduleDaysOverlap(daysA, daysB) &&
          rangesOverlapMinutes(startA, endA, startB, endB)) {
        error =
            "Schedules with the same action cannot overlap on the same repeat "
            "days";
        return false;
      }
    }
  }

  return true;
}

bool validateFutureSchedulesData(const String &data, String &error) {
  JsonDocument doc;
  if (deserializeJson(doc, data)) {
    error = "Invalid future schedule data";
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) {
    error = "Future schedule data must be an array";
    return false;
  }

  for (int i = 0; i < arr.size(); i++) {
    JsonObject entry = arr[i].as<JsonObject>();
    if (!entry["enabled"].as<bool>())
      continue;

    String date = entry["date"].as<String>();
    String fromTime = getFutureStartTime(entry);
    String toTime = getFutureEndTime(entry);
    bool legacyOneShot = !fromTime.isEmpty() && toTime.isEmpty() &&
                         entry["fromTime"].as<String>().isEmpty();
    int fromMin = parseClockMinutes(fromTime);

    if (date.length() < 10) {
      error = "Each enabled future schedule needs a valid date";
      return false;
    }
    if (fromMin < 0) {
      error = "Each enabled future schedule needs a valid From time";
      return false;
    }
    if (!legacyOneShot) {
      int toMin = parseClockMinutes(toTime);
      if (toMin < 0) {
        error = "Each enabled future schedule needs a valid To time";
        return false;
      }
      if (toMin <= fromMin) {
        error =
            "Each enabled future schedule needs a Time Range where To is after "
            "From";
        return false;
      }
    }
  }

  for (int i = 0; i < arr.size(); i++) {
    JsonObject entryA = arr[i].as<JsonObject>();
    if (!entryA["enabled"].as<bool>())
      continue;

    String dateA = entryA["date"].as<String>();
    int startA = parseClockMinutes(getFutureStartTime(entryA));
    String endTimeA = getFutureEndTime(entryA);
    int endA = endTimeA.isEmpty() ? startA + 1 : parseClockMinutes(endTimeA);

    for (int j = i + 1; j < arr.size(); j++) {
      JsonObject entryB = arr[j].as<JsonObject>();
      if (!entryB["enabled"].as<bool>())
        continue;

      if (dateA != entryB["date"].as<String>())
        continue;

      int startB = parseClockMinutes(getFutureStartTime(entryB));
      String endTimeB = getFutureEndTime(entryB);
      int endB = endTimeB.isEmpty() ? startB + 1 : parseClockMinutes(endTimeB);

      if (getScheduleActionValue(entryA, "on") ==
              getScheduleActionValue(entryB, "on") &&
          rangesOverlapMinutes(startA, endA, startB, endB)) {
        error =
            "Future schedules with the same action cannot overlap on the same "
            "date";
        return false;
      }
    }
  }

  return true;
}

//  SCHEDULE CHECK (loop)
void checkSchedules() {
  if (!timeSynced)
    return;
  struct tm ti;
  if (!getLocalTime(&ti))
    return;

  int curMin = ti.tm_hour * 60 + ti.tm_min;
  if (curMin == lastCheckedMinute)
    return;
  lastCheckedMinute = curMin;

  const char *dn[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
  String today = dn[ti.tm_wday];

  for (int sw = 0; sw < NUM_SWITCHES; sw++) {
    // Regular schedules
    prefs.begin("sched", true);
    String sj = prefs.getString(("s" + String(sw)).c_str(), "[]");
    prefs.end();

    {
      JsonDocument sd;
      if (deserializeJson(sd, sj)) {
        sd.clear();
        continue;
      }

      for (JsonObject o : sd.as<JsonArray>()) {
        if (!o["enabled"].as<bool>())
          continue;
        bool dayOk = false;
        for (JsonVariant dv : o["days"].as<JsonArray>()) {
          if (dv.as<String>() == today) {
            dayOk = true;
            break;
          }
        }
        if (!dayOk)
          continue;

        String action = getScheduleActionValue(o, "on");
        String fromT = getRecurringStartTime(o);
        int fromMin = parseClockMinutes(fromT);
        if (fromMin >= 0 && curMin == fromMin) {
          queueAutomationAction(sw, action == "on", AUTOMATION_SCHEDULE);
        }

        String toT = getRecurringEndTime(o);
        int toMin = parseClockMinutes(toT);
        if (toMin >= 0 && curMin == toMin) {
          queueAutomationAction(sw, action != "on", AUTOMATION_SCHEDULE);
        }
      }

      sd.clear();
    }

    // Future schedules
    prefs.begin("fsched", false);
    String fj = prefs.getString(("f" + String(sw)).c_str(), "[]");

    {
      JsonDocument fd;
      if (deserializeJson(fd, fj)) {
        fd.clear();
        prefs.end();
        continue;
      }

      char ds[11];
      sprintf(ds, "%04d-%02d-%02d", ti.tm_year + 1900, ti.tm_mon + 1,
              ti.tm_mday);
      JsonArray fa = fd.as<JsonArray>();
      bool mod = false;
      for (int x = fa.size() - 1; x >= 0; x--) {
        JsonObject fo = fa[x];
        if (!fo["enabled"].as<bool>())
          continue;

        if (fo["date"].as<String>() != String(ds))
          continue;

        String action = getScheduleActionValue(fo, "on");
        String fromT = getFutureStartTime(fo);
        String toT = getFutureEndTime(fo);
        int fromMin = parseClockMinutes(fromT);
        int toMin = parseClockMinutes(toT);
        bool removeEntry = false;

        if (fromMin >= 0 && curMin == fromMin) {
          queueAutomationAction(sw, action == "on", AUTOMATION_FUTURE_SCHEDULE);
          if (toMin < 0) {
            removeEntry = true;
          }
        }

        if (toMin >= 0 && curMin == toMin) {
          queueAutomationAction(sw, action != "on", AUTOMATION_FUTURE_SCHEDULE);
          removeEntry = true;
        }

        if (removeEntry) {
          fa.remove(x);
          mod = true;
        }
      }

      if (mod) {
        String nj;
        serializeJson(fd, nj);
        prefs.putString(("f" + String(sw)).c_str(), nj);
        notifyStorage();
      }

      fd.clear();
    }

    prefs.end();
  }
}

void checkRestartSchedule() {
  if (!restartScheduleEnabled || restartScheduleMinute < 0 ||
      restartScheduleMinute > 1439 || restartScheduleDayMask == 0)
    return;
  if (!timeSynced)
    return;

  struct tm ti;
  if (!getLocalTime(&ti))
    return;
  if (ti.tm_wday < 0 || ti.tm_wday > 6)
    return;
  if ((restartScheduleDayMask & (1 << ti.tm_wday)) == 0)
    return;

  int curMin = ti.tm_hour * 60 + ti.tm_min;
  if (curMin != restartScheduleMinute)
    return;

  char slotBuf[20];
  sprintf(slotBuf, "%04d-%02d-%02d-%02d:%02d", ti.tm_year + 1900, ti.tm_mon + 1,
          ti.tm_mday, ti.tm_hour, ti.tm_min);
  String currentSlotKey = String(slotBuf);
  if (restartScheduleLastRunKey == currentSlotKey)
    return;

  long stamp =
      (long)(ti.tm_year + 1900) * 1000000L + (long)ti.tm_yday * 1440L + curMin;
  if (stamp == lastRestartScheduleStamp)
    return;

  lastRestartScheduleStamp = stamp;
  restartScheduleLastRunKey = currentSlotKey;
  prefs.begin("admin", false);
  prefs.putString("rsLast", restartScheduleLastRunKey);
  prefs.end();
  Serial.printf("[RESTART] Scheduled restart at %02d:%02d (wday=%d)\n",
                ti.tm_hour, ti.tm_min, ti.tm_wday);
  restartFlag = true;
  restartAt = millis() + 500;
}

//  SENSOR AUTOMATION CHECK (loop)
void checkSensors() {
  float cTemp = 0.0f;
  float cHum = 0.0f;
  if (ahtOk) {
    sensors_event_t hev, tev;
    aht.getEvent(&hev, &tev);
    cTemp = tev.temperature;
    cHum = hev.relative_humidity;
  }

  for (int sw = 0; sw < NUM_SWITCHES; sw++) {
    prefs.begin("sensor", true);
    String tj = prefs.getString(("t" + String(sw)).c_str(), "");
    String hj = prefs.getString(("h" + String(sw)).c_str(), "");
    String sj = prefs.getString(("x" + String(sw)).c_str(), "");
    prefs.end();

    // Temperature
    if (ahtOk && !tj.isEmpty()) {
      JsonDocument d;
      if (!deserializeJson(d, tj) && d["enabled"].as<bool>()) {
        String c = d["condition"].as<String>();
        bool act = false;
        if (c == "below")
          act = cTemp < d["value"].as<float>();
        else if (c == "above")
          act = cTemp > d["value"].as<float>();
        else if (c == "equal")
          act = abs(cTemp - d["value"].as<float>()) < 0.5;
        else if (c == "between")
          act = cTemp >= d["min"].as<float>() && cTemp <= d["max"].as<float>();
        if (act)
          queueAutomationAction(sw, d["action"].as<String>() == "on",
                                AUTOMATION_TEMPERATURE);
      }
    }
    // Humidity
    if (ahtOk && !hj.isEmpty()) {
      JsonDocument d;
      if (!deserializeJson(d, hj) && d["enabled"].as<bool>()) {
        String c = d["condition"].as<String>();
        bool act = false;
        if (c == "below")
          act = cHum < d["value"].as<float>();
        else if (c == "above")
          act = cHum > d["value"].as<float>();
        else if (c == "equal")
          act = abs(cHum - d["value"].as<float>()) < 0.5;
        else if (c == "between")
          act = cHum >= d["min"].as<float>() && cHum <= d["max"].as<float>();
        if (act)
          queueAutomationAction(sw, d["action"].as<String>() == "on",
                                AUTOMATION_HUMIDITY);
      }
    }
    // Sunrise/Sunset
    if (!sj.isEmpty() && locOk && timeSynced) {
      JsonDocument d;
      if (!deserializeJson(d, sj) && d["enabled"].as<bool>()) {
        struct tm ti;
        if (getLocalTime(&ti)) {
          int now = ti.tm_hour * 60 + ti.tm_min;
          int off = d["offset"].as<int>();
          String c = d["condition"].as<String>();
          bool act = false;
          if (c == "after_sunset")
            act = now >= ssMin + off;
          else if (c == "before_sunset")
            act = now <= ssMin + off;
          else if (c == "after_sunrise")
            act = now >= srMin + off;
          else if (c == "before_sunrise")
            act = now <= srMin + off;
          else if (c == "between_sunset_sunrise")
            act = now >= ssMin + off || now <= srMin + off;
          else if (c == "between_sunrise_sunset")
            act = now >= srMin + off && now <= ssMin + off;
          if (act)
            queueAutomationAction(sw, d["action"].as<String>() == "on",
                                  AUTOMATION_SUN);
        }
      }
    }
  }
}

//  WiFi CONNECTION
// void connectWiFi() {
//   WiFi.mode(WIFI_STA);
//   if (!dhcpOn && sIp.length() > 0) {
//     IPAddress ip, gw, sn, dns;
//     ip.fromString(sIp);
//     gw.fromString(sGw);
//     sn.fromString(sMask);
//     dns.fromString(sDns);
//     WiFi.config(ip, gw, sn, dns);
//   }

//   // Try saved credentials first
//   prefs.begin("wfcfg", true);
//   String savedSsid = prefs.getString("ssid", "");
//   String savedPass = prefs.getString("pass", "");
//   prefs.end();

//   if (savedSsid.length() > 0) {
//     WiFi.begin(savedSsid.c_str(), savedPass.c_str());
//     Serial.print("[WIFI] Connecting to " + savedSsid);
//   } else {
//     WiFi.begin();
//     Serial.print("[WIFI] Connecting");
//   }

//   for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
//     delay(500);
//     Serial.print(".");
//   }
//   Serial.println();

//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.printf("[WIFI] Connected: %s  IP: %s\n", WiFi.SSID().c_str(),
//     WiFi.localIP().toString().c_str());
//   } else {
//     Serial.println("[WIFI] Failed -> starting config portal...");
//     WiFiManager wm;
//     wm.setConfigPortalTimeout(180);
//     wm.setCaptivePortalEnable(true);
//     wm.setAPCallback([](WiFiManager *myWiFiManager) {
//       Serial.println("[WIFI] AP Started - connect to ESP HOME and go to
//       192.168.4.1");
//     });
//     if (!wm.autoConnect("ESP HOME")) {
//       Serial.println("[WIFI] Portal timeout - restarting...");
//       ESP.restart();
//     }
//     if (WiFi.status() == WL_CONNECTED) {
//       Serial.printf("[WIFI] Portal OK: %s\n",
//       WiFi.localIP().toString().c_str());
//       // Save the credentials WiFiManager just connected with
//       prefs.begin("wfcfg", false);
//       prefs.putString("ssid", WiFi.SSID());
//       prefs.putString("pass", WiFi.psk());
//       prefs.end();
//     }
//   }
// }

bool connectToSavedWiFi() {
  String savedSsid;
  String savedPass;

  Serial.println("\n==============================");
  Serial.println("WiFi Connection Started");
  Serial.println("==============================");

  if (!getSavedWifiCredentials(savedSsid, savedPass)) {
    Serial.println("\n[WARNING] No saved WiFi found!");
    Serial.println("[INFO] Starting Config Portal...");
    return runWifiConfigPortal();
  }

  applyStationIpConfig();
  WiFi.disconnect(false, false);
  delay(200);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());

  int attempts = 0;

  while (attempts < WIFI_CONNECT_MAX_ATTEMPTS) {
    char attemptStr[16];
    snprintf(attemptStr, sizeof(attemptStr), "Attempt: %d/%d", attempts + 1,
             WIFI_CONNECT_MAX_ATTEMPTS);
    Serial.printf("[INFO] %s\n", attemptStr);

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[SUCCESS] Connected to Saved WiFi");
      Serial.printf("SSID       : %s\n", WiFi.SSID().c_str());
      Serial.printf("IP Address : %s\n", WiFi.localIP().toString().c_str());
      WiFi.setSleep(false);
      Serial.println("[WIFI] Power save disabled after reconnect");
      Serial.println("==============================\n");
      delay(2000);
      return true;
    }

    delay(WIFI_CONNECT_RETRY_MS);
    attempts++;
  }

  Serial.println("\n[WARNING] Saved WiFi connection failed!");
  Serial.println("[INFO] Starting Config Portal...");
  Serial.println("AP SSID    : ESP HOME");
  Serial.println("AP IP      : 192.168.4.1");
  Serial.println("Timeout    : 180 seconds");
  Serial.println("------------------------------");
  return runWifiConfigPortal();
}

//  PHYSICAL INPUT SWITCH HANDLING
void checkPhysicalSwitches() {
  for (int i = 0; i < NUM_SWITCHES; i++) {
    bool reading = digitalRead(inPin[i]);
    if (reading != lastInState[i])
      lastDbMs[i] = millis();
    if ((millis() - lastDbMs[i]) > DB_DELAY && reading != lastInState[i]) {
      lastInState[i] = reading;
      if (reading == LOW) {
        bool currentState = readSwState(i);
        setRelayState(i, !currentState, "physical");
        bool newState = readSwState(i);
        Serial.printf("[SW] Physical toggle SW%d -> %s\n", i,
                      newState ? "ON" : "OFF");
      }
    }
    lastInState[i] = reading;
  }
}

void sendWebFile(AsyncWebServerRequest *request, const char *path,
                 const char *contentType) {
  // Reject page requests when heap is too low to safely allocate response objects
  if (ESP.getFreeHeap() < 28000) {
    request->send(503, "text/plain", "Low memory");
    return;
  }
  String gzPath = String(path) + ".gz";
  bool noStore = String(contentType) == "text/html";

  if (SPIFFS.exists(gzPath)) {
    Serial.printf("[HTTP] %s -> %s\n", request->url().c_str(), gzPath.c_str());
    AsyncWebServerResponse *response =
        request->beginResponse(SPIFFS, gzPath, contentType);
    response->addHeader("Content-Encoding", "gzip");
    if (noStore) {
      response->addHeader("Cache-Control", "no-store");
    }
    request->send(response);
    return;
  }

  if (SPIFFS.exists(path)) {
    Serial.printf("[HTTP] %s -> %s\n", request->url().c_str(), path);
    AsyncWebServerResponse *response =
        request->beginResponse(SPIFFS, path, contentType);
    if (noStore) {
      response->addHeader("Cache-Control", "no-store");
    }
    request->send(response);
    return;
  }

  Serial.printf("[HTTP] Missing file for %s (expected %s or %s)\n",
                request->url().c_str(), path, gzPath.c_str());
  request->send(404, "text/plain", "File not found");
}

//  WEB SERVER: ALL API ENDPOINTS
void setupWebServer() {
  // CORS
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods",
                                       "GET, POST, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers",
                                       "Content-Type");

  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", "pong");
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    sendWebFile(req, "/index.html", "text/html");
  });

  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    sendWebFile(req, "/index.html", "text/html");
  });

  server.on("/config.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    sendWebFile(req, "/config.html", "text/html");
  });

  server.on("/index.svg", HTTP_GET, [](AsyncWebServerRequest *req) {
    sendWebFile(req, "/index.svg", "image/svg+xml");
  });

  server.on("/settings.svg", HTTP_GET, [](AsyncWebServerRequest *req) {
    sendWebFile(req, "/settings.svg", "image/svg+xml");
  });
  // ---- STATUS ----
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument d;
    d["timeSynced"] = timeSynced;
    d["ahtOk"] = ahtOk;
    d["locOk"] = locOk;
    d["wifiOk"] = (WiFi.status() == WL_CONNECTED);
    d["fbOn"] = fbOn;
    d["mac"] = WiFi.macAddress();
    String r;
    serializeJson(d, r);
    req->send(200, "application/json", r);
  });
  // ---- LOGIN ----
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest *req) {
    bool resetAdminRequested = false;
    if (req->hasParam("resetAdmin", true)) {
      String resetValue = req->getParam("resetAdmin", true)->value();
      resetValue.trim();
      resetValue.toLowerCase();
      resetAdminRequested = (resetValue == "1" || resetValue == "true" ||
                             resetValue == "yes" || resetValue == "on");
    }

    if (resetAdminRequested) {
      if (consumeBootButtonHoldForReset()) {
        Serial.println("[AUTH] Admin reset requested: BOOT hold verified");
        resetAdminToFactoryDefault();
        req->send(200, "application/json",
                  "{\"ok\":true,\"resetAdmin\":true,\"msg\":\"Admin reset to "
                  "default (esp / 456456). Sign in now.\"}");
      } else {
        Serial.println(
            "[AUTH] Admin reset requested: BOOT hold not detected yet");
        req->send(
            400, "application/json",
            "{\"ok\":false,\"resetAdmin\":true,\"error\":\"Hold BOOT button "
            "for 5 seconds first, then press SIGN IN to reset admin.\"}");
      }
      return;
    }

    if (!req->hasParam("user", true) || !req->hasParam("pass", true)) {
      req->send(400, "application/json", "{\"error\":\"Missing credentials\"}");
      return;
    }
    String u = req->getParam("user", true)->value();
    String p = req->getParam("pass", true)->value();
    String role;
    if (verifyLogin(u, p, role))
      req->send(200, "application/json",
                "{\"ok\":true,\"role\":\"" + role + "\"}");
    else
      req->send(401, "application/json", "{\"error\":\"Invalid credentials\"}");
  });
  // ---- SWITCHES ----
  server.on("/api/switches", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument d;
    JsonArray na = d["names"].to<JsonArray>();
    JsonArray ic = d["icons"].to<JsonArray>();
    JsonArray st = d["states"].to<JsonArray>();
    JsonArray rl = d["relays"].to<JsonArray>();
    for (int i = 0; i < NUM_SWITCHES; i++) {
      na.add(swName[i]);
      ic.add(swIcon[i]);
      st.add(readSwState(i));

      // Convert Enum back to String for the Web UI
      if (relayMode[i] == RELAY_MODE_ON)
        rl.add("on");
      else if (relayMode[i] == RELAY_MODE_REMEMBER)
        rl.add("remember");
      else
        rl.add("off");
    }
    String r;
    serializeJson(d, r);
    req->send(200, "application/json", r);
  });

  server.on("/api/switch/toggle", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("index", true) || !req->hasParam("state", true)) {
      req->send(400, "application/json", "{\"error\":\"Missing params\"}");
      return;
    }
    int idx = req->getParam("index", true)->value().toInt();
    bool st = req->getParam("state", true)->value() == "true";
    setRelayState(idx, st, "api");
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/switches/names", HTTP_POST, [](AsyncWebServerRequest *req) {
    prefs.begin("sw", false);
    for (int i = 0; i < NUM_SWITCHES; i++) {
      String k = "name" + String(i);
      if (req->hasParam(k, true)) {
        swName[i] = req->getParam(k, true)->value();
        prefs.putString(("n" + String(i)).c_str(), swName[i]);
      }
    }
    prefs.end();
    notifyStorage();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/switches/icons", HTTP_POST, [](AsyncWebServerRequest *req) {
    prefs.begin("sw", false);
    for (int i = 0; i < NUM_SWITCHES; i++) {
      String k = "icon" + String(i);
      if (req->hasParam(k, true)) {
        swIcon[i] = req->getParam(k, true)->value();
        prefs.putString(("i" + String(i)).c_str(), swIcon[i]);
      }
    }
    prefs.end();
    notifyStorage();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/switches/relay", HTTP_POST, [](AsyncWebServerRequest *req) {
    prefs.begin("sw", false);
    for (int i = 0; i < NUM_SWITCHES; i++) {
      String k = "relay" + String(i);
      if (req->hasParam(k, true)) {
        // Read the string from the web request
        String modeStr = req->getParam(k, true)->value();

        // Save the raw string to NVS preferences
        prefs.putString(("r" + String(i)).c_str(), modeStr);

        // Convert to Enum for the active RAM state
        if (modeStr == "on")
          relayMode[i] = RELAY_MODE_ON;
        else if (modeStr == "remember")
          relayMode[i] = RELAY_MODE_REMEMBER;
        else
          relayMode[i] = RELAY_MODE_OFF;
      }
    }
    prefs.end();
    notifyStorage();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // Keep Firebase auth user APIs available even in low-memory mode.
  server.on("/api/fb/auth/users", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument stored;
    loadFirebaseAuthUsersDoc(stored);

    JsonDocument response;
    JsonArray out = response.to<JsonArray>();
    for (JsonObject user : stored.as<JsonArray>()) {
      JsonObject item = out.createNestedObject();
      item["email"] = user["email"].as<String>();
    }

    String r;
    serializeJson(response, r);
    req->send(200, "application/json", r);
  });

  server.on(
      "/api/fb/auth/users/password", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("email", true)) {
          req->send(400, "application/json", "{\"error\":\"Missing email\"}");
          return;
        }
        if (!requireAdminVerification(req)) {
          return;
        }

        String email = normalizeEmail(req->getParam("email", true)->value());
        if (!isValidEmailAddress(email)) {
          req->send(400, "application/json",
                    "{\"error\":\"Invalid email address\"}");
          return;
        }

        JsonDocument doc;
        loadFirebaseAuthUsersDoc(doc);
        JsonArray users = doc.as<JsonArray>();
        int userIndex = findFirebaseAuthUserIndex(users, email);
        if (userIndex < 0) {
          req->send(404, "application/json",
                    "{\"error\":\"Firebase auth user not found\"}");
          return;
        }

        JsonObject user = users[userIndex].as<JsonObject>();
        String password = user["password"].as<String>();

        JsonDocument response;
        response["ok"] = true;
        response["password"] = password;
        String r;
        serializeJson(response, r);
        req->send(200, "application/json", r);
      });

  server.on(
      "/api/fb/auth/users/add", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("email", true) || !req->hasParam("password", true)) {
          req->send(400, "application/json", "{\"error\":\"Missing params\"}");
          return;
        }
        if (!requireAdminVerification(req)) {
          return;
        }

        String email = normalizeEmail(req->getParam("email", true)->value());
        String password = req->getParam("password", true)->value();
        if (!isValidEmailAddress(email)) {
          req->send(400, "application/json",
                    "{\"error\":\"Invalid email address\"}");
          return;
        }
        if (password.isEmpty()) {
          req->send(400, "application/json",
                    "{\"error\":\"Password is required\"}");
          return;
        }

        JsonDocument doc;
        loadFirebaseAuthUsersDoc(doc);
        JsonArray users = doc.as<JsonArray>();
        if (findFirebaseAuthUserIndex(users, email) >= 0) {
          req->send(409, "application/json",
                    "{\"error\":\"Firebase auth user already exists\"}");
          return;
        }

        JsonObject user = users.createNestedObject();
        user["email"] = email;
        user["password"] = password;
        saveFirebaseAuthUsersDoc(doc);
        firebaseRuntimeDirty = true;
        notifyStorage();
        req->send(200, "application/json", "{\"ok\":true}");
      });

  server.on(
      "/api/fb/auth/users/remove", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("email", true)) {
          req->send(400, "application/json", "{\"error\":\"Missing email\"}");
          return;
        }
        if (!requireAdminVerification(req)) {
          return;
        }

        String email = normalizeEmail(req->getParam("email", true)->value());
        if (!isValidEmailAddress(email)) {
          req->send(400, "application/json",
                    "{\"error\":\"Invalid email address\"}");
          return;
        }

        JsonDocument doc;
        loadFirebaseAuthUsersDoc(doc);
        JsonArray users = doc.as<JsonArray>();
        int userIndex = findFirebaseAuthUserIndex(users, email);
        if (userIndex < 0) {
          req->send(404, "application/json",
                    "{\"error\":\"Firebase auth user not found\"}");
          return;
        }

        users.remove(userIndex);
        saveFirebaseAuthUsersDoc(doc);
        firebaseRuntimeDirty = true;
        notifyStorage();
        req->send(200, "application/json", "{\"ok\":true}");
      });

  uint32_t heapAfterCoreRoutes = ESP.getFreeHeap();
  if (heapAfterCoreRoutes < MIN_FREE_HEAP_FOR_EXTENDED_ROUTES) {
    coreRoutesOnly = true;
    Serial.printf("[SERVER] Low heap after core routes: %u bytes\n",
                  heapAfterCoreRoutes);
    Serial.println(
        "[SERVER] Starting in core-routes-only mode. Extended config APIs are "
        "skipped.");
  } else {
    coreRoutesOnly = false;
    // ---- TIMERS (volatile) ----
    server.on("/api/timer/set", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("sw", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing sw\"}");
        return;
      }
      int sw = req->getParam("sw", true)->value().toInt();
      int h = req->hasParam("h", true)
                  ? req->getParam("h", true)->value().toInt()
                  : 0;
      int m = req->hasParam("m", true)
                  ? req->getParam("m", true)->value().toInt()
                  : 0;
      int s = req->hasParam("s", true)
                  ? req->getParam("s", true)->value().toInt()
                  : 0;
      String act = req->hasParam("action", true)
                       ? req->getParam("action", true)->value()
                       : "off";
      unsigned long dur = (h * 3600UL + m * 60UL + s) * 1000UL;
      if (dur == 0 || sw < 0 || sw >= NUM_SWITCHES) {
        req->send(400, "application/json", "{\"error\":\"Invalid\"}");
        return;
      }
      swTimers[sw].active = true;
      swTimers[sw].paused = false;
      swTimers[sw].remainingMs = dur;
      swTimers[sw].endMs = millis() + dur;
      swTimers[sw].targetState = (act == "on");
      req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/timers", HTTP_GET, [](AsyncWebServerRequest *req) {
      int filterSw = -1;
      if (req->hasParam("sw")) {
        filterSw = req->getParam("sw")->value().toInt();
        if (filterSw < 0 || filterSw >= NUM_SWITCHES) {
          req->send(400, "application/json", "{\"error\":\"Invalid switch\"}");
          return;
        }
      }

      JsonDocument d;
      JsonArray timers = d.to<JsonArray>();
      unsigned long nowMs = millis();

      for (int i = 0; i < NUM_SWITCHES; i++) {
        if (filterSw >= 0 && i != filterSw) {
          continue;
        }

        if (!swTimers[i].active) {
          continue;
        }

        unsigned long remainingMs = 0;
        if (swTimers[i].paused) {
          remainingMs = swTimers[i].remainingMs;
        } else {
          remainingMs =
              swTimers[i].endMs > nowMs ? swTimers[i].endMs - nowMs : 0;
          swTimers[i].remainingMs = remainingMs;
        }

        if (remainingMs == 0) {
          swTimers[i].active = false;
          swTimers[i].paused = false;
          swTimers[i].endMs = 0;
          swTimers[i].remainingMs = 0;
          continue;
        }

        JsonObject timer = timers.createNestedObject();
        timer["sw"] = i;
        timer["action"] = swTimers[i].targetState ? "on" : "off";
        timer["remainingMs"] = remainingMs;
        timer["remainingSec"] = (remainingMs + 999) / 1000;
        timer["paused"] = swTimers[i].paused;
      }

      String r;
      serializeJson(d, r);
      req->send(200, "application/json", r);
    });

    server.on("/api/timer/pause", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("sw", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing sw\"}");
        return;
      }

      int sw = req->getParam("sw", true)->value().toInt();
      if (sw < 0 || sw >= NUM_SWITCHES) {
        req->send(400, "application/json", "{\"error\":\"Invalid switch\"}");
        return;
      }

      if (!swTimers[sw].active) {
        req->send(404, "application/json", "{\"error\":\"Timer not found\"}");
        return;
      }

      if (!swTimers[sw].paused) {
        unsigned long nowMs = millis();
        unsigned long remainingMs =
            swTimers[sw].endMs > nowMs ? swTimers[sw].endMs - nowMs : 0;
        if (remainingMs == 0) {
          swTimers[sw].active = false;
          swTimers[sw].paused = false;
          swTimers[sw].endMs = 0;
          swTimers[sw].remainingMs = 0;
          req->send(200, "application/json",
                    "{\"ok\":true,\"active\":false,\"paused\":false,"
                    "\"remainingSec\":0}");
          return;
        }
        swTimers[sw].remainingMs = remainingMs;
        swTimers[sw].paused = true;
      }

      JsonDocument d;
      d["ok"] = true;
      d["active"] = swTimers[sw].active;
      d["paused"] = swTimers[sw].paused;
      d["remainingSec"] = (swTimers[sw].remainingMs + 999) / 1000;
      String r;
      serializeJson(d, r);
      req->send(200, "application/json", r);
    });

    server.on("/api/timer/resume", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("sw", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing sw\"}");
        return;
      }

      int sw = req->getParam("sw", true)->value().toInt();
      if (sw < 0 || sw >= NUM_SWITCHES) {
        req->send(400, "application/json", "{\"error\":\"Invalid switch\"}");
        return;
      }

      if (!swTimers[sw].active) {
        req->send(404, "application/json", "{\"error\":\"Timer not found\"}");
        return;
      }

      if (swTimers[sw].paused) {
        if (swTimers[sw].remainingMs == 0) {
          swTimers[sw].active = false;
          swTimers[sw].paused = false;
          swTimers[sw].endMs = 0;
          req->send(200, "application/json",
                    "{\"ok\":true,\"active\":false,\"paused\":false,"
                    "\"remainingSec\":0}");
          return;
        }
        swTimers[sw].endMs = millis() + swTimers[sw].remainingMs;
        swTimers[sw].paused = false;
      }

      JsonDocument d;
      d["ok"] = true;
      d["active"] = swTimers[sw].active;
      d["paused"] = swTimers[sw].paused;
      d["remainingSec"] = (swTimers[sw].remainingMs + 999) / 1000;
      String r;
      serializeJson(d, r);
      req->send(200, "application/json", r);
    });

    server.on("/api/timer/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (req->hasParam("sw", true)) {
        int sw = req->getParam("sw", true)->value().toInt();
        if (sw >= 0 && sw < NUM_SWITCHES) {
          swTimers[sw].active = false;
          swTimers[sw].paused = false;
          swTimers[sw].endMs = 0;
          swTimers[sw].remainingMs = 0;
        }
      }
      req->send(200, "application/json", "{\"ok\":true}");
    });
    // ---- SCHEDULES (persistent) ----
    server.on("/api/schedules", HTTP_GET, [](AsyncWebServerRequest *req) {
      int sw = req->hasParam("sw") ? req->getParam("sw")->value().toInt() : 0;
      prefs.begin("sched", true);
      String j = prefs.getString(("s" + String(sw)).c_str(), "[]");
      prefs.end();
      req->send(200, "application/json", j);
    });

    server.on("/api/schedules", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("sw", true) || !req->hasParam("data", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing params\"}");
        return;
      }
      if (!timeSynced) {
        req->send(400, "application/json", "{\"error\":\"Update time first\"}");
        return;
      }
      int sw = req->getParam("sw", true)->value().toInt();
      String data = req->getParam("data", true)->value();
      String error;
      if (sw < 0 || sw >= NUM_SWITCHES) {
        req->send(400, "application/json", "{\"error\":\"Invalid switch\"}");
        return;
      }
      if (!validateRecurringSchedulesData(data, error)) {
        req->send(400, "application/json",
                  String("{\"error\":\"") + error + "\"}");
        return;
      }
      prefs.begin("sched", false);
      prefs.putString(("s" + String(sw)).c_str(), data);
      prefs.end();
      notifyStorage();
      req->send(200, "application/json", "{\"ok\":true}");
    });
    // ---- FUTURE SCHEDULES (persistent) ----
    server.on("/api/fschedules", HTTP_GET, [](AsyncWebServerRequest *req) {
      int sw = req->hasParam("sw") ? req->getParam("sw")->value().toInt() : 0;
      prefs.begin("fsched", true);
      String j = prefs.getString(("f" + String(sw)).c_str(), "[]");
      prefs.end();
      req->send(200, "application/json", j);
    });

    server.on("/api/fschedules", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("sw", true) || !req->hasParam("data", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing params\"}");
        return;
      }
      if (!timeSynced) {
        req->send(400, "application/json", "{\"error\":\"Update time first\"}");
        return;
      }
      int sw = req->getParam("sw", true)->value().toInt();
      String data = req->getParam("data", true)->value();
      String error;
      if (sw < 0 || sw >= NUM_SWITCHES) {
        req->send(400, "application/json", "{\"error\":\"Invalid switch\"}");
        return;
      }
      if (!validateFutureSchedulesData(data, error)) {
        req->send(400, "application/json",
                  String("{\"error\":\"") + error + "\"}");
        return;
      }
      prefs.begin("fsched", false);
      prefs.putString(("f" + String(sw)).c_str(), data);
      prefs.end();
      notifyStorage();
      req->send(200, "application/json", "{\"ok\":true}");
    });
    // ---- SENSOR CONTROL (persistent) ----
    server.on("/api/sensor/temp", HTTP_GET, [](AsyncWebServerRequest *req) {
      int sw = req->hasParam("sw") ? req->getParam("sw")->value().toInt() : 0;
      prefs.begin("sensor", true);
      String j = prefs.getString(("t" + String(sw)).c_str(), "{}");
      prefs.end();
      req->send(200, "application/json", j);
    });

    server.on("/api/sensor/temp", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("data", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing data\"}");
        return;
      }
      if (!ahtOk) {
        req->send(400, "application/json",
                  "{\"error\":\"AHT10 sensor not initialized\"}");
        return;
      }
      String data = req->getParam("data", true)->value();
      JsonDocument d;
      if (deserializeJson(d, data)) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      int sw = d["switchId"].as<int>();
      prefs.begin("sensor", false);
      prefs.putString(("t" + String(sw)).c_str(), data);
      prefs.end();
      notifyStorage();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/sensor/humid", HTTP_GET, [](AsyncWebServerRequest *req) {
      int sw = req->hasParam("sw") ? req->getParam("sw")->value().toInt() : 0;
      prefs.begin("sensor", true);
      String j = prefs.getString(("h" + String(sw)).c_str(), "{}");
      prefs.end();
      req->send(200, "application/json", j);
    });

    server.on("/api/sensor/humid", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("data", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing data\"}");
        return;
      }
      if (!ahtOk) {
        req->send(400, "application/json",
                  "{\"error\":\"AHT10 sensor not initialized\"}");
        return;
      }
      String data = req->getParam("data", true)->value();
      JsonDocument d;
      if (deserializeJson(d, data)) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      int sw = d["switchId"].as<int>();
      prefs.begin("sensor", false);
      prefs.putString(("h" + String(sw)).c_str(), data);
      prefs.end();
      notifyStorage();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/sensor/sun", HTTP_GET, [](AsyncWebServerRequest *req) {
      int sw = req->hasParam("sw") ? req->getParam("sw")->value().toInt() : 0;
      prefs.begin("sensor", true);
      String j = prefs.getString(("x" + String(sw)).c_str(), "{}");
      prefs.end();
      req->send(200, "application/json", j);
    });

    server.on("/api/sensor/sun", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("data", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing data\"}");
        return;
      }
      if (!locOk) {
        req->send(400, "application/json",
                  "{\"error\":\"Latitude/Longitude not configured\"}");
        return;
      }
      if (!timeSynced) {
        req->send(400, "application/json", "{\"error\":\"Update time first\"}");
        return;
      }
      String data = req->getParam("data", true)->value();
      JsonDocument d;
      if (deserializeJson(d, data)) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      int sw = d["switchId"].as<int>();
      prefs.begin("sensor", false);
      prefs.putString(("x" + String(sw)).c_str(), data);
      prefs.end();
      notifyStorage();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/sensor/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("sw", true) || !req->hasParam("type", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing params\"}");
        return;
      }

      int sw = req->getParam("sw", true)->value().toInt();
      if (sw < 0 || sw >= NUM_SWITCHES) {
        req->send(400, "application/json", "{\"error\":\"Invalid switch\"}");
        return;
      }

      String type = req->getParam("type", true)->value();
      type.trim();
      type.toLowerCase();

      char prefix = 0;
      if (type == "temp")
        prefix = 't';
      else if (type == "humid")
        prefix = 'h';
      else if (type == "sun")
        prefix = 'x';
      else {
        req->send(400, "application/json",
                  "{\"error\":\"Invalid sensor type\"}");
        return;
      }

      prefs.begin("sensor", false);
      prefs.remove((String(prefix) + String(sw)).c_str());
      prefs.end();

      notifyStorage();
      req->send(200, "application/json", "{\"ok\":true}");
    });
    // ---- WIFI ----
    server.on("/api/wifi/status", HTTP_GET, [](AsyncWebServerRequest *req) {
      JsonDocument d;
      bool connected = (WiFi.status() == WL_CONNECTED);
      String savedSsid;
      String savedPass;
      getSavedWifiCredentials(savedSsid, savedPass);
      d["connected"] = connected;
      d["ssid"] = connected ? WiFi.SSID() : savedSsid;
      d["savedSsid"] = savedSsid;
      d["ip"] = WiFi.localIP().toString();
      d["subnet"] = WiFi.subnetMask().toString();
      d["gateway"] = WiFi.gatewayIP().toString();
      d["dns"] = WiFi.dnsIP().toString();
      d["mac"] = WiFi.macAddress();
      d["rssi"] = connected ? WiFi.RSSI() : 0;
      d["dhcp"] = dhcpOn;
      d["staticIp"] = sIp;
      d["staticMask"] = sMask;
      d["staticGw"] = sGw;
      d["staticDns"] = sDns;
      String r;
      serializeJson(d, r);
      req->send(200, "application/json", r);
    });

    server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("ssid", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing SSID\"}");
        return;
      }
      if (!req->hasParam("adminUser", true) ||
          !req->hasParam("adminPass", true)) {
        req->send(400, "application/json",
                  "{\"error\":\"Admin credentials required\"}");
        return;
      }
      if (!verifyAdmin(req->getParam("adminUser", true)->value(),
                       req->getParam("adminPass", true)->value())) {
        req->send(403, "application/json",
                  "{\"error\":\"Admin verification failed\"}");
        return;
      }
      String ssid = req->getParam("ssid", true)->value();
      String pass = req->hasParam("pass", true)
                        ? req->getParam("pass", true)->value()
                        : "";
      if (ssid.isEmpty()) {
        req->send(400, "application/json", "{\"error\":\"SSID is required\"}");
        return;
      }
      prefs.begin("wfcfg", false);
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      prefs.end();
      notifyStorage();
      req->send(200, "application/json",
                "{\"ok\":true,\"msg\":\"WiFi credentials saved. Restarting to "
                "connect.\"}");
      restartFlag = true;
      restartAt = millis() + 1200;
    });

    server.on("/api/wifi/disconnect", HTTP_POST,
              [](AsyncWebServerRequest *req) {
                WiFi.disconnect();
                req->send(200, "application/json", "{\"ok\":true}");
              });

    server.on("/api/wifi/scan", HTTP_POST, [](AsyncWebServerRequest *req) {
      req->send(200, "application/json",
                "{\"ok\":true,\"msg\":\"WiFi portal starting. Connect to ESP "
                "HOME at 192.168.4.1. The current web session will disconnect "
                "until setup finishes.\"}");
      portalFlag = true;
    });

    server.on("/api/wifi/saved", HTTP_GET, [](AsyncWebServerRequest *req) {
      JsonDocument d;
      String savedSsid;
      String savedPass;
      bool connected = (WiFi.status() == WL_CONNECTED);
      getSavedWifiCredentials(savedSsid, savedPass);
      d["connected"] = connected;
      d["ssid"] = connected ? WiFi.SSID() : savedSsid;
      d["savedSsid"] = savedSsid;
      d["ip"] = connected ? WiFi.localIP().toString() : "";
      String r;
      serializeJson(d, r);
      req->send(200, "application/json", r);
    });

    server.on("/api/wifi/forget", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("adminUser", true) ||
          !req->hasParam("adminPass", true)) {
        req->send(400, "application/json",
                  "{\"error\":\"Admin credentials required\"}");
        return;
      }
      if (!verifyAdmin(req->getParam("adminUser", true)->value(),
                       req->getParam("adminPass", true)->value())) {
        req->send(403, "application/json",
                  "{\"error\":\"Admin verification failed\"}");
        return;
      }
      prefs.begin("wfcfg", false);
      prefs.remove("ssid");
      prefs.remove("pass");
      prefs.end();
      notifyStorage();
      req->send(200, "application/json",
                "{\"ok\":true,\"msg\":\"WiFi credentials forgotten. Device "
                "will disconnect and restart.\"}");
      forgetWifiFlag = true;
      forgetWifiAt = millis() + 1000;
    });

    server.on("/api/wifi/ip", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("dhcp", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing\"}");
        return;
      }
      if (!requireAdminVerification(req)) {
        return;
      }
      dhcpOn = req->getParam("dhcp", true)->value() == "true";
      prefs.begin("wfcfg", false);
      prefs.putBool("dhcp", dhcpOn);
      if (!dhcpOn) {
        if (!req->hasParam("ip", true) || !req->hasParam("mask", true) ||
            !req->hasParam("gw", true) || !req->hasParam("dns", true)) {
          prefs.end();
          req->send(400, "application/json",
                    "{\"error\":\"Static IP, subnet mask, gateway and DNS are "
                    "required\"}");
          return;
        }

        sIp = req->getParam("ip", true)->value();
        sMask = req->getParam("mask", true)->value();
        sGw = req->getParam("gw", true)->value();
        sDns = req->getParam("dns", true)->value();

        IPAddress ip, gw, sn, dns;
        if (!ip.fromString(sIp) || !gw.fromString(sGw) ||
            !sn.fromString(sMask) || !dns.fromString(sDns)) {
          prefs.end();
          req->send(400, "application/json",
                    "{\"error\":\"Invalid static IP settings\"}");
          return;
        }

        prefs.putString("sip", sIp);
        prefs.putString("mask", sMask);
        prefs.putString("gw", sGw);
        prefs.putString("dns", sDns);
      }
      prefs.end();
      notifyStorage();
      if (dhcpOn) {
        req->send(200, "application/json",
                  "{\"ok\":true,\"msg\":\"DHCP enabled. Device will reconnect "
                  "using DHCP.\"}");
      } else {
        req->send(200, "application/json",
                  "{\"ok\":true,\"msg\":\"Static IP settings saved. Device "
                  "will reconnect using the configured IP.\",\"ip\":\"" +
                      sIp + "\"}");
      }
      restartFlag = true;
      restartAt = millis() + 1200;
    });
    // ---- FIREBASE ----
    server.on("/api/fb", HTTP_GET, [](AsyncWebServerRequest *req) {
      JsonDocument d;
      d["enabled"] = fbOn;
      d["url"] = fbUrl;
      d["hasToken"] = !fbToken.isEmpty();
      prefs.begin("fb", true);
      String rules = prefs.getString("rules", DEFAULT_FIREBASE_RULES_JSON);
      prefs.end();

      if (rules.isEmpty()) {
        rules = DEFAULT_FIREBASE_RULES_JSON;
      }
      d["rules"] = rules;

      String r;
      serializeJson(d, r);
      req->send(200, "application/json", r);
    });

    server.on("/api/fb/toggle", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("enabled", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing\"}");
        return;
      }
      bool enableFirebase = req->getParam("enabled", true)->value() == "true";
      if (enableFirebase && !requireAdminVerification(req)) {
        return;
      }
      fbOn = enableFirebase;
      prefs.begin("fb", false);
      prefs.putBool("en", fbOn);
      prefs.end();
      firebaseRuntimeDirty = true;
      notifyStorage();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/fb/url", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("url", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing\"}");
        return;
      }
      if (!requireAdminVerification(req)) {
        return;
      }
      fbUrl = req->getParam("url", true)->value();
      prefs.begin("fb", false);
      prefs.putString("url", fbUrl);
      prefs.end();
      firebaseRuntimeDirty = true;
      notifyStorage();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/fb/reveal-token", HTTP_POST,
              [](AsyncWebServerRequest *req) {
                if (!requireAdminVerification(req)) {
                  return;
                }

                JsonDocument d;
                d["ok"] = true;
                d["hasToken"] = !fbToken.isEmpty();
                d["token"] = fbToken;
                String r;
                serializeJson(d, r);
                req->send(200, "application/json", r);
              });

    // Backward-compatible alias for existing UI builds.
    server.on("/api/fb/token/reveal", HTTP_POST,
              [](AsyncWebServerRequest *req) {
                if (!requireAdminVerification(req)) {
                  return;
                }

                JsonDocument d;
                d["ok"] = true;
                d["hasToken"] = !fbToken.isEmpty();
                d["token"] = fbToken;
                String r;
                serializeJson(d, r);
                req->send(200, "application/json", r);
              });

    server.on("/api/fb/token", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("token", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing\"}");
        return;
      }
      if (!requireAdminVerification(req)) {
        return;
      }
      fbToken = req->getParam("token", true)->value();
      prefs.begin("fb", false);
      prefs.putString("tok", fbToken);
      prefs.end();
      firebaseRuntimeDirty = true;
      notifyStorage();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/fb/test", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (fbUrl.isEmpty() || fbToken.isEmpty()) {
        req->send(400, "application/json",
                  "{\"error\":\"URL or Token not configured\"}");
        return;
      }
      if (WiFi.status() != WL_CONNECTED) {
        req->send(400, "application/json",
                  "{\"error\":\"WiFi not connected\"}");
        return;
      }
      HTTPClient http;
      String testUrl = fbUrl + "/.json?auth=" + fbToken + "&shallow=true";
      http.begin(testUrl);
      int code = http.GET();
      String body = http.getString();
      http.end();
      if (code == 200)
        req->send(200, "application/json",
                  "{\"ok\":true,\"msg\":\"Connection successful\"}");
      else
        req->send(
            400, "application/json",
            "{\"error\":\"Connection failed: HTTP " + String(code) + "\"}");
    });

    server.on("/api/fb/rules", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("rules", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing rules\"}");
        return;
      }
      if (!requireAdminVerification(req)) {
        return;
      }
      String rules = req->getParam("rules", true)->value();
      prefs.begin("fb", false);
      prefs.putString("rules", rules);
      prefs.end();
      notifyStorage();
      req->send(200, "application/json",
                "{\"ok\":true,\"msg\":\"Rules saved!\"}");
    });
    // ---- USERS ----
    server.on("/api/users", HTTP_GET, [](AsyncWebServerRequest *req) {
      prefs.begin("users", true);
      int n = prefs.getInt("cnt", 0);
      JsonDocument d;
      JsonArray a = d.to<JsonArray>();
      for (int i = 0; i < n; i++) {
        String js = prefs.getString(("u" + String(i)).c_str(), "");
        if (js.isEmpty())
          continue;
        JsonDocument ud;
        deserializeJson(ud, js);
        JsonObject o = a.createNestedObject();
        o["id"] = normalizeUserId(ud["id"].as<String>());
        o["role"] = normalizeUserRole(ud["role"].as<String>());
      }
      prefs.end();
      String r;
      serializeJson(d, r);
      req->send(200, "application/json", r);
    });

    server.on("/api/users/add", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("id", true) || !req->hasParam("pass", true) ||
          !req->hasParam("adminUser", true) ||
          !req->hasParam("adminPass", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing params\"}");
        return;
      }
      if (!hasAdmin()) {
        initDefaultUser();
        if (!hasAdmin()) {
          req->send(403, "application/json",
                    "{\"error\":\"No admin user exists\"}");
          return;
        }
      }
      String adminUser = req->getParam("adminUser", true)->value();
      String adminPass = req->getParam("adminPass", true)->value();
      if (!verifyAdmin(adminUser, adminPass)) {
        initDefaultUser();
        if (!verifyAdmin(adminUser, adminPass)) {
          req->send(403, "application/json",
                    "{\"error\":\"Admin verification failed\"}");
          return;
        }
      }
      String newId = normalizeUserId(req->getParam("id", true)->value());
      String newPass = normalizeUserPass(req->getParam("pass", true)->value());
      if (newId.isEmpty() || newPass.isEmpty()) {
        req->send(400, "application/json",
                  "{\"error\":\"User ID and password are required\"}");
        return;
      }
      JsonDocument nd;
      nd["id"] = newId;
      nd["pass"] = newPass;
      nd["role"] = "user";
      String nj;
      serializeJson(nd, nj);
      prefs.begin("users", false);
      int cnt = prefs.getInt("cnt", 0);

      for (int i = 0; i < cnt; i++) {
        String js = prefs.getString(("u" + String(i)).c_str(), "");
        if (js.isEmpty())
          continue;
        JsonDocument existing;
        if (deserializeJson(existing, js))
          continue;
        String existingId = normalizeUserId(existing["id"].as<String>());
        if (existingId == newId) {
          prefs.end();
          req->send(409, "application/json",
                    "{\"error\":\"User already exists\"}");
          return;
        }
      }

      prefs.putString(("u" + String(cnt)).c_str(), nj);
      prefs.putInt("cnt", cnt + 1);
      prefs.end();
      notifyStorage();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/users/remove", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("id", true) || !req->hasParam("adminUser", true) ||
          !req->hasParam("adminPass", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing params\"}");
        return;
      }
      if (!hasAdmin()) {
        initDefaultUser();
        if (!hasAdmin()) {
          req->send(403, "application/json",
                    "{\"error\":\"No admin user exists\"}");
          return;
        }
      }
      String adminUser = req->getParam("adminUser", true)->value();
      String adminPass = req->getParam("adminPass", true)->value();
      if (!verifyAdmin(adminUser, adminPass)) {
        initDefaultUser();
        if (!verifyAdmin(adminUser, adminPass)) {
          req->send(403, "application/json",
                    "{\"error\":\"Admin verification failed\"}");
          return;
        }
      }
      String uid = normalizeUserId(req->getParam("id", true)->value());
      if (uid.isEmpty()) {
        req->send(400, "application/json",
                  "{\"error\":\"User ID is required\"}");
        return;
      }

      prefs.begin("users", false);
      int cnt = prefs.getInt("cnt", 0);

      int matchedUsers = 0;
      int matchedAdmins = 0;
      for (int i = 0; i < cnt; i++) {
        String js = prefs.getString(("u" + String(i)).c_str(), "");
        if (js.isEmpty())
          continue;
        JsonDocument d;
        if (deserializeJson(d, js))
          continue;

        String existingId = normalizeUserId(d["id"].as<String>());
        if (existingId == uid) {
          matchedUsers++;
          if (normalizeUserRole(d["role"].as<String>()) == "admin") {
            matchedAdmins++;
          }
        }
      }

      if (matchedUsers <= 0) {
        prefs.end();
        req->send(404, "application/json", "{\"error\":\"User not found\"}");
        return;
      }

      if (matchedAdmins > 0) {
        prefs.end();
        req->send(403, "application/json",
                  "{\"error\":\"Admin user cannot be removed\"}");
        return;
      }

      int writeIdx = 0;
      for (int i = 0; i < cnt; i++) {
        String js = prefs.getString(("u" + String(i)).c_str(), "");
        if (js.isEmpty())
          continue;

        bool shouldRemove = false;
        JsonDocument d;
        if (!deserializeJson(d, js)) {
          String existingId = normalizeUserId(d["id"].as<String>());
          shouldRemove = (existingId == uid);
        }

        if (shouldRemove)
          continue;

        if (writeIdx != i) {
          prefs.putString(("u" + String(writeIdx)).c_str(), js);
        }
        writeIdx++;
      }

      for (int i = writeIdx; i < cnt; i++) {
        prefs.remove(("u" + String(i)).c_str());
      }
      prefs.putInt("cnt", writeIdx);
      prefs.end();
      notifyStorage();
      req->send(200, "application/json",
                "{\"ok\":true,\"removed\":" + String(matchedUsers) + "}");
    });

    server.on(
        "/api/users/admin/edit", HTTP_POST, [](AsyncWebServerRequest *req) {
          if (!req->hasParam("id", true) || !req->hasParam("pass", true) ||
              !req->hasParam("adminUser", true) ||
              !req->hasParam("adminPass", true)) {
            req->send(400, "application/json",
                      "{\"error\":\"Missing params\"}");
            return;
          }

          if (!hasAdmin()) {
            initDefaultUser();
            if (!hasAdmin()) {
              req->send(403, "application/json",
                        "{\"error\":\"No admin user exists\"}");
              return;
            }
          }

          String adminUser = req->getParam("adminUser", true)->value();
          String adminPass = req->getParam("adminPass", true)->value();
          if (!verifyAdmin(adminUser, adminPass)) {
            initDefaultUser();
            if (!verifyAdmin(adminUser, adminPass)) {
              req->send(403, "application/json",
                        "{\"error\":\"Admin verification failed\"}");
              return;
            }
          }

          String newAdminId =
              normalizeUserId(req->getParam("id", true)->value());
          String newAdminPass =
              normalizeUserPass(req->getParam("pass", true)->value());
          if (newAdminId.isEmpty() || newAdminPass.isEmpty()) {
            req->send(
                400, "application/json",
                "{\"error\":\"Admin user ID and password are required\"}");
            return;
          }

          String currentAdminId = normalizeUserId(adminUser);
          String currentAdminPass = normalizeUserPass(adminPass);

          prefs.begin("users", false);
          int cnt = prefs.getInt("cnt", 0);
          int targetAdminIndex = -1;

          for (int i = 0; i < cnt; i++) {
            String js = prefs.getString(("u" + String(i)).c_str(), "");
            if (js.isEmpty())
              continue;

            JsonDocument d;
            if (deserializeJson(d, js))
              continue;

            String id = normalizeUserId(d["id"].as<String>());
            String pass = normalizeUserPass(d["pass"].as<String>());
            String role = normalizeUserRole(d["role"].as<String>());
            if (role == "admin" && id == currentAdminId &&
                pass == currentAdminPass) {
              targetAdminIndex = i;
              break;
            }
          }

          if (targetAdminIndex < 0) {
            prefs.end();
            req->send(403, "application/json",
                      "{\"error\":\"Admin verification failed\"}");
            return;
          }

          for (int i = 0; i < cnt; i++) {
            if (i == targetAdminIndex)
              continue;

            String js = prefs.getString(("u" + String(i)).c_str(), "");
            if (js.isEmpty())
              continue;

            JsonDocument d;
            if (deserializeJson(d, js))
              continue;

            if (normalizeUserId(d["id"].as<String>()) == newAdminId) {
              prefs.end();
              req->send(409, "application/json",
                        "{\"error\":\"User ID already exists\"}");
              return;
            }
          }

          for (int i = 0; i < cnt; i++) {
            String js = prefs.getString(("u" + String(i)).c_str(), "");
            if (js.isEmpty())
              continue;

            JsonDocument d;
            if (deserializeJson(d, js))
              continue;

            bool changed = false;
            String role = normalizeUserRole(d["role"].as<String>());

            if (i == targetAdminIndex) {
              d["id"] = newAdminId;
              d["pass"] = newAdminPass;
              d["role"] = "admin";
              changed = true;
            } else if (role == "admin") {
              // Keep exactly one admin account in storage.
              d["role"] = "user";
              changed = true;
            }

            if (changed) {
              String updated;
              serializeJson(d, updated);
              prefs.putString(("u" + String(i)).c_str(), updated);
            }
          }

          prefs.end();
          persistConfiguredPrimaryAdmin(newAdminId, newAdminPass);
          notifyStorage();
          req->send(200, "application/json", "{\"ok\":true}");
        });
    // ---- ADMINISTRATOR ----
    server.on("/api/admin/device-name", HTTP_GET,
              [](AsyncWebServerRequest *req) {
                JsonDocument d;
                d["name"] = deviceName;
                d["defaultName"] = getDefaultDeviceName();
                String r;
                serializeJson(d, r);
                req->send(200, "application/json", r);
              });

    server.on(
        "/api/admin/device-name", HTTP_POST, [](AsyncWebServerRequest *req) {
          if (!req->hasParam("name", true)) {
            req->send(400, "application/json",
                      "{\"error\":\"Missing device name\"}");
            return;
          }
          if (!requireAdminVerification(req)) {
            return;
          }

          String newName = req->getParam("name", true)->value();
          newName.trim();
          if (newName.isEmpty()) {
            req->send(400, "application/json",
                      "{\"error\":\"Device name is required\"}");
            return;
          }
          if (newName.length() > 32) {
            req->send(
                400, "application/json",
                "{\"error\":\"Device name must be 32 characters or less\"}");
            return;
          }

          String previousName = firebaseDeviceNameValue();
          deviceName = newName;
          prefs.begin("admin", false);
          prefs.putString("devname", deviceName);
          prefs.end();

          WiFi.setHostname(deviceName.c_str());
          onDeviceNameUpdated(previousName);
          notifyStorage();

          JsonDocument d;
          d["ok"] = true;
          d["name"] = deviceName;
          String r;
          serializeJson(d, r);
          req->send(200, "application/json", r);
        });

    server.on("/api/admin/schedule-priority", HTTP_GET,
              [](AsyncWebServerRequest *req) {
                JsonDocument d;
                JsonArray arr = d["order"].to<JsonArray>();
                JsonArray labels = d["labels"].to<JsonArray>();
                for (int i = 0; i < AUTOMATION_SOURCE_COUNT; i++) {
                  uint8_t src = automationPriorityOrder[i];
                  arr.add(automationSourceKey(src));
                  labels.add(automationSourceLabel(src));
                }
                String r;
                serializeJson(d, r);
                req->send(200, "application/json", r);
              });

    server.on("/api/admin/schedule-priority", HTTP_POST,
              [](AsyncWebServerRequest *req) {
                if (!req->hasParam("data", true)) {
                  req->send(400, "application/json",
                            "{\"error\":\"Missing data\"}");
                  return;
                }
                if (!requireAdminVerification(req)) {
                  return;
                }

                String raw = req->getParam("data", true)->value();
                JsonDocument doc;
                if (deserializeJson(doc, raw)) {
                  req->send(400, "application/json",
                            "{\"error\":\"Invalid priority data\"}");
                  return;
                }

                JsonArray arr = doc.as<JsonArray>();
                if (arr.isNull() || arr.size() != AUTOMATION_SOURCE_COUNT) {
                  req->send(400, "application/json",
                            "{\"error\":\"Priority order must contain all "
                            "automation types\"}");
                  return;
                }

                bool seen[AUTOMATION_SOURCE_COUNT] = {false, false, false,
                                                      false, false, false};
                uint8_t newOrder[AUTOMATION_SOURCE_COUNT];
                for (int i = 0; i < AUTOMATION_SOURCE_COUNT; i++) {
                  int src = automationSourceFromKey(arr[i].as<String>());
                  if (src < 0 || src >= AUTOMATION_SOURCE_COUNT || seen[src]) {
                    req->send(400, "application/json",
                              "{\"error\":\"Priority order contains invalid or "
                              "duplicate items\"}");
                    return;
                  }
                  seen[src] = true;
                  newOrder[i] = (uint8_t)src;
                }

                for (int i = 0; i < AUTOMATION_SOURCE_COUNT; i++) {
                  automationPriorityOrder[i] = newOrder[i];
                }

                prefs.begin("admin", false);
                for (int i = 0; i < AUTOMATION_SOURCE_COUNT; i++) {
                  prefs.putInt(("pr" + String(i)).c_str(),
                               automationPriorityOrder[i]);
                }
                prefs.end();

                notifyStorage();
                req->send(200, "application/json", "{\"ok\":true}");
              });

    server.on("/api/admin/time", HTTP_GET, [](AsyncWebServerRequest *req) {
      JsonDocument d;
      d["ntp"] = ntpSrv;
      d["tz"] = tzStr;
      d["synced"] = timeSynced;
      String r;
      serializeJson(d, r);
      req->send(200, "application/json", r);
    });

    server.on("/api/admin/time", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (req->hasParam("ntp", true))
        ntpSrv = req->getParam("ntp", true)->value();
      if (req->hasParam("tz", true)) {
        tzStr = req->getParam("tz", true)->value();
        parseTZ();
      }
      prefs.begin("admin", false);
      prefs.putString("ntp", ntpSrv);
      prefs.putString("tz", tzStr);
      prefs.end();
      notifyStorage();
      req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/admin/location", HTTP_GET, [](AsyncWebServerRequest *req) {
      JsonDocument d;
      d["lat"] = geoLat;
      d["lon"] = geoLon;
      d["configured"] = locOk;

      if (!timeSynced) {
        struct tm currentTm;
        if (readCurrentLocalTime(&currentTm)) {
          timeSynced = true;
        }
      }

      bool sunReady = locOk && timeSynced;
      if (sunReady) {
        calcSunriseSunset();
      }

      bool hasSunValues = lastCalcDay >= 0 && srMin >= 0 && srMin <= 1439 &&
                          ssMin >= 0 && ssMin <= 1439;

      d["sunReady"] = hasSunValues;
      d["sunrise"] = hasSunValues ? formatClockFromMinutes(srMin) : "";
      d["sunset"] = hasSunValues ? formatClockFromMinutes(ssMin) : "";
      String r;
      serializeJson(d, r);
      req->send(200, "application/json", r);
    });

    server.on("/api/admin/location", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("lat", true) || !req->hasParam("lon", true)) {
        req->send(400, "application/json", "{\"error\":\"Missing params\"}");
        return;
      }
      geoLat = req->getParam("lat", true)->value().toFloat();
      geoLon = req->getParam("lon", true)->value().toFloat();
      locOk = true;
      lastCalcDay = -1;
      prefs.begin("admin", false);
      prefs.putFloat("lat", geoLat);
      prefs.putFloat("lon", geoLon);
      prefs.end();
      notifyStorage();

      if (!timeSynced) {
        struct tm currentTm;
        if (readCurrentLocalTime(&currentTm)) {
          timeSynced = true;
        }
      }

      calcSunriseSunset();

      JsonDocument d;
      d["ok"] = true;
      d["lat"] = geoLat;
      d["lon"] = geoLon;
      bool sunReady = locOk && timeSynced;

      bool hasSunValues = lastCalcDay >= 0 && srMin >= 0 && srMin <= 1439 &&
                          ssMin >= 0 && ssMin <= 1439;

      d["sunReady"] = hasSunValues;
      d["sunrise"] = hasSunValues ? formatClockFromMinutes(srMin) : "";
      d["sunset"] = hasSunValues ? formatClockFromMinutes(ssMin) : "";
      String r;
      serializeJson(d, r);
      req->send(200, "application/json", r);
    });

    server.on("/api/admin/restart-schedule", HTTP_GET,
              [](AsyncWebServerRequest *req) {
                JsonDocument d;
                d["enabled"] = restartScheduleEnabled;
                d["time"] = restartScheduleEnabled
                                ? formatClockFromMinutes(restartScheduleMinute)
                                : "";
                JsonArray days = d["days"].to<JsonArray>();
                const char *dayKeys[] = {"sun", "mon", "tue", "wed",
                                         "thu", "fri", "sat"};
                for (int i = 0; i < 7; i++) {
                  if (restartScheduleDayMask & (1 << i)) {
                    days.add(dayKeys[i]);
                  }
                }
                String r;
                serializeJson(d, r);
                req->send(200, "application/json", r);
              });

    server.on(
        "/api/admin/restart-schedule", HTTP_POST,
        [](AsyncWebServerRequest *req) {
          if (!req->hasParam("data", true)) {
            if (!requireAdminVerification(req)) {
              return;
            }

            clearRestartScheduleOnly(true);
            req->send(200, "application/json",
                      "{\"ok\":true,\"msg\":\"Restart schedule cancelled\"}");
            return;
          }
          if (!requireAdminVerification(req)) {
            return;
          }

          String raw = req->getParam("data", true)->value();
          JsonDocument doc;
          if (deserializeJson(doc, raw)) {
            req->send(400, "application/json",
                      "{\"error\":\"Invalid restart schedule data\"}");
            return;
          }

          JsonObject payload = doc.as<JsonObject>();
          if (payload.isNull()) {
            req->send(400, "application/json",
                      "{\"error\":\"Restart schedule must be an object\"}");
            return;
          }

          int scheduleMinute = parseClockMinutes(payload["time"].as<String>());
          if (scheduleMinute < 0) {
            req->send(400, "application/json",
                      "{\"error\":\"Restart schedule time must be HH:MM\"}");
            return;
          }

          JsonArray selectedDays = payload["days"].as<JsonArray>();
          if (selectedDays.isNull() || selectedDays.size() == 0) {
            req->send(400, "application/json",
                      "{\"error\":\"Select at least one weekday\"}");
            return;
          }

          uint8_t dayMask = 0;
          for (JsonVariant day : selectedDays) {
            int idx = weekdayIndexFromText(day.as<String>());
            if (idx < 0) {
              req->send(400, "application/json",
                        "{\"error\":\"Restart schedule has invalid weekday\"}");
              return;
            }
            dayMask |= (uint8_t)(1 << idx);
          }

          restartScheduleEnabled = true;
          restartScheduleMinute = scheduleMinute;
          restartScheduleDayMask = dayMask;
          lastRestartScheduleStamp = -1;
          restartScheduleLastRunKey = "";

          prefs.begin("admin", false);
          prefs.remove("rsLast");
          prefs.putBool("rsEn", restartScheduleEnabled);
          prefs.putInt("rsMin", restartScheduleMinute);
          prefs.putUInt("rsMask", restartScheduleDayMask);
          prefs.end();

          notifyStorage();

          JsonDocument response;
          response["ok"] = true;
          response["enabled"] = true;
          response["time"] = formatClockFromMinutes(restartScheduleMinute);
          JsonArray outDays = response["days"].to<JsonArray>();
          const char *dayKeys[] = {"sun", "mon", "tue", "wed",
                                   "thu", "fri", "sat"};
          for (int i = 0; i < 7; i++) {
            if (restartScheduleDayMask & (1 << i)) {
              outDays.add(dayKeys[i]);
            }
          }
          String r;
          serializeJson(response, r);
          req->send(200, "application/json", r);
        });

    server.on("/api/admin/restart-schedule/cancel", HTTP_POST,
              [](AsyncWebServerRequest *req) {
                if (!requireAdminVerification(req)) {
                  return;
                }

                clearRestartScheduleOnly(true);
                req->send(
                    200, "application/json",
                    "{\"ok\":true,\"msg\":\"Restart schedule cancelled\"}");
              });

    server.on("/api/admin/restart", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("adminUser", true) ||
          !req->hasParam("adminPass", true)) {
        req->send(400, "application/json",
                  "{\"error\":\"Admin credentials required\"}");
        return;
      }
      if (!verifyAdmin(req->getParam("adminUser", true)->value(),
                       req->getParam("adminPass", true)->value())) {
        req->send(403, "application/json",
                  "{\"error\":\"Admin verification failed\"}");
        return;
      }
      req->send(200, "application/json",
                "{\"ok\":true,\"msg\":\"Restarting...\"}");
      restartFlag = true;
      restartAt = millis() + 500;
    });

    server.on(
        "/api/admin/reset/storage", HTTP_POST, [](AsyncWebServerRequest *req) {
          if (!requireAdminVerification(req)) {
            return;
          }
          if (req->hasParam("scope", true)) {
            String scope = req->getParam("scope", true)->value();
            scope.trim();
            scope.toLowerCase();
            if (scope != "storage") {
              req->send(
                  400, "application/json",
                  "{\"error\":\"Reset type mismatch (expected storage)\"}");
              return;
            }
          }
          if (!requireBootHoldResetGesture(req)) {
            return;
          }

          JsonDocument response;
          response["ok"] = true;
          response["type"] = "storage";
          response["msg"] = "Reset Storage complete. Device restarting...";
          response["restartIn"] = 45;
          response["stepGapMs"] = RESET_CHECKPOINT_GAP_MS;
          response["finalGapMs"] = RESET_ALL_DONE_GAP_MS;
          JsonArray steps = response["steps"].to<JsonArray>();

          resetSwitchNamesToDefault();
          addResetStep(
              steps, "switch-names",
              "Switch names set to DEVICE-1, DEVICE-2, DEVICE-3, DEVICE-4",
              420);

          resetSwitchIconsToDefault();
          addResetStep(steps, "switch-icons",
                       "Switch icons set to home for all switches", 360);

          clearRecurringSchedulesStorage();
          addResetStep(steps, "schedules", "All recurring schedules cleared",
                       760);

          clearFutureSchedulesStorage();
          addResetStep(steps, "future-schedules",
                       "All future schedules cleared", 720);

          clearSensorControlStorage();
          addResetStep(steps, "sensor-control",
                       "All sensor control settings cleared", 680);

          resetSchedulePriorityToDefault();
          addResetStep(steps, "schedule-priority",
                       "Schedule priority restored to default order", 540);

          notifyStorage();

          String r;
          serializeJson(response, r);
          req->send(200, "application/json", r);

          restartFlag = true;
          restartAt = millis() + (6UL * RESET_CHECKPOINT_GAP_MS) +
                      RESET_ALL_DONE_GAP_MS + RESET_RESTART_BUFFER_MS;
        });

    server.on(
        "/api/admin/reset/settings", HTTP_POST, [](AsyncWebServerRequest *req) {
          if (!requireAdminVerification(req)) {
            return;
          }
          if (req->hasParam("scope", true)) {
            String scope = req->getParam("scope", true)->value();
            scope.trim();
            scope.toLowerCase();
            if (scope != "settings") {
              req->send(
                  400, "application/json",
                  "{\"error\":\"Reset type mismatch (expected settings)\"}");
              return;
            }
          }
          if (!requireBootHoldResetGesture(req)) {
            return;
          }

          JsonDocument response;
          response["ok"] = true;
          response["type"] = "settings";
          response["msg"] = "Reset Settings complete. Device restarting...";
          response["restartIn"] = 45;
          response["stepGapMs"] = RESET_CHECKPOINT_GAP_MS;
          response["finalGapMs"] = RESET_ALL_DONE_GAP_MS;
          JsonArray steps = response["steps"].to<JsonArray>();

          resetRelayStatesToDefault();
          addResetStep(
              steps, "relay-states",
              "Relay startup state restored to Output OFF for all switches",
              640);

          disableFirebaseOnly();
          addResetStep(steps, "firebase-toggle", "Firebase disabled", 380);

          resetDeviceNameToDefault();
          addResetStep(steps, "device-name",
                       "Device name reset to MAC-based default", 450);

          resetNtpServerToDefault();
          addResetStep(steps, "ntp", "NTP server reset to pool.ntp.org", 430);

          resetTimezoneToDefault();
          addResetStep(steps, "timezone", "Time zone reset to +00:00 (London)",
                       520);

          clearStaticIpConfiguration();
          addResetStep(steps, "static-ip",
                       "Static IP configuration cleared and DHCP restored",
                       520);

          resetLocationToDefault();
          addResetStep(steps, "location",
                       "Latitude and longitude reset to London defaults", 600);

          clearRestartScheduleOnly();
          addResetStep(steps, "restart-schedule",
                       "Automatic restart schedule cleared", 460);

          notifyStorage();

          String r;
          serializeJson(response, r);
          req->send(200, "application/json", r);

          restartFlag = true;
          restartAt = millis() + (8UL * RESET_CHECKPOINT_GAP_MS) +
                      RESET_ALL_DONE_GAP_MS + RESET_RESTART_BUFFER_MS;
        });

    server.on(
        "/api/admin/reset/factory", HTTP_POST, [](AsyncWebServerRequest *req) {
          if (!requireAdminVerification(req)) {
            return;
          }
          if (req->hasParam("scope", true)) {
            String scope = req->getParam("scope", true)->value();
            scope.trim();
            scope.toLowerCase();
            if (scope != "factory") {
              req->send(
                  400, "application/json",
                  "{\"error\":\"Reset type mismatch (expected factory)\"}");
              return;
            }
          }
          if (!requireBootHoldResetGesture(req)) {
            return;
          }

          JsonDocument response;
          response["ok"] = true;
          response["type"] = "factory";
          response["msg"] = "Factory Reset complete. Device restarting...";
          response["restartIn"] = 45;
          response["stepGapMs"] = RESET_CHECKPOINT_GAP_MS;
          response["finalGapMs"] = RESET_ALL_DONE_GAP_MS;
          JsonArray steps = response["steps"].to<JsonArray>();

          resetSwitchNamesToDefault();
          resetSwitchIconsToDefault();
          clearRecurringSchedulesStorage();
          clearFutureSchedulesStorage();
          clearSensorControlStorage();
          clearStaticIpConfiguration();
          resetSchedulePriorityToDefault();
          resetLocationToDefault();
          addResetStep(steps, "reset-storage", "Reset Storage applied", 980);

          resetRelayStatesToDefault();
          disableFirebaseOnly();
          resetDeviceNameToDefault();
          resetNtpServerToDefault();
          resetTimezoneToDefault();
          clearRestartScheduleOnly();
          addResetStep(steps, "reset-settings", "Reset Settings applied", 760);

          clearFirebaseCredentialsOnly();
          addResetStep(steps, "firebase-credentials",
                       "Firebase credentials cleared", 560);

          resetFirebaseRulesToDefault();
          addResetStep(steps, "firebase-rules",
                       "Database rules reset to default template", 620);

          clearFirebaseUsersOnly();
          addResetStep(steps, "firebase-users", "Firebase users cleared", 700);

          resetUsersToSingleDefaultAdmin();
          addResetStep(steps, "default-admin",
                       "Default admin user restored to esp / 456456", 650);
          addResetStep(steps, "normal-users", "All normal users cleared", 580);
          addResetStep(steps, "wifi-credentials", "WiFi credentials cleared",
                       520);
          notifyStorage();

          String r;
          serializeJson(response, r);
          req->send(200, "application/json", r);

          factoryResetWifiClearPending = true;
          factoryResetWifiClearAt =
              millis() + FACTORY_RESET_WIFI_CLEAR_DELAY_MS;

          restartFlag = true;
          restartAt = millis() + FACTORY_RESET_RESTART_DELAY_MS;
        });
  }

  server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->method() == HTTP_OPTIONS) {
      req->send(204);
      return;
    }

    if (req->url().startsWith("/api/")) {
      if (coreRoutesOnly) {
        req->send(503, "application/json",
                  "{\"error\":\"Extended API disabled in low-memory mode\"}");
      } else {
        req->send(404, "application/json",
                  "{\"error\":\"API route not found\"}");
      }
      Serial.printf("[HTTP] API miss %s\n", req->url().c_str());
      return;
    }

    Serial.printf("[HTTP] 404 %s\n", req->url().c_str());
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("[SERVER] Web server started");
}

//  SETUP
void setup() {
  std::set_new_handler(oomNewHandler);
  Serial.begin(115200);
  Serial.println("\n==============================");
  Serial.println("  ESP32 Smart Home Starting");
  Serial.println("==============================");

  // Pin modes
  for (int i = 0; i < NUM_SWITCHES; i++) {
    pinMode(outPin[i], OUTPUT);
    pinMode(inPin[i], INPUT);
    lastInState[i] = digitalRead(inPin[i]);
  }
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  swStateMutex = xSemaphoreCreateMutex();
  if (!swStateMutex) {
    Serial.println("[SYS] Failed to create swState mutex");
  }

  configureFirebaseSslClient(fbSslClient);
  configureFirebaseSslClient(fbStreamSslClient);
  initDeviceId();
  initFirebaseWriteQueue();

  // Startup beep
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
  delay(2000);

  // On demand WiFi setup
  if (digitalRead(BOOT_BUTTON) == LOW) {
    Serial.println("[BOOT] Boot button pressed - entering WiFi setup mode...");
    // Double beep to indicate WiFi setup mode
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);

    runWifiConfigPortal(true);
  }

  // --- Storage Init ---
  Serial.println("\n==============================");
  Serial.println("Settings Initialization");
  Serial.println("==============================");
  Serial.println("[INFO] Initializing EEPROM...");
  delay(1000);
  EEPROM.begin(512);
  // loadSettings();
  Serial.println("[SUCCESS] EEPROM Initialized.");
  Serial.printf("[INFO] EEPROM Size: %d bytes\n", 512);
  Serial.println("==============================\n");

  delay(1000);

  // --- SPIFFS ---
  Serial.println("\n==============================");
  Serial.println("SPIFFS Initialization");
  Serial.println("==============================");
  if (!SPIFFS.begin(true)) {
    // u8g2.drawStr(0, 18, "SPIFFS: ERROR");
    Serial.println("[ERROR] SPIFFS Mount Failed!");
    Serial.println("[INFO] Filesystem not available.");
    Serial.println("==============================\n");
  } else {
    // u8g2.drawStr(0, 18, "SPIFFS: OK");
    Serial.println("[SUCCESS] SPIFFS Mounted Successfully.");
    Serial.println("[INFO] Listing Files:");
    Serial.println("------------------------------");
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    int fileCount = 0;
    while (file) {
      Serial.printf("File %02d : %s  |  Size: %d bytes\n", fileCount + 1,
                    file.name(), file.size());
      fileCount++;
      file = root.openNextFile();
    }
    Serial.println("------------------------------");
    Serial.printf("Total Files: %d\n", fileCount);
    Serial.println("==============================\n");
    root.close();
  }

  // Load all settings from NVS
  loadSwitchSettings();
  loadWifiSettings();
  loadFbSettings();
  loadAdminSettings();
  initDefaultUser();
  Serial.println("[OK] Settings loaded from NVS");

  // AHT10 sensor
  Wire.begin();
  if (aht.begin()) {
    ahtOk = true;
    Serial.println("[OK] AHT10 sensor detected");
  } else {
    Serial.println("[WARN] AHT10 not detected");
  }

  // WiFi
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WIFI] Disconnected! ESP32 will auto-reconnect...");
      WiFi.reconnect();
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[WIFI] Reconnected! New IP: ");
      Serial.println(WiFi.localIP());
      pendingWebServerRestart = true; // Signal the loop to relaunch smoothly
      break;
    default:
      break;
    }
  });
  connectToSavedWiFi();
  delay(1000);
  // connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    Serial.println("[WIFI] Power save disabled for stable web server");
  }

  // Time sync
  if (WiFi.status() == WL_CONNECTED) {
    syncTime();
  }

  // Sunrise/Sunset
  calcSunriseSunset();

  // Web server
  if (WiFi.status() == WL_CONNECTED) {
    setupWebServer();
  }

  startFirebaseTask();

  Serial.println("==============================");
  Serial.println("  System Ready");
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("  IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.println("==============================\n");
}

//  LOOP
void loop() {

  // 1. SAFELY WRITE TO FLASH (Away from Web callbacks)
  for (int i = 0; i < NUM_SWITCHES; i++) {
    if (nvsRelayDirty[i]) {
      nvsRelayDirty[i] = false;
      prefs.begin("sw", false);
      prefs.putBool(("l" + String(i)).c_str(), readSwState(i));
      prefs.end();
    }
  }

  // 2. SAFELY RELAUNCH WEBSERVER AFTER WIFI RECONNECT
  if (pendingWebServerRestart) {
    pendingWebServerRestart = false;
    Serial.println("[SERVER] Relaunching WebServer after WiFi reconnect...");
    server.end();
    delay(100); // Give LwIP a moment to release ports
    setupWebServer();
    syncTime(); // Trigger a background time check just in case
    Serial.println("[SERVER] WebServer successfully relaunched.");
  }

  serviceNotifyStorage();
  updateBootButtonHoldState();

  // Physical switch input
  checkPhysicalSwitches();

  // Collect automation actions and resolve conflicts by configured priority.
  clearPendingAutomationActions();

  // Timer execution (volatile)
  checkTimers();

  // Schedule checks (every 15s)
  unsigned long now = millis();
  if (now - lastHeapLog >= HEAP_LOG_INTERVAL_MS) {
    lastHeapLog = now;
    Serial.printf("[HEAP] Free heap: %u bytes\n", ESP.getFreeHeap());
  }

  if (now - lastSchedCheck >= 15000) {
    lastSchedCheck = now;
    checkSchedules();
    calcSunriseSunset();
    checkRestartSchedule();
  }

  // Sensor checks (every 30s)
  if (now - lastSensorCheck >= 30000) {
    lastSensorCheck = now;
    checkSensors();
  }

  applyPendingAutomationActions();

  // WiFi portal request
  if (portalFlag) {
    portalFlag = false;
    server.end();
    if (!runWifiConfigPortal(true)) {
      restartFlag = true;
      restartAt = millis() + WIFI_PORTAL_RECOVERY_RESTART_MS;
    }
  }

  if (forgetWifiFlag && millis() >= forgetWifiAt) {
    forgetWifiFlag = false;
    WiFi.disconnect(true, true);
    ESP.restart();
  }

  if (factoryResetWifiClearPending && millis() >= factoryResetWifiClearAt) {
    factoryResetWifiClearPending = false;
    clearWifiCredentialsOnly();
    WiFi.disconnect(true, true);
  }

  // Critical heap failsafe
  if (!restartFlag && ESP.getFreeHeap() < CRITICAL_LOW_HEAP_RESTART_BYTES) {
    Serial.printf("[HEAP] Critical low heap %u bytes — scheduling restart\n",
                  ESP.getFreeHeap());
    restartFlag = true;
    restartAt = millis() + 1500;
  }

  // Restart request
  if (restartFlag && millis() >= restartAt) {
    ESP.restart();
  }
}