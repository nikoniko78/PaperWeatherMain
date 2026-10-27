#include <SD.h>
#include <M5Unified.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Update.h>
#include <SPIFFS.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <functional>
#include <vector>
#include "images.h"

namespace {

constexpr char kSetupSsid[] = "YAWOS-Setup";
constexpr char kSetupDomain[] = "yawos.home.arpa";
constexpr char kPreferencesNamespace[] = "weather";
constexpr uint16_t kDefaultRefreshIntervalMinutes = 15;
constexpr uint16_t kMinimumRefreshIntervalMinutes = 1;
constexpr uint16_t kMaximumRefreshIntervalMinutes = 1440;
constexpr size_t kPrimaryNetworks = 3;
constexpr size_t kMaximumNetworks = 50;
constexpr size_t kNetworkDropdownVisible = 3;
constexpr size_t kMaxNetworkEntries = 50;
constexpr uint32_t kSensorRefreshIntervalMs = 60UL * 1000UL;
constexpr uint32_t kClockSyncRetryIntervalMs = 1000UL;
constexpr uint32_t kWifiRetryIntervalMs = 30UL * 1000UL;
constexpr time_t kValidClockEpoch = 1700000000;
constexpr size_t kMaxForecastPeriods = 14;
constexpr size_t kForecastDays = 7;
constexpr size_t kMaximumCustomImages = 12;
constexpr uint8_t kSht30Address = 0x44;

enum class WeatherApiProvider : uint8_t {
  openMeteo,
  noaa,
  metNorway,
  wttr
};

Preferences preferences;
DNSServer dnsServer;
WebServer webServer(80);
WiFiUDP ntpUdp;
String savedSsids[kMaximumNetworks];
String savedPasswords[kMaximumNetworks];
String savedLocationName;
String savedLocationQuery;
String activeLocationQuery;
String resolvedLocationQuery;
String savedZipCode;
String savedState;
String savedNetworkLocations[kMaximumNetworks];
String setupPassword;
uint8_t savedNetworkCount = 0;   // total saved nets (>=3, <=kMaxNetworkEntries)
uint8_t selectedNetworkIndex = 0;  // index into savedNetworkCount for the dropdown
String lastWeatherError;
String calendarId;
String calendarApiKey;
String todayCalendarText;
uint32_t lastRefreshMs = 0;
uint32_t lastClockCheckMs = 0;
uint32_t lastClockSyncAttemptMs = 0;
uint32_t lastWifiRetryMs = 0;
int32_t locationUtcOffsetSeconds = 0;
bool utcOffsetKnown = false;
uint16_t adaptiveRefreshIntervalMinutes = 0;
uint16_t slowPollMinutes = 30;
uint16_t fastPollMinutes = 15;
bool sdCardReady = false;
bool useCelsius = false;
bool batterySaver = false;
bool isApprovedSdImage(const char* path);
bool pngDimensions(const char* path, uint32_t& width, uint32_t& height);
String customWeatherImage(const String& forecastText, bool isDaytime);
uint16_t refreshIntervalMinutes = kDefaultRefreshIntervalMinutes;
bool adaptiveUpdates = false;
bool nightlyUpdates = false;
uint8_t flashMode = 0;
WeatherApiProvider weatherApiProvider = WeatherApiProvider::openMeteo;
bool weatherApiChangedInSettings = false;
bool clockReady = false;
bool clockUpdatesEnabled = false;
bool ntpUdpReady = false;
bool locationLookupPending = false;
bool apiExpired = false;
bool wifiUnavailable = false;
bool showWeekForecast = false;
bool showDayDetails = false;
bool simpleMode = false;
bool nightShutdownEnabled = false;
uint16_t nightShutdownStartMinute = 22 * 60;
uint16_t nightShutdownEndMinute = 6 * 60;
bool useNightIcons = false;
uint8_t wifiRetryCount = 0;
size_t selectedForecastIndex = 1;
float savedLatitude = NAN;
float savedLongitude = NAN;
String nightIconChoice = "/night/dayicons.png";
String nightIconSetChoice = "/night/dayiconsn.png";
bool nightIconChoiceSaved = false;
bool nightIconSetChoiceSaved = false;
uint8_t nightIconSetSelected = 0;
float localTemperatureC = NAN;
float localHumidity = NAN;
String nightIconSetChoiceDay = "/night/dayicons.png";
String nightIconSetChoiceNight = "/night/dayiconsn.png";
uint32_t lastSensorReadMs = 0;

struct ForecastPeriod {
  struct Part {
    String shortForecast;
    String windSpeed;
    String windDirection;
    float temperature = NAN;
    int precipitation = -1;
    bool valid = false;
  };

  String name;
  String shortForecast;
  String windSpeed;
  String windDirection;
  String detailedForecast;
  Part morning;
  Part night;
  int temperature = 0;
  int highTemperature = 0;
  int lowTemperature = 0;
  float apparentTemperature = NAN;
  int humidity = -1;
  int precipitation = -1;
  bool isDaytime = false;
};

struct Forecast {
  String location;
  String updated;
  ForecastPeriod periods[kMaxForecastPeriods];
  size_t periodCount = 0;
  bool valid = false;
};

Forecast forecast;
Forecast forecastScratch;
Forecast liveForecastBeforeTest;
bool weatherTestMode = false;
String sunriseText;
String sunsetText;
bool sunTimesValid = false;
uint16_t sunriseMinuteOfDay = 0;
uint16_t sunsetMinuteOfDay = 0;
String selectedTimezone = "auto";
bool clockWaitScreenDrawn = false;
String liveLocationNameBeforeTest;
String liveSunriseBeforeTest;
String liveSunsetBeforeTest;
String liveCalendarTextBeforeTest;
bool liveSunTimesValidBeforeTest = false;
String testClockText;
float testLocalTemperatureC = NAN;
float testLocalHumidity = NAN;
int testBatteryPercent = -1;
int testRssi = -101;
bool testCharging = false;
float liveLocalTemperatureCBeforeTest = NAN;
float liveLocalHumidityBeforeTest = NAN;
String selectedCustomWeatherIcon;
bool bootSettingsRequested = false;
bool ignoreBootCClick = false;
String wifiConnectLines[12];
uint8_t wifiConnectLineCount = 0;
bool setupStatusScreenDrawn = false;

bool customFontLoaded = false;
float customFontScale = 1.0f;

float applyFontScale(float size) {
  return customFontLoaded ? size * customFontScale : size;
}

#define setTextSize(...) setTextSize(applyFontScale(__VA_ARGS__))

bool darkMode = false;
bool quietBackground = false;
bool quietBackgroundSetting = false;
bool instantLoad = false;
uint8_t darkPref = 0;

bool applyAutoDarkMode(bool daytime) {
  if (darkPref < 2) return false;
  const bool wantDark = sdCardReady && (darkPref == 2 ? !daytime : daytime);
  if (wantDark == darkMode) return false;
  darkMode = wantDark;
  preferences.putBool("dark_mode", darkMode);
  return true;
}
bool timedWakeBoot = false;

#define TFT_WHITE (darkMode ? 0x0000 : 0xFFFF)
#define TFT_BLACK (darkMode ? 0xFFFF : 0x0000)
#define TFT_LIGHTGREY (darkMode ? 0x2965 : 0xD69A)
#undef TFT_RED
#define TFT_RED (darkMode ? 0xFFFF : 0xF800)

bool touchTapped(const m5::touch_detail_t& detail) {
  return detail.wasClicked() || (detail.wasReleased() && !detail.wasFlicked());
}

const char* alertIconPath() {
  return isApprovedSdImage("/alerticon.png") ? "/alerticon.png"
                                              : "/extremeevent.png";
}

bool consumeBootCClick() {
  if (!ignoreBootCClick) return false;
  if (!M5.BtnC.isPressed()) {
    ignoreBootCClick = false;
    M5.BtnC.wasClicked();
  }
  return true;
}

String htmlEscape(const String& text) {
  String escaped;
  escaped.reserve(text.length());
  for (char ch : text) {
    switch (ch) {
      case '&': escaped += F("&amp;"); break;
      case '<': escaped += F("&lt;"); break;
      case '>': escaped += F("&gt;"); break;
      case '"': escaped += F("&quot;"); break;
      case '\'': escaped += F("&#39;"); break;
      default: escaped += ch; break;
    }
  }
  return escaped;
}

void drawMessage(const String& title, const String& message) {
  auto& display = M5.Display;
  setupStatusScreenDrawn = false;
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextSize(3);
  display.setCursor(32, 48);
  display.println(title);
  display.drawFastHLine(32, 100, display.width() - 64, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(32, 132);
  display.println(message);
  display.waitDisplay();
}

void showSetupStatus(const String& line) {
  if (quietBackground) return;
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  if (!setupStatusScreenDrawn) {
    display.setEpdMode(lgfx::epd_mode::epd_quality);
    display.clear(TFT_WHITE);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(4);
    display.drawString("Setting up Wi-Fi", display.width() / 2,
                       display.height() / 2 - 48);
    setupStatusScreenDrawn = true;
  } else {
    display.setEpdMode(lgfx::epd_mode::epd_text);
    display.fillRect(0, display.height() / 2 + 16, display.width(), 40,
                     TFT_WHITE);
  }
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(2);
  display.drawString(line, display.width() / 2, display.height() / 2 + 24);
  display.setTextDatum(textdatum_t::top_left);
  display.waitDisplay();
  display.setEpdMode(originalMode);
}

void drawWeatherIconFitted(const char* path, int32_t x, int32_t y,
                           int32_t maxWidth, int32_t maxHeight);

void drawLocationNeededScreen() {
  auto& display = M5.Display;
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(3);
  display.drawString("Location needed", display.width() / 2, 44);
  drawWeatherIconFitted(alertIconPath(), 24, 116,
                        display.width() - 48, display.height() - 280);
  display.setTextSize(2);
  display.drawString("Set a location in setup, then restart.",
                     display.width() / 2, display.height() - 116);
  display.setTextDatum(textdatum_t::top_left);
  display.waitDisplay();
}

String lowercase(String text) {
  text.toLowerCase();
  return text;
}

const char* weatherApiName(WeatherApiProvider provider) {
  switch (provider) {
    case WeatherApiProvider::openMeteo: return "Open-Meteo";
    case WeatherApiProvider::noaa: return "NOAA";
    case WeatherApiProvider::metNorway: return "MET Norway";
    case WeatherApiProvider::wttr: return "wttr.in";
  }
  return "Open-Meteo";
}

bool parseWeatherApiProvider(const String& value,
                             WeatherApiProvider& provider) {
  if (value == "open-meteo") {
    provider = WeatherApiProvider::openMeteo;
  } else if (value == "noaa") {
    provider = WeatherApiProvider::noaa;
  } else if (value == "met-norway") {
    provider = WeatherApiProvider::metNorway;
  } else if (value == "wttr") {
    provider = WeatherApiProvider::wttr;
  } else {
    return false;
  }
  return true;
}

const char* weatherApiPreferenceValue(WeatherApiProvider provider) {
  switch (provider) {
    case WeatherApiProvider::openMeteo: return "open-meteo";
    case WeatherApiProvider::noaa: return "noaa";
    case WeatherApiProvider::metNorway: return "met-norway";
    case WeatherApiProvider::wttr: return "wttr";
  }
  return "open-meteo";
}

bool parseClockOffsetHours(const String& value, int& hours) {
  if (value == "auto") return false;
  char* end = nullptr;
  const long parsed = strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0' || parsed < -12 || parsed > 14) {
    return false;
  }
  hours = static_cast<int>(parsed);
  return true;
}

bool validClockOffsetSelection(const String& value) {
  int hours = 0;
  return value == "auto" || parseClockOffsetHours(value, hours);
}

bool parseMinuteOfDay(const String& value, uint16_t& minuteOfDay) {
  if (value.length() != 5 || value[2] != ':') return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (i != 2 && (value[i] < '0' || value[i] > '9')) return false;
  }
  const int hour = value.substring(0, 2).toInt();
  const int minute = value.substring(3, 5).toInt();
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
  minuteOfDay = static_cast<uint16_t>(hour * 60 + minute);
  return true;
}

bool parseAmPmMinuteOfDay(String value, uint16_t& minuteOfDay) {
  value.trim();
  value.toUpperCase();
  if (value.length() != 8 || value[2] != ':' || value[5] != ' ' ||
      value[7] != 'M' || (value[6] != 'A' && value[6] != 'P') ||
      value[0] < '0' || value[0] > '9' ||
      value[1] < '0' || value[1] > '9' ||
      value[3] < '0' || value[3] > '9' ||
      value[4] < '0' || value[4] > '9') {
    return false;
  }
  const int hour12 = value.substring(0, 2).toInt();
  const int minute = value.substring(3, 5).toInt();
  if (hour12 < 1 || hour12 > 12 || minute > 59) return false;
  const int hour24 = (hour12 % 12) + (value[6] == 'P' ? 12 : 0);
  minuteOfDay = static_cast<uint16_t>(hour24 * 60 + minute);
  return true;
}

String formatAmPmMinuteOfDay(uint16_t minuteOfDay) {
  const unsigned hour24 = (minuteOfDay / 60) % 24;
  const unsigned hour12 = hour24 % 12 == 0 ? 12 : hour24 % 12;
  char value[9] = {
      static_cast<char>('0' + hour12 / 10),
      static_cast<char>('0' + hour12 % 10),
      ':',
      static_cast<char>('0' + (minuteOfDay % 60) / 10),
      static_cast<char>('0' + (minuteOfDay % 60) % 10),
      ' ',
      hour24 < 12 ? 'A' : 'P',
      'M',
      '\0'};
  return String(value);
}

bool parseTwelveHourTime(const String& value, bool isPm,
                         uint16_t& minuteOfDay) {
  if (value.length() != 4) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
  }
  const int hour12 = value.substring(0, 2).toInt();
  const int minute = value.substring(2, 4).toInt();
  if (hour12 < 1 || hour12 > 12 || minute > 59) return false;
  const int hour24 = (hour12 % 12) + (isPm ? 12 : 0);
  minuteOfDay = static_cast<uint16_t>(hour24 * 60 + minute);
  return true;
}

bool parseStrictFloat(const String& value, float& parsed) {
  if (value.isEmpty()) return false;
  char* end = nullptr;
  parsed = strtof(value.c_str(), &end);
  return end != value.c_str() && *end == '\0' && isfinite(parsed);
}

bool validTestTime(const String& value) {
  uint16_t minuteOfDay = 0;
  return parseMinuteOfDay(value, minuteOfDay);
}

bool nightShutdownIsActive() {
  if (!nightShutdownEnabled || !clockReady ||
      nightShutdownStartMinute == nightShutdownEndMinute) return false;
  int configuredOffsetHours = 0;
  if (!parseClockOffsetHours(selectedTimezone, configuredOffsetHours) &&
      !utcOffsetKnown) {
    return false;
  }
  const int32_t offsetSeconds =
      parseClockOffsetHours(selectedTimezone, configuredOffsetHours)
          ? configuredOffsetHours * 3600
          : locationUtcOffsetSeconds;
  const time_t localNow = time(nullptr) + offsetSeconds;
  int32_t secondsToday = localNow % 86400;
  if (secondsToday < 0) secondsToday += 86400;
  const uint16_t minuteOfDay = static_cast<uint16_t>(secondsToday / 60);
  if (nightShutdownStartMinute < nightShutdownEndMinute) {
    return minuteOfDay >= nightShutdownStartMinute &&
           minuteOfDay < nightShutdownEndMinute;
  }
  return minuteOfDay >= nightShutdownStartMinute ||
         minuteOfDay < nightShutdownEndMinute;
}

uint32_t secondsUntilNightShutdownEnd() {
  int configuredOffsetHours = 0;
  const int32_t offsetSeconds =
      parseClockOffsetHours(selectedTimezone, configuredOffsetHours)
          ? configuredOffsetHours * 3600
          : locationUtcOffsetSeconds;
  const time_t localNow = time(nullptr) + offsetSeconds;
  int32_t secondsToday = localNow % 86400;
  if (secondsToday < 0) secondsToday += 86400;
  const uint32_t endSeconds =
      static_cast<uint32_t>(nightShutdownEndMinute) * 60;
  if (endSeconds > static_cast<uint32_t>(secondsToday)) {
    return endSeconds - static_cast<uint32_t>(secondsToday);
  }
  return 86400U - static_cast<uint32_t>(secondsToday) + endSeconds;
}

bool parseRefreshIntervalMinutes(const String& value, uint16_t& minutes) {
  if (value.isEmpty()) return false;
  char* end = nullptr;
  const long parsed = strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0' ||
      parsed < kMinimumRefreshIntervalMinutes ||
      parsed > kMaximumRefreshIntervalMinutes) {
    return false;
  }
  minutes = static_cast<uint16_t>(parsed);
  return true;
}

bool batteryBelowTwentyPercent() {
  static uint32_t lastCheckMs = 0;
  static bool low = false;
  if (lastCheckMs == 0 || millis() - lastCheckMs > 30000UL) {
    lastCheckMs = millis() | 1;
    const int level = M5.Power.getBatteryLevel();
    low = level >= 0 && level < 20;
  }
  return low;
}

uint16_t effectiveRefreshIntervalMinutes() {
  if (batteryBelowTwentyPercent()) return 15;
  if (!adaptiveUpdates) return refreshIntervalMinutes;
  return adaptiveRefreshIntervalMinutes == 0
      ? slowPollMinutes : adaptiveRefreshIntervalMinutes;
}

uint32_t refreshIntervalMs() {
  return static_cast<uint32_t>(effectiveRefreshIntervalMinutes()) * 60UL *
         1000UL;
}

uint32_t secondsUntilNextPollSlot() {
  return static_cast<uint32_t>(effectiveRefreshIntervalMinutes()) * 60UL;
}

bool scheduledRefreshIsDue() {
  return clockReady && millis() - lastRefreshMs >= refreshIntervalMs();
}

String abbreviatedState(const String& value) {
  struct StateCode {
    const char* name;
    const char* code;
  };
  static const StateCode states[] = {
      {"Alabama", "AL"}, {"Alaska", "AK"}, {"Arizona", "AZ"},
      {"Arkansas", "AR"}, {"California", "CA"}, {"Colorado", "CO"},
      {"Connecticut", "CT"}, {"Delaware", "DE"}, {"Florida", "FL"},
      {"Georgia", "GA"}, {"Hawaii", "HI"}, {"Idaho", "ID"},
      {"Illinois", "IL"}, {"Indiana", "IN"}, {"Iowa", "IA"},
      {"Kansas", "KS"}, {"Kentucky", "KY"}, {"Louisiana", "LA"},
      {"Maine", "ME"}, {"Maryland", "MD"}, {"Massachusetts", "MA"},
      {"Michigan", "MI"}, {"Minnesota", "MN"}, {"Mississippi", "MS"},
      {"Missouri", "MO"}, {"Montana", "MT"}, {"Nebraska", "NE"},
      {"Nevada", "NV"}, {"New Hampshire", "NH"}, {"New Jersey", "NJ"},
      {"New Mexico", "NM"}, {"New York", "NY"},
      {"North Carolina", "NC"}, {"North Dakota", "ND"},
      {"Ohio", "OH"}, {"Oklahoma", "OK"}, {"Oregon", "OR"},
      {"Pennsylvania", "PA"}, {"Rhode Island", "RI"},
      {"South Carolina", "SC"}, {"South Dakota", "SD"},
      {"Tennessee", "TN"}, {"Texas", "TX"}, {"Utah", "UT"},
      {"Vermont", "VT"}, {"Virginia", "VA"}, {"Washington", "WA"},
      {"West Virginia", "WV"}, {"Wisconsin", "WI"}, {"Wyoming", "WY"},
      {"District of Columbia", "DC"},
  };
  String trimmed = value;
  trimmed.trim();
  for (const StateCode& state : states) {
    if (lowercase(trimmed) == lowercase(state.name)) return state.code;
  }
  if (trimmed.length() == 2) trimmed.toUpperCase();
  return trimmed;
}

String displayLocationName() {
  String location = forecast.location;
  location.trim();
  const int comma = location.lastIndexOf(',');
  if (comma >= 0) {
    const String state = location.substring(comma + 1);
    const String code = abbreviatedState(state);
    if (code != state) location = location.substring(0, comma + 1) + " " + code;
  }
  return location.isEmpty() ? "WEATHER" : location;
}

String updateRepo = "nikoniko78/PaperWeatherMain";
String updateBranch = "main";
bool autoUpdates = true;
uint32_t dataUsageMonth = 0;
uint64_t dataUsageBytes = 0;
uint32_t dataUsageUnsaved = 0;
constexpr char kDataUsagePath[] = "/datausage.txt";
uint8_t* loadedFontData = nullptr;
String activeFontPath;

String sdImageFile(const char* path) {
  return String("/images") + path;
}

bool parseRepoLocation(String input, String& repo, String& branch) {
  input.trim();
  if (input.isEmpty()) return false;
  input.replace("https://", "");
  input.replace("http://", "");
  if (input.startsWith("www.")) input.remove(0, 4);
  if (input.startsWith("github.com/")) input.remove(0, 11);
  while (input.startsWith("/")) input.remove(0, 1);
  const int hash = input.indexOf('#');
  if (hash >= 0) input.remove(hash);
  const int query = input.indexOf('?');
  if (query >= 0) input.remove(query);
  while (input.endsWith("/")) input.remove(input.length() - 1);
  if (input.endsWith(".git")) input.remove(input.length() - 4);
  const int firstSlash = input.indexOf('/');
  if (firstSlash <= 0) return false;
  const int secondSlash = input.indexOf('/', firstSlash + 1);
  String owner = input.substring(0, firstSlash);
  String name = secondSlash < 0 ? input.substring(firstSlash + 1)
                                : input.substring(firstSlash + 1, secondSlash);
  String nextBranch = "main";
  if (secondSlash >= 0) {
    String rest = input.substring(secondSlash + 1);
    if (rest.startsWith("tree/")) {
      rest.remove(0, 5);
      const int slash = rest.indexOf('/');
      nextBranch = slash < 0 ? rest : rest.substring(0, slash);
    }
  }
  if (name.endsWith(".git")) name.remove(name.length() - 4);
  if (owner.isEmpty() || name.isEmpty() || nextBranch.isEmpty()) return false;
  for (const String* part : {&owner, &name, &nextBranch}) {
    for (size_t i = 0; i < part->length(); ++i) {
      const char ch = (*part)[i];
      if (!isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_' &&
          ch != '.') {
        return false;
      }
    }
  }
  repo = owner + "/" + name;
  branch = nextBranch;
  return true;
}

uint32_t currentDataMonth() {
  if (!clockReady) return dataUsageMonth;
  const time_t now = time(nullptr);
  struct tm parts;
  gmtime_r(&now, &parts);
  return static_cast<uint32_t>((parts.tm_year + 1900) * 100 + parts.tm_mon + 1);
}

void saveDataUsage() {
  preferences.putUInt("data_month", dataUsageMonth);
  preferences.putULong64("data_bytes", dataUsageBytes);
  if (sdCardReady) {
    File file = SD.open(kDataUsagePath, FILE_WRITE);
    if (file) {
      file.printf("%lu %llu\n", static_cast<unsigned long>(dataUsageMonth),
                  static_cast<unsigned long long>(dataUsageBytes));
      file.close();
    }
  }
  dataUsageUnsaved = 0;
}

void noteDataUsage(size_t bytes) {
  const uint32_t month = currentDataMonth();
  if (month != 0 && month != dataUsageMonth) {
    dataUsageMonth = month;
    dataUsageBytes = 0;
  }
  dataUsageBytes += bytes;
  dataUsageUnsaved += bytes;
  if (dataUsageUnsaved >= 40000) saveDataUsage();
}

void loadDataUsage() {
  dataUsageMonth = preferences.getUInt("data_month", 0);
  dataUsageBytes = preferences.getULong64("data_bytes", 0);
  if (sdCardReady && SD.exists(kDataUsagePath)) {
    File file = SD.open(kDataUsagePath, FILE_READ);
    if (file) {
      const String line = file.readStringUntil('\n');
      file.close();
      const int space = line.indexOf(' ');
      if (space > 0) {
        const uint32_t month = static_cast<uint32_t>(line.substring(0, space).toInt());
        const uint64_t bytes = strtoull(line.substring(space + 1).c_str(), nullptr, 10);
        if (month > dataUsageMonth ||
            (month == dataUsageMonth && bytes > dataUsageBytes)) {
          dataUsageMonth = month;
          dataUsageBytes = bytes;
        }
      }
    }
  }
  const uint32_t month = currentDataMonth();
  if (month != 0 && month != dataUsageMonth) {
    dataUsageMonth = month;
    dataUsageBytes = 0;
  }
}

String formatDataUsage() {
  const double megabytes = static_cast<double>(dataUsageBytes) / 1048576.0;
  if (megabytes < 1.0) {
    return String(static_cast<double>(dataUsageBytes) / 1024.0, 0) + " KB";
  }
  return String(megabytes, 2) + " MB";
}

void unloadCustomFont() {
  if (customFontLoaded) M5.Display.unloadFont();
  customFontLoaded = false;
  customFontScale = 1.0f;
  if (loadedFontData != nullptr) {
    free(loadedFontData);
    loadedFontData = nullptr;
  }
}

bool loadCustomFont(const String& path) {
  unloadCustomFont();
  if (!sdCardReady || path.isEmpty() || !SD.exists(path.c_str())) return false;
  File file = SD.open(path.c_str(), FILE_READ);
  if (!file) return false;
  const size_t size = file.size();
  if (size < 64 || size > 1800000) {
    file.close();
    return false;
  }
  uint8_t* data = static_cast<uint8_t*>(ps_malloc(size));
  if (data == nullptr) data = static_cast<uint8_t*>(malloc(size));
  if (data == nullptr) {
    file.close();
    return false;
  }
  const size_t got = file.read(data, size);
  file.close();
  if (got != size || !M5.Display.loadFont(data)) {
    free(data);
    M5.Display.unloadFont();
    return false;
  }
  loadedFontData = data;
  M5.Display.setTextSize(1);
  const int32_t height = M5.Display.fontHeight();
  const char* sample = "Weather Forecast 0123456789 Humidity Sunrise";
  const int32_t sampleWidth = M5.Display.textWidth(sample);
  const float heightScale = height > 0 ? 8.0f / static_cast<float>(height) : 1.0f;
  const float widthScale = sampleWidth > 0
      ? 6.0f * static_cast<float>(strlen(sample)) / static_cast<float>(sampleWidth)
      : 1.0f;
  customFontScale = min(widthScale, heightScale * 1.3f);
  customFontLoaded = true;
  activeFontPath = path;
  return true;
}

bool configureSecureRequest(NetworkClientSecure& client, HTTPClient& http,
                            const String& url) {
  if (!clockReady) {
    lastWeatherError = "Device clock is not synchronized; HTTPS is unavailable.";
    return false;
  }
  client.useBuiltinCACertBundle();
  client.setHandshakeTimeout(8);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setReuse(false);
  http.setUserAgent("YAWOS/1.0 (M5Paper; local personal weather display)");
  if (!http.begin(client, url)) {
    lastWeatherError = "Could not start HTTPS request.";
    Serial.println(lastWeatherError);
    return false;
  }
  return true;
}

String urlEncode(const String& value) {
  static const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (uint8_t ch : value) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
        ch == '.' || ch == '~') {
      encoded += static_cast<char>(ch);
    } else {
      encoded += '%';
      encoded += hex[ch >> 4];
      encoded += hex[ch & 0x0f];
    }
  }
  return encoded;
}

String defaultLocationQuery() {
  if (!savedLocationQuery.isEmpty()) return savedLocationQuery;
  if (!savedZipCode.isEmpty()) {
    return savedState.isEmpty() ? savedZipCode
                                : savedZipCode + ", " + savedState;
  }
  return "";
}

bool parseCoordinates(const String& input, float& latitude, float& longitude) {
  const char* cursor = input.c_str();
  char* end = nullptr;
  latitude = strtof(cursor, &end);
  if (end == cursor) return false;
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != ',') return false;
  cursor = end + 1;
  while (*cursor == ' ' || *cursor == '\t') ++cursor;
  longitude = strtof(cursor, &end);
  if (end == cursor) return false;
  while (*end == ' ' || *end == '\t') ++end;
  return *end == '\0' && isfinite(latitude) && isfinite(longitude) &&
         latitude >= -90.0f && latitude <= 90.0f &&
         longitude >= -180.0f && longitude <= 180.0f;
}

bool hasLocationConfiguration() {
  return !activeLocationQuery.isEmpty() ||
         !defaultLocationQuery().isEmpty() ||
         (isfinite(savedLatitude) && isfinite(savedLongitude));
}

bool getJson(const String& url, JsonDocument& document);

String weatherDescription(int code) {
  if (code == 0) return "Clear sky";
  if (code == 1) return "Mostly clear";
  if (code == 2) return "Partly cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code == 51) return "Light drizzle";
  if (code == 53) return "Drizzle";
  if (code == 55) return "Heavy drizzle";
  if (code == 56) return "Light freezing drizzle";
  if (code == 57) return "Heavy freezing drizzle";
  if (code == 61) return "Light rain";
  if (code == 63) return "Rain";
  if (code == 65) return "Heavy rain";
  if (code == 66) return "Light freezing rain";
  if (code == 67) return "Heavy freezing rain";
  if (code == 77) return "Snow grains";
  if (code >= 71 && code <= 77) return "Snow";
  if (code == 80) return "Light rain showers";
  if (code == 81) return "Rain showers";
  if (code == 82) return "Heavy rain showers";
  if (code == 85) return "Light snow showers";
  if (code == 86) return "Heavy snow showers";
  if (code == 95) return "Thunderstorm";
  if (code == 96) return "Thunderstorm with light hail";
  if (code == 99) return "Thunderstorm with heavy hail";
  return "Forecast unavailable";
}

String windDirectionText(int degrees);

void clearForecastPart(ForecastPeriod::Part& part) {
  part.shortForecast = "";
  part.windSpeed = "";
  part.windDirection = "";
  part.temperature = NAN;
  part.precipitation = -1;
  part.valid = false;
}

void clearForecast(Forecast& forecastToClear) {
  forecastToClear.location = "";
  forecastToClear.updated = "";
  forecastToClear.periodCount = 0;
  forecastToClear.valid = false;
  for (size_t i = 0; i < kMaxForecastPeriods; ++i) {
    ForecastPeriod& period = forecastToClear.periods[i];
    period.name = "";
    period.shortForecast = "";
    period.windSpeed = "";
    period.windDirection = "";
    period.detailedForecast = "";
    clearForecastPart(period.morning);
    clearForecastPart(period.night);
    period.temperature = 0;
    period.highTemperature = 0;
    period.lowTemperature = 0;
    period.apparentTemperature = NAN;
    period.humidity = -1;
    period.precipitation = -1;
    period.isDaytime = false;
  }
}

void populateForecastPart(ForecastPeriod::Part& part, const String& date,
                         int firstHour, int lastHour, int targetHour,
                         JsonArray times, JsonArray temperatures,
                         JsonArray codes, JsonArray descriptions,
                         JsonArray precipitation,
                         JsonArray windSpeeds, JsonArray windDirections) {
  int bestDistance = 24;
  for (size_t i = 0; i < times.size(); ++i) {
    const String time = times[i] | "";
    if (time.length() < 13 || time.substring(0, 10) != date) continue;
    const int hour = time.substring(11, 13).toInt();
    if (hour < firstHour || hour > lastHour ||
        i >= temperatures.size() || i >= codes.size()) {
      continue;
    }
    const int distance = abs(hour - targetHour);
    if (distance >= bestDistance) continue;
    const float temperature = temperatures[i] | NAN;
    if (!isfinite(temperature)) continue;

    bestDistance = distance;
    part.temperature = temperature;
    part.shortForecast = weatherDescription(codes[i] | -1);
    if (!descriptions.isNull() && i < descriptions.size()) {
      part.shortForecast = descriptions[i] | part.shortForecast;
    }
    part.precipitation = i < precipitation.size()
        ? precipitation[i] | -1 : -1;
    const float windSpeed = i < windSpeeds.size()
        ? windSpeeds[i] | NAN : NAN;
    part.windSpeed = isfinite(windSpeed)
        ? String(windSpeed, 0) + " mph" : "";
    part.windDirection = i < windDirections.size()
        ? windDirectionText(windDirections[i] | 0) : "";
    part.valid = true;
  }
}

String windDirectionText(int degrees) {
  static const char* directions[] = {
      "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
      "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
  return directions[((degrees + 11) / 22) % 16];
}

int windDirectionDegrees(const String& direction) {
  static const char* directions[] = {
      "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
      "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
  for (size_t i = 0; i < sizeof(directions) / sizeof(directions[0]); ++i) {
    if (direction.equalsIgnoreCase(directions[i])) {
      return static_cast<int>(i * 22.5f);
    }
  }
  return 315;
}

String formatLocalTime(const String& isoTime) {
  if (isoTime.length() < 16 || isoTime[10] != 'T') return "";
  const int hour = isoTime.substring(11, 13).toInt();
  const int minute = isoTime.substring(14, 16).toInt();
  if (hour > 23 || minute > 59) return "";
  const int hour12 = hour % 12 == 0 ? 12 : hour % 12;
  char formatted[24];
  snprintf(formatted, sizeof(formatted), "%d:%02d %s", hour12, minute,
           hour < 12 ? "AM" : "PM");
  return formatted;
}

bool parseIsoMinuteOfDay(const String& isoTime, uint16_t& minuteOfDay) {
  if (isoTime.length() < 16 || isoTime[10] != 'T') return false;
  const int hour = isoTime.substring(11, 13).toInt();
  const int minute = isoTime.substring(14, 16).toInt();
  if (hour > 23 || minute > 59) return false;
  minuteOfDay = static_cast<uint16_t>(hour * 60 + minute);
  return true;
}

String formatUpdatedTime(const String& isoTime) {
  if (isoTime.length() < 16 || isoTime[10] != 'T') return isoTime;
  const String date = isoTime.substring(0, 10);
  const String time = formatLocalTime(isoTime);
  if (time.isEmpty()) return isoTime;
  return date + " " + time;
}

String forecastDayName(const String& date, size_t dayOffset) {
  if (dayOffset == 0) return "Today";
  if (dayOffset == 1) return "Tomorrow";
  if (date.length() < 10) return "Day " + String(dayOffset + 1);
  tm forecastDate = {};
  forecastDate.tm_year = date.substring(0, 4).toInt() - 1900;
  forecastDate.tm_mon = date.substring(5, 7).toInt() - 1;
  forecastDate.tm_mday = date.substring(8, 10).toInt();
  forecastDate.tm_isdst = -1;
  const time_t epoch = mktime(&forecastDate);
  if (epoch < 0) return "Day " + String(dayOffset + 1);
  localtime_r(&epoch, &forecastDate);
  char label[12];
  strftime(label, sizeof(label), "%A", &forecastDate);
  return String(label);
}

size_t selectNetworkIndex(size_t requested) {
  if (savedNetworkCount == 0) return 0;
  if (requested >= savedNetworkCount) return savedNetworkCount - 1;
  return requested;
}

size_t findSavedNetworkIndex(String ssid) {
  for (size_t i = 0; i < savedNetworkCount && i < kMaximumNetworks; ++i) {
    if (savedSsids[i] == ssid) return i;
  }
  return kMaximumNetworks;
}

void assignNetworkSelection() {
  if (savedNetworkCount < 3) savedNetworkCount = 3;
  if (savedNetworkCount > kMaxNetworkEntries) savedNetworkCount = kMaxNetworkEntries;
  if (savedSsids[selectedNetworkIndex].isEmpty()) {
    selectedNetworkIndex = 0;
  }
  if (selectedNetworkIndex >= savedNetworkCount) {
    selectedNetworkIndex = savedNetworkCount - 1;
  }
}

void selectLocationForConnectedNetwork() {
  const String connectedSsid = WiFi.SSID();
  for (size_t i = 0; i < kMaximumNetworks; ++i) {
    if (connectedSsid == savedSsids[i] && !savedNetworkLocations[i].isEmpty()) {
      activeLocationQuery = savedNetworkLocations[i];
      break;
    }
  }
  if (activeLocationQuery != resolvedLocationQuery) {
    savedLatitude = NAN;
    savedLongitude = NAN;
    locationLookupPending = !activeLocationQuery.isEmpty();
    forecast.valid = false;
    forecast.periodCount = 0;
    clockUpdatesEnabled = false;
  }
}

bool resolveSavedLocation() {
  if (!locationLookupPending) {
    if (isfinite(savedLatitude) && isfinite(savedLongitude)) return true;
    lastWeatherError = "No location is configured and public-IP lookup failed.";
    return false;
  }
  if (activeLocationQuery.isEmpty()) {
    lastWeatherError = "No location is available for this Wi-Fi network.";
    return false;
  }

  float coordinateLatitude;
  float coordinateLongitude;
  if (parseCoordinates(activeLocationQuery, coordinateLatitude,
                       coordinateLongitude)) {
    savedLatitude = coordinateLatitude;
    savedLongitude = coordinateLongitude;
    savedLocationName = activeLocationQuery;
    resolvedLocationQuery = activeLocationQuery;
    locationUtcOffsetSeconds = 0;
    sunTimesValid = false;
    preferences.putFloat("latitude", savedLatitude);
    preferences.putFloat("longitude", savedLongitude);
    preferences.putString("location", savedLocationName);
    locationLookupPending = false;
    preferences.putBool("geocode_pending", false);
    return true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    lastWeatherError = "Wi-Fi disconnected before location lookup.";
    return false;
  }
  const String url = "http://geocoding-api.open-meteo.com/v1/search?name=" +
      urlEncode(activeLocationQuery) + "&count=1&language=en&format=json";
  JsonDocument results;
  if (!getJson(url, results)) return false;
  JsonArray matches = results["results"].as<JsonArray>();
  if (matches.isNull() || matches.size() == 0) {
    lastWeatherError = "No city or place matched that search.";
    return false;
  }

  const float latitude = matches[0]["latitude"] | NAN;
  const float longitude = matches[0]["longitude"] | NAN;
  if (!isfinite(latitude) || !isfinite(longitude) ||
      latitude < -90.0f || latitude > 90.0f ||
      longitude < -180.0f || longitude > 180.0f) {
    lastWeatherError = "Location lookup returned invalid coordinates.";
    return false;
  }

  savedLatitude = latitude;
  savedLongitude = longitude;
  savedLocationName = matches[0]["name"] | activeLocationQuery;
  resolvedLocationQuery = activeLocationQuery;
  const String region = matches[0]["admin1"] | "";
  const String country = matches[0]["country_code"] | "";
  if (!region.isEmpty()) savedLocationName += ", " + region;
  if (country != "US" && !country.isEmpty()) {
    savedLocationName += ", " + country;
  }
  locationUtcOffsetSeconds = 0;
  sunTimesValid = false;
  preferences.putFloat("latitude", savedLatitude);
  preferences.putFloat("longitude", savedLongitude);
  preferences.putString("location", savedLocationName);
  locationLookupPending = false;
  preferences.putBool("geocode_pending", false);
  return true;
}

// Returns true when the current local time is between sunset and sunrise.
bool isNightTime() {
  if (!sunTimesValid) return false;
  int configuredOffsetHours = 0;
  const int32_t offsetSeconds =
      parseClockOffsetHours(selectedTimezone, configuredOffsetHours)
          ? configuredOffsetHours * 3600
          : locationUtcOffsetSeconds;
  const time_t localNow = time(nullptr) + offsetSeconds;
  int32_t secondsToday = localNow % 86400;
  if (secondsToday < 0) secondsToday += 86400;
  const uint16_t minuteOfDay = static_cast<uint16_t>(secondsToday / 60);
  // Night: from sunset to sunrise (wraps past midnight)
  return minuteOfDay >= sunsetMinuteOfDay || minuteOfDay < sunriseMinuteOfDay;
}

const char* weatherIconRaw(const String& forecastText, bool isDaytime) {
  const String text = lowercase(forecastText);
  selectedCustomWeatherIcon =
      customWeatherImage(forecastText, isDaytime || !useNightIcons);
  if (!selectedCustomWeatherIcon.isEmpty()) {
    return selectedCustomWeatherIcon.c_str();
  }
  const bool night = useNightIcons && !isDaytime;
  if (text.indexOf("tornado") >= 0)
    return night ? "/tornadonight.png" : "/tornado.png";
  if (text.indexOf("hurricane") >= 0 || text.indexOf("tropical") >= 0)
    return night ? "/hurricanenight.png" : "/hurricane.png";
  if (text.indexOf("thunder") >= 0 || text.indexOf("hail") >= 0)
    return night ? "/thunderstormnight.png" : "/thunderstorm.png";
  if (text.indexOf("snow") >= 0 || text.indexOf("flurr") >= 0 ||
      text.indexOf("sleet") >= 0 || text.indexOf("ice") >= 0)
    return night ? "/snownight.png" : "/snow.png";
  if (text.indexOf("rain") >= 0 || text.indexOf("drizzle") >= 0 ||
      text.indexOf("shower") >= 0)
    return night ? "/rainnight.png" : "/rain.png";
  if (text.indexOf("fog") >= 0 || text.indexOf("haze") >= 0)
    return night ? "/fognight.png" : "/fog.png";
  if (text.indexOf("wind") >= 0 || text.indexOf("squall") >= 0)
    return night ? "/windnight.png" : "/wind.png";
  if (text.indexOf("dust") >= 0 || text.indexOf("smoke") >= 0)
    return night ? "/fognight.png" : "/fog.png";
  if (text.indexOf("partly") >= 0 || text.indexOf("mostly cloudy") >= 0 ||
      text.indexOf("few clouds") >= 0)
    return night ? "/partialcloudynight.png" : "/partialclouds.png";
  if (text.indexOf("overcast") >= 0 &&
      isApprovedSdImage(night ? "/overcastnight.png" : "/overcast.png")) {
    return night ? "/overcastnight.png" : "/overcast.png";
  }
  if (text.indexOf("cloud") >= 0 || text.indexOf("overcast") >= 0)
    return night ? "/cloudynight.png" : "/cloudy.png";
  if (text.indexOf("clear") >= 0 || text.indexOf("sun") >= 0 ||
      text.indexOf("mostly clear") >= 0) {
    return night ? "/clearnight.png" : "/sunny.png";
  }
  return night ? "/cloudynight.png" : "/cloudy.png";
}

const char* weatherIcon(const String& forecastText, bool isDaytime = true) {
  const char* result = weatherIconRaw(forecastText, isDaytime);
  if (isDaytime || !useNightIcons || !selectedCustomWeatherIcon.isEmpty()) {
    return result;
  }
  const String dayPath = weatherIconRaw(forecastText, true);
  selectedCustomWeatherIcon = "";
  static String prefixedNightIcon;
  prefixedNightIcon = String("/night") + (dayPath.c_str() + 1);
  if (sdCardReady && isApprovedSdImage(prefixedNightIcon.c_str())) {
    return prefixedNightIcon.c_str();
  }
  return result;
}

String unmappedWeatherIconHint(const String& forecastText, bool isDaytime) {
  String text = lowercase(forecastText);
  text.trim();
  if (text.isEmpty() || text == "forecast unavailable" ||
      !customWeatherImage(forecastText, isDaytime).isEmpty()) {
    return "";
  }
  const bool knownCondition =
      text.indexOf("tornado") >= 0 || text.indexOf("hurricane") >= 0 ||
      text.indexOf("tropical") >= 0 || text.indexOf("thunder") >= 0 ||
      text.indexOf("hail") >= 0 || text.indexOf("snow") >= 0 ||
      text.indexOf("flurr") >= 0 || text.indexOf("sleet") >= 0 ||
      text.indexOf("ice") >= 0 || text.indexOf("rain") >= 0 ||
      text.indexOf("drizzle") >= 0 || text.indexOf("shower") >= 0 ||
      text.indexOf("fog") >= 0 || text.indexOf("haze") >= 0 ||
      text.indexOf("dust") >= 0 || text.indexOf("smoke") >= 0 ||
      text.indexOf("wind") >= 0 || text.indexOf("squall") >= 0 ||
      text.indexOf("partly") >= 0 || text.indexOf("cloud") >= 0 ||
      text.indexOf("overcast") >= 0 || text.indexOf("clear") >= 0 ||
      text.indexOf("sun") >= 0 || text.indexOf("mist") >= 0;
  if (knownCondition) return "";

  String filename;
  bool previousUnderscore = false;
  for (size_t i = 0; i < text.length() && filename.length() < 36; ++i) {
    const char ch = text[i];
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      filename += ch;
      previousUnderscore = false;
    } else if (!filename.isEmpty() && !previousUnderscore) {
      filename += '_';
      previousUnderscore = true;
    }
  }
  while (filename.endsWith("_")) filename.remove(filename.length() - 1);
  if (filename.isEmpty()) return "";
  return "Make an icon for " + filename + ".png";
}

bool getJson(const String& url, JsonDocument& document) {
  if (!clockReady) {
    lastWeatherError = "Device clock is not synchronized.";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    lastWeatherError = "Wi-Fi disconnected before API request.";
    return false;
  }
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setReuse(false);
  http.setUserAgent("YAWOS/1.0 (M5Paper; local personal weather display)");
  if (!http.begin(client, url)) {
    lastWeatherError = "Could not start HTTP request.";
    Serial.println(lastWeatherError);
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");

  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    if (status == HTTP_CODE_GONE) apiExpired = true;
    String details;
    if (status <= 0) {
      details = http.errorToString(status);
    }
    lastWeatherError = "API HTTP " + String(status);
    if (!details.isEmpty()) lastWeatherError += ": " + details;
    Serial.printf("%s\n", lastWeatherError.c_str());
    http.end();
    return false;
  }

  const String responseBody = http.getString();
  noteDataUsage(responseBody.length() + 900);
  const DeserializationError error = deserializeJson(document, responseBody);
  http.end();
  if (error) {
    lastWeatherError = "Could not parse API response: ";
    lastWeatherError += error.c_str();
    Serial.printf("%s\n", lastWeatherError.c_str());
    Serial.printf("API response preview (%u bytes): %.120s\n",
                  static_cast<unsigned>(responseBody.length()),
                  responseBody.c_str());
    return false;
  }
  return true;
}

bool getSecureJson(const String& url, JsonDocument& document,
                   String& errorMessage) {
  NetworkClientSecure client;
  HTTPClient http;
  if (!configureSecureRequest(client, http, url)) {
    errorMessage = lastWeatherError;
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    errorMessage = "Calendar API HTTP " + String(status);
    if (status <= 0) errorMessage += ": " + http.errorToString(status);
    http.end();
    return false;
  }
  const String responseBody = http.getString();
  noteDataUsage(responseBody.length() + 6500);
  JsonDocument listingFilter;
  const bool filtered = url.indexOf("/contents/") >= 0;
  if (filtered) {
    listingFilter[0]["name"] = true;
    listingFilter[0]["type"] = true;
  }
  const DeserializationError parseError =
      filtered ? deserializeJson(document, responseBody,
                                 DeserializationOption::Filter(listingFilter))
               : deserializeJson(document, responseBody);
  http.end();
  if (parseError) {
    errorMessage = "Could not parse Calendar API response: ";
    errorMessage += parseError.c_str();
    return false;
  }
  return true;
}

struct FirmwareVersion {
  bool valid = false;
  bool nightly = false;
  bool unusable = false;
  bool hasInfo = false;
  bool denyInstall = false;
  uint32_t parts[4] = {};
};

FirmwareVersion parseFirmwareVersion(String value) {
  FirmwareVersion version;
  value.trim();
  size_t index = 0;
  while (index < value.length()) {
    const char prefix = static_cast<char>(tolower(value[index]));
    if (prefix == 'n') {
      version.nightly = true;
    } else if (prefix == 'u') {
      version.unusable = true;
    } else if (prefix == 'd') {
      if (index + 1 >= value.length()) break;
      const char choice = static_cast<char>(tolower(value[index + 1]));
      if (choice != 'n' && choice != 'y') break;
      version.hasInfo = true;
      version.denyInstall = choice == 'n';
      ++index;
    } else if (prefix != 'v') {
      break;
    }
    ++index;
  }
  uint32_t current = 0;
  bool haveDigits = false;
  size_t count = 0;
  for (; index < value.length(); ++index) {
    const char ch = value[index];
    if (ch >= '0' && ch <= '9') {
      if (current > 99999999UL) return version;
      current = current * 10 + static_cast<uint32_t>(ch - '0');
      haveDigits = true;
    } else if (ch == '.' && haveDigits) {
      if (count >= 4) return version;
      version.parts[count++] = current;
      current = 0;
      haveDigits = false;
    } else {
      break;
    }
  }
  if (haveDigits) {
    if (count >= 4) return version;
    version.parts[count++] = current;
  }
  version.valid = count >= 1;
  return version;
}

int compareFirmwareVersions(const String& a, const String& b) {
  const FirmwareVersion first = parseFirmwareVersion(a);
  const FirmwareVersion second = parseFirmwareVersion(b);
  if (!first.valid && !second.valid) return 0;
  if (!first.valid) return -1;
  if (!second.valid) return 1;
  for (size_t i = 0; i < 4; ++i) {
    if (first.parts[i] != second.parts[i]) {
      return first.parts[i] > second.parts[i] ? 1 : -1;
    }
  }
  if (first.nightly == second.nightly) return 0;
  return first.nightly ? -1 : 1;
}

String firmwareDisplayVersion(const String& folder) {
  size_t index = 0;
  while (index < folder.length() &&
         (folder[index] < '0' || folder[index] > '9')) {
    ++index;
  }
  const String version = folder.substring(index);
  return version == "0.0.0" ? String("Installer") : version;
}

bool isInstallerFolder(const String& folder) {
  return firmwareDisplayVersion(folder) == "Installer";
}

constexpr size_t kMaxFirmwareEntries = 96;
String firmwareFolders[kMaxFirmwareEntries];
size_t firmwareFolderCount = 0;
String allFirmwareFolders[kMaxFirmwareEntries];
size_t allFirmwareFolderCount = 0;
String newestEligibleFirmware;

bool firmwareNetworkReady() {
  return clockReady && WiFi.status() == WL_CONNECTED;
}

bool useDirectFlash() {
  return flashMode == 1 || (flashMode == 0 && !sdCardReady);
}

bool updateStorageReady() {
  return flashMode != 2 || sdCardReady;
}

bool firmwareAutoEligible(const FirmwareVersion& version) {
  return version.valid && !version.unusable && !version.denyInstall &&
         (!version.nightly || nightlyUpdates);
}

void updateNewestEligibleFirmware() {
  newestEligibleFirmware = "";
  for (size_t i = 0; i < firmwareFolderCount; ++i) {
    if (firmwareAutoEligible(parseFirmwareVersion(firmwareFolders[i]))) {
      newestEligibleFirmware = firmwareFolders[i];
      break;
    }
  }
}

void applyFirmwareVisibility() {
  firmwareFolderCount = 0;
  for (size_t i = 0; i < allFirmwareFolderCount; ++i) {
    const FirmwareVersion version = parseFirmwareVersion(allFirmwareFolders[i]);
    if (version.nightly && !nightlyUpdates) continue;
    firmwareFolders[firmwareFolderCount++] = allFirmwareFolders[i];
  }
  updateNewestEligibleFirmware();
}

String stagedUpdatePath(const String& folder) {
  return String("/update-") + folder + ".bin";
}

bool fetchFirmwareList(String& error) {
  JsonDocument listing;
  const String url = String("https://api.github.com/repos/") + updateRepo +
                     "/contents/updates?ref=" + updateBranch;
  if (!getSecureJson(url, listing, error)) {
    error.replace("Calendar API", "Update server");
    return false;
  }
  JsonArray items = listing.as<JsonArray>();
  if (items.isNull()) {
    error = String(listing["message"] | "Unexpected update response");
    return false;
  }
  String found[kMaxFirmwareEntries];
  size_t count = 0;
  for (JsonVariant item : items) {
    if (String(item["type"] | "") != "dir") continue;
    const String name = item["name"] | "";
    if (!parseFirmwareVersion(name).valid) continue;
    size_t position = count;
    while (position > 0 &&
           compareFirmwareVersions(name, found[position - 1]) > 0) {
      --position;
    }
    if (position >= kMaxFirmwareEntries) continue;
    const size_t last =
        count < kMaxFirmwareEntries ? count : kMaxFirmwareEntries - 1;
    for (size_t j = last; j > position; --j) found[j] = found[j - 1];
    found[position] = name;
    if (count < kMaxFirmwareEntries) ++count;
  }
  allFirmwareFolderCount = count;
  for (size_t i = 0; i < count; ++i) allFirmwareFolders[i] = found[i];
  applyFirmwareVisibility();
  const String installed = preferences.getString("fw_version", "0.0.0");
  if (!newestEligibleFirmware.isEmpty() &&
      compareFirmwareVersions(installed, newestEligibleFirmware) >= 0) {
    preferences.putBool("test_mode", false);
  }
  return true;
}

void drawForecast(const String& status);
void drawWeekForecast();

void drawUpdateBar(int32_t y, const char* label, int percent) {
  auto& display = M5.Display;
  const int32_t left = 40;
  const int32_t width = display.width() - 80;
  display.fillRect(left - 2, y - 2, width + 4, 90, TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextSize(2);
  display.setTextDatum(textdatum_t::top_left);
  display.drawString(label, left, y);
  display.setTextDatum(textdatum_t::top_right);
  display.drawString(String(percent) + "%", left + width, y);
  display.drawRect(left, y + 32, width, 36, TFT_BLACK);
  display.fillRect(left + 3, y + 35, (width - 6) * percent / 100, 30,
                   TFT_BLACK);
  display.setTextDatum(textdatum_t::top_left);
}

void drawUpdateScreen(const String& version, bool changingVersion) {
  auto& display = M5.Display;
  setupStatusScreenDrawn = false;
  display.setEpdMode(lgfx::epd_mode::epd_quality);
  display.startWrite();
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(4);
  display.drawString(changingVersion ? "Changing Version..."
                                       : "An update was found!",
                     display.width() / 2, 200);
  display.setTextSize(2);
  display.drawString("Version " + firmwareDisplayVersion(version),
                     display.width() / 2, 280);
  display.drawString("Do not power off the device.", display.width() / 2,
                     330);
  drawUpdateBar(430, "Downloading", 0);
  drawUpdateBar(550, "Installing", 0);
  display.setTextDatum(textdatum_t::top_left);
  display.endWrite();
  display.waitDisplay();
  display.setEpdMode(lgfx::epd_mode::epd_fast);
}

void drawCheckingUpdatesScreen() {
  showSetupStatus("Checking for updates...");
}

void promptNoSdForUpdate(const String& version) {
  quietBackground = false;
  auto& display = M5.Display;
  setupStatusScreenDrawn = false;
  display.setEpdMode(lgfx::epd_mode::epd_quality);
  display.startWrite();
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(4);
  display.drawString("An update was found!", display.width() / 2, 200);
  display.setTextSize(2);
  display.drawString("Version " + firmwareDisplayVersion(version),
                     display.width() / 2, 280);
  display.setTextSize(3);
  display.drawString("No SD to safely update", display.width() / 2, 400);
  display.setTextSize(2);
  display.drawString("Insert an SD card, or set Direct", display.width() / 2,
                     470);
  display.drawString("flash to AUTO or ON in Updates manager.",
                     display.width() / 2, 500);
  display.setTextDatum(textdatum_t::top_left);
  display.endWrite();
  display.waitDisplay();
  delay(6000);
}

void cleanupStagedUpdates() {
  if (!sdCardReady) return;
  SD.remove("/update.bin");
  const String installed = preferences.getString("fw_version", "0.0.0");
  String stale[8];
  size_t staleCount = 0;
  File root = SD.open("/");
  if (!root) return;
  for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (entry.isDirectory()) continue;
    String name = entry.name();
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.startsWith("update-") || !name.endsWith(".bin")) continue;
    const String version = name.substring(7, name.length() - 4);
    if (!parseFirmwareVersion(version).valid ||
        compareFirmwareVersions(version, installed) <= 0) {
      if (staleCount < 8) stale[staleCount++] = name;
    }
  }
  root.close();
  for (size_t i = 0; i < staleCount; ++i) {
    SD.remove((String("/") + stale[i]).c_str());
  }
}

constexpr char kRecoveryImage[] = "/recovery.bin";
constexpr char kRecoveryMeta[] = "/recovery.txt";
bool recoveryStorageReady = false;

bool mountRecoveryStorage() {
  if (!recoveryStorageReady) recoveryStorageReady = SPIFFS.begin(true);
  return recoveryStorageReady;
}

size_t runningImageLength(const esp_partition_t* part) {
  if (part == nullptr) return 0;
  uint8_t header[24];
  if (esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK ||
      header[0] != 0xE9) {
    return 0;
  }
  const uint8_t segments = header[1];
  const bool hashAppended = header[23] == 1;
  size_t offset = sizeof(header);
  for (uint8_t i = 0; i < segments; ++i) {
    uint8_t segment[8];
    if (esp_partition_read(part, offset, segment, sizeof(segment)) != ESP_OK) {
      return 0;
    }
    const uint32_t length = static_cast<uint32_t>(segment[4]) |
                            (static_cast<uint32_t>(segment[5]) << 8) |
                            (static_cast<uint32_t>(segment[6]) << 16) |
                            (static_cast<uint32_t>(segment[7]) << 24);
    offset += sizeof(segment) + length;
    if (offset > part->size) return 0;
  }
  offset = (offset + 16) & ~static_cast<size_t>(15);
  if (hashAppended) offset += 32;
  return offset <= part->size ? offset : 0;
}

String runningImageHash(const esp_partition_t* part, size_t length) {
  if (part == nullptr || length < 32) return "";
  uint8_t hash[32];
  if (esp_partition_read(part, length - 32, hash, sizeof(hash)) != ESP_OK) return "";
  String text;
  for (uint8_t value : hash) {
    char pair[3];
    snprintf(pair, sizeof(pair), "%02x", value);
    text += pair;
  }
  return text;
}

bool readRecoveryMeta(String& version, String& hash, size_t& length) {
  if (!SPIFFS.exists(kRecoveryMeta) || !SPIFFS.exists(kRecoveryImage)) return false;
  File meta = SPIFFS.open(kRecoveryMeta, FILE_READ);
  if (!meta) return false;
  version = meta.readStringUntil('\n');
  hash = meta.readStringUntil('\n');
  length = static_cast<size_t>(meta.readStringUntil('\n').toInt());
  meta.close();
  version.trim();
  hash.trim();
  File image = SPIFFS.open(kRecoveryImage, FILE_READ);
  const bool sizeMatches = image && image.size() == length;
  if (image) image.close();
  return !version.isEmpty() && hash.length() == 64 && length > 0 && sizeMatches;
}

void saveRecoveryImage() {
  if (!mountRecoveryStorage()) return;
  const esp_partition_t* running = esp_ota_get_running_partition();
  const size_t length = runningImageLength(running);
  const String hash = runningImageHash(running, length);
  if (length == 0 || hash.isEmpty()) return;
  String savedVersion;
  String savedHash;
  size_t savedLength = 0;
  const String version = preferences.getString("fw_version", "0.0.0");
  if (readRecoveryMeta(savedVersion, savedHash, savedLength) && savedHash == hash) {
    if (savedVersion != version) {
      File meta = SPIFFS.open(kRecoveryMeta, FILE_WRITE);
      if (meta) {
        meta.print(version + "\n" + hash + "\n" + String(length) + "\n");
        meta.close();
      }
    }
    return;
  }
  SPIFFS.remove(kRecoveryMeta);
  SPIFFS.remove(kRecoveryImage);
  if (length + 4096 > SPIFFS.totalBytes()) {
    Serial.println("Recovery image skipped: not enough flash storage.");
    return;
  }
  File out = SPIFFS.open(kRecoveryImage, FILE_WRITE);
  if (!out) return;
  static uint8_t chunk[4096];
  size_t copied = 0;
  bool ok = true;
  while (copied < length) {
    const size_t piece = min<size_t>(sizeof(chunk), length - copied);
    if (esp_partition_read(running, copied, chunk, piece) != ESP_OK ||
        out.write(chunk, piece) != piece) {
      ok = false;
      break;
    }
    copied += piece;
  }
  out.close();
  if (!ok) {
    SPIFFS.remove(kRecoveryImage);
    return;
  }
  File meta = SPIFFS.open(kRecoveryMeta, FILE_WRITE);
  if (!meta) {
    SPIFFS.remove(kRecoveryImage);
    return;
  }
  meta.print(version + "\n" + hash + "\n" + String(length) + "\n");
  meta.close();
  Serial.printf("Recovery image saved for %s.\n", version.c_str());
}

void recoverInterruptedInstall() {
  if (!preferences.getBool("ota_busy", false)) return;
  preferences.putBool("ota_busy", false);
  if (!mountRecoveryStorage()) return;
  String version;
  String hash;
  size_t length = 0;
  if (!readRecoveryMeta(version, hash, length)) return;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (runningImageHash(running, runningImageLength(running)) == hash) {
    preferences.putString("fw_version", version);
    return;
  }
  drawMessage("Recovering OS", "An update was interrupted. Restoring " + version + "...");
  File image = SPIFFS.open(kRecoveryImage, FILE_READ);
  if (!image || !Update.begin(length)) {
    if (image) image.close();
    return;
  }
  static uint8_t chunk[4096];
  size_t written = 0;
  bool ok = true;
  while (written < length) {
    const int read = image.read(chunk, min<size_t>(sizeof(chunk), length - written));
    if (read <= 0 || Update.write(chunk, static_cast<size_t>(read)) !=
                         static_cast<size_t>(read)) {
      ok = false;
      break;
    }
    written += static_cast<size_t>(read);
  }
  image.close();
  if (!ok || written != length || !Update.end() || !Update.isFinished()) {
    Update.abort();
    return;
  }
  preferences.putString("fw_version", version);
  delay(500);
  ESP.restart();
}

bool installFirmware(const String& folder, bool changingVersion = false) {
  quietBackground = false;
  const bool staged = !useDirectFlash();
  if (staged && !sdCardReady) {
    Serial.println("Update skipped: no SD card and direct flash is off.");
    return false;
  }
  const String url = String("https://raw.githubusercontent.com/") +
                     updateRepo + "/" + updateBranch + "/updates/" + folder + "/main.bin";
  NetworkClientSecure client;
  HTTPClient http;
  if (!configureSecureRequest(client, http, url)) return false;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Firmware download HTTP %d\n", status);
    http.end();
    return false;
  }
  const int length = http.getSize();
  NetworkClient* stream = http.getStreamPtr();
  const String updatePath = stagedUpdatePath(folder);
  File file;
  bool reuse = false;
  if (staged && length > 0) {
    File existing = SD.open(updatePath.c_str(), FILE_READ);
    if (existing) {
      reuse = existing.size() == static_cast<size_t>(length);
      existing.close();
    }
    if (!reuse) {
      SD.remove(updatePath.c_str());
      file = SD.open(updatePath.c_str(), FILE_WRITE);
    }
  }
  if (length <= 0 || stream == nullptr || (staged && !reuse && !file)) {
    Serial.println("Firmware download unusable or SD file could not be created.");
    if (file) file.close();
    if (staged && !reuse) SD.remove(updatePath.c_str());
    http.end();
    return false;
  }

  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  drawUpdateScreen(folder, changingVersion);
  static uint8_t buffer[4096];
  const size_t total = static_cast<size_t>(length);
  size_t downloaded = reuse ? total : 0;
  size_t installed = 0;
  int lastDownload = -100;
  int lastInstall = -100;
  auto drawProgress = [&](bool force) {
    const int downloadPercent = static_cast<int>(downloaded * 100 / total);
    const int installPercent = static_cast<int>(installed * 100 / total);
    if (!force && downloadPercent - lastDownload < 5 &&
        installPercent - lastInstall < 5) {
      return;
    }
    lastDownload = downloadPercent;
    lastInstall = installPercent;
    display.startWrite();
    drawUpdateBar(430, "Downloading", downloadPercent);
    drawUpdateBar(550, "Installing", installPercent);
    display.endWrite();
    display.waitDisplay();
  };
  auto abandon = [&]() {
    preferences.putBool("ota_busy", false);
    Update.abort();
    if (staged) SD.remove(updatePath.c_str());
    display.setEpdMode(originalMode);
    return false;
  };

  preferences.putBool("ota_busy", true);
  if (!staged && !Update.begin(total)) {
    Serial.println("No OTA partition available for a direct update.");
    http.end();
    return abandon();
  }
  uint32_t lastData = millis();
  bool failed = false;
  while (downloaded < total) {
    const size_t available = stream->available();
    if (available == 0) {
      if (!http.connected() || millis() - lastData > 15000) {
        failed = true;
        break;
      }
      delay(5);
      continue;
    }
    const size_t toRead = min<size_t>(min<size_t>(available, sizeof(buffer)),
                                      total - downloaded);
    const int received = stream->readBytes(buffer, toRead);
    if (received <= 0) {
      failed = true;
      break;
    }
    const size_t chunk = static_cast<size_t>(received);
    const size_t wrote =
        staged ? file.write(buffer, chunk) : Update.write(buffer, chunk);
    if (wrote != chunk) {
      failed = true;
      break;
    }
    lastData = millis();
    downloaded += chunk;
    if (!staged) installed += chunk;
    drawProgress(false);
  }
  if (file) file.close();
  http.end();
  noteDataUsage(downloaded + 6500);
  if (failed || downloaded != total) {
    Serial.printf("Firmware download failed (%u of %u bytes).\n",
                  static_cast<unsigned>(downloaded),
                  static_cast<unsigned>(total));
    return abandon();
  }
  drawProgress(true);

  if (staged) {
    File image = SD.open(updatePath.c_str(), FILE_READ);
    if (!image || image.size() != total || !Update.begin(total)) {
      Serial.println("Downloaded firmware is unreadable or no OTA partition.");
      if (image) image.close();
      return abandon();
    }
    while (installed < total) {
      const int read = image.read(
          buffer, min<size_t>(sizeof(buffer), total - installed));
      if (read <= 0 ||
          Update.write(buffer, static_cast<size_t>(read)) !=
              static_cast<size_t>(read)) {
        failed = true;
        break;
      }
      installed += static_cast<size_t>(read);
      drawProgress(false);
    }
    image.close();
  }
  if (failed || installed != total || !Update.end() || !Update.isFinished()) {
    Serial.printf("Firmware install failed (%u of %u bytes).\n",
                  static_cast<unsigned>(installed),
                  static_cast<unsigned>(total));
    return abandon();
  }
  if (staged) SD.remove(updatePath.c_str());
  preferences.putBool("ota_busy", false);
  preferences.putString("fw_version", folder);
  const bool isNewest = newestEligibleFirmware.isEmpty() ||
                        compareFirmwareVersions(folder, newestEligibleFirmware) >= 0;
  preferences.putBool("test_mode",
                      changingVersion && !isNewest && !isInstallerFolder(folder));
  drawProgress(true);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(3);
  display.drawString("Update complete", display.width() / 2, 700);
  display.setTextSize(2);
  display.drawString("Restarting...", display.width() / 2, 752);
  display.setTextDatum(textdatum_t::top_left);
  display.waitDisplay();
  delay(1500);
  ESP.restart();
  return true;
}

enum class FirmwareInstallResult : uint8_t { upToDate, noStorage, failed };

bool syncInstalledImagePack();

FirmwareInstallResult installNewestFirmware(String& candidate) {
  const String installed = preferences.getString("fw_version", "0.0.0");
  size_t attempts = 0;
  bool attemptedAny = false;
  for (size_t i = 0; i < firmwareFolderCount && attempts < 3; ++i) {
    const String& folder = firmwareFolders[i];
    const FirmwareVersion version = parseFirmwareVersion(folder);
    if (!firmwareAutoEligible(version)) continue;
    if (compareFirmwareVersions(folder, installed) <= 0) break;
    candidate = folder;
    if (!updateStorageReady()) return FirmwareInstallResult::noStorage;
    ++attempts;
    attemptedAny = true;
    syncInstalledImagePack();
    if (installFirmware(folder)) return FirmwareInstallResult::upToDate;
  }
  return attemptedAny ? FirmwareInstallResult::failed
                      : FirmwareInstallResult::upToDate;
}

void checkForFirmwareUpdate() {
  if (!autoUpdates || weatherTestMode || !firmwareNetworkReady()) return;
  if (preferences.getBool("test_mode", false)) {
    Serial.println("Test mode is on; skipping automatic updates.");
    return;
  }
  const int battery = M5.Power.getBatteryLevel();
  if (battery >= 0 && battery < 25 &&
      M5.Power.isCharging() != M5.Power.is_charging) {
    return;
  }
  drawCheckingUpdatesScreen();
  String error;
  if (!fetchFirmwareList(error)) {
    Serial.printf("Update check failed: %s\n", error.c_str());
    return;
  }
  Serial.printf("Update check: installed %s, newest eligible %s.\n",
                preferences.getString("fw_version", "0.0.0").c_str(),
                newestEligibleFirmware.c_str());
  String candidate;
  if (installNewestFirmware(candidate) == FirmwareInstallResult::noStorage) {
    promptNoSdForUpdate(candidate);
  }
}

bool parseForecastResponse(JsonDocument& weather, Forecast& result,
                           const String& location, bool persistTimezone) {
  JsonObject current = weather["current"];
  JsonObject daily = weather["daily"];
  JsonArray dates = daily["time"].as<JsonArray>();
  if (current.isNull() || dates.isNull() || dates.size() == 0) {
    lastWeatherError = "Weather API returned incomplete forecast data.";
    Serial.println(lastWeatherError);
    return false;
  }

  clearForecast(result);
  result.location = location;
  if (result.location.isEmpty()) result.location = "Your location";
  result.updated = current["time"] | "";
  locationUtcOffsetSeconds = weather["utc_offset_seconds"] | 0;
  utcOffsetKnown = true;
  if (persistTimezone) preferences.putInt("utc_offset", locationUtcOffsetSeconds);

  JsonArray sunrises = daily["sunrise"].as<JsonArray>();
  JsonArray sunsets = daily["sunset"].as<JsonArray>();
  sunTimesValid = false;
  sunriseMinuteOfDay = 0;
  sunsetMinuteOfDay = 0;
  if (!sunrises.isNull() && !sunsets.isNull() && dates.size() > 0) {
    sunriseText = formatLocalTime(String(sunrises[0] | ""));
    sunsetText = formatLocalTime(String(sunsets[0] | ""));
    sunTimesValid = !sunriseText.isEmpty() && !sunsetText.isEmpty();
    if (sunTimesValid) {
      parseIsoMinuteOfDay(String(sunrises[0] | ""), sunriseMinuteOfDay);
      parseIsoMinuteOfDay(String(sunsets[0] | ""), sunsetMinuteOfDay);
    }
  }

  ForecastPeriod& now = result.periods[0];
  now.name = "Now";
  now.shortForecast = weatherDescription(current["weather_code"] | -1);
  const String currentDescription = current["weather_description"] | "";
  if (!currentDescription.isEmpty()) now.shortForecast = currentDescription;
  now.temperature = lroundf(current["temperature_2m"] | 0.0f);
  now.highTemperature = lroundf(daily["temperature_2m_max"][0] | 0.0f);
  now.lowTemperature = lroundf(daily["temperature_2m_min"][0] | 0.0f);
  now.apparentTemperature = current["apparent_temperature"] | NAN;
  now.humidity = current["relative_humidity_2m"] | -1;
  now.isDaytime = current["is_day"] | false;
  uint16_t currentMinute = 0;
  uint16_t sunriseMinute = 0;
  uint16_t sunsetMinute = 0;
  if (sunTimesValid &&
      parseIsoMinuteOfDay(String(current["time"] | ""), currentMinute) &&
      parseIsoMinuteOfDay(String(sunrises[0] | ""), sunriseMinute) &&
      parseIsoMinuteOfDay(String(sunsets[0] | ""), sunsetMinute)) {
    now.isDaytime =
        currentMinute >= sunriseMinute && currentMinute < sunsetMinute;
  }
  now.windSpeed = String(current["wind_speed_10m"] | 0.0f, 0) + " mph";
  now.windDirection = windDirectionText(current["wind_direction_10m"] | 0);
  now.precipitation = daily["precipitation_probability_max"][0] | -1;
  now.detailedForecast = "Updated " + formatUpdatedTime(result.updated);
  const String testDetails = current["test_details"] | "";
  if (!testDetails.isEmpty()) now.detailedForecast = testDetails;
  result.periodCount = 1;

  JsonArray codes = daily["weather_code"].as<JsonArray>();
  JsonArray descriptions = daily["weather_description"].as<JsonArray>();
  JsonArray highs = daily["temperature_2m_max"].as<JsonArray>();
  JsonArray lows = daily["temperature_2m_min"].as<JsonArray>();
  JsonArray rain = daily["precipitation_probability_max"].as<JsonArray>();
  JsonArray winds = daily["wind_speed_10m_max"].as<JsonArray>();
  JsonObject hourly = weather["hourly"];
  JsonArray hourlyTimes = hourly["time"].as<JsonArray>();
  JsonArray hourlyTemperatures = hourly["temperature_2m"].as<JsonArray>();
  JsonArray hourlyCodes = hourly["weather_code"].as<JsonArray>();
  JsonArray hourlyDescriptions =
      hourly["weather_description"].as<JsonArray>();
  JsonArray hourlyPrecipitation =
      hourly["precipitation_probability"].as<JsonArray>();
  JsonArray hourlyWindSpeeds = hourly["wind_speed_10m"].as<JsonArray>();
  JsonArray hourlyWindDirections = hourly["wind_direction_10m"].as<JsonArray>();
  const size_t dailyCount = min(dates.size(), kForecastDays);
  for (size_t i = 0; i < dailyCount && result.periodCount < kMaxForecastPeriods;
       ++i) {
    ForecastPeriod& target = result.periods[result.periodCount++];
    const float high = highs[i] | 0.0f;
    const float low = lows[i] | 0.0f;
    const String date = dates[i] | "";
    target.name = forecastDayName(date, i);
    target.shortForecast = weatherDescription(codes[i] | -1);
    if (!descriptions.isNull() && i < descriptions.size()) {
      target.shortForecast = descriptions[i] | target.shortForecast;
    }
    target.temperature = lroundf(high);
    target.highTemperature = lroundf(high);
    target.lowTemperature = lroundf(low);
    target.humidity = -1;
    target.precipitation = rain[i] | -1;
    target.isDaytime = true;
    target.windSpeed = String(winds[i] | 0.0f, 0) + " mph";
    target.detailedForecast = "High " + String(high, 0) + " F, low " +
        String(low, 0) + " F";
    target.morning.shortForecast = target.shortForecast;
    target.morning.windSpeed = target.windSpeed;
    target.morning.windDirection = target.windDirection;
    target.morning.temperature = high;
    target.morning.precipitation = target.precipitation;
    target.morning.valid = true;
    target.night.shortForecast = target.shortForecast;
    target.night.windSpeed = target.windSpeed;
    target.night.windDirection = target.windDirection;
    target.night.temperature = low;
    target.night.precipitation = target.precipitation;
    target.night.valid = true;
    populateForecastPart(target.morning, date, 6, 11, 9,
                         hourlyTimes, hourlyTemperatures, hourlyCodes,
                         hourlyDescriptions, hourlyPrecipitation,
                         hourlyWindSpeeds,
                         hourlyWindDirections);
    populateForecastPart(target.night, date, 18, 23, 21,
                         hourlyTimes, hourlyTemperatures, hourlyCodes,
                         hourlyDescriptions, hourlyPrecipitation,
                         hourlyWindSpeeds,
                         hourlyWindDirections);
  }

  result.valid = true;
  return true;
}

String jsonText(JsonVariantConst value) {
  if (value.is<JsonObjectConst>()) return value["value"] | "";
  return value.as<String>();
}

float jsonNumber(JsonVariantConst value, float fallback = NAN) {
  if (value.isNull()) return fallback;
  const String raw = jsonText(value);
  if (raw.isEmpty()) return fallback;
  char* end = nullptr;
  const float number = strtof(raw.c_str(), &end);
  return end == raw.c_str() || *end != '\0' || !isfinite(number)
      ? fallback : number;
}

String stringFromArray(JsonVariantConst array, size_t index,
                       const char* property = "value") {
  if (!array.is<JsonArrayConst>()) return "";
  JsonArrayConst values = array.as<JsonArrayConst>();
  if (index >= values.size()) return "";
  JsonVariantConst value = values[index];
  return value.is<JsonObjectConst>() ? String(value[property] | "")
                                     : value.as<String>();
}

void setForecastPart(ForecastPeriod::Part& part, const String& condition,
                     float temperature, float windMph, int windDirection,
                     int precipitation) {
  part.shortForecast = condition;
  part.temperature = temperature;
  part.windSpeed = isfinite(windMph) ? String(windMph, 0) + " mph" : "";
  part.windDirection = windDirectionText(windDirection);
  part.precipitation = precipitation;
  part.valid = isfinite(temperature);
}

String wttrCondition(const JsonVariantConst& source) {
  const String text = stringFromArray(source["weatherDesc"], 0);
  return text.isEmpty() ? "Forecast unavailable" : text;
}

bool parseWttrForecast(JsonDocument& weather, Forecast& result) {
  JsonArray currentConditions = weather["current_condition"].as<JsonArray>();
  JsonArray days = weather["weather"].as<JsonArray>();
  if (currentConditions.isNull() || currentConditions.size() == 0 ||
      days.isNull() || days.size() == 0) {
    lastWeatherError = "wttr.in returned incomplete forecast data.";
    return false;
  }

  const JsonObject current = currentConditions[0];
  clearForecast(result);
  result.location = stringFromArray(weather["nearest_area"][0]["areaName"], 0);
  if (result.location.isEmpty()) result.location = savedLocationName;
  result.updated = String(days[0]["date"] | "") + "T00:00";
  ForecastPeriod& now = result.periods[0];
  now.name = "Now";
  now.shortForecast = wttrCondition(current);
  now.temperature = jsonNumber(current["temp_F"], 0);
  now.apparentTemperature = jsonNumber(current["FeelsLikeF"]);
  now.humidity = jsonNumber(current["humidity"], -1);
  now.windSpeed = String(jsonNumber(current["windspeedMiles"], 0), 0) +
                  " mph";
  now.windDirection =
      windDirectionText(jsonNumber(current["winddirDegree"], 0));
  now.highTemperature = jsonNumber(days[0]["maxtempF"], now.temperature);
  now.lowTemperature = jsonNumber(days[0]["mintempF"], now.temperature);
  now.precipitation = -1;
  now.isDaytime = true;
  now.detailedForecast = "Updated " + formatUpdatedTime(result.updated);
  result.periodCount = 1;

  const size_t count = min(days.size(), kForecastDays);
  for (size_t day = 0; day < count &&
       result.periodCount < kMaxForecastPeriods; ++day) {
    const JsonObject daily = days[day];
    JsonArray hours = daily["hourly"].as<JsonArray>();
    ForecastPeriod& target = result.periods[result.periodCount++];
    const String date = daily["date"] | "";
    target.name = forecastDayName(date, day);
    target.shortForecast = hours.size() > 0
        ? wttrCondition(hours[day < hours.size() ? 4 : 0])
        : now.shortForecast;
    target.temperature = jsonNumber(daily["maxtempF"], now.temperature);
    target.highTemperature = target.temperature;
    target.lowTemperature = jsonNumber(daily["mintempF"], now.temperature);
    target.precipitation = -1;
    target.isDaytime = true;
    target.humidity = -1;
    target.windSpeed = hours.size() > 0
        ? String(jsonNumber(hours[4]["windspeedMiles"], 0), 0) + " mph"
        : now.windSpeed;
    target.windDirection = hours.size() > 0
        ? windDirectionText(
              jsonNumber(hours[4]["winddirDegree"], 0))
        : now.windDirection;
    target.detailedForecast = target.shortForecast + "; high " +
        String(target.highTemperature, 0) + " F, low " +
        String(target.lowTemperature, 0) + " F";

    const JsonObject morning = hours.size() > 0
        ? hours[min(size_t{3}, hours.size() - 1)].as<JsonObject>()
        : JsonObject();
    const JsonObject night = hours.size() > 0
        ? hours[min(size_t{7}, hours.size() - 1)].as<JsonObject>()
        : JsonObject();
    setForecastPart(target.morning, wttrCondition(morning),
                    jsonNumber(morning["tempF"], target.highTemperature),
                    jsonNumber(morning["windspeedMiles"]),
                    jsonNumber(morning["winddirDegree"], 0), -1);
    setForecastPart(target.night, wttrCondition(night),
                    jsonNumber(night["tempF"], target.lowTemperature),
                    jsonNumber(night["windspeedMiles"]),
                    jsonNumber(night["winddirDegree"], 0), -1);
  }

  JsonArray astronomy = days[0]["astronomy"].as<JsonArray>();
  if (!astronomy.isNull() && astronomy.size() > 0) {
    sunriseText = astronomy[0]["sunrise"] | "";
    sunsetText = astronomy[0]["sunset"] | "";
    sunTimesValid = !sunriseText.isEmpty() && !sunsetText.isEmpty();
  } else {
    sunTimesValid = false;
  }
  result.valid = true;
  return true;
}

String metNorwayCondition(const JsonVariantConst& data) {
  const String symbol =
      data["summary"]["symbol_code"] | "cloudy";
  String condition = symbol;
  const int suffix = condition.indexOf('_');
  if (suffix >= 0) condition.remove(suffix);
  condition.replace("partlycloudy", "partly cloudy");
  condition.replace("clearsky", "clear sky");
  condition.replace("heavyrain", "heavy rain");
  condition.replace("lightrain", "light rain");
  condition.replace("rainshowers", "rain showers");
  condition.replace("snowshowers", "snow showers");
  condition.replace("heavysnow", "heavy snow");
  condition.replace("lightsnow", "light snow");
  condition.replace("sleet", "sleet");
  condition.replace("fog", "fog");
  condition.replace("cloudy", "cloudy");
  condition.replace("fair", "mostly clear");
  condition.replace("clear sky", "Clear sky");
  if (condition.length() > 0) {
    condition.setCharAt(0, static_cast<char>(toupper(condition[0])));
  }
  return condition;
}

bool parseMetNorwayForecast(JsonDocument& weather, Forecast& result) {
  JsonArray timeseries =
      weather["properties"]["timeseries"].as<JsonArray>();
  if (timeseries.isNull() || timeseries.size() == 0) {
    lastWeatherError = "MET Norway returned no forecast timeseries.";
    return false;
  }

  clearForecast(result);
  result.location = savedLocationName;
  result.updated = weather["properties"]["meta"]["updated_at"] | "";
  locationUtcOffsetSeconds = 0;
  sunTimesValid = false;
  result.periodCount = 1;
  ForecastPeriod& now = result.periods[0];
  now.name = "Now";
  const JsonObject firstData = timeseries[0]["data"];
  const JsonObject nowDetails = firstData["instant"]["details"];
  now.shortForecast = metNorwayCondition(firstData["next_1_hours"]);
  now.temperature = jsonNumber(nowDetails["air_temperature"], 0);
  now.apparentTemperature = now.temperature;
  now.humidity = jsonNumber(nowDetails["relative_humidity"], -1);
  now.windSpeed = String(jsonNumber(nowDetails["wind_speed"], 0) * 2.23694f,
                          0) + " mph";
  now.windDirection =
      windDirectionText(jsonNumber(nowDetails["wind_from_direction"], 0));
  now.precipitation = -1;
  now.isDaytime = true;
  now.detailedForecast = result.updated.isEmpty()
      ? now.shortForecast : "Updated " + formatUpdatedTime(result.updated);

  String dates[kForecastDays];
  size_t dailyCount = 0;
  float morningTemps[kForecastDays];
  float nightTemps[kForecastDays];
  String morningConditions[kForecastDays];
  String nightConditions[kForecastDays];
  for (size_t i = 0; i < kForecastDays; ++i) {
    morningTemps[i] = NAN;
    nightTemps[i] = NAN;
  }
  for (JsonVariant item : timeseries) {
    const String timestamp = item["time"] | "";
    if (timestamp.length() < 10) continue;
    const String date = timestamp.substring(0, 10);
    size_t day = 0;
    while (day < dailyCount && dates[day] != date) ++day;
    if (day == dailyCount) {
      if (dailyCount >= kForecastDays) continue;
      dates[dailyCount] = date;
      ForecastPeriod& target = result.periods[result.periodCount++];
      target.name = forecastDayName(date, dailyCount);
      target.highTemperature = -1000;
      target.lowTemperature = 1000;
      target.temperature = 0;
      target.isDaytime = true;
      target.precipitation = -1;
      target.humidity = -1;
      target.shortForecast = metNorwayCondition(item["data"]["next_1_hours"]);
      ++dailyCount;
    }
    const JsonObject data = item["data"];
    const JsonObject details = data["instant"]["details"];
    const float temperature = jsonNumber(details["air_temperature"]);
    if (!isfinite(temperature)) continue;
    ForecastPeriod& target = result.periods[day + 1];
    if (temperature > target.highTemperature) {
      target.highTemperature = static_cast<int>(lroundf(temperature));
    }
    if (temperature < target.lowTemperature) {
      target.lowTemperature = static_cast<int>(lroundf(temperature));
    }
    target.temperature = target.highTemperature;
    const int hour = timestamp.substring(11, 13).toInt();
    const String condition = metNorwayCondition(data["next_1_hours"]);
    if (hour >= 6 && hour <= 12 &&
        (!isfinite(morningTemps[day]) ||
         abs(hour - 9) < abs(9 - 9))) {
      morningTemps[day] = temperature;
      morningConditions[day] = condition;
    }
    if (hour >= 18 && hour <= 23 &&
        (!isfinite(nightTemps[day]) ||
         abs(hour - 21) < abs(21 - 21))) {
      nightTemps[day] = temperature;
      nightConditions[day] = condition;
    }
    if (hour >= 10 && hour <= 14) target.shortForecast = condition;
    const float windMps = jsonNumber(details["wind_speed"]);
    target.windSpeed = isfinite(windMps)
        ? String(windMps * 2.23694f, 0) + " mph" : "";
    target.windDirection =
        windDirectionText(jsonNumber(details["wind_from_direction"], 0));
    const JsonObject nextHour = data["next_1_hours"]["details"];
    target.precipitation = -1;
    const float rain = jsonNumber(nextHour["precipitation_amount"]);
    if (isfinite(rain) && rain > 0) target.precipitation = 100;
  }
  for (size_t day = 0; day < dailyCount; ++day) {
    ForecastPeriod& target = result.periods[day + 1];
    if (target.highTemperature == -1000) target.highTemperature = 0;
    if (target.lowTemperature == 1000) target.lowTemperature = 0;
    target.detailedForecast = target.shortForecast;
    setForecastPart(target.morning, morningConditions[day],
                    morningTemps[day], NAN, 0, -1);
    setForecastPart(target.night, nightConditions[day],
                    nightTemps[day], NAN, 0, -1);
  }
  result.valid = dailyCount > 0;
  if (!result.valid) lastWeatherError = "MET Norway forecast had no daily data.";
  return result.valid;
}

int32_t isoTimezoneOffsetSeconds(const String& timestamp) {
  if (timestamp.length() < 25) return 0;
  const int offsetStart = timestamp.length() - 6;
  const char sign = timestamp[offsetStart];
  if ((sign != '+' && sign != '-') || timestamp[offsetStart + 3] != ':') {
    return 0;
  }
  const int hours = timestamp.substring(offsetStart + 1,
                                        offsetStart + 3).toInt();
  const int minutes = timestamp.substring(offsetStart + 4,
                                          offsetStart + 6).toInt();
  if (hours > 14 || minutes > 59) return 0;
  const int32_t offset = (hours * 60 + minutes) * 60;
  return sign == '-' ? -offset : offset;
}

float noaaTemperatureF(JsonVariantConst value, const String& unitCode) {
  const float temperature = jsonNumber(value);
  if (!isfinite(temperature)) return NAN;
  return unitCode.indexOf("degC") >= 0
      ? temperature * 9.0f / 5.0f + 32.0f : temperature;
}

float noaaWindMph(JsonVariantConst value, const String& unitCode) {
  const float speed = jsonNumber(value);
  if (!isfinite(speed)) return NAN;
  if (unitCode.indexOf("km_h") >= 0) return speed * 0.621371f;
  if (unitCode.indexOf("m_s") >= 0) return speed * 2.23694f;
  return speed;
}

bool fetchNoaaForecast(Forecast& result, const String& latitude,
                       const String& longitude) {
  JsonDocument points;
  String error;
  const String pointsUrl = "https://api.weather.gov/points/" + latitude +
                           "," + longitude;
  if (!getSecureJson(pointsUrl, points, error)) {
    lastWeatherError = "NOAA location lookup failed: " + error;
    return false;
  }
  JsonObject pointProperties = points["properties"];
  const String forecastUrl = pointProperties["forecast"] | "";
  const String hourlyUrl = pointProperties["forecastHourly"] | "";
  const String stationsUrl = pointProperties["observationStations"] | "";
  if (forecastUrl.isEmpty()) {
    lastWeatherError =
        "NOAA does not provide a forecast for this location. NOAA coverage "
        "is limited to the United States.";
    return false;
  }

  JsonDocument dailyResponse;
  if (!getSecureJson(forecastUrl, dailyResponse, error)) {
    lastWeatherError = "NOAA forecast request failed: " + error;
    return false;
  }
  JsonDocument hourlyResponse;
  const bool haveHourly = !hourlyUrl.isEmpty() &&
      getSecureJson(hourlyUrl, hourlyResponse, error);
  if (!haveHourly) {
    Serial.printf("NOAA hourly forecast unavailable: %s\n", error.c_str());
  }

  JsonDocument observation;
  bool haveObservation = false;
  if (!stationsUrl.isEmpty()) {
    JsonDocument stations;
    if (getSecureJson(stationsUrl, stations, error)) {
      JsonArray features = stations["features"].as<JsonArray>();
      if (!features.isNull() && features.size() > 0) {
        String stationUrl = features[0]["id"] | "";
        if (!stationUrl.isEmpty()) {
          if (!stationUrl.endsWith("/")) stationUrl += "/";
          haveObservation = getSecureJson(
              stationUrl + "observations/latest", observation, error);
        }
      }
    }
    if (!haveObservation) {
      Serial.printf("NOAA current observation unavailable: %s\n",
                    error.c_str());
    }
  }

  JsonArray periods =
      dailyResponse["properties"]["periods"].as<JsonArray>();
  if (periods.isNull() || periods.size() == 0) {
    lastWeatherError = "NOAA returned no forecast periods.";
    return false;
  }
  clearForecast(result);
  result.location = savedLocationName;
  JsonObject firstPeriod = periods[0];
  const JsonObject observationProperties = observation["properties"];
  const String observationTimestamp =
      observationProperties["timestamp"] | "";
  result.updated = observationTimestamp.isEmpty()
      ? String(dailyResponse["properties"]["updated"] | "")
      : observationTimestamp;
  locationUtcOffsetSeconds = isoTimezoneOffsetSeconds(result.updated);
  utcOffsetKnown = true;
  preferences.putInt("utc_offset", locationUtcOffsetSeconds);

  ForecastPeriod& now = result.periods[0];
  now.name = "Now";
  now.shortForecast = firstPeriod["shortForecast"] | "Forecast unavailable";
  now.temperature = noaaTemperatureF(firstPeriod["temperature"],
                                      firstPeriod["temperatureUnit"] | "F");
  now.highTemperature = now.temperature;
  now.lowTemperature = now.temperature;
  now.apparentTemperature = now.temperature;
  now.humidity = -1;
  now.isDaytime = firstPeriod["isDaytime"] | false;
  now.windSpeed = firstPeriod["windSpeed"] | "";
  now.windDirection = firstPeriod["windDirection"] | "";
  now.precipitation = -1;
  now.detailedForecast = firstPeriod["detailedForecast"] | "";
  if (haveObservation) {
    const String description =
        observationProperties["textDescription"] | "";
    if (!description.isEmpty()) now.shortForecast = description;
    const String tempUnit =
        observationProperties["temperature"]["unitCode"] | "";
    now.temperature = noaaTemperatureF(
        observationProperties["temperature"]["value"], tempUnit);
    const String heatUnit =
        observationProperties["heatIndex"]["unitCode"] | "";
    float apparent = noaaTemperatureF(
        observationProperties["heatIndex"]["value"], heatUnit);
    if (!isfinite(apparent)) {
      const String chillUnit =
          observationProperties["windChill"]["unitCode"] | "";
      apparent = noaaTemperatureF(
          observationProperties["windChill"]["value"], chillUnit);
    }
    now.apparentTemperature = isfinite(apparent) ? apparent : now.temperature;
    now.humidity = jsonNumber(
        observationProperties["relativeHumidity"]["value"], -1);
    now.windSpeed = String(noaaWindMph(
        observationProperties["windSpeed"]["value"],
        observationProperties["windSpeed"]["unitCode"]), 0) + " mph";
    now.windDirection = windDirectionText(jsonNumber(
        observationProperties["windDirection"]["value"], 0));
  }
  now.detailedForecast = "Updated " + formatUpdatedTime(result.updated);
  if (!haveObservation && !firstPeriod["detailedForecast"].isNull()) {
    now.detailedForecast = firstPeriod["detailedForecast"].as<String>();
  }
  result.periodCount = 1;

  JsonArray hourlyPeriods =
      hourlyResponse["properties"]["periods"].as<JsonArray>();
  if (!hourlyPeriods.isNull() && hourlyPeriods.size() > 0) {
    const JsonObject hour = hourlyPeriods[0];
    now.precipitation = jsonNumber(
        hour["probabilityOfPrecipitation"]["value"], -1);
    if (hour["shortForecast"].is<String>() &&
        !haveObservation && now.shortForecast == "Forecast unavailable") {
      now.shortForecast = hour["shortForecast"].as<String>();
    }
  }

  for (size_t i = 0; i < periods.size() &&
       result.periodCount < kMaxForecastPeriods; ++i) {
    JsonObject period = periods[i];
    if (!(period["isDaytime"] | false)) continue;
    ForecastPeriod& target = result.periods[result.periodCount++];
    target.name = period["name"] | "Forecast";
    target.shortForecast = period["shortForecast"] | "Forecast unavailable";
    target.temperature = noaaTemperatureF(
        period["temperature"], period["temperatureUnit"] | "F");
    target.highTemperature = target.temperature;
    target.lowTemperature = target.temperature;
    if (i + 1 < periods.size() &&
        !(periods[i + 1]["isDaytime"] | true)) {
      const float nighttimeTemperature = noaaTemperatureF(
          periods[i + 1]["temperature"],
          periods[i + 1]["temperatureUnit"] | "F");
      if (isfinite(nighttimeTemperature)) {
        target.lowTemperature = nighttimeTemperature;
        setForecastPart(target.night,
                        periods[i + 1]["shortForecast"] | "",
                        nighttimeTemperature, NAN, 0, -1);
      }
    }
    target.humidity = -1;
    target.precipitation = jsonNumber(
        period["probabilityOfPrecipitation"]["value"], -1);
    target.isDaytime = true;
    target.windSpeed = period["windSpeed"] | "";
    target.windDirection = period["windDirection"] | "";
    target.detailedForecast = period["detailedForecast"] | "";
    setForecastPart(target.morning, target.shortForecast,
                    target.highTemperature, NAN, 0,
                    target.precipitation);
  }
  sunTimesValid = false;
  result.valid = true;
  return true;
}

bool fetchForecast(Forecast& result) {
  lastWeatherError = "";
  const String latitude = String(savedLatitude, 4);
  const String longitude = String(savedLongitude, 4);
  if (weatherApiProvider == WeatherApiProvider::openMeteo) {
    const String forecastUrl =
        "http://api.open-meteo.com/v1/forecast?latitude=" + latitude +
        "&longitude=" + longitude +
        "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m,wind_direction_10m"
        "&hourly=temperature_2m,weather_code,precipitation_probability,wind_speed_10m,wind_direction_10m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,wind_speed_10m_max,sunrise,sunset"
        "&temperature_unit=fahrenheit&wind_speed_unit=mph&timezone=auto&forecast_days=7";
    JsonDocument weather;
    if (!getJson(forecastUrl, weather)) return false;
    return parseForecastResponse(weather, result, savedLocationName, true);
  }
  if (weatherApiProvider == WeatherApiProvider::wttr) {
    const String url = "https://wttr.in/" + latitude + "," + longitude +
                       "?format=j1";
    JsonDocument weather;
    String error;
    if (!getSecureJson(url, weather, error)) {
      lastWeatherError = error;
      return false;
    }
    return parseWttrForecast(weather, result);
  }
  if (weatherApiProvider == WeatherApiProvider::metNorway) {
    const String url =
        "https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=" +
        latitude + "&lon=" + longitude;
    JsonDocument weather;
    String error;
    if (!getSecureJson(url, weather, error)) {
      lastWeatherError = error;
      return false;
    }
    return parseMetNorwayForecast(weather, result);
  }
  return fetchNoaaForecast(result, latitude, longitude);
}

void fetchCalendarEvent(const String& currentLocalTime) {
  todayCalendarText = "";
  if (calendarId.isEmpty() || calendarApiKey.isEmpty()) return;
  if (currentLocalTime.length() < 10) {
    Serial.println("Calendar lookup skipped: forecast timestamp is invalid.");
    return;
  }

  const int offsetMinutes = locationUtcOffsetSeconds / 60;
  const int absoluteOffset = abs(offsetMinutes);
  char zone[16];
  snprintf(zone, sizeof(zone), "%c%02d:%02d",
           offsetMinutes < 0 ? '-' : '+',
           absoluteOffset / 60, absoluteOffset % 60);
  const String date = currentLocalTime.substring(0, 10);
  const String timeMin = date + "T00:00:00" + zone;
  const String timeMax = date + "T23:59:59" + zone;
  const String url =
      "https://www.googleapis.com/calendar/v3/calendars/" +
      urlEncode(calendarId) + "/events?key=" + urlEncode(calendarApiKey) +
      "&singleEvents=true&orderBy=startTime&maxResults=1&timeMin=" +
      urlEncode(timeMin) + "&timeMax=" + urlEncode(timeMax);
  JsonDocument response;
  String error;
  if (!getSecureJson(url, response, error)) {
    Serial.printf("Calendar event lookup failed: %s\n", error.c_str());
    return;
  }

  JsonArray items = response["items"].as<JsonArray>();
  if (items.isNull() || items.size() == 0) return;
  const String summary = items[0]["summary"] | "Calendar event";
  const String description = items[0]["description"] | "";
  todayCalendarText = summary;
  if (!description.isEmpty()) todayCalendarText += ": " + description;
  if (todayCalendarText.length() > 120) {
    todayCalendarText.remove(117);
    todayCalendarText += "...";
  }
}

float displayTemperature(int temperature, const String& unit) {
  if (useCelsius && unit == "F") return (temperature - 32.0f) * 5.0f / 9.0f;
  if (!useCelsius && unit == "C") return temperature * 9.0f / 5.0f + 32.0f;
  return temperature;
}

bool readLocalEnvironment() {
  auto& i2c = M5.In_I2C;
  if (!i2c.isEnabled() || !i2c.start(kSht30Address, false, 100000)) {
    Serial.println("Could not address the built-in SHT30 sensor.");
    return false;
  }
  const bool commandSent = i2c.write(0x2c) && i2c.write(0x06);
  const bool commandStopped = i2c.stop();
  if (!commandSent || !commandStopped) {
    Serial.println("Could not request a reading from the built-in SHT30.");
    return false;
  }

  delay(20);
  uint8_t data[6];
  if (!i2c.start(kSht30Address, true, 100000)) {
    Serial.println("Could not read the built-in SHT30 response.");
    return false;
  }
  const bool responseRead = i2c.read(data, sizeof(data), true);
  const bool responseStopped = i2c.stop();
  if (!responseRead || !responseStopped) {
    Serial.println("Could not read the built-in SHT30 response.");
    return false;
  }

  auto crc8 = [](const uint8_t* bytes, size_t length) {
    uint8_t crc = 0xff;
    for (size_t i = 0; i < length; ++i) {
      crc ^= bytes[i];
      for (uint8_t bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
      }
    }
    return crc;
  };
  if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5]) {
    Serial.println("Built-in SHT30 response failed its CRC check.");
    return false;
  }

  const uint16_t rawTemperature =
      (static_cast<uint16_t>(data[0]) << 8) | data[1];
  const uint16_t rawHumidity =
      (static_cast<uint16_t>(data[3]) << 8) | data[4];
  localTemperatureC = -45.0f + 175.0f * rawTemperature / 65535.0f;
  localHumidity = 100.0f * rawHumidity / 65535.0f;
  lastSensorReadMs = millis();
  return true;
}

float feelsLikeF(const ForecastPeriod& period) {
  if (isfinite(period.apparentTemperature)) return period.apparentTemperature;
  const float temperature = period.temperature;
  const float humidity = period.humidity;
  const float windMph = period.windSpeed.toFloat();
  if (humidity >= 0 && temperature >= 80.0f && humidity >= 40.0f) {
    float heatIndex = -42.379f + 2.04901523f * temperature +
        10.14333127f * humidity - 0.22475541f * temperature * humidity -
        0.00683783f * temperature * temperature -
        0.05481717f * humidity * humidity +
        0.00122874f * temperature * temperature * humidity +
        0.00085282f * temperature * humidity * humidity -
        0.00000199f * temperature * temperature * humidity * humidity;
    if (humidity < 13.0f && temperature <= 112.0f) {
      heatIndex -= ((13.0f - humidity) / 4.0f) *
          sqrtf((17.0f - fabsf(temperature - 95.0f)) / 17.0f);
    } else if (humidity > 85.0f && temperature <= 87.0f) {
      heatIndex += ((humidity - 85.0f) / 10.0f) *
          ((87.0f - temperature) / 5.0f);
    }
    return heatIndex;
  }
  if (temperature <= 50.0f && windMph > 3.0f) {
    const float windFactor = powf(windMph, 0.16f);
    return 35.74f + 0.6215f * temperature -
        35.75f * windFactor + 0.4275f * temperature * windFactor;
  }
  return temperature;
}

String twelveHourTime(const tm& localTime) {
  char buffer[12];
  strftime(buffer, sizeof(buffer), "%I:%M %p", &localTime);
  if (buffer[0] == '0') return String(buffer + 1);
  return String(buffer);
}

void drawClock(bool force = false) {
  static String lastClockText;
  static String lastLocationText;
  String text = "--:--";
  if (weatherTestMode) {
    text = testClockText;
  } else if (clockReady) {
    tm localTime;
    int configuredOffsetHours = 0;
    const int32_t offsetSeconds =
        parseClockOffsetHours(selectedTimezone, configuredOffsetHours)
            ? configuredOffsetHours * 3600
            : locationUtcOffsetSeconds;
    const time_t displayEpoch = time(nullptr) + offsetSeconds;
    if (gmtime_r(&displayEpoch, &localTime)) {
      text = twelveHourTime(localTime);
    }
  }
  const String location = displayLocationName();
  if (batterySaver) text = "~" + text;
  const bool clockChanged = force || text != lastClockText;
  const bool locationChanged = force || location != lastLocationText;
  if (!clockChanged && !locationChanged) return;

  auto& display = M5.Display;
  const int32_t clockWidth = 210;
  const int32_t barY = 8;
  const int32_t barHeight = 52;
  const auto originalMode = display.getEpdMode();
  if (!force) display.setEpdMode(lgfx::epd_mode::epd_text);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_left);
  int textSize = 3;
  display.setTextSize(textSize);
  const int32_t locationWidth = display.width() - clockWidth - 56;
  while (textSize > 1 && display.textWidth(location) > locationWidth) {
    display.setTextSize(--textSize);
  }
  String fittedLocation = location;
  while (fittedLocation.length() > 1 &&
         display.textWidth(fittedLocation) > locationWidth) {
    fittedLocation.remove(fittedLocation.length() - 1);
    if (!fittedLocation.endsWith("...")) fittedLocation += "...";
  }
  if (locationChanged) {
    display.fillRect(16, barY, locationWidth + 16, barHeight, TFT_WHITE);
    display.setTextDatum(textdatum_t::top_left);
    display.setTextSize(textSize);
    display.drawString(fittedLocation, 24, 16);
  }
  if (clockChanged) {
    display.fillRect(display.width() - clockWidth - 40, barY,
                     clockWidth + 24, barHeight, TFT_WHITE);
    display.setTextDatum(textdatum_t::top_right);
    display.setTextSize(3);
    display.drawString(text, display.width() - 24, 16);
  }
  display.setTextDatum(textdatum_t::top_left);
  lastClockText = text;
  lastLocationText = location;
  if (!force) {
    display.waitDisplay();
    display.setEpdMode(originalMode);
  }
}

bool fetchIpLocationAndTimezone() {
  JsonDocument response;
  if (!getJson("http://ip-api.com/json/?fields=status,message,lat,lon,city,regionName,timezone",
               response)) return false;
  if (String(response["status"] | "") != "success") {
    Serial.printf("Public-IP location lookup failed: %s\n",
                  String(response["message"] | "unknown error").c_str());
    return false;
  }

  if (activeLocationQuery.isEmpty()) {
    const float latitude = response["lat"] | NAN;
    const float longitude = response["lon"] | NAN;
    if (isfinite(latitude) && isfinite(longitude) &&
        latitude >= -90.0f && latitude <= 90.0f &&
        longitude >= -180.0f && longitude <= 180.0f) {
      savedLatitude = latitude;
      savedLongitude = longitude;
      savedLocationName = response["city"] | "Approximate IP location";
      const String region = response["regionName"] | "";
      if (!region.isEmpty()) savedLocationName += ", " + region;
      Serial.println("Using approximate location from public IP.");
    }
  }
  return true;
}

void imageSize(const char* path, int32_t& width, int32_t& height) {
  if (isApprovedSdImage(path)) {
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    if (pngDimensions(path, imageWidth, imageHeight)) {
      width = static_cast<int32_t>(imageWidth);
      height = static_cast<int32_t>(imageHeight);
      return;
    }
  }
  const String name = path;
  if (name.startsWith("/wifi") || name == "/wifioff.png") {
    width = 176;
    height = 80;
  } else if (name.startsWith("/battery") || name.startsWith("/charge")) {
    width = 40;
    height = 20;
  } else if (name == "/tornado.png" || name == "/hurricane.png") {
    width = 150;
    height = 150;
  } else if (name == "/extremehot.png" || name == "/extremecold.png") {
    width = 20;
    height = 60;
  } else if (name == "/lostapp.png") {
    width = 500;
    height = 468;
  } else if (name == "/square.png") {
    width = 33;
    height = 33;
  } else {
    width = 120;
    height = 120;
  }
}

constexpr const char* kSupportedImages[] = {
    "/alert.png", "/apiexpired.png", "/battery25.png", "/battery50.png",
    "/battery75.png", "/batterycritical.png", "/batteryfull.png",
    "/batteryunknown.png", "/charge1.png", "/charge2.png", "/charge3.png",
    "/chargefull.png", "/clearnight.png", "/cloudy.png", "/extremecold.png",
    "/extremeevent.png", "/extremehot.png", "/fog.png", "/hurricane.png",
    "/lostapp.png", "/nowifi.png", "/partialclouds.png", "/rain.png",
    "/snow.png", "/square.png", "/sundown.png",
    "/sunny.png", "/sunup.png", "/thunderstorm.png", "/tornado.png",
    "/overcast.png", "/timesync.png",
    "/wifi0.png", "/wifi1.png", "/wifi2.png", "/wifi3.png", "/wifioff.png",
    "/wind.png", "/tornadonight.png", "/hurricanenight.png",
    "/thunderstormnight.png", "/snownight.png", "/rainnight.png",
    "/fognight.png", "/windnight.png", "/partialcloudynight.png",
    "/overcastnight.png", "/cloudynight.png", "/alerticon.png",
    "/batterycharge.png"};
constexpr size_t kSupportedImageCount =
    sizeof(kSupportedImages) / sizeof(kSupportedImages[0]);
bool preferencesReady = false;
bool isApprovedSdImage(const char* path);

const char* const kCustomWeatherEvents[] = {
    "clear", "partly-cloudy", "cloudy", "overcast", "rain", "snow",
    "fog", "thunderstorm", "hurricane", "tornado", "wind"};
const char* const kCustomWeatherEventLabels[] = {
    "Clear", "Partly cloudy", "Cloudy", "Overcast", "Rain", "Snow",
    "Fog", "Thunderstorms", "Hurricane", "Tornado", "Wind"};
constexpr size_t kCustomWeatherEventCount =
    sizeof(kCustomWeatherEvents) / sizeof(kCustomWeatherEvents[0]);

String customImageKey(size_t slot, const char* suffix) {
  return String("c") + suffix + String(slot);
}

bool customImageIsMapped(const char* path) {
  if (!preferencesReady || path == nullptr) return false;
  for (size_t i = 0; i < kMaximumCustomImages; ++i) {
    if (preferences.getString(customImageKey(i, "img").c_str(), "") != path) {
      continue;
    }
    return preferences.getString(customImageKey(i, "evt").c_str(), "none") !=
           "none";
  }
  return false;
}

bool customEventMatches(const String& event, const String& forecastText) {
  const String text = lowercase(forecastText);
  if (event == "clear") {
    return text.indexOf("clear") >= 0 || text.indexOf("sun") >= 0;
  }
  if (event == "partly-cloudy") {
    return text.indexOf("partly") >= 0 || text.indexOf("mostly cloudy") >= 0 ||
           text.indexOf("few clouds") >= 0;
  }
  if (event == "cloudy") {
    return text.indexOf("cloud") >= 0 && text.indexOf("partly") < 0 &&
           text.indexOf("mostly cloudy") < 0 &&
           text.indexOf("overcast") < 0;
  }
  if (event == "overcast") return text.indexOf("overcast") >= 0;
  if (event == "rain") {
    return text.indexOf("rain") >= 0 || text.indexOf("drizzle") >= 0 ||
           text.indexOf("shower") >= 0;
  }
  if (event == "snow") {
    return text.indexOf("snow") >= 0 || text.indexOf("flurr") >= 0 ||
           text.indexOf("sleet") >= 0 || text.indexOf("ice") >= 0;
  }
  if (event == "fog") {
    return text.indexOf("fog") >= 0 || text.indexOf("haze") >= 0;
  }
  if (event == "thunderstorm") return text.indexOf("thunder") >= 0;
  if (event == "hurricane") {
    return text.indexOf("hurricane") >= 0 || text.indexOf("tropical") >= 0;
  }
  if (event == "tornado") return text.indexOf("tornado") >= 0;
  if (event == "wind") return text.indexOf("wind") >= 0;
  return false;
}

String customWeatherImage(const String& forecastText, bool isDaytime) {
  if (!preferencesReady || !sdCardReady) return "";
  const String dayPart = isDaytime ? "day" : "night";
  for (size_t i = 0; i < kMaximumCustomImages; ++i) {
    const String path = preferences.getString(
        customImageKey(i, "img").c_str(), "");
    if (path.isEmpty() ||
        preferences.getString(customImageKey(i, "evt").c_str(), "none") ==
            "none" ||
        preferences.getString(customImageKey(i, "day").c_str(), "") !=
            dayPart ||
        !customEventMatches(
            preferences.getString(customImageKey(i, "evt").c_str(), ""),
            forecastText) ||
        !SD.exists(sdImageFile(path.c_str()))) {
      continue;
    }
    return path;
  }
  return "";
}

size_t supportedImageIndex(const char* path) {
  for (size_t i = 0; i < kSupportedImageCount; ++i) {
    if (strcmp(path, kSupportedImages[i]) == 0) return i;
  }
  return kSupportedImageCount;
}

size_t approvalImageIndex(const char* path) {
  const size_t index = supportedImageIndex(path);
  if (index < kSupportedImageCount || path == nullptr ||
      strncmp(path, "/night", 6) != 0 || path[6] == '\0') {
    return index;
  }
  const String base = String("/") + (path + 6);
  return supportedImageIndex(base.c_str());
}

bool isApprovedSdImage(const char* path) {
  const size_t index = approvalImageIndex(path);
  if (!preferencesReady || !sdCardReady || path == nullptr ||
      !SD.exists(sdImageFile(path).c_str())) return false;
  if (index < kSupportedImageCount) {
    return preferences.getUChar((String("img") + String(index)).c_str(), 0) ==
           1;
  }
  return customImageIsMapped(path);
}

bool pngDimensionsFromData(const uint8_t* data, size_t len, uint32_t& width,
                           uint32_t& height);

String darkImageFile(const char* path) {
  return String("/images/dark") + path;
}

struct DarkImage {
  String path;
  uint8_t* data;
  size_t length;
};
std::vector<DarkImage> darkImages;

const DarkImage* findDarkImage(const char* path) {
  for (const DarkImage& image : darkImages) {
    if (image.path == path) return &image;
  }
  return nullptr;
}

void freeDarkImages() {
  for (DarkImage& image : darkImages) free(image.data);
  darkImages.clear();
}

uint8_t* allocateDarkBuffer(size_t length) {
  uint8_t* buffer = static_cast<uint8_t*>(ps_malloc(length));
  if (buffer == nullptr) buffer = static_cast<uint8_t*>(malloc(length));
  return buffer;
}

bool storeDarkImage(const char* path, const uint8_t* data, size_t length) {
  uint8_t* copy = allocateDarkBuffer(length);
  if (copy == nullptr) return false;
  memcpy(copy, data, length);
  darkImages.push_back({String(path), copy, length});
  return true;
}

bool loadDarkImage(const char* path) {
  if (findDarkImage(path) != nullptr) return true;
  if (!sdCardReady) return false;
  const String target = darkImageFile(path);
  if (SD.exists(target.c_str())) {
    File cached = SD.open(target.c_str(), FILE_READ);
    if (cached) {
      const size_t size = cached.size();
      uint8_t* buffer = size > 0 ? allocateDarkBuffer(size) : nullptr;
      if (buffer != nullptr && cached.read(buffer, size) == size) {
        cached.close();
        darkImages.push_back({String(path), buffer, size});
        return true;
      }
      if (buffer != nullptr) free(buffer);
      cached.close();
    }
  }
  if (!SD.exists("/images/dark")) SD.mkdir("/images/dark");
  size_t len = 0;
  const uint8_t* embedded = nullptr;
  String file;
  if (isApprovedSdImage(path)) {
    file = sdImageFile(path);
  } else {
    embedded = getEmbeddedPng(path, len);
    if (embedded == nullptr || len == 0) {
      embedded = nullptr;
      file = sdImageFile(path);
      if (!SD.exists(file.c_str())) return false;
    }
  }
  uint32_t width = 0;
  uint32_t height = 0;
  if (embedded != nullptr) {
    if (!pngDimensionsFromData(embedded, len, width, height)) return false;
  } else {
    File image = SD.open(file.c_str(), FILE_READ);
    uint8_t header[24];
    const bool readable = image && image.size() >= sizeof(header) &&
        image.read(header, sizeof(header)) == sizeof(header);
    if (image) image.close();
    if (!readable || !pngDimensionsFromData(header, sizeof(header), width, height)) {
      return false;
    }
  }
  if (width == 0 || height == 0 || width * height > 600000UL) return false;
  M5Canvas sprite(&M5.Display);
  sprite.setPsram(true);
  sprite.setColorDepth(16);
  if (sprite.createSprite(width, height) == nullptr) return false;
  sprite.fillSprite(0xFFFF);
  const bool drawn = embedded != nullptr
      ? sprite.drawPng(embedded, len, 0, 0)
      : sprite.drawPngFile(SD, file.c_str(), 0, 0);
  uint8_t* pixels = static_cast<uint8_t*>(sprite.getBuffer());
  bool stored = false;
  if (drawn && pixels != nullptr) {
    const size_t bytes = static_cast<size_t>(width) * height * 2;
    for (size_t i = 0; i < bytes; ++i) pixels[i] = static_cast<uint8_t>(~pixels[i]);
    size_t pngLength = 0;
    void* png = sprite.createPng(&pngLength);
    if (png != nullptr && pngLength > 0) {
      stored = storeDarkImage(path, static_cast<const uint8_t*>(png), pngLength);
      File out = SD.open(target.c_str(), FILE_WRITE);
      if (out) {
        const bool written =
            out.write(static_cast<const uint8_t*>(png), pngLength) == pngLength;
        out.close();
        if (!written) SD.remove(target.c_str());
      }
    }
    if (png != nullptr) free(png);
  }
  sprite.deleteSprite();
  if (!stored) Serial.printf("Dark image failed: %s\n", path);
  return stored;
}

void clearDarkImageCache() {
  freeDarkImages();
  if (!sdCardReady || !SD.exists("/images/dark")) return;
  File dir = SD.open("/images/dark");
  if (dir) {
    String names[64];
    size_t count = 0;
    for (File entry = dir.openNextFile(); entry && count < 64;
         entry = dir.openNextFile()) {
      String name = entry.name();
      const int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      names[count++] = name;
    }
    dir.close();
    for (size_t i = 0; i < count; ++i) {
      SD.remove((String("/images/dark/") + names[i]).c_str());
    }
    if (count == 64) clearDarkImageCache();
  }
  SD.rmdir("/images/dark");
}

bool drawImage(const char* path, int32_t x, int32_t y,
               float scaleX = 1.0f, float scaleY = 1.0f) {
  if (darkMode) {
    const DarkImage* dark = findDarkImage(path);
    if (dark == nullptr && loadDarkImage(path)) dark = findDarkImage(path);
    if (dark != nullptr) {
      return M5.Display.drawPng(dark->data, dark->length, x, y, 0, 0, 0, 0,
                                scaleX, scaleY);
    }
  }
  if (isApprovedSdImage(path)) {
    return M5.Display.drawPngFile(SD, sdImageFile(path).c_str(), x, y, 0, 0,
                                  0, 0, scaleX, scaleY);
  }
  size_t len = 0;
  const uint8_t* embedded = getEmbeddedPng(path, len);
  if (embedded && len > 0) {
    return M5.Display.drawPng(embedded, len, x, y, 0, 0, 0, 0, scaleX, scaleY);
  }
  if (sdCardReady && SD.exists(sdImageFile(path).c_str())) {
    return M5.Display.drawPngFile(SD, sdImageFile(path).c_str(), x, y, 0, 0,
                                  0, 0, scaleX, scaleY);
  }
  int32_t imageWidth;
  int32_t imageHeight;
  imageSize(path, imageWidth, imageHeight);
    const int32_t scaledWidth = static_cast<int32_t>(imageWidth * scaleX);
    const int32_t scaledHeight = static_cast<int32_t>(imageHeight * scaleY);
    const int32_t boxWidth = scaledWidth < 112 ? 112
      : scaledWidth > 240 ? 240 : scaledWidth;
    const int32_t boxHeight = scaledHeight < 52 ? 52
      : scaledHeight > 220 ? 220 : scaledHeight;
  x = min(x, M5.Display.width() - boxWidth - 2);
  y = min(y, M5.Display.height() - boxHeight - 2);
  M5.Display.drawRect(x, y, boxWidth, boxHeight, TFT_BLACK);
  M5.Display.setTextDatum(textdatum_t::top_left);
  M5.Display.setTextSize(1);
  M5.Display.setTextWrap(true, true);
  M5.Display.setCursor(x + 4, y + 4);
  String filename = path;
  if (filename.startsWith("/")) filename.remove(0, 1);
  M5.Display.print(filename);
  M5.Display.setTextWrap(false);
  static String lastMissingAsset;
  if (lastMissingAsset != path) {
    Serial.printf("Image unavailable: %s\n", path);
    lastMissingAsset = path;
  }
  return false;
}

bool pngDimensionsFromData(const uint8_t* data, size_t len, uint32_t& width,
                          uint32_t& height) {
  if (data == nullptr || len < 24) return false;
  if (data[0] != 0x89 || data[1] != 0x50 || data[2] != 0x4E || data[3] != 0x47) {
    return false;
  }
  width = (static_cast<uint32_t>(data[8]) << 24) |
          (static_cast<uint32_t>(data[9]) << 16) |
          (static_cast<uint32_t>(data[10]) << 8) |
          static_cast<uint32_t>(data[11]);
  height = (static_cast<uint32_t>(data[12]) << 24) |
           (static_cast<uint32_t>(data[13]) << 16) |
           (static_cast<uint32_t>(data[14]) << 8) |
           static_cast<uint32_t>(data[15]);
  return width > 0 && height > 0;
}

bool pngDimensions(const char* path, uint32_t& width, uint32_t& height) {
  if (isApprovedSdImage(path)) {
    File image = SD.open(sdImageFile(path).c_str(), FILE_READ);
    if (!image || image.size() < 24 || !image.seek(16)) return false;
    uint8_t dimensions[8];
    if (image.read(dimensions, sizeof(dimensions)) != sizeof(dimensions)) {
      image.close();
      return false;
    }
    image.close();
    width = (static_cast<uint32_t>(dimensions[0]) << 24) |
            (static_cast<uint32_t>(dimensions[1]) << 16) |
            (static_cast<uint32_t>(dimensions[2]) << 8) |
            static_cast<uint32_t>(dimensions[3]);
    height = (static_cast<uint32_t>(dimensions[4]) << 24) |
             (static_cast<uint32_t>(dimensions[5]) << 16) |
             (static_cast<uint32_t>(dimensions[6]) << 8) |
             static_cast<uint32_t>(dimensions[7]);
    return width > 0 && height > 0;
  }
  size_t len = 0;
  const uint8_t* embedded = getEmbeddedPng(path, len);
  if (embedded != nullptr && len >= 24) {
    return pngDimensionsFromData(embedded, len, width, height);
  }
  if (!sdCardReady || !SD.exists(sdImageFile(path).c_str())) return false;
  File image = SD.open(sdImageFile(path).c_str(), FILE_READ);
  if (!image || image.size() < 24 || !image.seek(16)) return false;
  uint8_t dimensions[8];
  if (image.read(dimensions, sizeof(dimensions)) != sizeof(dimensions)) {
    image.close();
    return false;
  }
  image.close();
  width = (static_cast<uint32_t>(dimensions[0]) << 24) |
          (static_cast<uint32_t>(dimensions[1]) << 16) |
          (static_cast<uint32_t>(dimensions[2]) << 8) |
          static_cast<uint32_t>(dimensions[3]);
  height = (static_cast<uint32_t>(dimensions[4]) << 24) |
           (static_cast<uint32_t>(dimensions[5]) << 16) |
           (static_cast<uint32_t>(dimensions[6]) << 8) |
           static_cast<uint32_t>(dimensions[7]);
  return width > 0 && height > 0;
}

void drawWeatherIcon(const char* path, int32_t slotX, int32_t slotY,
                     int32_t slotWidth = 150, int32_t slotHeight = 150) {
  int32_t imageWidth;
  int32_t imageHeight;
  imageSize(path, imageWidth, imageHeight);
  const int32_t x = slotX + (slotWidth - imageWidth) / 2;
  const int32_t y = slotY + (slotHeight - imageHeight) / 2;
  drawImage(path, x, y);
}

void drawWeatherIconFitted(const char* path, int32_t x, int32_t y,
                           int32_t maxWidth, int32_t maxHeight) {
  int32_t imageWidth;
  int32_t imageHeight;
  imageSize(path, imageWidth, imageHeight);
  const float scale = min(static_cast<float>(maxWidth) / imageWidth,
                          static_cast<float>(maxHeight) / imageHeight);
  const int32_t width = static_cast<int32_t>(imageWidth * scale);
  const int32_t height = static_cast<int32_t>(imageHeight * scale);
  drawImage(path, x + (maxWidth - width) / 2, y + (maxHeight - height) / 2,
            scale, scale);
}

bool pngLooksUsable(const String& file, uint32_t& width, uint32_t& height) {
  File image = SD.open(file.c_str(), FILE_READ);
  if (!image) return false;
  uint8_t header[24];
  const size_t size = image.size();
  const bool readable = size >= sizeof(header) && size <= 2000000 &&
      image.read(header, sizeof(header)) == sizeof(header);
  image.close();
  return readable && pngDimensionsFromData(header, sizeof(header), width, height) &&
         width <= 2000 && height <= 2000;
}

void prepareImagesFolder() {
  if (!sdCardReady) return;
  if (!SD.exists("/images")) SD.mkdir("/images");
  for (size_t i = 0; i < kSupportedImageCount; ++i) {
    const String target = sdImageFile(kSupportedImages[i]);
    if (SD.exists(kSupportedImages[i]) && !SD.exists(target.c_str())) {
      SD.rename(kSupportedImages[i], target.c_str());
    }
  }
}

void reviewSdImagesOnBoot() {
  if (!preferencesReady || !sdCardReady) return;
  prepareImagesFolder();
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  for (size_t i = 0; i < kSupportedImageCount; ++i) {
    const char* path = kSupportedImages[i];
    const String file = sdImageFile(path);
    const String key = String("img") + String(i);
    if (!SD.exists(file.c_str()) || preferences.getUChar(key.c_str(), 0) != 0) {
      continue;
    }
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    if (!pngLooksUsable(file, imageWidth, imageHeight)) {
      Serial.printf("Skipping invalid SD image: %s\n", path);
      preferences.putUChar(key.c_str(), 2);
      continue;
    }
    const float scale = min(static_cast<float>(display.width() - 80) / imageWidth,
                            static_cast<float>(display.height() - 260) / imageHeight);
    const int32_t width = static_cast<int32_t>(imageWidth * scale);
    const int32_t x = (display.width() - width) / 2;
    display.setEpdMode(lgfx::epd_mode::epd_quality);
    display.clear(TFT_WHITE);
    if (!display.drawPngFile(SD, file.c_str(), x, 45, 0, 0, 0, 0, scale, scale)) {
      Serial.printf("Could not preview SD image: %s\n", path);
      preferences.putUChar(key.c_str(), 2);
      continue;
    }
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(2);
    display.drawString(String("Approve ") + path + "?", display.width() / 2,
                       display.height() - 150);
    display.setTextSize(1);
    display.drawString("A: approve   B: skip   C: finish review",
                       display.width() / 2, display.height() - 96);
    display.waitDisplay();

    bool reviewed = false;
    while (!reviewed) {
      M5.update();
      const bool suppressBootClick = consumeBootCClick();
      if (M5.BtnA.wasClicked()) {
        preferences.putUChar(key.c_str(), 1);
        reviewed = true;
      } else if (M5.BtnB.wasClicked()) {
        preferences.putUChar(key.c_str(), 2);
        reviewed = true;
      } else if (!suppressBootClick && M5.BtnC.wasClicked()) {
        display.setEpdMode(originalMode);
        return;
      }
      delay(20);
    }
  }

  File root = SD.open("/images");
  if (!root) {
    display.setEpdMode(originalMode);
    return;
  }
  String candidates[kMaximumCustomImages];
  size_t candidateCount = 0;
  File entry = root.openNextFile();
  while (entry && candidateCount < kMaximumCustomImages) {
    if (!entry.isDirectory()) {
      String name = entry.name();
      const int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      const String path = "/" + name;
      bool alreadyKnown = approvalImageIndex(path.c_str()) < kSupportedImageCount;
      for (size_t i = 0; i < kMaximumCustomImages; ++i) {
        if (preferences.getString(customImageKey(i, "img").c_str(), "") == path) {
          alreadyKnown = true;
        }
      }
      if (lowercase(path).endsWith(".png") && !alreadyKnown) {
        candidates[candidateCount++] = path;
      }
    }
    entry.close();
    entry = root.openNextFile();
  }
  entry.close();
  root.close();

  for (size_t index = 0; index < candidateCount; ++index) {
    const String path = candidates[index];
    const String file = sdImageFile(path.c_str());
    size_t slot = kMaximumCustomImages;
    for (size_t i = 0; i < kMaximumCustomImages; ++i) {
      if (preferences.getString(customImageKey(i, "img").c_str(), "").isEmpty()) {
        slot = i;
        break;
      }
    }
    if (slot == kMaximumCustomImages) {
      Serial.println("Custom image limit reached; remaining SD images skipped.");
      break;
    }
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    if (!pngLooksUsable(file, imageWidth, imageHeight)) {
      Serial.printf("Skipping invalid custom SD image: %s\n", path.c_str());
      preferences.putString(customImageKey(slot, "img").c_str(), path);
      preferences.putString(customImageKey(slot, "evt").c_str(), "none");
      continue;
    }

    bool useDay = true;
    size_t selectedEvent = 0;
    bool approve = false;
    const float scale = min(
        static_cast<float>(display.width() - 100) / imageWidth,
        static_cast<float>(display.height() - 420) / imageHeight);
    const int32_t width = static_cast<int32_t>(imageWidth * scale);
    const int32_t height = static_cast<int32_t>(imageHeight * scale);
    const int32_t x = (display.width() - width) / 2;
    auto drawDropdowns = [&]() {
      display.setEpdMode(lgfx::epd_mode::epd_text);
      display.startWrite();
      display.fillRect(20, height + 90, display.width() - 40, 150, TFT_WHITE);
      display.setTextColor(TFT_BLACK);
      display.setTextDatum(textdatum_t::top_left);
      display.setTextSize(2);
      display.drawRect(30, height + 95, display.width() - 60, 62, TFT_BLACK);
      display.drawString(String("Time of day: ") + (useDay ? "Day" : "Night") +
                             "  v", 44, height + 112);
      display.drawRect(30, height + 170, display.width() - 60, 62, TFT_BLACK);
      display.drawString(String("Weather event: ") +
                             kCustomWeatherEventLabels[selectedEvent] + "  v",
                         44, height + 187);
      display.endWrite();
      display.waitDisplay();
    };
    display.setEpdMode(lgfx::epd_mode::epd_quality);
    display.clear(TFT_WHITE);
    display.drawPngFile(SD, file.c_str(), x, 35, 0, 0, 0, 0, scale, scale);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(2);
    display.drawString("Assign custom weather image", display.width() / 2,
                       height + 52);
    display.drawString("Tap a dropdown or A/B to change.", display.width() / 2,
                       display.height() - 142);
    display.drawString("C: approve   Hold C: skip", display.width() / 2,
                       display.height() - 92);
    display.waitDisplay();
    drawDropdowns();

    bool reviewed = false;
    while (!reviewed) {
      M5.update();
      const bool suppressBootClick = consumeBootCClick();
      bool changed = false;
      if (M5.BtnA.wasClicked()) {
        useDay = !useDay;
        changed = true;
      } else if (M5.BtnB.wasClicked()) {
        selectedEvent = (selectedEvent + 1) % kCustomWeatherEventCount;
        changed = true;
      } else if (!suppressBootClick && M5.BtnC.isPressed() &&
                 M5.BtnC.pressedFor(900)) {
        reviewed = true;
      } else if (!suppressBootClick && M5.BtnC.wasClicked()) {
        approve = true;
        reviewed = true;
      } else if (M5.Touch.getCount() > 0) {
        const auto& touch = M5.Touch.getDetail();
        if (touchTapped(touch) && touch.y >= height + 95 &&
            touch.y < height + 157) {
          useDay = !useDay;
          changed = true;
        } else if (touchTapped(touch) && touch.y >= height + 170 &&
                   touch.y < height + 232) {
          selectedEvent = (selectedEvent + 1) % kCustomWeatherEventCount;
          changed = true;
        }
      }
      if (changed) drawDropdowns();
      delay(20);
    }
    preferences.putString(customImageKey(slot, "img").c_str(), path);
    preferences.putString(customImageKey(slot, "evt").c_str(),
                          approve ? kCustomWeatherEvents[selectedEvent] : "none");
    preferences.putString(customImageKey(slot, "day").c_str(),
                          useDay ? "day" : "night");
  }
  display.setEpdMode(originalMode);
}

bool drawFullScreenImage(const char* path) {
  uint32_t imageWidth = 0;
  uint32_t imageHeight = 0;
  if (!pngDimensions(path, imageWidth, imageHeight)) {
    Serial.printf("Could not read dimensions for full-screen image: %s\n", path);
    return false;
  }
  const float scale = min(
      static_cast<float>(M5.Display.width()) / imageWidth,
      static_cast<float>(M5.Display.height()) / imageHeight);
  const int32_t width = static_cast<int32_t>(imageWidth * scale);
  const int32_t height = static_cast<int32_t>(imageHeight * scale);
  M5.Display.clear(TFT_WHITE);
  return drawImage(path, (M5.Display.width() - width) / 2,
                   (M5.Display.height() - height) / 2, scale, scale);
}

bool shouldPowerOffForScreen(const char* imagePath) {
  return strcmp(imagePath, "/nowifi.png") == 0 ||
         strcmp(imagePath, "/apiexpired.png") == 0 ||
         strcmp(imagePath, "/lostapp.png") == 0;
}

void showDeadBatteryScreen() {
  auto& display = M5.Display;
  display.clear(TFT_WHITE);
  drawWeatherIconFitted("/batterycritical.png", display.width() / 2 - 100,
                        display.height() / 2 - 180, 200, 200);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(3);
  display.drawString("Please recharge the device", display.width() / 2,
                     display.height() / 2 + 70);
  display.setTextDatum(textdatum_t::top_left);
  display.waitDisplay();
  delay(500);
  M5.Power.powerOff();
}

void drawTimeSyncScreen() {
  setupStatusScreenDrawn = false;
  showSetupStatus("Synchronizing time...");
}

void showErrorAndPowerOff(const char* imagePath, const String& title,
                          const String& fallback) {
  const bool rendered = drawFullScreenImage(imagePath);
  if (!rendered) {
    drawMessage(title, fallback);
    drawImage(imagePath, 32, 300);
    Serial.printf("Full-screen asset unavailable: %s\n", imagePath);
  }
  M5.Display.waitDisplay();
  delay(250);
  if (rendered && batterySaver && shouldPowerOffForScreen(imagePath)) {
    M5.Power.powerOff();
  }
}

void enterNightShutdownSleep();
void refreshWeather(bool automatic);

bool goodnightScreenAllowed() {
  return nightShutdownEnabled && nightShutdownIsActive();
}

// Final gate: independently verify the current local time is actually within
// the configured night shutdown window before committing to goodnight sleep.
bool nightShutdownTimeConfirmed() {
  if (!nightShutdownEnabled) return false;
  if (time(nullptr) < kValidClockEpoch) return false;
  int configuredOffsetHours = 0;
  if (!parseClockOffsetHours(selectedTimezone, configuredOffsetHours) &&
      !utcOffsetKnown) {
    return false;
  }
  const int32_t offsetSeconds =
      parseClockOffsetHours(selectedTimezone, configuredOffsetHours)
          ? configuredOffsetHours * 3600
          : locationUtcOffsetSeconds;
  const time_t localNow = time(nullptr) + offsetSeconds;
  int32_t secondsToday = localNow % 86400;
  if (secondsToday < 0) secondsToday += 86400;
  const uint16_t minuteOfDay = static_cast<uint16_t>(secondsToday / 60);
  if (nightShutdownStartMinute < nightShutdownEndMinute) {
    return minuteOfDay >= nightShutdownStartMinute &&
           minuteOfDay < nightShutdownEndMinute;
  }
  return minuteOfDay >= nightShutdownStartMinute ||
         minuteOfDay < nightShutdownEndMinute;
}

void prepareForTimedSleep() {
  ntpUdp.stop();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
}

void armedTimerSleep(int seconds) {
  if (preferencesReady) {
    const time_t now = time(nullptr);
    preferences.putUInt("wake_at", now >= kValidClockEpoch
                                       ? static_cast<uint32_t>(now) + seconds
                                       : 0);
  }
  M5.Power.timerSleep(seconds);
}

void drawGoodnightScreen(const String& message) {
  auto& display = M5.Display;
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(4);
  display.drawString("Goodnight", display.width() / 2, 230);
  display.setTextSize(2);
  display.drawString(message, display.width() / 2, 310);
  constexpr int32_t weatherIconHeight = 400;
  const int32_t weatherIconWidth = display.width() - 120;
  constexpr int32_t messageBottom = 350;
  const int32_t iconCenterY =
      (messageBottom + display.height()) / 2;
  drawWeatherIconFitted("/clearnight.png", 60,
                        iconCenterY - weatherIconHeight / 2,
                        weatherIconWidth, weatherIconHeight);
  display.setTextDatum(textdatum_t::top_left);
  display.waitDisplay();
}

void showGoodnightScreenIfAllowed(const String& message) {
  if (goodnightScreenAllowed()) drawGoodnightScreen(message);
}

void enterBatterySaverSleep() {
  if (weatherTestMode) return;
  if (nightShutdownIsActive() && nightShutdownTimeConfirmed()) {
    enterNightShutdownSleep();
    return;
  }
  if (!batterySaver || !forecast.valid || !clockReady) return;
  const uint32_t secondsUntilPoll = secondsUntilNextPollSlot();
  if (secondsUntilPoll == 0 ||
      (adaptiveUpdates && secondsUntilPoll >= 60 * 60)) {
    // Poll the API once per minute when battery saver is active with no future
    // poll slot. Keep the RTC timer active in a loop so the device stays on
    // but wakes at the configured minutes interval, then resets the timer
    // and shuts down again, repeating the cycle.
    uint32_t lastWake = 0;
    while (true) {
      const uint32_t now = millis();
      if (now - lastWake >= (effectiveRefreshIntervalMinutes() * 60 * 1000UL)) {
        lastWake = now;
        Serial.println("Battery saver: waking to refresh.");
        refreshWeather(true);
        prepareForTimedSleep();
        armedTimerSleep(static_cast<int>(effectiveRefreshIntervalMinutes() * 60));
        Serial.println("Battery saver: timer sleep returned (cycle repeat).");
      } else {
        delay(1000);
      }
    }
  } else {
    Serial.println("Battery saver: sleeping until the next scheduled poll.");
    prepareForTimedSleep();
    armedTimerSleep(static_cast<int>(secondsUntilPoll));
    Serial.println("Battery-saver timer sleep returned.");
  }
}

void enterNightShutdownSleep() {
  if (!nightShutdownIsActive()) return;
  if (!nightShutdownTimeConfirmed()) return;
  const uint32_t sleepSeconds = secondsUntilNightShutdownEnd();
  Serial.println("Night shutdown schedule active; powering off until morning.");
  showGoodnightScreenIfAllowed("Night shutdown schedule is active.");
  prepareForTimedSleep();
  armedTimerSleep(static_cast<int>(sleepSeconds));
}

void drawForecast(const String& status);

void enterWifiRetrySleep() {
  if (!batterySaver) return;
  Serial.println("Battery saver: RTC shutdown for one hour before Wi-Fi retry.");
  drawForecast("Wi-Fi unavailable. C: retry Wi-Fi. Auto retry in one hour.");
  prepareForTimedSleep();
  armedTimerSleep(60 * 60);
  lastWifiRetryMs = millis();
}

void requestClockSync() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!ntpUdpReady) ntpUdpReady = ntpUdp.begin(2390);
  if (!ntpUdpReady) return;
  uint8_t packet[48] = {};
  packet[0] = 0x23;
  if (ntpUdp.beginPacket(IPAddress(162, 159, 200, 1), 123)) {
    ntpUdp.write(packet, sizeof(packet));
    ntpUdp.endPacket();
  }
  lastClockSyncAttemptMs = millis();
}

bool receiveClockSync() {
  const int packetSize = ntpUdp.parsePacket();
  if (packetSize <= 0) return false;
  uint8_t packet[48];
  if (packetSize < static_cast<int>(sizeof(packet)) ||
      ntpUdp.read(packet, sizeof(packet)) != sizeof(packet)) {
    while (ntpUdp.available()) ntpUdp.read();
    return false;
  }
  while (ntpUdp.available()) ntpUdp.read();
  const uint8_t mode = packet[0] & 0x07;
  if ((mode != 4 && mode != 5) || packet[1] == 0) return false;

  const uint32_t ntpSeconds =
      (static_cast<uint32_t>(packet[40]) << 24) |
      (static_cast<uint32_t>(packet[41]) << 16) |
      (static_cast<uint32_t>(packet[42]) << 8) | packet[43];
  constexpr uint32_t kNtpUnixEpochOffset = 2208988800UL;
  if (ntpSeconds <= kNtpUnixEpochOffset) return false;
  timeval currentTime;
  currentTime.tv_sec = ntpSeconds - kNtpUnixEpochOffset;
  const uint32_t fraction =
      (static_cast<uint32_t>(packet[44]) << 24) |
      (static_cast<uint32_t>(packet[45]) << 16) |
      (static_cast<uint32_t>(packet[46]) << 8) | packet[47];
  currentTime.tv_usec =
      (static_cast<uint64_t>(fraction) * 1000000ULL) >> 32;
  return currentTime.tv_sec >= kValidClockEpoch &&
         settimeofday(&currentTime, nullptr) == 0;
}

bool updateClockSync() {
  if (clockReady) return false;

  if (WiFi.status() != WL_CONNECTED) return false;
  if (ntpUdpReady && receiveClockSync()) {
    clockReady = true;
  } else if (time(nullptr) >= kValidClockEpoch) {
    clockReady = true;
  }
  if (!clockReady) {
    if (millis() - lastClockSyncAttemptMs >= kClockSyncRetryIntervalMs) {
      requestClockSync();
    }
    return false;
  }

  Serial.println("Device clock synchronized.");
  if (!hasLocationConfiguration()) fetchIpLocationAndTimezone();
  lastRefreshMs = 0;
  return true;
}

bool mountSdCard() {
  if (!M5.hasSD()) {
    Serial.println("M5Paper did not report an SD card interface.");
    return false;
  }

  int chipSelect = M5.getPin(m5::pin_name_t::sd_spi_cs);
  if (chipSelect < 0) chipSelect = GPIO_NUM_4;
  SPI.begin(M5.getPin(m5::pin_name_t::sd_spi_sclk),
            M5.getPin(m5::pin_name_t::sd_spi_miso),
            M5.getPin(m5::pin_name_t::sd_spi_mosi), chipSelect);
  if (!SD.begin(chipSelect, SPI, 25000000)) {
    Serial.println("Could not mount SD card. Check card format and seating.");
    return false;
  }

  Serial.printf("Mounted SD card: %llu MB\n",
                static_cast<unsigned long long>(SD.cardSize() / (1024ULL * 1024ULL)));
  return true;
}

void drawDeviceStatus() {
  auto& display = M5.Display;
  const int rssi = weatherTestMode
      ? testRssi : WiFi.isConnected() ? WiFi.RSSI() : -100;
  if (forecast.valid || showWeekForecast) {
    if ((weatherTestMode && rssi <= -100) ||
        (!weatherTestMode && !WiFi.isConnected())) {
      drawWeatherIconFitted("/wifioff.png", 18, 897, 76, 34);
    } else {
      const int level = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
      String wifiIcon = "/wifi" + String(level) + ".png";
      if (!sdCardReady || !SD.exists(sdImageFile(wifiIcon.c_str()).c_str())) {
        wifiIcon = "/wifi" + String(level - 1) + ".png";
      }
      drawWeatherIconFitted(wifiIcon.c_str(), 18, 897, 76, 34);
    }
  }

  const int battery = weatherTestMode
      ? testBatteryPercent : M5.Power.getBatteryLevel();
  bool inferredCharging = false;
  if (!weatherTestMode && battery >= 0) {
    const uint8_t fullCount = preferences.getUChar("batt_full", 0);
    const uint8_t nextCount = battery >= 100
        ? static_cast<uint8_t>(min<int>(fullCount + 1, 2)) : 0;
    if (nextCount != fullCount) preferences.putUChar("batt_full", nextCount);
    inferredCharging = nextCount >= 2;
  }
  const bool charging = weatherTestMode
      ? testCharging
      : M5.Power.isCharging() == M5.Power.is_charging || inferredCharging;
  const char* chargeLevelIcon = battery >= 95 ? "/chargefull.png"
      : battery >= 65 ? "/charge3.png"
      : battery >= 30 ? "/charge2.png"
                      : "/charge1.png";
  auto iconAvailable = [](const char* path) {
    size_t length = 0;
    return isApprovedSdImage(path) || getEmbeddedPng(path, length) != nullptr;
  };
  const bool chargeSetComplete = iconAvailable("/chargefull.png") &&
      iconAvailable("/charge3.png") && iconAvailable("/charge2.png") &&
      iconAvailable("/charge1.png");
  const char* batteryIcon = charging
      ? (!chargeSetComplete && isApprovedSdImage("/batterycharge.png"))
            ? "/batterycharge.png" : chargeLevelIcon
      : battery < 0 ? "/batteryunknown.png"
        : battery < 10 ? "/batterycritical.png"
        : battery < 35 ? "/battery25.png"
        : battery < 60 ? "/battery50.png"
        : battery < 85 ? "/battery75.png"
                       : "/batteryfull.png";
  drawWeatherIconFitted(batteryIcon, display.width() - 72, 895, 64, 32);
  display.setTextSize(2);
  display.setCursor(display.width() - 148, 900);
  if (battery >= 0) {
    display.printf("%d%%%s", battery,
                   batterySaver || charging ? "+" : "");
  }
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(2);
  display.drawString(
      "ver. " + preferences.getString("fw_version", "0.0.0"),
      display.width() / 2, 903);
  display.setTextDatum(textdatum_t::top_left);
}

String currentWeatherIcon() {
  if (forecast.periodCount == 0) return "/cloudy.png";
  return weatherIcon(forecast.periods[0].shortForecast,
                     forecast.periods[0].isDaytime);
}

void drawLocalEnvironmentLine() {
  auto& display = M5.Display;
  display.fillRect(16, 598, display.width() - 32, 34, TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(2);
  const float sensorTemperature = weatherTestMode
      ? testLocalTemperatureC : localTemperatureC;
  const float sensorHumidity = weatherTestMode
      ? testLocalHumidity : localHumidity;
  if (isfinite(sensorTemperature) && isfinite(sensorHumidity)) {
    const float localTemp = useCelsius
        ? sensorTemperature : sensorTemperature * 9.0f / 5.0f + 32.0f;
    display.drawString("M5Paper sensor  " +
                           String(localTemp, 1) +
                           (useCelsius ? " C  /  " : " F  /  ") +
                           String(sensorHumidity, 0) + "% RH",
                       display.width() / 2, 603);
  } else {
    display.drawString("M5Paper sensor unavailable",
                       display.width() / 2, 603);
  }
  display.setTextDatum(textdatum_t::top_left);
}

void applyForecastEpdMode() {
  if (instantLoad) M5.Display.setEpdMode(lgfx::epd_mode::epd_text);
}

void drawForecastContent(bool partialUpdate) {
  applyForecastEpdMode();
  auto& display = M5.Display;
  display.setTextColor(TFT_BLACK);
  display.setTextWrap(false);

  if (!forecast.valid || forecast.periodCount == 0) {
    drawWeatherIconFitted(alertIconPath(), 40, 116, 460, 400);
    drawImage("/wifioff.png", (display.width() - 176) / 2, 536);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(3);
    display.drawString("WEATHER UNAVAILABLE", display.width() / 2, 628);
    display.setTextDatum(textdatum_t::top_left);
    display.setTextWrap(true);
    display.setTextSize(2);
    display.setCursor(32, 686);
    display.println(lastWeatherError);
    display.setTextWrap(false);
    return;
  }

  const String icon = currentWeatherIcon();
  static String displayedIcon;
  if (!simpleMode && (!partialUpdate || icon != displayedIcon)) {
    display.fillRect(30, 92, display.width() - 60, 386, TFT_WHITE);
    drawWeatherIconFitted(icon.c_str(), 35, 96, display.width() - 70, 374);
    displayedIcon = icon;
  }

  const ForecastPeriod& current = forecast.periods[0];
  const int32_t centerX = display.width() / 2;
  if (simpleMode) {
    constexpr int32_t margin = 5;
    constexpr int32_t temperatureMargin = 20;
    constexpr int32_t contentTop = 90;
    constexpr int32_t contentBottom = 752;
    String feelsLikeTemperature =
        String(displayTemperature(feelsLikeF(current), "F"), 0);
    feelsLikeTemperature += useCelsius ? " C" : " F";
    auto fitSize = [&](const String& text, float cap,
                       int32_t sideMargin) -> float {
      const int32_t maxTextWidth = display.width() - sideMargin * 2;
      display.setTextSize(1);
      const int32_t baseWidth = max<int32_t>(1, display.textWidth(text));
      float size = min(cap, static_cast<float>(maxTextWidth) / baseWidth);
      display.setTextSize(size);
      while (size > 0.5f && display.textWidth(text) > maxTextWidth) {
        size *= 0.97f;
        display.setTextSize(size);
      }
      return size;
    };
    String conditionLines[2];
    conditionLines[0] = current.shortForecast;
    size_t conditionLineCount = 1;
    float conditionSize = fitSize(conditionLines[0], 9.0f, margin);
    if (conditionSize < 3.5f) {
      const String full = current.shortForecast;
      int split = -1;
      for (int i = 0; i < static_cast<int>(full.length()); ++i) {
        if (full[i] != ' ') continue;
        if (split < 0 || abs(i - static_cast<int>(full.length()) / 2) <
                             abs(split - static_cast<int>(full.length()) / 2)) {
          split = i;
        }
      }
      if (split > 0) {
        conditionLines[0] = full.substring(0, split);
        conditionLines[1] = full.substring(split + 1);
        conditionLineCount = 2;
        conditionSize = min(fitSize(conditionLines[0], 9.0f, margin),
                            fitSize(conditionLines[1], 9.0f, margin));
      }
    }
    display.setTextSize(conditionSize);
    const int32_t conditionLineHeight = display.fontHeight();
    const int32_t conditionHeight =
        static_cast<int32_t>(conditionLineCount) * conditionLineHeight +
        (static_cast<int32_t>(conditionLineCount) - 1) * 4;
    float temperatureSize = fitSize(feelsLikeTemperature, 22.0f, temperatureMargin);
    int32_t temperatureHeight = 0;
    int32_t iconHeight = 0;
    for (;;) {
      display.setTextSize(temperatureSize);
      temperatureHeight = display.fontHeight();
      const int32_t conditionTop =
          contentBottom - temperatureHeight - 26 - 8 - conditionHeight;
      iconHeight = conditionTop - 8 - contentTop;
      if (iconHeight >= 170 || temperatureSize <= 6.0f) break;
      temperatureSize *= 0.93f;
    }
    const int32_t temperatureTop = contentBottom - temperatureHeight;
    const int32_t labelTop = temperatureTop - 26;
    const int32_t conditionTop = labelTop - 8 - conditionHeight;
    display.fillRect(0, contentTop - 4, display.width(),
                     contentBottom - contentTop + 12, TFT_WHITE);
    if (iconHeight > 40) {
      drawWeatherIconFitted(icon.c_str(), 20, contentTop,
                            display.width() - 40, iconHeight);
    }
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(conditionSize);
    for (size_t i = 0; i < conditionLineCount; ++i) {
      display.drawString(conditionLines[i], centerX,
                         conditionTop + static_cast<int32_t>(i) *
                             (conditionLineHeight + 4));
    }
    display.setTextSize(2);
    display.drawString("FEELS LIKE", centerX, labelTop);
    display.setTextSize(temperatureSize);
    display.drawString(feelsLikeTemperature, centerX, temperatureTop);
    const int32_t highLowTop = 764;
    const int32_t highLowHeight = 84;
    display.drawRoundRect(24, highLowTop, display.width() - 48,
                          highLowHeight, 8, TFT_BLACK);
    display.drawFastVLine(display.width() / 2, highLowTop + 8,
                          highLowHeight - 16, TFT_LIGHTGREY);
    display.setTextSize(2);
    display.drawString("HIGH", display.width() / 4, highLowTop + 8);
    display.drawString("LOW", display.width() * 3 / 4, highLowTop + 8);
    display.setTextSize(4);
    display.drawString(
        String(displayTemperature(current.highTemperature, "F"), 0) +
            (useCelsius ? " C" : " F"),
        display.width() / 4, highLowTop + 36);
    display.drawString(
        String(displayTemperature(current.lowTemperature, "F"), 0) +
            (useCelsius ? " C" : " F"),
        display.width() * 3 / 4, highLowTop + 36);
    if (!todayCalendarText.isEmpty()) {
      display.setTextSize(1);
      display.drawString("Today: " + todayCalendarText, centerX, 850);
    }
    display.setTextDatum(textdatum_t::top_left);
    return;
  }

  display.fillRect(8, 484, display.width() - 16, 396, TFT_WHITE);
  const int32_t metricSpacing = 106;
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(2);
  display.drawString("FEELS LIKE", centerX - metricSpacing, 492);
  display.drawString("ACTUAL", centerX + metricSpacing, 492);
  display.setTextSize(5);
  String feelsLikeTemperature =
      String(displayTemperature(feelsLikeF(current), "F"), 0);
  feelsLikeTemperature += useCelsius ? " C" : " F";
  display.drawString(feelsLikeTemperature, centerX - metricSpacing, 520);
  String actualTemperature =
      String(displayTemperature(current.temperature, "F"), 0);
  actualTemperature += useCelsius ? " C" : " F";
  display.drawString(actualTemperature, centerX + metricSpacing, 520);
  display.setTextSize(2);
  display.drawString(
      "High " + String(displayTemperature(current.highTemperature, "F"), 0) +
          (useCelsius ? " C   Low " : " F   Low ") +
          String(displayTemperature(current.lowTemperature, "F"), 0) +
          (useCelsius ? " C" : " F"),
      centerX, 576);
  drawLocalEnvironmentLine();

  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(3);
  display.drawString(current.shortForecast, centerX, 640);
  display.setTextDatum(textdatum_t::top_left);
  if (current.temperature <= 0) {
    drawImage("/extremecold.png", display.width() - 44, 674);
    display.setTextSize(2);
    display.setTextDatum(textdatum_t::top_center);
    display.drawString("Extreme cold advisory", centerX, 678);
  } else if (current.temperature >= 100) {
    drawImage("/extremehot.png", display.width() - 44, 674);
    display.setTextSize(2);
    display.setTextDatum(textdatum_t::top_center);
    display.drawString("Extreme heat advisory", centerX, 678);
  }

  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(1);
  display.drawString(current.detailedForecast, centerX,
                     current.temperature <= 0 || current.temperature >= 100
                         ? 710 : 684);

  display.setTextSize(2);
  String wind = "Wind " + current.windSpeed + " " + current.windDirection;
  if (current.precipitation >= 0) {
    wind += "   Rain " + String(current.precipitation) + "%";
  }
  display.drawString(wind, centerX, 732);
  display.setTextSize(2);
  if (current.humidity >= 0) {
    display.drawString("Humidity " + String(current.humidity) + "%",
                       centerX, 762);
  }
  if (!todayCalendarText.isEmpty()) {
    display.setTextSize(1);
    display.drawString("Today: " + todayCalendarText, centerX, 792);
  }
  if (sunTimesValid) {
    constexpr int32_t sunIconSize = 34;
    const int32_t sunsetColumnX = display.width() / 2 + 14;
    drawWeatherIconFitted("/sunup.png", 24, 820, sunIconSize, sunIconSize);
    drawWeatherIconFitted("/sundown.png", sunsetColumnX, 820, sunIconSize,
                          sunIconSize);
    display.setTextSize(2);
    display.setTextDatum(textdatum_t::middle_left);
    display.drawString("Sunrise " + sunriseText, 24 + sunIconSize + 8, 833);
    display.drawString("Sunset " + sunsetText,
                       sunsetColumnX + sunIconSize + 8, 833);
    display.setTextDatum(textdatum_t::top_left);
  }
  display.setTextDatum(textdatum_t::top_left);
}

size_t dailyForecastIndices(size_t indices[kForecastDays]) {
  size_t count = 0;
  for (size_t i = 0; i < forecast.periodCount && count < kForecastDays; ++i) {
    if (i > 0 && forecast.periods[i].isDaytime) indices[count++] = i;
  }
  if (count == 0 && forecast.periodCount > 0) indices[count++] = 0;
  return count;
}

void drawForecastDayDetails();

void drawWeekForecast() {
  applyForecastEpdMode();
  if (showDayDetails) {
    drawForecastDayDetails();
    return;
  }

  auto& display = M5.Display;
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextSize(3);
  display.setCursor(24, 18);
  display.print(simpleMode ? "3-DAY FORECAST" : "7-DAY FORECAST");
  display.setTextSize(2);
  display.setTextDatum(textdatum_t::top_right);
  display.drawString(displayLocationName().substring(0, 25),
                     display.width() - 24, 28);
  display.setTextDatum(textdatum_t::top_left);
  display.drawFastHLine(24, 66, display.width() - 48, TFT_BLACK);

  size_t indices[kForecastDays] = {};
  const size_t availableCount = dailyForecastIndices(indices);
  const size_t count = min(availableCount, simpleMode ? size_t{3} : kForecastDays);
  if (sunTimesValid) {
    constexpr int32_t sunIconSize = 31;
    constexpr int32_t leftColumnX = 28;
    const int32_t rightColumnX = display.width() / 2 + 14;
    drawWeatherIconFitted("/sunup.png", leftColumnX, 82, sunIconSize,
                          sunIconSize);
    drawWeatherIconFitted("/sundown.png", rightColumnX, 82, sunIconSize,
                          sunIconSize);
    display.setTextSize(2);
    display.setTextDatum(textdatum_t::middle_left);
    display.drawString("Sunrise " + sunriseText,
                       leftColumnX + sunIconSize + 8, 94);
    display.drawString("Sunset " + sunsetText,
                       rightColumnX + sunIconSize + 8, 94);
    display.setTextDatum(textdatum_t::top_left);
  }

  constexpr int32_t rowStartY = 128;
  constexpr int32_t textLeft = 136;
  const int32_t textCenter = (textLeft + display.width() - 24) / 2;
  const int32_t rowHeight = simpleMode
      ? (display.height() - rowStartY - 60) / (count == 0 ? 1 : count)
      : 105;
  for (size_t row = 0; row < count; ++row) {
    const ForecastPeriod& period = forecast.periods[indices[row]];
    const int y = rowStartY + static_cast<int>(row) * rowHeight;
    display.drawFastHLine(24, y, display.width() - 48, TFT_LIGHTGREY);
    if (simpleMode) {
      constexpr int32_t textLeft = 164;
      constexpr int32_t textRight = 24;
      drawWeatherIconFitted(weatherIcon(period.shortForecast, period.isDaytime),
                            28, y + 14, 120, 126);
      const int32_t textCenter =
          (textLeft + display.width() - textRight) / 2;
      const int32_t textWidth = display.width() - textLeft - textRight;
      String heading = period.name + "  H " +
          String(static_cast<int>(displayTemperature(
              period.highTemperature, "F"))) +
          (useCelsius ? " C  L " : " F  L ") +
          String(static_cast<int>(displayTemperature(
              period.lowTemperature, "F"))) + (useCelsius ? " C" : " F");
      display.setTextDatum(textdatum_t::top_center);
      display.setTextSize(2);
      while (heading.length() > 1 && display.textWidth(heading) > textWidth) {
        heading.remove(heading.length() - 1);
      }
      display.drawString(heading, textCenter, y + 20);

      String condition = period.shortForecast;
      display.setTextSize(3);
      while (condition.length() > 1 &&
             display.textWidth(condition) > textWidth) {
        condition.remove(condition.length() - 1);
      }
      display.drawString(condition, textCenter, y + 66);

      String wind = period.windSpeed + " " + period.windDirection +
          "  Rain " + (period.precipitation >= 0
              ? String(period.precipitation) : String("--")) + "%";
      display.setTextSize(2);
      while (wind.length() > 1 && display.textWidth(wind) > textWidth) {
        wind.remove(wind.length() - 1);
      }
      display.drawString(wind, textCenter, y + 112);

      String detail = period.detailedForecast;
      while (detail.length() > 1 && display.textWidth(detail) > textWidth) {
        detail.remove(detail.length() - 1);
      }
      if (detail.length() < period.detailedForecast.length() &&
          detail.length() > 3) {
        detail.remove(detail.length() - 3);
        detail += "...";
      }
      display.drawString(detail, textCenter, y + 154);
      display.setTextDatum(textdatum_t::top_left);
      continue;
    }
    drawWeatherIconFitted(weatherIcon(period.shortForecast, period.isDaytime),
                           28, y + 4, 92, 94);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(2);
    display.drawString(period.name + "   " +
                           String(static_cast<int>(displayTemperature(
                               period.highTemperature, "F"))) +
                           (useCelsius ? " C  Low " : " F  Low ") +
                           String(static_cast<int>(displayTemperature(
                               period.lowTemperature, "F"))) +
                           (useCelsius ? " C" : " F"),
                       textCenter, y + 5);
    display.setTextSize(2);
    display.drawString(period.shortForecast + "   " + period.windSpeed +
                           " " + period.windDirection + "   Rain " +
                           (period.precipitation >= 0
                                ? String(period.precipitation) : String("--")) +
                           "%",
                       textCenter, y + 34);
    String detail = period.detailedForecast;
    while (detail.length() > 1 &&
           display.textWidth(detail) > display.width() - textLeft - 40) {
      detail.remove(detail.length() - 1);
    }
    if (detail.length() < period.detailedForecast.length() &&
        detail.length() > 3) {
      detail.remove(detail.length() - 3);
      detail += "...";
    }
    display.setTextSize(1);
    display.drawString(detail, textCenter, y + 57);
    display.setTextDatum(textdatum_t::top_left);
  }
  display.drawFastHLine(24, 874, display.width() - 48, TFT_BLACK);
  display.setTextSize(1);
  display.setCursor(24, 882);
  display.print(simpleMode
                    ? "A: setup  hold A: settings  B: refresh  C: now  hold C: details"
                    : "A: setup  hold A: settings  B: refresh  C: now  hold C: details");
  drawDeviceStatus();
}

void drawForecastDayDetails() {
  applyForecastEpdMode();
  if (selectedForecastIndex >= forecast.periodCount) {
    selectedForecastIndex = forecast.periodCount > 1 ? 1 : 0;
  }
  const ForecastPeriod& period = forecast.periods[selectedForecastIndex];
  auto& display = M5.Display;
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(3);
  display.drawString(period.name + " FORECAST", display.width() / 2, 20);
  display.setTextSize(2);
  display.drawString(period.detailedForecast, display.width() / 2, 68);
  display.drawFastHLine(24, 108, display.width() - 48, TFT_BLACK);

  const int32_t columnCenter[2] = {
      display.width() / 4, display.width() * 3 / 4};
  const ForecastPeriod::Part* parts[2] = {&period.morning, &period.night};
  const char* titles[2] = {"MORNING", "NIGHT"};
  for (size_t i = 0; i < 2; ++i) {
    const ForecastPeriod::Part& part = *parts[i];
    const bool isMorning = i == 0;
    const bool isDaytime = isMorning;
    const char* icon = weatherIcon(part.shortForecast, isDaytime);
    drawWeatherIconFitted(icon, columnCenter[i] - 70, 132, 140, 140);

    display.setTextSize(3);
    display.drawString(titles[i], columnCenter[i], 282);
    display.setTextSize(4);
    const float displayTemp = useCelsius
        ? (part.temperature - 32.0f) * 5.0f / 9.0f
        : part.temperature;
    String temperature = String(displayTemp, 0);
    temperature += useCelsius ? " C" : " F";
    display.drawString(temperature, columnCenter[i], 324);
    display.setTextSize(2);
    display.drawString(part.shortForecast, columnCenter[i], 382);
    display.setTextSize(1);
    String wind = part.windSpeed;
    if (!part.windDirection.isEmpty()) wind += " " + part.windDirection;
    display.drawString("Wind " + wind, columnCenter[i], 420);
    display.drawString(
        "Rain " +
            (part.precipitation >= 0
                 ? String(part.precipitation) : String("--")) +
            "%",
        columnCenter[i], 444);
  }
  display.drawFastVLine(display.width() / 2, 124, 350, TFT_LIGHTGREY);
  display.drawFastHLine(24, 500, display.width() - 48, TFT_LIGHTGREY);
  display.setTextSize(2);
  display.drawString(period.detailedForecast, display.width() / 2, 522);
  display.drawRect(92, 854, display.width() - 184, 54, TFT_BLACK);
  display.setTextSize(2);
  display.drawString("C: BACK TO FORECAST",
                     display.width() / 2, 870);
  display.setTextDatum(textdatum_t::top_left);
}

void drawForecast(const String& status = "") {
  if (!clockReady && status == "Waiting for network time synchronization.") {
    drawTimeSyncScreen();
    return;
  }
  applyForecastEpdMode();
  auto& display = M5.Display;
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  drawClock(true);
  display.drawFastHLine(24, 66, display.width() - 48, TFT_BLACK);

  display.setTextDatum(textdatum_t::top_left);
  drawForecastContent(false);
  display.drawFastHLine(24, 866, display.width() - 48, TFT_BLACK);
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setCursor(24, 874);
  String footerText = status;
  if (footerText.isEmpty() && forecast.valid && forecast.periodCount > 0) {
    footerText = unmappedWeatherIconHint(
        forecast.periods[0].shortForecast,
        forecast.periods[0].isDaytime);
  }
  if (footerText.isEmpty()) {
    footerText = "A: setup  hold A: settings  B: refresh  C: 7-day";
  }
  display.println(footerText);
  drawDeviceStatus();
}

void drawForecastFooter(const String& status = "") {
  auto& display = M5.Display;
  display.fillRect(0, 866, display.width(), 32, TFT_WHITE);
  display.drawFastHLine(24, 866, display.width() - 48, TFT_BLACK);
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setCursor(24, 874);
  String footerText = status;
  if (footerText.isEmpty() && forecast.valid && forecast.periodCount > 0) {
    footerText = unmappedWeatherIconHint(
        forecast.periods[0].shortForecast,
        forecast.periods[0].isDaytime);
  }
  if (footerText.isEmpty()) {
    footerText = "A: setup  hold A: settings  B: refresh  C: 7-day";
  }
  display.println(footerText);
  drawDeviceStatus();
}

void showWeatherUnavailable(const String& message) {
  wifiUnavailable = true;
  clockUpdatesEnabled = false;
  lastWeatherError = message;
  forecast.valid = false;
  drawForecast(message);
}

bool applyWeatherTest() {
  const String condition = webServer.arg("condition");
  const String testTime = webServer.arg("testTime");
  const String location = webServer.arg("testLocation");
  const String detailedForecast = webServer.arg("testDetails");
  const String windDirection = webServer.arg("testWindDirection");
  const String calendarText = webServer.arg("testCalendar");
  const String sunrise = webServer.arg("testSunrise");
  const String sunset = webServer.arg("testSunset");
  float temperature = 0;
  float feelsLike = 0;
  float high = 0;
  float low = 0;
  float humidity = 0;
  float windSpeed = 0;
  float precipitation = 0;
  float sensorTemperature = 0;
  float sensorHumidity = 0;
  float battery = 0;
  float rssi = 0;
  uint16_t sunriseMinute = 0;
  uint16_t sunsetMinute = 0;
  const bool validCondition =
      condition == "clear" || condition == "partly-cloudy" ||
      condition == "cloudy" || condition == "overcast" ||
      condition == "rain" || condition == "snow" || condition == "fog" ||
      condition == "thunderstorm" || condition == "hurricane" ||
      condition == "tornado" || condition == "wind" ||
      condition == "drizzle" || condition == "freezing-rain" ||
      condition == "rain-showers" || condition == "snow-showers" ||
      condition == "hail" || condition == "sleet" || condition == "dust" ||
      condition == "smoke" || condition == "squalls" ||
      condition == "volcanic-ash";
  const bool validSunTimes =
      (sunrise.isEmpty() && sunset.isEmpty()) ||
      (parseMinuteOfDay(sunrise, sunriseMinute) &&
       parseMinuteOfDay(sunset, sunsetMinute));
  if (!validCondition || !validTestTime(testTime) || location.isEmpty() ||
      location.length() > 60 || detailedForecast.length() > 180 ||
      windDirection.isEmpty() || windDirection.length() > 20 ||
      calendarText.length() > 120 || !validSunTimes ||
      !parseStrictFloat(webServer.arg("testTemperature"), temperature) ||
      !parseStrictFloat(webServer.arg("testFeelsLike"), feelsLike) ||
      !parseStrictFloat(webServer.arg("testHigh"), high) ||
      !parseStrictFloat(webServer.arg("testLow"), low) ||
      !parseStrictFloat(webServer.arg("testHumidity"), humidity) ||
      !parseStrictFloat(webServer.arg("testWindSpeed"), windSpeed) ||
      !parseStrictFloat(webServer.arg("testPrecipitation"), precipitation) ||
      !parseStrictFloat(webServer.arg("testSensorTemperature"),
                        sensorTemperature) ||
      !parseStrictFloat(webServer.arg("testSensorHumidity"), sensorHumidity) ||
      !parseStrictFloat(webServer.arg("testBattery"), battery) ||
      !parseStrictFloat(webServer.arg("testRssi"), rssi) ||
      temperature < -100 || temperature > 150 ||
      feelsLike < -100 || feelsLike > 150 ||
      high < -100 || high > 150 || low < -100 || low > 150 || high < low ||
      humidity < 0 || humidity > 100 || windSpeed < 0 || windSpeed > 300 ||
      precipitation < 0 || precipitation > 100 ||
      sensorTemperature < -100 || sensorTemperature > 100 ||
      sensorHumidity < 0 || sensorHumidity > 100 || battery < 0 ||
      battery > 100 || rssi < -100 || rssi > 0) {
    lastWeatherError = "Invalid test values. Check the ranges and required fields.";
    return false;
  }

  if (!weatherTestMode) {
    liveForecastBeforeTest = forecast;
    liveLocationNameBeforeTest = savedLocationName;
    liveSunriseBeforeTest = sunriseText;
    liveSunsetBeforeTest = sunsetText;
    liveCalendarTextBeforeTest = todayCalendarText;
    liveSunTimesValidBeforeTest = sunTimesValid;
    liveLocalTemperatureCBeforeTest = localTemperatureC;
    liveLocalHumidityBeforeTest = localHumidity;
  }

  static const char* const conditions[] = {
      "Clear sky", "Partly cloudy", "Cloudy", "Overcast", "Rain", "Snow",
      "Fog", "Thunderstorms", "Hurricane", "Tornado", "Windy",
      "Drizzle", "Freezing rain", "Rain showers", "Snow showers",
      "Hail", "Sleet", "Dust", "Smoke", "Squalls", "Volcanic ash"};
  static const char* const conditionNames[] = {
      "clear", "partly-cloudy", "cloudy", "overcast", "rain", "snow",
      "fog", "thunderstorm", "hurricane", "tornado", "wind",
      "drizzle", "freezing-rain", "rain-showers", "snow-showers",
      "hail", "sleet", "dust", "smoke", "squalls", "volcanic-ash"};
  static const int conditionCodes[] = {
      0, 2, 3, 3, 61, 71, 45, 95, 95, 95, 3,
      53, 66, 80, 85, 99, 67, 3, 3, 3, 3};
  size_t conditionIndex = 0;
  while (conditionIndex < sizeof(conditionNames) / sizeof(conditionNames[0]) &&
         condition != conditionNames[conditionIndex]) {
    ++conditionIndex;
  }
  if (conditionIndex >= sizeof(conditionNames) / sizeof(conditionNames[0])) {
    lastWeatherError = "Unsupported test weather condition.";
    return false;
  }

  const int32_t testUtcOffsetSeconds = locationUtcOffsetSeconds;
  const time_t testBaseEpoch =
      (clockReady ? time(nullptr) : static_cast<time_t>(1735689600)) +
      testUtcOffsetSeconds;
  JsonDocument weather;
  weather["utc_offset_seconds"] = testUtcOffsetSeconds;
  JsonObject current = weather["current"].to<JsonObject>();
  JsonObject daily = weather["daily"].to<JsonObject>();
  JsonObject hourly = weather["hourly"].to<JsonObject>();

  JsonArray dates = daily["time"].to<JsonArray>();
  JsonArray dailyCodes = daily["weather_code"].to<JsonArray>();
  JsonArray dailyDescriptions =
      daily["weather_description"].to<JsonArray>();
  JsonArray highs = daily["temperature_2m_max"].to<JsonArray>();
  JsonArray lows = daily["temperature_2m_min"].to<JsonArray>();
  JsonArray rainChances =
      daily["precipitation_probability_max"].to<JsonArray>();
  JsonArray dailyWinds = daily["wind_speed_10m_max"].to<JsonArray>();
  JsonArray sunrises = daily["sunrise"].to<JsonArray>();
  JsonArray sunsets = daily["sunset"].to<JsonArray>();

  JsonArray hourlyTimes = hourly["time"].to<JsonArray>();
  JsonArray hourlyTemperatures =
      hourly["temperature_2m"].to<JsonArray>();
  JsonArray hourlyCodes = hourly["weather_code"].to<JsonArray>();
  JsonArray hourlyDescriptions =
      hourly["weather_description"].to<JsonArray>();
  JsonArray hourlyPrecipitation =
      hourly["precipitation_probability"].to<JsonArray>();
  JsonArray hourlyWinds = hourly["wind_speed_10m"].to<JsonArray>();
  JsonArray hourlyDirections =
      hourly["wind_direction_10m"].to<JsonArray>();

  char testDate[11];
  tm testDateTime = {};
  gmtime_r(&testBaseEpoch, &testDateTime);
  strftime(testDate, sizeof(testDate), "%Y-%m-%d", &testDateTime);

  for (size_t day = 0; day < kForecastDays; ++day) {
    const time_t dayEpoch = testBaseEpoch + static_cast<time_t>(day) * 86400;
    tm dayTime = {};
    char date[11];
    gmtime_r(&dayEpoch, &dayTime);
    strftime(date, sizeof(date), "%Y-%m-%d", &dayTime);
    dates.add(date);
    dailyCodes.add(conditionCodes[conditionIndex]);
    dailyDescriptions.add(conditions[conditionIndex]);
    highs.add(high);
    lows.add(low);
    rainChances.add(precipitation);
    dailyWinds.add(windSpeed);
    sunrises.add(sunrise.isEmpty() ? "" : String(date) + "T" + sunrise + ":00");
    sunsets.add(sunset.isEmpty() ? "" : String(date) + "T" + sunset + ":00");

    for (int hour = 0; hour < 24; ++hour) {
      char isoHour[20];
      snprintf(isoHour, sizeof(isoHour), "%sT%02d:00", date, hour);
      hourlyTimes.add(isoHour);
      hourlyTemperatures.add(hour < 12 ? high : low);
      hourlyCodes.add(conditionCodes[conditionIndex]);
      hourlyDescriptions.add(conditions[conditionIndex]);
      hourlyPrecipitation.add(precipitation);
      hourlyWinds.add(windSpeed);
      hourlyDirections.add(windDirectionDegrees(windDirection));
    }
  }

  uint16_t testMinute = 0;
  parseMinuteOfDay(testTime, testMinute);
  const int testHour = testMinute / 60;
  String currentIso = String(testDate) + "T" + testTime + ":00";
  current["time"] = currentIso;
  current["temperature_2m"] = temperature;
  current["relative_humidity_2m"] = humidity;
  current["apparent_temperature"] = feelsLike;
  current["weather_code"] = conditionCodes[conditionIndex];
  current["weather_description"] = conditions[conditionIndex];
  current["is_day"] = testHour >= 6 && testHour < 18;
  current["wind_speed_10m"] = windSpeed;
  current["wind_direction_10m"] = windDirectionDegrees(windDirection);
  current["test_details"] = detailedForecast;

  if (!parseForecastResponse(weather, forecastScratch, location, false)) {
    return false;
  }
  forecast = forecastScratch;
  savedLocationName = location;
  todayCalendarText = calendarText;
  testClockText = testTime;
  testLocalTemperatureC = sensorTemperature;
  testLocalHumidity = sensorHumidity;
  testBatteryPercent = static_cast<int>(roundf(battery));
  testRssi = static_cast<int>(roundf(rssi));
  testCharging = webServer.arg("testCharging") == "1";
  localTemperatureC = sensorTemperature;
  localHumidity = sensorHumidity;
  weatherTestMode = true;
  wifiUnavailable = false;
  lastWeatherError = "";
  showWeekForecast ? drawWeekForecast() : drawForecast();
  return true;
}

void stopWeatherTest() {
  if (!weatherTestMode) {
    drawForecast();
    return;
  }
  forecast = liveForecastBeforeTest;
  savedLocationName = liveLocationNameBeforeTest;
  sunriseText = liveSunriseBeforeTest;
  sunsetText = liveSunsetBeforeTest;
  todayCalendarText = liveCalendarTextBeforeTest;
  sunTimesValid = liveSunTimesValidBeforeTest;
  localTemperatureC = liveLocalTemperatureCBeforeTest;
  localHumidity = liveLocalHumidityBeforeTest;
  weatherTestMode = false;
  testClockText = "";
  showWeekForecast ? drawWeekForecast() : drawForecast();
}

String setupPage() {
  String page;
  page.reserve(22000);
page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Yet Another Weather OS</title><style>*{box-sizing:border-box}body{font:17px/1.4 system-ui,sans-serif;background:#eceef1;color:#1d2330;max-width:36em;margin:0 auto;padding:1em}h1{font-size:1.5em;margin:.1em 0}h2{font-size:1.15em;margin:0 0 .6em}.card{background:#fff;border:1px solid #cfd4dc;border-radius:14px;padding:1em 1.1em;margin:0 0 1em;box-shadow:0 1px 2px rgba(0,0,0,.06)}.sub{color:#5a6372;margin:.2em 0}small,.hint{color:#5a6372;font-size:.88em}label{display:block;margin:.6em 0}input,select,textarea{font:inherit;width:100%;padding:.55em;margin:.3em 0 0;border:1px solid #b9bfca;border-radius:8px;background:#fff}label.check{display:flex;gap:.6em;align-items:flex-start}label.check input{width:auto;margin:.3em 0 0}button{font:inherit;padding:.65em 1.1em;margin:.4em .4em 0 0;border:0;border-radius:8px;background:#1d2330;color:#fff}button.danger{background:#b3261e}button.main{width:100%;padding:.9em;font-weight:600}summary{cursor:pointer;font-weight:600;padding:.3em 0}input[readonly]{opacity:.45;background:#e3e5e9}.test-grid{padding:.2em 0}.stat{font-size:1.3em;font-weight:600}</style></head><body>");
page += F("<div class='card'><h1>Yet Another Weather OS</h1><p class='sub'>YAWOS settings</p><p class='sub'>Data used this month</p><div class='stat'>");
  page += formatDataUsage();
page += F("</div><small>Counted by this device for weather, update and download requests. Saved to the SD card when one is inserted.</small></div>");
page += F("<form method='post' action='/save'>");
page += F("<div class='card'><h2>Location</h2><p class='hint'>Enter a city, address, place name, ZIP code, or latitude and longitude. Place names are geocoded by Open-Meteo. Public-IP location is used only when no manual location is set.</p><label>Location<input name='location' maxlength='100' placeholder='City, address, ZIP, or 40.01, -105.27' value='");
  page += htmlEscape(savedLocationQuery.isEmpty() ? savedZipCode : savedLocationQuery);
page += F("'></label><label class='check'><input type='checkbox' name='clearLocation' value='1'> Clear saved location</label><label>Clock offset (display only)<select name='timezone'>");
  page += F("<option value='auto'");
  if (selectedTimezone == "auto") page += F(" selected");
  page += F(">Auto (location)</option>");
  for (int offsetHours = -12; offsetHours <= 14; ++offsetHours) {
    const String offsetValue = String(offsetHours);
    page += "<option value='";
    page += offsetValue;
    page += '\'';
    if (selectedTimezone == offsetValue) page += F(" selected");
    page += ">UTC ";
    if (offsetHours > 0) page += '+';
    page += String(offsetHours);
    page += F("</option>");
  }
page += F("</select></label></div>");
page += F("<div class='card'><h2>Weather</h2><label>Weather API<select name='weatherApi'>");
  const WeatherApiProvider apiProviders[] = {
      WeatherApiProvider::openMeteo, WeatherApiProvider::noaa,
      WeatherApiProvider::metNorway, WeatherApiProvider::wttr};
  for (WeatherApiProvider provider : apiProviders) {
    page += "<option value='";
    page += weatherApiPreferenceValue(provider);
    page += '\'';
    if (provider == weatherApiProvider) page += F(" selected");
    page += '>';
    page += weatherApiName(provider);
    page += F("</option>");
  }
page += F("</select></label><p class='hint'>All providers work without an API key. NOAA covers the United States only.</p><label>Temperature units<select name='temperatureUnits'><option value='F'");
  if (!useCelsius) page += F(" selected");
page += F(">Fahrenheit</option><option value='C'");
  if (useCelsius) page += F(" selected");
page += F(">Celsius</option></select></label><label class='check'><input type='checkbox' name='simpleMode' value='1' ");
  if (simpleMode) page += F("checked");
page += F("> Simple display (large icon and feels-like, three-day forecast)</label></div>");
page += F("<div class='card'><h2>Polling and power</h2><label>Weather update frequency (minutes)<input type='number' id='refreshMinutes' name='refreshMinutes' min='1' max='1440' step='1' required value='");
  page += String(refreshIntervalMinutes);
page += F("'></label><label class='check'><input type='checkbox' id='adaptiveUpdates' name='adaptiveUpdates' value='1' onchange='syncAdaptive()' ");
  if (adaptiveUpdates) page += F("checked");
page += F("> Adaptive polling (overrides the update frequency)</label><label>Slow poll rate in minutes (not rain or worse)<input type='number' id='slowPoll' name='slowPoll' min='1' max='1440' step='1' required value='");
  page += String(slowPollMinutes);
page += F("'></label><label>Quick poll rate in minutes (rain or worse)<input type='number' id='fastPoll' name='fastPoll' min='1' max='1440' step='1' required value='");
  page += String(fastPollMinutes);
page += F("'></label><label class='check'><input type='checkbox' name='batterySaver' value='1' ");
  if (batterySaver) page += F("checked");
page += F("> Battery saver: power off between refreshes using the RTC wake timer. With it off, the device never powers itself off.</label></div>");
page += F("<div class='card'><h2>Night shutdown</h2><label class='check'><input type='checkbox' name='nightShutdown' value='1' ");
  if (nightShutdownEnabled) page += F("checked");
page += F("> Shut down during the nighttime window</label><label>Starts (AM/PM)<input type='text' name='nightStart' maxlength='8' pattern='(0[1-9]|1[0-2]):[0-5][0-9] [AaPp][Mm]' required placeholder='10:00 PM' value='");
  page += formatAmPmMinuteOfDay(nightShutdownStartMinute);
page += F("'></label><label>Ends (AM/PM)<input type='text' name='nightEnd' maxlength='8' pattern='(0[1-9]|1[0-2]):[0-5][0-9] [AaPp][Mm]' required placeholder='06:00 AM' value='");
  page += formatAmPmMinuteOfDay(nightShutdownEndMinute);
page += F("'></label><p class='hint'>When the device boots during this window it shows Goodnight and schedules its next wake for the end time.</p></div>");
page += F("<div class='card'><h2>Updates</h2><p>Installed version: <b>");
  page += htmlEscape(preferences.getString("fw_version", "0.0.0"));
page += F("</b></p><label class='check'><input type='checkbox' name='autoUpdates' value='1' ");
  if (autoUpdates) page += F("checked");
page += F("> Automatic updating on every boot</label><label class='check'><input type='checkbox' name='nightlyUpdates' value='1' ");
  if (nightlyUpdates) page += F("checked");
page += F("> Nightly updates (also install n-prefixed update folders)</label><label>Update Repository Location<input name='repoUrl' maxlength='200' placeholder='https://github.com/owner/repo' value='");
  page += htmlEscape(String("https://github.com/") + updateRepo + "/tree/" + updateBranch);
page += F("'></label><p class='hint'>Paste the link to your own GitHub repository. Updates are read from its updates folder, image packs from images, and fonts from fonts.</p></div>");
page += F("<div class='card'><h2>Public Google Calendar (optional)</h2><label>Public calendar ID<input name='calendarId' maxlength='200' value='");
  page += htmlEscape(calendarId);
page += F("'></label><label>Google Calendar API key<input name='calendarApiKey' type='password' maxlength='200' placeholder='Leave blank to keep the saved key'></label><p class='hint'>The calendar must be publicly readable. The API key is stored in device preferences.</p></div>");
page += F("<div class='card'><h2>Saved Wi-Fi networks</h2><p class='hint'>Networks are tried in order. Pick a network to edit it or delete it. Leave the password blank to keep the saved one. A location set for a network takes priority when connected there.</p><p id='networkCount' class='hint'></p><div id='existingNetworks'><label>Network<select id='networkSelect' onchange='loadNetwork()'></select></label><div id='networkEdit' style='display:none'><label>Wi-Fi name (SSID)<input id='editSsid' maxlength='32'></label><label>Password<input id='editPassword' type='password' maxlength='63' placeholder='Leave blank to keep'></label><label>Location (optional)<input id='editLocation' maxlength='100'></label><button type='button' onclick='saveNetwork()'>Save changes</button><button type='button' class='danger' onclick='deleteNetwork()'>Delete slot</button></div></div><p id='editStatus' class='hint'></p></div>");
page += F("<div class='card'><h2>Add a Wi-Fi network</h2><p class='hint'>Enter the exact network name and its password (8 to 63 characters, or blank for an open network). The optional location (a place name, ZIP code, or latitude and longitude) is used whenever the device connects to this network. The network goes into the first free slot and takes effect right away.</p><label>Wi-Fi name<input id='addSsid' maxlength='32'></label><label>Password<input id='addPassword' type='password' maxlength='63'></label><label>Location (optional)<input id='addLocation' maxlength='100' placeholder='ZIP, place, or latitude, longitude'></label><button type='button' onclick='addNetwork()'>Add Wi-Fi network</button><p id='networkStatus' class='hint'></p></div>");
page += F("<div class='card'><h2>Import Wi-Fi QR text</h2><p class='hint'>Scan a Wi-Fi QR with your phone, then paste its decoded <code>WIFI:T:WPA;S:name;P:password;;</code> text here.</p><textarea id='wifiQr' rows='3' placeholder='WIFI:T:WPA;S:...;P:...;;'></textarea><button type='button' onclick='importWifiQr()'>Fill in Add form</button><p id='qrStatus' class='hint'></p></div>");
  page += F("<div class='card'><details><summary>Test</summary>"
            "<p>Preview the normal forecast screen with temporary sample values. "
            "These values are not saved and do not change live weather settings.</p>"
            "<div class='test-grid' id='testControls'>"
            "<label>Weather condition<select name='condition' id='testCondition'>"
            "<option value='clear'>Clear sky</option>"
            "<option value='partly-cloudy'>Partly cloudy</option>"
            "<option value='cloudy'>Cloudy</option>"
            "<option value='overcast'>Overcast</option>"
            "<option value='rain'>Rain</option>"
            "<option value='snow'>Snow</option>"
            "<option value='fog'>Fog</option>"
            "<option value='thunderstorm'>Thunderstorms</option>"
            "<option value='hurricane'>Hurricane</option>"
            "<option value='tornado'>Tornado</option>"
            "<option value='wind'>Windy</option>"
            "<option value='drizzle'>Drizzle</option>"
            "<option value='freezing-rain'>Freezing rain</option>"
            "<option value='rain-showers'>Rain showers</option>"
            "<option value='snow-showers'>Snow showers</option>"
            "<option value='hail'>Hail</option>"
            "<option value='sleet'>Sleet</option>"
            "<option value='dust'>Dust</option>"
            "<option value='smoke'>Smoke</option>"
            "<option value='squalls'>Squalls</option>"
            "<option value='volcanic-ash'>Volcanic ash</option>"
            "</select></label>"
            "<label>Display time<input type='time' name='testTime' value='12:00' required></label>"
            "<label>Location label<input name='testLocation' maxlength='60' value='Boulder, CO' required></label>"
            "<label>Temperature (F)<input type='number' step='any' name='testTemperature' value='72' required></label>"
            "<label>Feels like (F)<input type='number' step='any' name='testFeelsLike' value='70' required></label>"
            "<label>High (F)<input type='number' step='any' name='testHigh' value='78' required></label>"
            "<label>Low (F)<input type='number' step='any' name='testLow' value='55' required></label>"
            "<label>Humidity (%)<input type='number' step='any' name='testHumidity' value='45' required></label>"
            "<label>Wind speed (mph)<input type='number' step='any' name='testWindSpeed' value='5' required></label>"
            "<label>Wind direction<input name='testWindDirection' maxlength='20' value='NW' required></label>"
            "<label>Rain chance (%)<input type='number' step='any' name='testPrecipitation' value='10' required></label>"
            "<label>Detailed forecast<input name='testDetails' maxlength='180' value='A pleasant day for a walk.'></label>"
            "<label>Sunrise<input type='time' name='testSunrise' value='06:15'></label>"
            "<label>Sunset<input type='time' name='testSunset' value='19:45'></label>"
            "<label>Calendar text<input name='testCalendar' maxlength='120' value='Sample event'></label>"
            "<label>Device sensor temperature (C)<input type='number' step='any' name='testSensorTemperature' value='21.5' required></label>"
            "<label>Device sensor humidity (%)<input type='number' step='any' name='testSensorHumidity' value='40' required></label>"
            "<label>Battery (%)<input type='number' step='any' name='testBattery' min='0' max='100' value='85' required></label>"
            "<label>Wi-Fi RSSI (dBm; -100 to 0)<input type='number' step='any' name='testRssi' min='-100' max='0' value='-58' required></label>"
            "<label><input type='checkbox' name='testCharging' value='1'> Show charging indicator</label>"
            "<button type='button' onclick='previewWeather()'>Preview on device</button>"
            "<button type='button' onclick='stopWeatherPreview()'>Stop preview</button>"
            "<p id='testStatus' role='status'></p></div></details></div>");
page += F("<button type='submit' class='main'>Save settings and restart</button></form>");
page += F("<script>function syncAdaptive(){const on=document.getElementById('adaptiveUpdates').checked;const f=document.getElementById('refreshMinutes');f.readOnly=on;document.getElementById('slowPoll').readOnly=!on;document.getElementById('fastPoll').readOnly=!on;}syncAdaptive();function loadNetworks(keep){fetch('/list-networks').then(r=>r.json()).then(data=>{const sel=document.getElementById('networkSelect');const previous=keep?sel.value:'';sel.innerHTML='';data.forEach(n=>{const o=document.createElement('option');o.value=n.index;o.textContent=n.ssid+(n.location?' ('+n.location+')':'');sel.appendChild(o);});if(previous&&[...sel.options].some(o=>o.value===previous))sel.value=previous;document.getElementById('networkCount').textContent=data.length+' of 50 slots used.';document.getElementById('existingNetworks').style.display=data.length>0?'block':'none';document.getElementById('networkEdit').style.display=data.length>0?'block':'none';if(data.length>0)loadNetwork();});}function loadNetwork(){const sel=document.getElementById('networkSelect');if(!sel.value)return;fetch('/network-detail?index='+sel.value).then(r=>r.text()).then(t=>{const parts=t.split('|');document.getElementById('editSsid').value=parts[0]||'';document.getElementById('editPassword').value='';document.getElementById('editLocation').value=parts[1]||'';});}function saveNetwork(){const sel=document.getElementById('networkSelect');const data=new URLSearchParams({index:sel.value,ssid:document.getElementById('editSsid').value,password:document.getElementById('editPassword').value,location:document.getElementById('editLocation').value});fetch('/save-network',{method:'POST',body:data}).then(r=>r.text()).then(t=>{document.getElementById('editStatus').textContent=t;loadNetworks(true);});}function deleteNetwork(){if(!confirm('Delete this network slot?'))return;const sel=document.getElementById('networkSelect');fetch('/delete-network',{method:'POST',body:new URLSearchParams({index:sel.value})}).then(r=>r.text()).then(t=>{document.getElementById('editStatus').textContent=t;loadNetworks(false);});}function addNetwork(){const data=new URLSearchParams({ssid:document.getElementById('addSsid').value,password:document.getElementById('addPassword').value,location:document.getElementById('addLocation').value});fetch('/add-network',{method:'POST',body:data}).then(r=>r.text().then(t=>{document.getElementById('networkStatus').textContent=t;if(r.ok){['addSsid','addPassword','addLocation'].forEach(i=>document.getElementById(i).value='');loadNetworks(false);}}));}function importWifiQr(){const raw=document.getElementById('wifiQr').value.trim();const status=document.getElementById('qrStatus');if(!raw.startsWith('WIFI:')){status.textContent='Paste the decoded WIFI: QR text.';return;}const fields={};const re=/(?:^|;)([A-Z]):((?:\\\\.|[^;])*)/g;let m;while((m=re.exec(raw.slice(5))))fields[m[1]]=m[2].replace(/\\\\([\\;,:\"'])/g,'$1');if(!fields.S){status.textContent='QR does not contain a network name.';return;}document.getElementById('addSsid').value=fields.S;document.getElementById('addPassword').value=fields.P||'';status.textContent='Filled in the Add form. Press Add Wi-Fi network.';}function previewWeather(){const data=new URLSearchParams();document.querySelectorAll('#testControls [name]').forEach(e=>{if(e.type!=='checkbox'||e.checked)data.set(e.name,e.value);});fetch('/test',{method:'POST',body:data}).then(async r=>{document.getElementById('testStatus').textContent=await r.text();}).catch(()=>{document.getElementById('testStatus').textContent='Preview request failed.';});}function stopWeatherPreview(){fetch('/test/stop',{method:'POST'}).then(async r=>{document.getElementById('testStatus').textContent=await r.text();}).catch(()=>{document.getElementById('testStatus').textContent='Preview request failed.';});}loadNetworks(false);</script></body></html>");
  return page;
}

void startSetupPortal() {
  char password[9];
  snprintf(password, sizeof(password), "%08lx",
           static_cast<unsigned long>(esp_random()));
  setupPassword = password;
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(kSetupSsid, setupPassword.c_str())) {
    drawMessage("Setup failed", "Could not start Wi-Fi hotspot.");
    Serial.println("Could not start Wi-Fi setup hotspot");
    return;
  }

  const IPAddress address = WiFi.softAPIP();
  const bool darkModeBeforePortal = darkMode;
  darkMode = false;
  dnsServer.start(53, "*", address);
  webServer.on("/", HTTP_GET, [] {
    webServer.send(200, "text/html; charset=utf-8", setupPage());
  });
  webServer.on("/add-network", HTTP_POST, [] {
    String ssid = webServer.arg("ssid");
    String password = webServer.arg("password");
    String location = webServer.arg("location");
    ssid.trim();
    password.trim();
    location.trim();
    size_t slot = kMaximumNetworks;
    for (size_t i = 0; i < kMaximumNetworks; ++i) {
      if (savedSsids[i].isEmpty()) {
        slot = i;
        break;
      }
    }
    if (slot == kMaximumNetworks || ssid.isEmpty() || ssid.length() > 32 ||
        password.length() > 63 ||
        (!password.isEmpty() && password.length() < 8) ||
        location.length() > 100) {
      webServer.send(400, "text/plain; charset=utf-8",
                     slot == kMaximumNetworks
                         ? "All 50 network slots are in use."
                         : "Enter a valid SSID, password, and location.");
      return;
    }
    for (size_t i = 0; i < kMaximumNetworks; ++i) {
      if (savedSsids[i] == ssid) {
        webServer.send(409, "text/plain; charset=utf-8",
                       "That SSID is already saved.");
        return;
      }
    }
    savedSsids[slot] = ssid;
    savedPasswords[slot] = password;
    savedNetworkLocations[slot] = location;
    const String suffix = String(slot + 1);
    preferences.putString((String("ssid") + suffix).c_str(), ssid);
    preferences.putString((String("password") + suffix).c_str(), password);
    preferences.putString((String("loc") + suffix).c_str(), location);
    webServer.send(200, "text/plain; charset=utf-8", "Network added.");
  });
  webServer.on("/list-networks", HTTP_GET, [] {
    JsonDocument list;
    JsonArray entries = list.to<JsonArray>();
    for (size_t i = 0; i < kMaximumNetworks; ++i) {
      if (savedSsids[i].isEmpty()) continue;
      JsonObject entry = entries.add<JsonObject>();
      entry["index"] = i;
      entry["ssid"] = savedSsids[i];
      entry["location"] = savedNetworkLocations[i];
    }
    String json;
    serializeJson(list, json);
    webServer.send(200, "application/json", json);
  });
  webServer.on("/network-detail", HTTP_GET, [] {
    const int idx = webServer.arg("index").toInt();
    if (idx < 0 || idx >= static_cast<int>(kMaximumNetworks) ||
        savedSsids[idx].isEmpty()) {
      webServer.send(400, "text/plain; charset=utf-8", "Invalid network index.");
      return;
    }
    webServer.send(200, "text/plain; charset=utf-8",
                   savedSsids[idx] + "|" + savedNetworkLocations[idx]);
  });
  webServer.on("/save-network", HTTP_POST, [] {
    const int idx = webServer.arg("index").toInt();
    if (idx < 0 || idx >= static_cast<int>(kMaximumNetworks) ||
        savedSsids[idx].isEmpty()) {
      webServer.send(400, "text/plain; charset=utf-8", "Invalid network index.");
      return;
    }
    String ssid = webServer.arg("ssid");
    String password = webServer.arg("password");
    String location = webServer.arg("location");
    ssid.trim();
    password.trim();
    location.trim();
    if (password.isEmpty()) password = savedPasswords[idx];
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63 ||
        (!password.isEmpty() && password.length() < 8) ||
        location.length() > 100) {
      webServer.send(400, "text/plain; charset=utf-8",
                     "Enter a valid SSID, password, and location.");
      return;
    }
    for (size_t i = 0; i < kMaximumNetworks; ++i) {
      if (i != static_cast<size_t>(idx) && savedSsids[i] == ssid) {
        webServer.send(409, "text/plain; charset=utf-8",
                       "That SSID is already saved.");
        return;
      }
    }
    savedSsids[idx] = ssid;
    savedPasswords[idx] = password;
    savedNetworkLocations[idx] = location;
    const String suffix = String(idx + 1);
    preferences.putString((String("ssid") + suffix).c_str(), ssid);
    preferences.putString((String("password") + suffix).c_str(), password);
    preferences.putString((String("loc") + suffix).c_str(), location);
    webServer.send(200, "text/plain; charset=utf-8", "Network saved.");
  });
  webServer.on("/delete-network", HTTP_POST, [] {
    const int idx = webServer.arg("index").toInt();
    if (idx < 0 || idx >= static_cast<int>(kMaximumNetworks) ||
        savedSsids[idx].isEmpty()) {
      webServer.send(400, "text/plain; charset=utf-8", "Invalid network index.");
      return;
    }
    const String suffix = String(idx + 1);
    savedSsids[idx] = "";
    savedPasswords[idx] = "";
    savedNetworkLocations[idx] = "";
    preferences.remove((String("ssid") + suffix).c_str());
    preferences.remove((String("password") + suffix).c_str());
    preferences.remove((String("loc") + suffix).c_str());
    webServer.send(200, "text/plain; charset=utf-8", "Network deleted.");
  });
  webServer.on("/test", HTTP_POST, [] {
    if (!applyWeatherTest()) {
      webServer.send(400, "text/plain; charset=utf-8",
                     lastWeatherError);
      return;
    }
    webServer.send(200, "text/plain; charset=utf-8",
                   "Preview shown on the device; test values are temporary.");
  });
  webServer.on("/test/stop", HTTP_POST, [] {
    stopWeatherTest();
    webServer.send(200, "text/plain; charset=utf-8",
                   "Preview stopped; live forecast restored.");
  });
  webServer.on("/save", HTTP_POST, [] {
    bool hasNetwork = false;
    for (const String& ssid : savedSsids) hasNetwork = hasNetwork || !ssid.isEmpty();
    String locationQuery = webServer.arg("location");
    if (webServer.arg("clearLocation") == "1") locationQuery = "";
    String state = webServer.hasArg("state")
        ? webServer.arg("state") : savedState;
    String timezone = webServer.arg("timezone");
    const String submittedWeatherApi = webServer.arg("weatherApi");
    String nightStart = webServer.arg("nightStart");
    String nightEnd = webServer.arg("nightEnd");
    uint16_t submittedNightStartMinute = 0;
    uint16_t submittedNightEndMinute = 0;
    const bool validNightSchedule =
        parseAmPmMinuteOfDay(nightStart, submittedNightStartMinute) &&
        parseAmPmMinuteOfDay(nightEnd, submittedNightEndMinute);
    WeatherApiProvider submittedWeatherApiProvider;
    const bool validWeatherApi =
        parseWeatherApiProvider(submittedWeatherApi,
                                submittedWeatherApiProvider);
    String submittedCalendarId = webServer.arg("calendarId");
    const String temperatureUnits = webServer.arg("temperatureUnits");
    String submittedCalendarApiKey = webServer.arg("calendarApiKey");
    if (submittedCalendarApiKey.isEmpty()) {
      submittedCalendarApiKey = calendarApiKey;
    }
    uint16_t submittedRefreshMinutes = 0;
    uint16_t submittedSlowPoll = 0;
    uint16_t submittedFastPoll = 0;
    const bool validRefreshInterval =
        parseRefreshIntervalMinutes(webServer.arg("refreshMinutes"),
                                    submittedRefreshMinutes) &&
        parseRefreshIntervalMinutes(webServer.arg("slowPoll"),
                                    submittedSlowPoll) &&
        parseRefreshIntervalMinutes(webServer.arg("fastPoll"),
                                    submittedFastPoll);
    String submittedRepo = updateRepo;
    String submittedBranch = updateBranch;
    String repoUrl = webServer.arg("repoUrl");
    repoUrl.trim();
    const bool validRepo = repoUrl.isEmpty() ||
        parseRepoLocation(repoUrl, submittedRepo, submittedBranch);
    if (timezone.isEmpty()) timezone = "auto";
    locationQuery.trim();
    state.trim();
    const bool validFields = locationQuery.length() <= 100 &&
        state.length() <= 40 &&
        validClockOffsetSelection(timezone) && validRefreshInterval &&
        validNightSchedule && validWeatherApi && validRepo &&
        (temperatureUnits == "C" || temperatureUnits == "F") &&
        submittedCalendarId.length() <= 200 &&
        submittedCalendarApiKey.length() <= 200;
    if (!hasNetwork || !validFields) {
      webServer.send(400, "text/plain; charset=utf-8",
                     !hasNetwork ? "Add at least one Wi-Fi network first."
                                 : "Check the location, clock offset, poll "
                                   "rates, night schedule, and repository "
                                   "link.");
      return;
    }

    preferences.putString("location_query", locationQuery);
    savedLocationQuery = locationQuery;
    savedZipCode = "";
    preferences.remove("zip");
    if (webServer.hasArg("state")) preferences.putString("state", state);
    preferences.putString("timezone", timezone);
    weatherApiProvider = submittedWeatherApiProvider;
    preferences.putString("weather_api",
                          weatherApiPreferenceValue(weatherApiProvider));
    preferences.putUShort("refresh_minutes", submittedRefreshMinutes);
    preferences.putUShort("poll_slow", submittedSlowPoll);
    preferences.putUShort("poll_fast", submittedFastPoll);
    preferences.putBool("night_shutdown",
                        webServer.arg("nightShutdown") == "1");
    preferences.putUShort("night_start", submittedNightStartMinute);
    preferences.putUShort("night_end", submittedNightEndMinute);
    preferences.putBool("adapt_on", webServer.arg("adaptiveUpdates") == "1");
    preferences.putBool("geocode_pending", !locationQuery.isEmpty());
    if (locationQuery.isEmpty()) {
      preferences.remove("latitude");
      preferences.remove("longitude");
      preferences.remove("location");
    }
    useCelsius = temperatureUnits == "C";
    preferences.putBool("celsius", useCelsius);
    preferences.putBool("battery_saver",
                        webServer.arg("batterySaver") == "1");
    if (webServer.arg("nightlyUpdates") == "1" && !nightlyUpdates) {
      preferences.putBool("test_mode", false);
    }
    preferences.putBool("nightly_updates",
                        webServer.arg("nightlyUpdates") == "1");
    preferences.putBool("auto_update", webServer.arg("autoUpdates") == "1");
    preferences.putString("repo", submittedRepo);
    preferences.putString("repo_branch", submittedBranch);
    preferences.putBool("simple_mode",
                        webServer.arg("simpleMode") == "1");
    preferences.putString("calendar_id", submittedCalendarId);
    preferences.putString("calendar_key", submittedCalendarApiKey);
    saveDataUsage();
    webServer.send(200, "text/html; charset=utf-8",
                   "<!doctype html><meta name='viewport' "
                   "content='width=device-width'><h1>Saved</h1>"
                   "<p>YAWOS is restarting. Reconnect your phone "
                   "to your usual Wi-Fi.</p>");
    delay(1200);
    ESP.restart();
  });
  webServer.onNotFound([] {
    webServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    webServer.send(302, "text/plain", "");
  });
  webServer.begin();

  auto& display = M5.Display;
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(3);
  display.drawString("YAWOS setup", display.width() / 2, 28);
  display.setTextSize(2);
  display.drawString("Join Wi-Fi, then scan the QR",
                     display.width() / 2, 78);
  const String wifiQr = String("WIFI:T:WPA;S:") + kSetupSsid + ";P:" + setupPassword + ";;";
  display.qrcode(wifiQr, (display.width() - 330) / 2, 132, 330, 4);
  display.setTextSize(2);
  display.setTextDatum(textdatum_t::top_left);
  display.setCursor(40, 500);
  display.printf("Hotspot: %s\n", kSetupSsid);
  display.printf("Password: %s\n", setupPassword.c_str());
  display.printf("Setup page: %s\n", kSetupDomain);
  display.printf("Fallback: http://%s/\n", address.toString().c_str());
  display.println();
  display.println("C: exit setup");

  Serial.printf("Wi-Fi setup page: http://%s/\n", address.toString().c_str());
  bool keepPortalRunning = true;
  while (keepPortalRunning) {
    M5.update();
    const bool suppressBootClick = consumeBootCClick();
    dnsServer.processNextRequest();
    webServer.handleClient();
    if (!suppressBootClick && M5.BtnC.wasClicked()) keepPortalRunning = false;
    delay(2);
  }
  webServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  darkMode = darkModeBeforePortal;
}

String editOnDeviceNumber(const String& label, const String& currentValue,
                         size_t maxDigits, bool twelveHour = false,
                         bool initialPm = false, bool* selectedPm = nullptr,
                         bool* clearRequested = nullptr) {
  String entered;
  uint8_t digit = 0;
  bool pm = initialPm;
  bool firstDraw = true;
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  display.setEpdMode(lgfx::epd_mode::epd_text);
  auto drawEntered = [&]() {
    display.fillRect(100, 186, display.width() - 200, 90, TFT_WHITE);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(6);
    display.drawString(entered, display.width() / 2, 190);
  };
  while (true) {
    M5.update();
    if (firstDraw) {
      display.setEpdMode(lgfx::epd_mode::epd_quality);
      display.startWrite();
      display.clear(TFT_WHITE);
      display.setTextColor(TFT_BLACK);
      display.setTextDatum(textdatum_t::top_center);
      display.setTextSize(4);
      display.drawString(label, display.width() / 2, 24);
      display.setTextSize(3);
      display.drawString("Current: " + currentValue, display.width() / 2, 96);
      drawEntered();
      if (twelveHour) {
        display.drawRect(display.width() / 2 - 100, 300, 200, 58, TFT_BLACK);
        display.setTextSize(3);
        display.drawString(pm ? "PM" : "AM", display.width() / 2, 312);
      }

      static constexpr const char* keys[4][3] = {
          {"1", "2", "3"}, {"4", "5", "6"},
          {"7", "8", "9"}, {"DEL", "0", "SAVE"}};
      constexpr int32_t keyWidth = 104;
      constexpr int32_t keyHeight = 76;
      constexpr int32_t keyGap = 8;
      const int32_t keyX = (display.width() -
                            (keyWidth * 3 + keyGap * 2)) / 2;
      constexpr int32_t keyY = 390;
      for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 3; ++column) {
          const int32_t x = keyX + column * (keyWidth + keyGap);
          const int32_t y = keyY + row * (keyHeight + keyGap);
          display.drawRect(x, y, keyWidth, keyHeight, TFT_BLACK);
          display.setTextSize(3);
          display.drawString(keys[row][column],
                             x + keyWidth / 2, y + keyHeight / 2 - 14);
        }
      }
      if (clearRequested != nullptr) {
        const int32_t clearButtonY = 752;
        display.drawRect(52, clearButtonY, display.width() - 104, 64,
                         TFT_BLACK);
        display.setTextSize(2);
        display.drawString("CLEAR LOCATION", display.width() / 2,
                           clearButtonY + 20);
      }
      display.setTextSize(2);
      display.drawString(twelveHour
                             ? "A: next digit  B: type  C: back"
                             : "A: next digit  B: type  C: back",
                         display.width() / 2, 850);
      display.setTextDatum(textdatum_t::top_left);
      display.endWrite();
      display.waitDisplay();
      display.setEpdMode(lgfx::epd_mode::epd_text);
      firstDraw = false;
    }

    if (M5.BtnA.wasClicked()) {
      digit = (digit + 1) % 10;
    }
    if (M5.BtnB.wasClicked() && entered.length() < maxDigits) {
      entered += String(digit);
      drawEntered();
      display.waitDisplay();
    }
    if (M5.BtnC.wasClicked()) {
      display.setEpdMode(originalMode);
      return "";
    }
    if (M5.Touch.getCount() > 0) {
      const auto& touch = M5.Touch.getDetail();
      if (touchTapped(touch)) {
        if (clearRequested != nullptr && touch.y >= 752 && touch.y < 816 &&
            touch.x >= 52 && touch.x < display.width() - 52) {
          *clearRequested = true;
          display.setEpdMode(originalMode);
          return "";
        }
        if (twelveHour && touch.y >= 300 && touch.y < 358 &&
            touch.x >= display.width() / 2 - 100 &&
            touch.x <= display.width() / 2 + 100) {
          pm = !pm;
          display.fillRect(display.width() / 2 - 96, 304, 192, 50,
                           TFT_WHITE);
          display.setTextColor(TFT_BLACK);
          display.setTextDatum(textdatum_t::top_center);
          display.setTextSize(3);
          display.drawString(pm ? "PM" : "AM", display.width() / 2, 312);
          display.waitDisplay();
          continue;
        }
        constexpr int32_t keyWidth = 104;
        constexpr int32_t keyHeight = 76;
        constexpr int32_t keyGap = 8;
        const int32_t keyX = (M5.Display.width() -
                              (keyWidth * 3 + keyGap * 2)) / 2;
        constexpr int32_t keyY = 390;
        const int32_t relativeX = touch.x - keyX;
        const int32_t relativeY = touch.y - keyY;
        if (relativeX >= 0 && relativeY >= 0) {
          const int column = relativeX / (keyWidth + keyGap);
          const int row = relativeY / (keyHeight + keyGap);
          if (row < 4 && column < 3 &&
              relativeX % (keyWidth + keyGap) < keyWidth &&
              relativeY % (keyHeight + keyGap) < keyHeight) {
            const int keyIndex = row * 3 + column;
            if (keyIndex == 11) {
              if (selectedPm != nullptr) *selectedPm = pm;
              display.setEpdMode(originalMode);
              return entered;
            }
            if (keyIndex == 9) {
              if (!entered.isEmpty()) {
                entered.remove(entered.length() - 1);
                drawEntered();
                display.waitDisplay();
              }
            } else if (keyIndex == 10 && entered.length() < maxDigits) {
              entered += "0";
              drawEntered();
              display.waitDisplay();
            } else if (keyIndex < 9 && entered.length() < maxDigits) {
              entered += String(keyIndex + 1);
              drawEntered();
              display.waitDisplay();
            }
          }
        }
      }
    }
    delay(20);
  }
}

bool connectToSavedWifi();
bool hasSavedWifi();

bool ensureUpdateNetwork() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!hasSavedWifi() || !connectToSavedWifi()) return false;
  }
  const uint32_t started = millis();
  while (!clockReady && millis() - started < 20000) {
    M5.update();
    updateClockSync();
    delay(50);
  }
  return firmwareNetworkReady();
}

void drawUpdateMenuButton(int32_t y, int32_t height, const String& label,
                          bool filled, bool highlighted,
                          uint8_t textSize = 3) {
  auto& display = M5.Display;
  const int32_t x = 24;
  const int32_t width = display.width() - 48;
  if (filled) {
    display.fillRect(x, y, width, height, TFT_BLACK);
    if (highlighted) {
      display.drawRect(x + 5, y + 5, width - 10, height - 10, TFT_WHITE);
    }
    display.setTextColor(TFT_WHITE);
  } else {
    display.fillRect(x, y, width, height, TFT_WHITE);
    display.drawRect(x, y, width, height,
                     highlighted ? TFT_RED : TFT_LIGHTGREY);
    if (highlighted) {
      display.drawRect(x + 1, y + 1, width - 2, height - 2, TFT_RED);
    }
    display.setTextColor(highlighted ? TFT_RED : TFT_BLACK);
  }
  display.setTextDatum(textdatum_t::middle_center);
  display.setTextSize(textSize);
  display.drawString(label, x + width / 2, y + height / 2);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_left);
}

bool fetchFirmwareInfo(const String& folder, String& text) {
  const String url = String("https://raw.githubusercontent.com/") +
                     updateRepo + "/" + updateBranch + "/updates/" + folder + "/info.txt";
  NetworkClientSecure client;
  HTTPClient http;
  if (!configureSecureRequest(client, http, url)) return false;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int status = http.GET();
  if (status != HTTP_CODE_OK || http.getSize() > 4096) {
    http.end();
    return false;
  }
  text = http.getString();
  noteDataUsage(text.length() + 6500);
  http.end();
  text.replace("\r", "");
  text.trim();
  if (text.length() > 600) text = text.substring(0, 600) + "...";
  return true;
}

size_t wrapInfoText(const String& text, size_t columns, String* lines,
                    size_t maxLines) {
  size_t count = 0;
  String current;
  String word;
  auto flush = [&]() {
    if (count < maxLines) lines[count++] = current;
    current = "";
  };
  auto pushWord = [&]() {
    if (word.isEmpty()) return;
    if (current.isEmpty()) {
      current = word;
    } else if (current.length() + 1 + word.length() <= columns) {
      current += " " + word;
    } else {
      flush();
      current = word;
    }
    word = "";
  };
  for (size_t i = 0; i < text.length() && count < maxLines; ++i) {
    const char ch = text[i];
    if (ch == '\n') {
      pushWord();
      flush();
    } else if (ch == ' ' || ch == '\t') {
      pushWord();
    } else {
      word += ch;
      if (word.length() >= columns) {
        if (!current.isEmpty()) flush();
        current = word;
        flush();
        word = "";
      }
    }
  }
  pushWord();
  if (!current.isEmpty() && count < maxLines) flush();
  return count;
}

bool confirmInstallerPrompt() {
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  size_t selected = 0;
  const int32_t buttonY[2] = {600, 700};
  const char* labels[2] = {"Continue", "Cancel"};
  auto drawButtons = [&]() {
    display.setEpdMode(lgfx::epd_mode::epd_text);
    display.startWrite();
    for (size_t i = 0; i < 2; ++i) {
      drawUpdateMenuButton(buttonY[i], 80, labels[i], false, selected == i);
    }
    display.endWrite();
  };
  String lines[6];
  const size_t lineCount = wrapInfoText(
      "This is the YAWOS Installer. Downgrading to this version will "
      "reinstall the latest version on boot.",
      34, lines, 6);
  display.setEpdMode(lgfx::epd_mode::epd_quality);
  display.startWrite();
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(4);
  display.drawString("Installer", display.width() / 2, 60);
  display.setTextSize(2);
  for (size_t i = 0; i < lineCount; ++i) {
    display.drawString(lines[i], display.width() / 2,
                       200 + static_cast<int32_t>(i) * 34);
  }
  for (size_t i = 0; i < 2; ++i) {
    drawUpdateMenuButton(buttonY[i], 80, labels[i], false, selected == i);
  }
  display.setTextDatum(textdatum_t::top_left);
  display.endWrite();
  display.waitDisplay();
  display.setEpdMode(lgfx::epd_mode::epd_text);
  while (true) {
    M5.update();
    const size_t previous = selected;
    bool choose = false;
    if (M5.BtnA.wasClicked() || M5.BtnC.wasClicked()) selected = 1 - selected;
    if (M5.BtnB.wasClicked()) choose = true;
    if (M5.Touch.getCount() > 0) {
      const auto& touch = M5.Touch.getDetail();
      if (touchTapped(touch)) {
        for (size_t i = 0; i < 2; ++i) {
          if (touch.y >= buttonY[i] && touch.y < buttonY[i] + 80) {
            selected = i;
            choose = true;
          }
        }
      }
    }
    if (selected != previous && !choose) drawButtons();
    if (choose) {
      display.setEpdMode(originalMode);
      return selected == 0;
    }
    delay(20);
  }
}

void showUpdateDetail(const String& folder) {
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  const FirmwareVersion version = parseFirmwareVersion(folder);
  const String installed = preferences.getString("fw_version", "0.0.0");
  const bool canInstall = !version.unusable && !version.denyInstall;
  const bool older = !newestEligibleFirmware.isEmpty() &&
                     compareFirmwareVersions(folder, newestEligibleFirmware) < 0;
  String infoLines[12];
  size_t infoCount = 0;
  if (version.hasInfo) {
    drawMessage("Version " + firmwareDisplayVersion(folder),
                "Loading info...");
    String text;
    if (!ensureUpdateNetwork() || !fetchFirmwareInfo(folder, text)) {
      text = "Could not load the version info.";
    } else if (text.isEmpty()) {
      text = "No info was provided for this version.";
    }
    infoCount = wrapInfoText(text, 40, infoLines, 12);
  }
  const size_t buttonCount = canInstall ? 2 : 1;
  constexpr int32_t buttonHeight = 76;
  constexpr int32_t infoBottom = 626;
  const int32_t installY = 640;
  const int32_t backY = canInstall ? 730 : 640;
  const bool showIcon = version.unusable || version.nightly;
  size_t selected = 0;
  auto draw = [&](bool full) {
    display.setEpdMode(full ? lgfx::epd_mode::epd_quality
                            : lgfx::epd_mode::epd_text);
    display.startWrite();
    display.clear(TFT_WHITE);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    const int32_t centerX = display.width() / 2;
    display.setTextSize(4);
    display.drawString(isInstallerFolder(folder)
                           ? firmwareDisplayVersion(folder)
                           : "Version " + firmwareDisplayVersion(folder),
                       centerX, 60);
    display.setTextSize(2);
    display.drawString(folder == installed
                           ? String("Currently installed")
                           : String("Installed: ver. ") + installed,
                       centerX, 130);
    int32_t y = 320;
    if (version.unusable) {
      display.drawString("This update has been marked as", centerX, y);
      display.drawString("unstable and cannot be used.", centerX, y + 30);
      y += 60;
    } else if (version.nightly) {
      display.drawString("This is a nightly update and", centerX, y);
      display.drawString("could be unstable.", centerX, y + 30);
      y += 60;
    }
    if (version.denyInstall && !version.unusable) {
      display.drawString("This version cannot be installed.", centerX, y);
      y += 30;
    }
    if (older && canInstall && !isInstallerFolder(folder)) {
      display.drawString("This is older than the latest version.", centerX, y);
      display.drawString("Installing it turns on Test mode,", centerX, y + 30);
      display.drawString("which pauses automatic updates.", centerX, y + 60);
      y += 90;
    }
    if (!showIcon && !version.hasInfo && !older && canInstall) {
      display.drawString("Ready to install.", centerX, y);
    }
    if (infoCount > 0) {
      y += 10;
      const size_t maxLines =
          y < infoBottom ? static_cast<size_t>((infoBottom - y) / 28) : 0;
      for (size_t i = 0; i < infoCount && i < maxLines; ++i) {
        String line = infoLines[i];
        if (i + 1 == maxLines && i + 1 < infoCount) line += "...";
        display.drawString(line, centerX, y + static_cast<int32_t>(i) * 28);
      }
    }
    if (canInstall) {
      drawUpdateMenuButton(installY, buttonHeight,
                           folder == installed ? "Reinstall" : "Install",
                           true, selected == 0);
    }
    drawUpdateMenuButton(backY, buttonHeight, "Back", false,
                         selected == buttonCount - 1);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(2);
    display.drawString("A/C: move  B: select", centerX, display.height() - 44);
    display.setTextDatum(textdatum_t::top_left);
    display.endWrite();
    if (showIcon) {
      drawWeatherIconFitted(version.nightly ? "/square.png" : alertIconPath(),
                            (display.width() - 96) / 2, 190, 96, 96);
    }
    display.waitDisplay();
    display.setEpdMode(lgfx::epd_mode::epd_text);
  };
  draw(true);
  while (true) {
    M5.update();
    bool choose = false;
    if ((M5.BtnA.wasClicked() || M5.BtnC.wasClicked()) && buttonCount > 1) {
      selected = (selected + 1) % buttonCount;
      draw(false);
    }
    if (M5.BtnB.wasClicked()) choose = true;
    if (M5.Touch.getCount() > 0) {
      const auto& touch = M5.Touch.getDetail();
      if (touchTapped(touch) && touch.x >= 24 &&
          touch.x < display.width() - 24) {
        if (canInstall && touch.y >= installY &&
            touch.y < installY + buttonHeight) {
          selected = 0;
          choose = true;
        } else if (touch.y >= backY && touch.y < backY + buttonHeight) {
          selected = buttonCount - 1;
          choose = true;
        }
      }
    }
    if (choose) {
      if (!(canInstall && selected == 0)) {
        display.setEpdMode(originalMode);
        return;
      }
      if (isInstallerFolder(folder) && !confirmInstallerPrompt()) {
        draw(true);
        continue;
      }
      if (!updateStorageReady()) {
        drawMessage("No SD to safely update",
                    "Insert an SD card, or set Direct flash to AUTO or ON.");
        delay(3500);
      } else if (!ensureUpdateNetwork()) {
        drawMessage("Update failed", "No Wi-Fi connection or clock.");
        delay(3000);
      } else if (!installFirmware(folder, folder != newestEligibleFirmware)) {
        drawMessage("Update failed", "Could not install this version.");
        delay(3000);
      }
      draw(true);
    }
    delay(20);
  }
}

void showUpdatesManager() {
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  constexpr size_t visibleRows = 5;
  constexpr int32_t checkY = 24;
  constexpr int32_t checkHeight = 76;
  constexpr int32_t listY = 186;
  constexpr int32_t rowHeight = 78;
  constexpr int32_t toggleHeight = 60;
  constexpr int32_t nightlyY = 584;
  constexpr int32_t autoY = 652;
  constexpr int32_t flashY = 720;
  constexpr int32_t backHeight = 70;
  constexpr int32_t backY = 788;
  size_t selected = 0;
  size_t listOffset = 0;
  uint8_t upHoldHandled = false;
  uint8_t downHoldHandled = false;
  uint32_t lastUpRepeatMs = 0;
  uint32_t lastDownRepeatMs = 0;

  size_t listSelection = 1;
  auto stepList = [&](bool down) {
    if (firmwareFolderCount == 0) return false;
    const size_t index = (selected >= 1 && selected <= firmwareFolderCount)
        ? selected - 1 : (down ? firmwareFolderCount - 1 : 0);
    selected = (down ? (index + 1) % firmwareFolderCount
                     : (index + firmwareFolderCount - 1) % firmwareFolderCount) + 1;
    return true;
  };
  auto syncWindow = [&]() {
    if (selected >= 1 && selected <= firmwareFolderCount) {
      const size_t index = selected - 1;
      if (index < listOffset) listOffset = index;
      if (index >= listOffset + visibleRows) listOffset = index - visibleRows + 1;
    }
  };
  auto drawRowBody = [&](size_t row) -> uint8_t {
    const int32_t rowWidth = display.width() - 64;
    const int32_t y = listY + static_cast<int32_t>(row) * rowHeight;
    display.fillRect(24, y, rowWidth, rowHeight - 8, TFT_WHITE);
    const size_t index = listOffset + row;
    if (index >= firmwareFolderCount) return 0;
    const String installed = preferences.getString("fw_version", "0.0.0");
    const bool highlighted = selected == index + 1;
    const FirmwareVersion version = parseFirmwareVersion(firmwareFolders[index]);
    display.drawRect(24, y, rowWidth, rowHeight - 8,
                     highlighted ? TFT_RED : TFT_LIGHTGREY);
    if (highlighted) {
      display.drawRect(25, y + 1, rowWidth - 2, rowHeight - 10, TFT_RED);
    }
    const String label = firmwareDisplayVersion(firmwareFolders[index]);
    display.setTextColor(highlighted ? TFT_RED : TFT_BLACK);
    display.setTextDatum(textdatum_t::top_left);
    display.setTextSize(4);
    display.drawString(label, 44, y + 19);
    int32_t tagX = 44 + display.textWidth(label.c_str()) + 14;
    display.setTextSize(2);
    if (firmwareFolders[index] == installed) {
      display.drawString("installed", tagX, y + 27);
    }
    return static_cast<uint8_t>((version.unusable ? 1 : 0) |
                                (version.nightly ? 2 : 0));
  };
  auto drawRowIcon = [&](size_t row, uint8_t flags) {
    const int32_t rowWidth = display.width() - 64;
    const int32_t y = listY + static_cast<int32_t>(row) * rowHeight;
    const int32_t alertX = 24 + rowWidth - 56;
    if (flags & 1) {
      drawWeatherIconFitted(alertIconPath(), alertX, y + 15, 40, 40);
    }
    if (flags & 2) {
      drawWeatherIconFitted("/square.png", (flags & 1) ? alertX - 48 : alertX,
                            y + 15, 40, 40);
    }
  };
  auto drawScrollbar = [&]() {
    const int32_t trackHeight = visibleRows * rowHeight - 8;
    display.fillRect(display.width() - 34, listY, 16, trackHeight, TFT_WHITE);
    if (firmwareFolderCount > visibleRows) {
      const int32_t thumbHeight = max<int32_t>(
          24, trackHeight * static_cast<int32_t>(visibleRows) /
                  static_cast<int32_t>(firmwareFolderCount));
      const int32_t thumbY = listY +
          (trackHeight - thumbHeight) * static_cast<int32_t>(listOffset) /
              static_cast<int32_t>(firmwareFolderCount - visibleRows);
      display.drawRect(display.width() - 30, listY, 8, trackHeight,
                       TFT_LIGHTGREY);
      display.fillRect(display.width() - 30, thumbY, 8, thumbHeight,
                       TFT_BLACK);
    }
  };
  auto updateList = [&](const bool* rows, bool scrollbar) {
    uint8_t icons[visibleRows] = {};
    display.setEpdMode(lgfx::epd_mode::epd_text);
    display.startWrite();
    for (size_t row = 0; row < visibleRows; ++row) {
      if (rows[row]) icons[row] = drawRowBody(row);
    }
    if (scrollbar) drawScrollbar();
    display.endWrite();
    for (size_t row = 0; row < visibleRows; ++row) {
      if (icons[row]) drawRowIcon(row, icons[row]);
    }
  };
  bool showEmptyHint = false;
  auto drawHeaderBody = [&]() {
    const String installed = preferences.getString("fw_version", "0.0.0");
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_left);
    display.setTextSize(2);
    display.drawString("Installed: ver. " + installed, 24, 112);
    if (preferences.getBool("test_mode", false)) {
      display.drawString("Test mode: automatic updates paused", 24, 140);
    } else if (!newestEligibleFirmware.isEmpty()) {
      display.drawString(
          "Latest: ver. " + firmwareDisplayVersion(newestEligibleFirmware), 24,
          140);
    }
  };
  auto updateHeader = [&]() {
    display.setEpdMode(lgfx::epd_mode::epd_text);
    display.startWrite();
    drawUpdateMenuButton(checkY, checkHeight, "Check for Updates", true,
                         selected == 0);
    display.fillRect(0, 104, display.width(), 76, TFT_WHITE);
    drawHeaderBody();
    display.endWrite();
    display.waitDisplay();
  };
  auto showStatus = [&](const String& title, const String& message) {
    String lines[4];
    const size_t lineCount = wrapInfoText(message, 40, lines, 4);
    display.setEpdMode(lgfx::epd_mode::epd_text);
    display.startWrite();
    display.fillRect(0, listY, display.width(), visibleRows * rowHeight - 8,
                     TFT_WHITE);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(3);
    display.drawString(title, display.width() / 2, listY + 100);
    display.setTextSize(2);
    for (size_t i = 0; i < lineCount; ++i) {
      display.drawString(lines[i], display.width() / 2,
                         listY + 160 + static_cast<int32_t>(i) * 30);
    }
    display.setTextDatum(textdatum_t::top_left);
    display.endWrite();
    display.waitDisplay();
  };
  auto refreshDynamic = [&]() {
    bool all[visibleRows];
    for (size_t row = 0; row < visibleRows; ++row) all[row] = true;
    updateHeader();
    updateList(all, true);
  };
  auto draw = [&](bool full) {
    uint8_t rowIcon[visibleRows] = {};
    display.setEpdMode(full ? lgfx::epd_mode::epd_quality
                            : lgfx::epd_mode::epd_text);
    display.startWrite();
    display.clear(TFT_WHITE);
    drawUpdateMenuButton(checkY, checkHeight, "Check for Updates", true,
                         selected == 0);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_left);
    drawHeaderBody();
    display.setTextSize(2);
    if (firmwareFolderCount == 0 && showEmptyHint) {
      display.drawString("No versions loaded yet.", 24, listY + 12);
      display.drawString("Select Check for Updates.", 24, listY + 42);
    }
    for (size_t row = 0; row < visibleRows; ++row) {
      rowIcon[row] = drawRowBody(row);
    }
    drawScrollbar();
    drawUpdateMenuButton(
        nightlyY, toggleHeight,
        String("Nightly updates: ") + (nightlyUpdates ? "ON" : "OFF"), false,
        selected == firmwareFolderCount + 1, 2);
    drawUpdateMenuButton(
        autoY, toggleHeight,
        String("Automatic updating: ") + (autoUpdates ? "ON" : "OFF"), false,
        selected == firmwareFolderCount + 2, 2);
    drawUpdateMenuButton(
        flashY, toggleHeight,
        String("Direct flash: ") +
            (flashMode == 0 ? "AUTO" : flashMode == 1 ? "ON" : "OFF"),
        false, selected == firmwareFolderCount + 3, 2);
    drawUpdateMenuButton(backY, backHeight, "Back to settings", false,
                         selected == firmwareFolderCount + 4);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(2);
    display.drawString("A/C: scroll  B: select  Touch: buttons",
                       display.width() / 2, display.height() - 44);
    display.setTextDatum(textdatum_t::top_left);
    display.endWrite();
    for (size_t row = 0; row < visibleRows; ++row) {
      if (!rowIcon[row]) continue;
      drawRowIcon(row, rowIcon[row]);
    }
    display.waitDisplay();
    display.setEpdMode(lgfx::epd_mode::epd_text);
  };
  auto checkNow = [&]() -> bool {
    const bool needsFull = WiFi.status() != WL_CONNECTED;
    showStatus("Connecting...", "");
    if (!ensureUpdateNetwork()) {
      if (needsFull) draw(true);
      showStatus("Update check failed", "No Wi-Fi connection or clock.");
      delay(3000);
      return false;
    }
    if (needsFull) draw(true);
    showStatus("Checking for updates...", "");
    String error;
    if (!fetchFirmwareList(error)) {
      showStatus("Update check failed", error);
      delay(3500);
      return false;
    }
    listOffset = 0;
    bool imagesUpdated = false;
    String candidate;
    const FirmwareInstallResult result = installNewestFirmware(candidate);
    if (result == FirmwareInstallResult::noStorage) {
      showStatus("No SD to safely update",
                 "Insert an SD card, or set Direct flash to AUTO or ON.");
      delay(3500);
      return false;
    }
    if (result == FirmwareInstallResult::failed) {
      drawMessage("Update failed", "Could not install the update.");
      delay(3000);
      return true;
    }
    imagesUpdated = syncInstalledImagePack();
    if (imagesUpdated) draw(true);
    showStatus("Up to date",
               "ver. " + preferences.getString("fw_version", "0.0.0") +
                   " is the newest version.");
    delay(2500);
    return imagesUpdated;
  };

  const bool openNeedsFull = WiFi.status() != WL_CONNECTED;
  draw(true);
  showStatus("Loading versions...", "");
  if (ensureUpdateNetwork()) {
    String error;
    fetchFirmwareList(error);
  }
  showEmptyHint = true;
  selected = firmwareFolderCount > 0 ? 1 : 0;
  if (openNeedsFull) {
    draw(true);
  } else {
    refreshDynamic();
  }
  while (true) {
    M5.update();
    bool choose = false;
    bool moved = false;
    const size_t previousSelected = selected;
    const size_t previousOffset = listOffset;
    const uint32_t nowMs = millis();
    if (M5.BtnA.wasClicked() && !upHoldHandled) {
      moved = stepList(false) || moved;
    }
    if (M5.BtnC.wasClicked() && !downHoldHandled) {
      moved = stepList(true) || moved;
    }
    if (M5.BtnA.isPressed() && M5.BtnA.pressedFor(350) &&
        (!upHoldHandled || nowMs - lastUpRepeatMs >= 220)) {
      upHoldHandled = true;
      lastUpRepeatMs = nowMs;
      moved = stepList(false) || moved;
    }
    if (M5.BtnC.isPressed() && M5.BtnC.pressedFor(350) &&
        (!downHoldHandled || nowMs - lastDownRepeatMs >= 220)) {
      downHoldHandled = true;
      lastDownRepeatMs = nowMs;
      moved = stepList(true) || moved;
    }
    if (M5.BtnA.wasReleased()) upHoldHandled = false;
    if (M5.BtnC.wasReleased()) downHoldHandled = false;
    if (M5.BtnB.wasClicked()) choose = true;
    if (M5.Touch.getCount() > 0) {
      const auto& touch = M5.Touch.getDetail();
      if (touchTapped(touch) && touch.x >= 24 &&
          touch.x < display.width() - 24) {
        if (touch.y >= checkY && touch.y < checkY + checkHeight) {
          selected = 0;
          choose = true;
        } else if (touch.y >= listY &&
                   touch.y < listY + static_cast<int32_t>(visibleRows) * rowHeight) {
          const size_t index =
              listOffset + static_cast<size_t>((touch.y - listY) / rowHeight);
          if (index < firmwareFolderCount) {
            selected = index + 1;
            choose = true;
          }
        } else if (touch.y >= nightlyY && touch.y < nightlyY + toggleHeight) {
          selected = firmwareFolderCount + 1;
          choose = true;
        } else if (touch.y >= autoY && touch.y < autoY + toggleHeight) {
          selected = firmwareFolderCount + 2;
          choose = true;
        } else if (touch.y >= flashY && touch.y < flashY + toggleHeight) {
          selected = firmwareFolderCount + 3;
          choose = true;
        } else if (touch.y >= backY && touch.y < backY + backHeight) {
          selected = firmwareFolderCount + 4;
          choose = true;
        }
      }
    }
    if (selected >= 1 && selected <= firmwareFolderCount) {
      listSelection = selected;
    }
    if (moved && !choose) {
      syncWindow();
      if (previousSelected >= 1 && previousSelected <= firmwareFolderCount &&
          selected >= 1 && selected <= firmwareFolderCount) {
        bool rows[visibleRows] = {};
        if (listOffset != previousOffset) {
          for (size_t row = 0; row < visibleRows; ++row) rows[row] = true;
        } else {
          rows[previousSelected - 1 - listOffset] = true;
          rows[selected - 1 - listOffset] = true;
        }
        updateList(rows, listOffset != previousOffset);
      } else {
        draw(false);
      }
    }
    if (choose) {
      if (selected == 0) {
        const bool needsFull = checkNow();
        selected = firmwareFolderCount > 0 ? 1 : 0;
        listSelection = 1;
        syncWindow();
        if (needsFull) {
          draw(true);
        } else {
          refreshDynamic();
        }
      } else if (selected <= firmwareFolderCount) {
        showUpdateDetail(firmwareFolders[selected - 1]);
        draw(true);
      } else if (selected == firmwareFolderCount + 1) {
        nightlyUpdates = !nightlyUpdates;
        preferences.putBool("nightly_updates", nightlyUpdates);
        if (nightlyUpdates) preferences.putBool("test_mode", false);
        applyFirmwareVisibility();
        listOffset = 0;
        listSelection = 1;
        selected = firmwareFolderCount > 0 ? 1 : 0;
        draw(false);
      } else if (selected == firmwareFolderCount + 2) {
        autoUpdates = !autoUpdates;
        preferences.putBool("auto_update", autoUpdates);
        selected = firmwareFolderCount > 0
            ? min(listSelection, firmwareFolderCount) : 0;
        draw(false);
      } else if (selected == firmwareFolderCount + 3) {
        flashMode = (flashMode + 1) % 3;
        preferences.putUChar("flash_mode", flashMode);
        selected = firmwareFolderCount > 0
            ? min(listSelection, firmwareFolderCount) : 0;
        draw(false);
      } else {
        break;
      }
    }
    delay(20);
  }
  display.setEpdMode(originalMode);
}

void showShutdownPeriodSettings() {
  enum Row : uint8_t { toggleRow, startRow, endRow, backRow, rowCount };
  constexpr int32_t rowStartY = 120;
  constexpr int32_t rowHeight = 80;
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  Row selected = toggleRow;
  auto rowText = [&](Row row) -> String {
    switch (row) {
      case toggleRow:
        return String("Night shutdown: ") + (nightShutdownEnabled ? "ON" : "OFF");
      case startRow:
        return "Starts: " + formatAmPmMinuteOfDay(nightShutdownStartMinute);
      case endRow:
        return "Ends: " + formatAmPmMinuteOfDay(nightShutdownEndMinute);
      default:
        return "Back to settings";
    }
  };
  auto drawRow = [&](Row row, bool highlighted) {
    const int32_t y = rowStartY + static_cast<int32_t>(row) * rowHeight;
    display.fillRect(22, y - 4, display.width() - 44, rowHeight - 12,
                     TFT_WHITE);
    display.drawRect(22, y - 4, display.width() - 44, rowHeight - 12,
                     highlighted ? TFT_RED : TFT_LIGHTGREY);
    display.setTextColor(highlighted ? TFT_RED : TFT_BLACK);
    display.setTextDatum(textdatum_t::top_left);
    display.setTextSize(3);
    display.drawString(String(highlighted ? "> " : "  ") + rowText(row), 36,
                       y + 14);
  };
  auto drawOne = [&](Row row, bool highlighted) {
    display.setEpdMode(lgfx::epd_mode::epd_text);
    display.startWrite();
    drawRow(row, highlighted);
    display.endWrite();
  };
  auto drawAll = [&]() {
    display.setEpdMode(lgfx::epd_mode::epd_quality);
    display.startWrite();
    display.clear(TFT_WHITE);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(3);
    display.drawString("Shutdown period", display.width() / 2, 40);
    for (uint8_t i = 0; i < rowCount; ++i) {
      drawRow(static_cast<Row>(i), i == selected);
    }
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(2);
    display.drawString("A: up  C: down  B: select  Tap: select",
                       display.width() / 2, display.height() - 44);
    display.setTextDatum(textdatum_t::top_left);
    display.endWrite();
    display.waitDisplay();
    display.setEpdMode(lgfx::epd_mode::epd_text);
  };
  auto editTime = [&](const char* label, uint16_t& minuteOfDay,
                      const char* key) {
    bool pm = minuteOfDay >= 12 * 60;
    const String edited = editOnDeviceNumber(
        label, formatAmPmMinuteOfDay(minuteOfDay), 4, true, pm, &pm);
    uint16_t parsed = 0;
    if (parseTwelveHourTime(edited, pm, parsed)) {
      minuteOfDay = parsed;
      preferences.putUShort(key, parsed);
    }
  };
  drawAll();
  uint8_t upHoldHandled = false;
  uint8_t downHoldHandled = false;
  while (true) {
    M5.update();
    bool choose = false;
    Row next = selected;
    if (M5.BtnA.wasClicked() && !upHoldHandled) {
      next = static_cast<Row>((selected + rowCount - 1) % rowCount);
    }
    if (M5.BtnC.wasClicked() && !downHoldHandled) {
      next = static_cast<Row>((selected + 1) % rowCount);
    }
    if (M5.BtnA.isPressed() && M5.BtnA.pressedFor(350) && !upHoldHandled) {
      upHoldHandled = true;
      next = static_cast<Row>((selected + rowCount - 1) % rowCount);
    }
    if (M5.BtnC.isPressed() && M5.BtnC.pressedFor(350) && !downHoldHandled) {
      downHoldHandled = true;
      next = static_cast<Row>((selected + 1) % rowCount);
    }
    if (M5.BtnA.wasReleased()) upHoldHandled = false;
    if (M5.BtnC.wasReleased()) downHoldHandled = false;
    if (M5.BtnB.wasClicked()) choose = true;
    if (M5.Touch.getCount() > 0) {
      const auto& touch = M5.Touch.getDetail();
      if (touchTapped(touch) && touch.y >= rowStartY - 4) {
        const int32_t row = (touch.y - rowStartY + 4) / rowHeight;
        if (row >= 0 && row < rowCount) {
          next = static_cast<Row>(row);
          choose = true;
        }
      }
    }
    if (next != selected) {
      const Row previous = selected;
      selected = next;
      drawOne(previous, false);
      drawOne(selected, true);
    }
    if (choose) {
      if (selected == backRow) break;
      if (selected == toggleRow) {
        nightShutdownEnabled = !nightShutdownEnabled;
        preferences.putBool("night_shutdown", nightShutdownEnabled);
        drawOne(toggleRow, true);
      } else {
        if (selected == startRow) {
          editTime("Start sleep time", nightShutdownStartMinute, "night_start");
        } else {
          editTime("End sleep time", nightShutdownEndMinute, "night_end");
        }
        drawAll();
      }
    }
    delay(20);
  }
  display.setEpdMode(originalMode);
}

void waitForAnyInput() {
  while (true) {
    M5.update();
    if (M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || M5.BtnC.wasClicked()) {
      return;
    }
    if (M5.Touch.getCount() > 0 && touchTapped(M5.Touch.getDetail())) return;
    delay(20);
  }
}

void showSdRequiredScreen(const char* feature) {
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  setupStatusScreenDrawn = false;
  display.setEpdMode(lgfx::epd_mode::epd_quality);
  display.startWrite();
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(4);
  display.drawString("SD card required", display.width() / 2, 300);
  display.setTextSize(2);
  display.drawString(String("Insert an SD card to ") + feature + ",",
                     display.width() / 2, 380);
  display.drawString("then try again.", display.width() / 2, 410);
  display.drawString("Press any button or tap to go back.",
                     display.width() / 2, 520);
  display.setTextDatum(textdatum_t::top_left);
  display.endWrite();
  display.waitDisplay();
  waitForAnyInput();
  display.setEpdMode(originalMode);
}

void showCustomizationMessage(const String& title, const String& message) {
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  display.setEpdMode(lgfx::epd_mode::epd_quality);
  drawMessage(title, message);
  display.setEpdMode(originalMode);
}

void restartForCustomization(const String& message) {
  showCustomizationMessage("Restarting", message);
  delay(1500);
  ESP.restart();
}

void drawDownloadScreen(const String& title, const String& subtitle) {
  if (quietBackground) return;
  auto& display = M5.Display;
  setupStatusScreenDrawn = false;
  display.setEpdMode(lgfx::epd_mode::epd_quality);
  display.startWrite();
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(4);
  display.drawString(title, display.width() / 2, 220);
  display.setTextSize(2);
  display.drawString(subtitle, display.width() / 2, 300);
  display.drawString("Do not power off the device.", display.width() / 2, 340);
  drawUpdateBar(450, "Preparing", 0);
  display.setTextDatum(textdatum_t::top_left);
  display.endWrite();
  display.waitDisplay();
  display.setEpdMode(lgfx::epd_mode::epd_fast);
}

void drawDownloadBar(const String& label, int percent) {
  if (quietBackground) return;
  auto& display = M5.Display;
  display.startWrite();
  drawUpdateBar(450, label.c_str(), constrain(percent, 0, 100));
  display.endWrite();
  display.waitDisplay();
}

bool downloadToSd(const String& url, const String& path,
                  const std::function<void(size_t, size_t)>& onProgress) {
  NetworkClientSecure client;
  HTTPClient http;
  if (!configureSecureRequest(client, http, url)) return false;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Download HTTP %d for %s\n", status, url.c_str());
    http.end();
    return false;
  }
  const int length = http.getSize();
  NetworkClient* stream = http.getStreamPtr();
  const String temp = path + ".part";
  SD.remove(temp.c_str());
  File file = SD.open(temp.c_str(), FILE_WRITE);
  if (length <= 0 || stream == nullptr || !file) {
    if (file) file.close();
    SD.remove(temp.c_str());
    http.end();
    return false;
  }
  static uint8_t buffer[4096];
  const size_t total = static_cast<size_t>(length);
  size_t done = 0;
  uint32_t lastData = millis();
  bool failed = false;
  while (done < total) {
    const size_t available = stream->available();
    if (available == 0) {
      if (!http.connected() || millis() - lastData > 15000) {
        failed = true;
        break;
      }
      delay(5);
      continue;
    }
    const int received = stream->readBytes(
        buffer, min<size_t>(min<size_t>(available, sizeof(buffer)), total - done));
    if (received <= 0 ||
        file.write(buffer, static_cast<size_t>(received)) !=
            static_cast<size_t>(received)) {
      failed = true;
      break;
    }
    done += static_cast<size_t>(received);
    lastData = millis();
    if (onProgress) onProgress(done, total);
  }
  file.close();
  http.end();
  noteDataUsage(done + 6500);
  if (failed || done != total) {
    SD.remove(temp.c_str());
    return false;
  }
  SD.remove(path.c_str());
  return SD.rename(temp.c_str(), path.c_str());
}

size_t fetchRepoFileNames(const String& folder, const char* extension,
                          String* names, size_t maxNames, bool directories,
                          String& error) {
  JsonDocument listing;
  const String url = String("https://api.github.com/repos/") + updateRepo +
                     "/contents/" + folder + "?ref=" + updateBranch;
  if (!getSecureJson(url, listing, error)) {
    error.replace("Calendar API", "Update server");
    return 0;
  }
  JsonArray items = listing.as<JsonArray>();
  if (items.isNull()) {
    error = String(listing["message"] | "Unexpected server response");
    return 0;
  }
  size_t count = 0;
  for (JsonVariant item : items) {
    if (count >= maxNames) break;
    const String type = item["type"] | "";
    const String name = item["name"] | "";
    if (directories ? type != "dir" : type != "file") continue;
    if (!directories && !lowercase(name).endsWith(extension)) continue;
    size_t position = count;
    while (position > 0 && lowercase(names[position - 1]) > lowercase(name)) {
      names[position] = names[position - 1];
      --position;
    }
    names[position] = name;
    ++count;
  }
  return count;
}

int pickFromList(const char* title, const String* items, size_t count,
                 const String& installedName,
                 const std::function<void(size_t, int32_t, int32_t, int32_t)>&
                     drawPreview = nullptr) {
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  constexpr size_t visibleRows = 8;
  constexpr int32_t listY = 96;
  constexpr int32_t rowHeight = 88;
  constexpr int32_t backY = 812;
  constexpr int32_t backHeight = 70;
  const size_t backIndex = count;
  size_t selected = 0;
  size_t offset = 0;
  uint8_t upHoldHandled = false;
  uint8_t downHoldHandled = false;
  uint32_t lastUpRepeatMs = 0;
  uint32_t lastDownRepeatMs = 0;
  auto drawRow = [&](size_t row) {
    const int32_t rowWidth = display.width() - 64 - (drawPreview ? 170 : 0);
    const int32_t y = listY + static_cast<int32_t>(row) * rowHeight;
    display.fillRect(24, y, rowWidth, rowHeight - 8, TFT_WHITE);
    const size_t index = offset + row;
    if (index >= count) return;
    const bool highlighted = selected == index;
    display.drawRect(24, y, rowWidth, rowHeight - 8,
                     highlighted ? TFT_RED : TFT_LIGHTGREY);
    if (highlighted) {
      display.drawRect(25, y + 1, rowWidth - 2, rowHeight - 10, TFT_RED);
    }
    display.setTextColor(highlighted ? TFT_RED : TFT_BLACK);
    display.setTextDatum(textdatum_t::middle_left);
    const bool installed = !installedName.isEmpty() && items[index] == installedName;
    const int32_t textWidthLimit = rowWidth - 40 - (installed ? 130 : 0);
    display.setTextSize(3);
    if (display.textWidth(items[index]) > textWidthLimit) display.setTextSize(2);
    display.drawString(items[index], 40, y + (rowHeight - 8) / 2);
    if (installed) {
      display.setTextSize(2);
      display.setTextDatum(textdatum_t::middle_right);
      display.drawString("installed", 24 + rowWidth - 16, y + (rowHeight - 8) / 2);
    }
    display.setTextDatum(textdatum_t::top_left);
  };
  auto drawScrollbar = [&]() {
    const int32_t trackHeight = visibleRows * rowHeight - 8;
    display.fillRect(display.width() - 34, listY, 16, trackHeight, TFT_WHITE);
    if (count > visibleRows) {
      const int32_t thumbHeight = max<int32_t>(
          24, trackHeight * static_cast<int32_t>(visibleRows) /
                  static_cast<int32_t>(count));
      const int32_t thumbY = listY +
          (trackHeight - thumbHeight) * static_cast<int32_t>(offset) /
              static_cast<int32_t>(count - visibleRows);
      display.drawRect(display.width() - 30, listY, 8, trackHeight,
                       TFT_LIGHTGREY);
      display.fillRect(display.width() - 30, thumbY, 8, thumbHeight, TFT_BLACK);
    }
  };
  auto drawBack = [&]() {
    drawUpdateMenuButton(backY, backHeight, "Back", false, selected == backIndex);
  };
  const int32_t previewX = display.width() - 200;
  constexpr int32_t previewSize = 150;
  bool previewPending = drawPreview != nullptr;
  uint32_t previewDueMs = millis() + 700;
  auto clearPreview = [&]() {
    display.fillRect(previewX, listY, previewSize, previewSize, TFT_WHITE);
    display.drawRect(previewX, listY, previewSize, previewSize, TFT_LIGHTGREY);
  };
  auto drawAll = [&](bool full) {
    display.setEpdMode(full ? lgfx::epd_mode::epd_quality
                            : lgfx::epd_mode::epd_text);
    display.startWrite();
    display.clear(TFT_WHITE);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(3);
    display.drawString(title, display.width() / 2, 30);
    for (size_t row = 0; row < visibleRows; ++row) drawRow(row);
    drawScrollbar();
    drawBack();
    if (drawPreview) clearPreview();
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(2);
    display.drawString("A/C: scroll  B: select  Touch: buttons",
                       display.width() / 2, display.height() - 44);
    display.setTextDatum(textdatum_t::top_left);
    display.endWrite();
    display.waitDisplay();
    display.setEpdMode(lgfx::epd_mode::epd_text);
  };
  auto step = [&](bool down) {
    const size_t positions = count + 1;
    selected = down ? (selected + 1) % positions
                    : (selected + positions - 1) % positions;
    if (selected < count) {
      if (selected < offset) offset = selected;
      if (selected >= offset + visibleRows) offset = selected - visibleRows + 1;
    }
  };
  auto redraw = [&](size_t previous, size_t previousOffset) {
    display.setEpdMode(lgfx::epd_mode::epd_text);
    display.startWrite();
    if (offset != previousOffset) {
      for (size_t row = 0; row < visibleRows; ++row) drawRow(row);
      drawScrollbar();
    } else if (previous < count) {
      drawRow(previous - offset);
    }
    if (selected < count && offset == previousOffset) {
      drawRow(selected - offset);
    }
    drawBack();
    display.endWrite();
  };
  drawAll(true);
  while (true) {
    M5.update();
    bool choose = false;
    bool moved = false;
    const size_t previous = selected;
    const size_t previousOffset = offset;
    const uint32_t nowMs = millis();
    if (M5.BtnA.wasClicked() && !upHoldHandled) {
      step(false);
      moved = true;
    }
    if (M5.BtnC.wasClicked() && !downHoldHandled) {
      step(true);
      moved = true;
    }
    if (M5.BtnA.isPressed() && M5.BtnA.pressedFor(350) &&
        (!upHoldHandled || nowMs - lastUpRepeatMs >= 220)) {
      upHoldHandled = true;
      lastUpRepeatMs = nowMs;
      step(false);
      moved = true;
    }
    if (M5.BtnC.isPressed() && M5.BtnC.pressedFor(350) &&
        (!downHoldHandled || nowMs - lastDownRepeatMs >= 220)) {
      downHoldHandled = true;
      lastDownRepeatMs = nowMs;
      step(true);
      moved = true;
    }
    if (M5.BtnA.wasReleased()) upHoldHandled = false;
    if (M5.BtnC.wasReleased()) downHoldHandled = false;
    if (M5.BtnB.wasClicked()) choose = true;
    if (M5.Touch.getCount() > 0) {
      const auto& touch = M5.Touch.getDetail();
      if (touchTapped(touch) && touch.x >= 24 && touch.x < display.width() - 24) {
        if (touch.y >= backY && touch.y < backY + backHeight) {
          selected = backIndex;
          choose = true;
        } else if (touch.y >= listY &&
                   touch.y < listY + static_cast<int32_t>(visibleRows) * rowHeight) {
          const size_t index =
              offset + static_cast<size_t>((touch.y - listY) / rowHeight);
          if (index < count) {
            selected = index;
            choose = true;
          }
        }
      }
    }
    if (moved && !choose) {
      redraw(previous, previousOffset);
      if (drawPreview) {
        clearPreview();
        previewPending = true;
        previewDueMs = millis() + 700;
      }
    }
    if (drawPreview && previewPending && !choose &&
        static_cast<int32_t>(millis() - previewDueMs) >= 0) {
      previewPending = false;
      if (selected < count) drawPreview(selected, previewX, listY, previewSize);
    }
    if (choose) {
      display.setEpdMode(originalMode);
      return selected == backIndex ? -1 : static_cast<int>(selected);
    }
    delay(20);
  }
}

bool prepareCustomizationNetwork(const char* feature) {
  if (!sdCardReady) {
    showSdRequiredScreen(feature);
    return false;
  }
  if (!ensureUpdateNetwork()) {
    showCustomizationMessage("No connection",
                             "Could not reach Wi-Fi or sync the clock.");
    delay(2500);
    return false;
  }
  return true;
}

void registerDownloadedImage(const String& name) {
  const String path = "/" + name;
  const size_t index = approvalImageIndex(path.c_str());
  if (index < kSupportedImageCount) {
    preferences.putUChar((String("img") + String(index)).c_str(), 1);
    return;
  }
  for (size_t i = 0; i < kMaximumCustomImages; ++i) {
    const String stored = preferences.getString(customImageKey(i, "img").c_str(), "");
    if (stored == path) return;
  }
  for (size_t i = 0; i < kMaximumCustomImages; ++i) {
    if (preferences.getString(customImageKey(i, "img").c_str(), "").isEmpty()) {
      preferences.putString(customImageKey(i, "img").c_str(), path);
      preferences.putString(customImageKey(i, "evt").c_str(), "none");
      preferences.putString(customImageKey(i, "day").c_str(), "day");
      return;
    }
  }
}

bool confirmAlertIconWarning(const String& pack) {
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  size_t selected = 0;
  const int32_t buttonY[2] = {600, 700};
  const char* labels[2] = {"Install anyway", "Cancel"};
  auto drawButtons = [&]() {
    display.setEpdMode(lgfx::epd_mode::epd_text);
    display.startWrite();
    for (size_t i = 0; i < 2; ++i) {
      drawUpdateMenuButton(buttonY[i], 80, labels[i], false, selected == i);
    }
    display.endWrite();
  };
  String lines[6];
  const size_t lineCount = wrapInfoText(
      "This pack has alerticon.png but no extremeevent.png. If you downgrade "
      "to an older version of YAWOS, the alert icon might not be used there.",
      34, lines, 6);
  display.setEpdMode(lgfx::epd_mode::epd_quality);
  display.startWrite();
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(4);
  display.drawString("Heads up", display.width() / 2, 60);
  display.setTextSize(2);
  display.drawString(pack, display.width() / 2, 140);
  for (size_t i = 0; i < lineCount; ++i) {
    display.drawString(lines[i], display.width() / 2,
                       220 + static_cast<int32_t>(i) * 34);
  }
  for (size_t i = 0; i < 2; ++i) {
    drawUpdateMenuButton(buttonY[i], 80, labels[i], false, selected == i);
  }
  display.setTextDatum(textdatum_t::top_left);
  display.endWrite();
  display.waitDisplay();
  display.setEpdMode(lgfx::epd_mode::epd_text);
  while (true) {
    M5.update();
    const size_t previous = selected;
    bool choose = false;
    if (M5.BtnA.wasClicked() || M5.BtnC.wasClicked()) selected = 1 - selected;
    if (M5.BtnB.wasClicked()) choose = true;
    if (M5.Touch.getCount() > 0) {
      const auto& touch = M5.Touch.getDetail();
      if (touchTapped(touch)) {
        for (size_t i = 0; i < 2; ++i) {
          if (touch.y >= buttonY[i] && touch.y < buttonY[i] + 80) {
            selected = i;
            choose = true;
          }
        }
      }
    }
    if (selected != previous && !choose) drawButtons();
    if (choose) {
      display.setEpdMode(originalMode);
      return selected == 0;
    }
    delay(20);
  }
}

bool syncInstalledImagePack() {
  const String pack = preferences.getString("img_pack", "");
  if (pack.isEmpty() || !sdCardReady || !firmwareNetworkReady()) return false;
  static String files[80];
  static size_t missing[80];
  String error;
  const size_t fileCount = fetchRepoFileNames("images/" + urlEncode(pack), ".png",
                                              files, 80, false, error);
  size_t missingCount = 0;
  for (size_t i = 0; i < fileCount; ++i) {
    if (!SD.exists(("/images/" + files[i]).c_str())) missing[missingCount++] = i;
  }
  if (missingCount == 0) return false;
  prepareImagesFolder();
  drawDownloadScreen("Updating images", pack);
  for (size_t m = 0; m < missingCount; ++m) {
    const String& name = files[missing[m]];
    const String label = "Image " + String(m + 1) + " of " + String(missingCount);
    drawDownloadBar(label, static_cast<int>(m * 100 / missingCount));
    const String url = String("https://raw.githubusercontent.com/") + updateRepo +
                       "/" + updateBranch + "/images/" + urlEncode(pack) + "/" +
                       urlEncode(name);
    if (downloadToSd(url, "/images/" + name, nullptr)) registerDownloadedImage(name);
  }
  drawDownloadBar("Done", 100);
  return true;
}

void showImagePacks() {
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  if (!prepareCustomizationNetwork("download image packs")) return;
  showCustomizationMessage("Loading image packs", "Please wait...");
  static String packs[32];
  String error;
  const size_t packCount = fetchRepoFileNames("images", "", packs, 32, true, error);
  if (packCount == 0) {
    showCustomizationMessage("No image packs",
                             error.isEmpty() ? "No packs were found in the repository."
                                             : error);
    delay(3000);
    return;
  }
  const String installedPack = preferences.getString("img_pack", "");
  auto previewPack = [&](size_t index, int32_t px, int32_t py, int32_t size) {
    auto& d = M5.Display;
    d.fillRect(px, py, size, size, 0xFFFF);
    d.drawRect(px, py, size, size, 0x0000);
    const String url = String("https://raw.githubusercontent.com/") + updateRepo +
                       "/" + updateBranch + "/images/" + urlEncode(packs[index]) +
                       "/partialclouds.png";
    uint32_t previewWidth = 0;
    uint32_t previewHeight = 0;
    if (downloadToSd(url, "/preview.png", nullptr) &&
        pngLooksUsable("/preview.png", previewWidth, previewHeight) &&
        previewWidth > 0 && previewHeight > 0) {
      const float fit = min((size - 8.0f) / previewWidth, (size - 8.0f) / previewHeight);
      d.drawPngFile(SD, "/preview.png", px + 4, py + 4, 0, 0, 0, 0, fit, fit);
    } else {
      d.setTextColor(0x0000);
      d.setTextDatum(textdatum_t::middle_center);
      d.setTextSize(2);
      d.drawString("No preview", px + size / 2, py + size / 2);
      d.setTextDatum(textdatum_t::top_left);
    }
    SD.remove("/preview.png");
  };
  const int choice = pickFromList("Image Packs", packs, packCount, installedPack,
                                  previewPack);
  if (choice < 0) {
    display.setEpdMode(originalMode);
    return;
  }
  const String pack = packs[choice];
  static String files[80];
  const size_t fileCount = fetchRepoFileNames("images/" + urlEncode(pack), ".png",
                                              files, 80, false, error);
  if (fileCount == 0) {
    showCustomizationMessage("Pack is empty",
                             error.isEmpty() ? "No PNG images were found in that pack."
                                             : error);
    delay(3000);
    return;
  }
  bool hasAlertIcon = false;
  bool hasLegacyAlert = false;
  for (size_t i = 0; i < fileCount; ++i) {
    if (lowercase(files[i]) == "alerticon.png") hasAlertIcon = true;
    if (lowercase(files[i]) == "extremeevent.png") hasLegacyAlert = true;
  }
  if (hasAlertIcon && !hasLegacyAlert && !confirmAlertIconWarning(pack)) return;
  preferences.putString("pend_pack", pack);
  showCustomizationMessage("Pack selected",
                           pack + " will install when you choose Save and exit.");
  delay(2000);
}

String fontNameFromFile(const String& file) {
  String name = file;
  if (lowercase(name).endsWith(".vlw")) name.remove(name.length() - 4);
  return name;
}

void showFontSample(const String& name) {
  auto& display = M5.Display;
  display.setEpdMode(lgfx::epd_mode::epd_quality);
  display.startWrite();
  display.clear(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(3);
  display.drawString(name, display.width() / 2, 30);
  display.drawFastHLine(24, 100, display.width() - 48, TFT_BLACK);
  display.setTextDatum(textdatum_t::top_left);
  display.setTextWrap(true, true);
  display.setTextSize(3);
  display.setCursor(24, 130);
  display.print("The quick brown fox jumps over the lazy dog.");
  display.setTextSize(2);
  display.setCursor(24, 380);
  display.print("The quick brown fox jumps over the lazy dog. 0123456789");
  display.setTextSize(5);
  display.setCursor(24, 540);
  display.print("Aa Bb Cc 123");
  display.setTextWrap(false);
  display.setTextDatum(textdatum_t::top_center);
  display.setTextSize(2);
  display.drawString("Press any button or tap to go back.",
                     display.width() / 2, display.height() - 44);
  display.setTextDatum(textdatum_t::top_left);
  display.endWrite();
  display.waitDisplay();
}

bool fetchFontFile(const String& file, const String& destination) {
  const String url = String("https://raw.githubusercontent.com/") + updateRepo +
                     "/" + updateBranch + "/fonts/" + urlEncode(file);
  int lastPercent = -10;
  drawDownloadBar("Downloading font", 0);
  const bool ok = downloadToSd(url, destination, [&](size_t done, size_t total) {
    const int percent = static_cast<int>(done * 100 / total);
    if (percent - lastPercent >= 4) {
      lastPercent = percent;
      drawDownloadBar("Downloading font", percent);
    }
  });
  if (ok) drawDownloadBar("Downloading font", 100);
  return ok;
}

void showFontDetail(const String& file) {
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  const String name = fontNameFromFile(file);
  size_t selected = 0;
  constexpr int32_t buttonHeight = 90;
  const int32_t buttonY[3] = {330, 450, 570};
  const char* labels[3] = {"Preview", "Select", "Back"};
  auto drawButtons = [&]() {
    display.setEpdMode(lgfx::epd_mode::epd_text);
    display.startWrite();
    for (size_t i = 0; i < 3; ++i) {
      drawUpdateMenuButton(buttonY[i], buttonHeight, labels[i], false,
                           selected == i);
    }
    display.endWrite();
  };
  auto drawScreen = [&]() {
    display.setEpdMode(lgfx::epd_mode::epd_quality);
    display.startWrite();
    display.clear(TFT_WHITE);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(4);
    display.drawString(name, display.width() / 2, 120);
    display.setTextSize(2);
    display.drawString("Preview needs a download. Select", display.width() / 2, 220);
    display.drawString("applies when you Save and exit.", display.width() / 2, 250);
    for (size_t i = 0; i < 3; ++i) {
      drawUpdateMenuButton(buttonY[i], buttonHeight, labels[i], false,
                           selected == i);
    }
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(2);
    display.drawString("A/C: move  B: select  Touch: buttons",
                       display.width() / 2, display.height() - 44);
    display.setTextDatum(textdatum_t::top_left);
    display.endWrite();
    display.waitDisplay();
    display.setEpdMode(lgfx::epd_mode::epd_text);
  };
  drawScreen();
  while (true) {
    M5.update();
    bool choose = false;
    const size_t previous = selected;
    if (M5.BtnA.wasClicked()) selected = (selected + 2) % 3;
    if (M5.BtnC.wasClicked()) selected = (selected + 1) % 3;
    if (M5.BtnB.wasClicked()) choose = true;
    if (M5.Touch.getCount() > 0) {
      const auto& touch = M5.Touch.getDetail();
      if (touchTapped(touch) && touch.x >= 24 && touch.x < display.width() - 24) {
        for (size_t i = 0; i < 3; ++i) {
          if (touch.y >= buttonY[i] && touch.y < buttonY[i] + buttonHeight) {
            selected = i;
            choose = true;
          }
        }
      }
    }
    if (selected != previous && !choose) drawButtons();
    if (choose) {
      if (selected == 2) break;
      if (selected == 0 && !prepareCustomizationNetwork("preview fonts")) {
        drawScreen();
        continue;
      }
      if (!SD.exists("/fonts")) SD.mkdir("/fonts");
      if (selected == 0) {
        const String previousPath = activeFontPath;
        const bool hadFont = customFontLoaded;
        drawDownloadScreen("Preparing preview", name);
        const String previewPath = "/fonts/preview.vlw";
        const bool ok = fetchFontFile(file, previewPath);
        if (!ok || !loadCustomFont(previewPath)) {
          if (hadFont) {
            loadCustomFont(previousPath);
          } else {
            unloadCustomFont();
          }
          SD.remove(previewPath.c_str());
          showCustomizationMessage("Preview failed",
                                   "The font could not be downloaded or read.");
          delay(2500);
        } else {
          showFontSample(name);
          waitForAnyInput();
          if (hadFont) {
            loadCustomFont(previousPath);
          } else {
            unloadCustomFont();
          }
          SD.remove(previewPath.c_str());
        }
      } else {
        preferences.putString("pend_font", file);
        showCustomizationMessage("Font selected",
                                 name + " will install when you choose Save and exit.");
        delay(2000);
        break;
      }
      selected = 0;
      drawScreen();
    }
    delay(20);
  }
  display.setEpdMode(originalMode);
}

void showFontsMenu() {
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  if (!prepareCustomizationNetwork("download fonts")) return;
  showCustomizationMessage("Loading fonts", "Please wait...");
  static String files[40];
  String error;
  const size_t fileCount = fetchRepoFileNames("fonts", ".vlw", files, 40, false, error);
  static String names[41];
  names[0] = "Default font";
  for (size_t i = 0; i < fileCount; ++i) names[i + 1] = fontNameFromFile(files[i]);
  if (fileCount == 0 && !error.isEmpty()) {
    showCustomizationMessage("No fonts", error);
    delay(3000);
  }
  String installedName = "Default font";
  const String installedPath = preferences.getString("font_path", "");
  if (!installedPath.isEmpty()) {
    const int slash = installedPath.lastIndexOf('/');
    installedName = fontNameFromFile(installedPath.substring(slash + 1));
  }
  while (true) {
    const int choice = pickFromList("Fonts", names, fileCount + 1, installedName);
    if (choice < 0) break;
    if (choice == 0) {
      if (!installedPath.isEmpty() || customFontLoaded) {
        preferences.putString("pend_font", "-");
        showCustomizationMessage("Default font selected",
                                 "It will apply when you choose Save and exit.");
        delay(2000);
      }
      continue;
    }
    showFontDetail(files[choice - 1]);
  }
  display.setEpdMode(originalMode);
}

void prepareDarkImages() {
  if (!sdCardReady || !darkMode) return;
  std::vector<String> paths;
  for (size_t i = 0; i < kSupportedImageCount; ++i) {
    paths.push_back(kSupportedImages[i]);
    paths.push_back(String("/night") + (kSupportedImages[i] + 1));
  }
  for (size_t i = 0; i < kMaximumCustomImages; ++i) {
    const String stored = preferences.getString(customImageKey(i, "img").c_str(), "");
    if (!stored.isEmpty()) paths.push_back(stored);
  }
  size_t missing = 0;
  for (const String& path : paths) {
    if (findDarkImage(path.c_str()) == nullptr &&
        !SD.exists(darkImageFile(path.c_str()).c_str())) {
      ++missing;
    }
  }
  bool screenShown = false;
  size_t done = 0;
  for (const String& path : paths) {
    if (findDarkImage(path.c_str()) != nullptr) continue;
    const bool needsBuild = !SD.exists(darkImageFile(path.c_str()).c_str());
    if (needsBuild && missing > 0) {
      if (!screenShown) {
        drawDownloadScreen("Preparing dark mode", "Inverting images (one time)");
        screenShown = true;
      }
      drawDownloadBar("Image " + String(min(done + 1, missing)) + " of " + String(missing),
                      static_cast<int>(min(done, missing) * 100 / missing));
      ++done;
    }
    loadDarkImage(path.c_str());
  }
  if (screenShown) drawDownloadBar("Done", 100);
}

void applyPendingCustomization() {
  const String pack = preferences.getString("pend_pack", "");
  const String font = preferences.getString("pend_font", "");
  if (pack.isEmpty() && font.isEmpty()) return;
  if (!prepareCustomizationNetwork("apply customization")) return;
  bool restart = false;
  if (!font.isEmpty()) {
    if (font == "-") {
      preferences.remove("font_path");
      restart = true;
    } else {
      if (!SD.exists("/fonts")) SD.mkdir("/fonts");
      drawDownloadScreen("Installing font", fontNameFromFile(font));
      const String target = "/fonts/" + font;
      if (fetchFontFile(font, target)) {
        preferences.putString("font_path", target);
        restart = true;
      } else {
        showCustomizationMessage("Font download failed",
                                 "The font could not be downloaded.");
        delay(2500);
      }
    }
    preferences.remove("pend_font");
  }
  if (!pack.isEmpty()) {
    static String files[80];
    String error;
    const size_t fileCount = fetchRepoFileNames("images/" + urlEncode(pack), ".png",
                                                files, 80, false, error);
    if (fileCount == 0) {
      showCustomizationMessage("Pack is empty",
                               error.isEmpty() ? "No PNG images were found in that pack."
                                               : error);
      delay(3000);
    } else {
      prepareImagesFolder();
      drawDownloadScreen("Installing images", pack);
      size_t failures = 0;
      for (size_t i = 0; i < fileCount; ++i) {
        const String label = "Image " + String(i + 1) + " of " + String(fileCount);
        drawDownloadBar(label, static_cast<int>(i * 100 / fileCount));
        const String url = String("https://raw.githubusercontent.com/") + updateRepo +
                           "/" + updateBranch + "/images/" + urlEncode(pack) + "/" +
                           urlEncode(files[i]);
        int lastPercent = -10;
        const bool ok = downloadToSd(
            url, "/images/" + files[i],
            [&](size_t done, size_t total) {
              const int filePercent = static_cast<int>(done * 100 / total);
              const int percent = static_cast<int>((i * 100 + filePercent) / fileCount);
              if (percent - lastPercent >= 4) {
                lastPercent = percent;
                drawDownloadBar(label, percent);
              }
            });
        if (ok) {
          registerDownloadedImage(files[i]);
        } else {
          ++failures;
        }
      }
      drawDownloadBar("Done", 100);
      if (failures == fileCount) {
        showCustomizationMessage("Download failed", "No images could be downloaded.");
        delay(3000);
      } else {
        preferences.putString("img_pack", pack);
        clearDarkImageCache();
        restart = true;
      }
    }
    preferences.remove("pend_pack");
  }
  if (restart) restartForCustomization("Customization applied.");
}

void showCustomizationMenu() {
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  constexpr int32_t boxX = 24;
  constexpr int32_t boxWidth = 492;
  constexpr int32_t boxHeight = 280;
  constexpr int32_t darkY = 690;
  const int32_t boxY[2] = {92, 382};
  constexpr int32_t backY = 796;
  constexpr int32_t backHeight = 76;
  size_t selected = 0;
  auto drawBox = [&](size_t index) {
    const bool highlighted = selected == index;
    const int32_t y = boxY[index];
    display.startWrite();
    display.fillRect(boxX, y, boxWidth, boxHeight, TFT_WHITE);
    display.drawRect(boxX, y, boxWidth, boxHeight,
                     highlighted ? TFT_RED : TFT_BLACK);
    display.drawRect(boxX + 1, y + 1, boxWidth - 2, boxHeight - 2,
                     highlighted ? TFT_RED : TFT_BLACK);
    display.setTextColor(highlighted ? TFT_RED : TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    if (index == 0) {
      display.setTextSize(4);
      display.drawString("Image Packs", boxX + boxWidth / 2, y + 186);
      display.setTextSize(2);
      const String pack = preferences.getString("img_pack", "");
      const String pendingPack = preferences.getString("pend_pack", "");
      display.drawString(!pendingPack.isEmpty() ? "Pending: " + pendingPack
                         : pack.isEmpty() ? "Built-in images" : "Current: " + pack,
                         boxX + boxWidth / 2, y + 238);
    } else {
      display.setTextSize(16);
      display.drawString("Aa", boxX + boxWidth / 2, y + 16);
      display.setTextSize(4);
      display.drawString("Fonts", boxX + boxWidth / 2, y + 186);
      display.setTextSize(2);
      const String path = preferences.getString("font_path", "");
      String shown = "Default font";
      if (!path.isEmpty()) {
        shown = fontNameFromFile(path.substring(path.lastIndexOf('/') + 1));
      }
      const String pendingFont = preferences.getString("pend_font", "");
      if (pendingFont == "-") {
        display.drawString("Pending: Default font", boxX + boxWidth / 2, y + 238);
      } else if (!pendingFont.isEmpty()) {
        display.drawString("Pending: " + fontNameFromFile(pendingFont),
                           boxX + boxWidth / 2, y + 238);
      } else {
        display.drawString("Current: " + shown, boxX + boxWidth / 2, y + 238);
      }
    }
    display.setTextDatum(textdatum_t::top_left);
    display.endWrite();
  };
  auto drawDark = [&]() {
    display.startWrite();
    drawUpdateMenuButton(darkY, backHeight,
                         String("Dark mode: ") +
                             (darkPref == 0 ? "OFF" : darkPref == 1 ? "ON"
                              : darkPref == 2 ? "AUTO" : "AUTO-INV"),
                         false, selected == 2);
    display.endWrite();
  };
  auto drawBack = [&]() {
    display.startWrite();
    drawUpdateMenuButton(backY, backHeight, "Back to settings", false, selected == 3);
    display.endWrite();
  };
  auto drawAll = [&](bool quality = false) {
    display.setEpdMode(quality ? lgfx::epd_mode::epd_quality
                               : lgfx::epd_mode::epd_text);
    display.startWrite();
    display.clear(TFT_WHITE);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(3);
    display.drawString("Customization", display.width() / 2, 30);
    display.setTextSize(2);
    display.drawString("A/C: move  B: select  Touch: buttons",
                       display.width() / 2, display.height() - 44);
    display.setTextDatum(textdatum_t::top_left);
    drawBox(0);
    drawBox(1);
    drawDark();
    drawBack();
    display.endWrite();
    drawWeatherIconFitted("/partialclouds.png", boxX + 96, boxY[0] + 10, 300, 160);
    display.waitDisplay();
  };
  auto drawPart = [&](size_t index) {
    if (index == 3) {
      drawBack();
    } else if (index == 2) {
      drawDark();
    } else {
      drawBox(index);
      if (index == 0) {
        drawWeatherIconFitted("/partialclouds.png", boxX + 96, boxY[0] + 10, 300, 160);
      }
    }
  };
  drawAll();
  while (true) {
    M5.update();
    bool choose = false;
    const size_t previous = selected;
    if (M5.BtnA.wasClicked()) selected = (selected + 3) % 4;
    if (M5.BtnC.wasClicked()) selected = (selected + 1) % 4;
    if (M5.BtnB.wasClicked()) choose = true;
    if (M5.Touch.getCount() > 0) {
      const auto& touch = M5.Touch.getDetail();
      if (touchTapped(touch) && touch.x >= boxX && touch.x < boxX + boxWidth) {
        if (touch.y >= boxY[0] && touch.y < boxY[0] + boxHeight) {
          selected = 0;
          choose = true;
        } else if (touch.y >= boxY[1] && touch.y < boxY[1] + boxHeight) {
          selected = 1;
          choose = true;
        } else if (touch.y >= darkY && touch.y < darkY + backHeight) {
          selected = 2;
          choose = true;
        } else if (touch.y >= backY && touch.y < backY + backHeight) {
          selected = 3;
          choose = true;
        }
      }
    }
    if (selected != previous && !choose) {
      drawPart(previous);
      drawPart(selected);
    } else if (selected != previous) {
      drawPart(previous);
    }
    if (choose) {
      if (selected == 3) break;
      bool toggled = false;
      if (selected == 0) {
        showImagePacks();
      } else if (selected == 1) {
        showFontsMenu();
      } else {
        darkPref = (darkPref + 1) % 4;
        preferences.putUChar("dark_pref", darkPref);
        if (darkPref < 2) {
          darkMode = darkPref == 1;
        } else if (forecast.valid && forecast.periodCount > 0) {
          applyAutoDarkMode(forecast.periods[0].isDaytime);
        }
        if (!sdCardReady) darkMode = false;
        preferences.putBool("dark_mode", darkMode);
        if (darkMode) {
          prepareDarkImages();
        } else {
          freeDarkImages();
        }
        toggled = true;
      }
      drawAll(toggled);
    }
    delay(20);
  }
  display.setEpdMode(originalMode);
}

void runButtonMenu(const char* title, size_t count,
                   const std::function<String(size_t)>& rowText,
                   const std::function<int(size_t)>& onChoose,
                   const std::function<bool(size_t)>& greyed) {
  constexpr int32_t rowStartY = 120;
  constexpr int32_t rowHeight = 80;
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  size_t selected = 0;
  auto drawRow = [&](size_t row) {
    const bool highlighted = row == selected;
    const int32_t y = rowStartY + static_cast<int32_t>(row) * rowHeight;
    display.fillRect(22, y - 4, display.width() - 44, rowHeight - 12, TFT_WHITE);
    display.drawRect(22, y - 4, display.width() - 44, rowHeight - 12,
                     highlighted ? TFT_RED : TFT_LIGHTGREY);
    const bool isGreyed = greyed && greyed(row);
    display.setTextColor(isGreyed ? TFT_LIGHTGREY
                                  : highlighted ? TFT_RED : TFT_BLACK);
    display.setTextDatum(textdatum_t::top_left);
    display.setTextSize(2);
    display.drawString(String(highlighted ? "> " : "  ") + rowText(row), 36,
                       y + 16);
  };
  auto drawRows = [&](bool full) {
    display.setEpdMode(full ? lgfx::epd_mode::epd_quality
                            : lgfx::epd_mode::epd_text);
    display.startWrite();
    if (full) {
      display.clear(TFT_WHITE);
      display.setTextColor(TFT_BLACK);
      display.setTextDatum(textdatum_t::top_center);
      display.setTextSize(3);
      display.drawString(title, display.width() / 2, 40);
      display.setTextSize(2);
      display.drawString("A: up  C: down  B: select  Tap: select",
                         display.width() / 2, display.height() - 44);
    }
    for (size_t i = 0; i < count; ++i) drawRow(i);
    display.setTextDatum(textdatum_t::top_left);
    display.endWrite();
    display.waitDisplay();
    display.setEpdMode(lgfx::epd_mode::epd_text);
  };
  drawRows(true);
  while (true) {
    M5.update();
    bool choose = false;
    const size_t previous = selected;
    if (M5.BtnA.wasClicked()) selected = (selected + count - 1) % count;
    if (M5.BtnC.wasClicked()) selected = (selected + 1) % count;
    if (M5.BtnB.wasClicked()) choose = true;
    if (M5.Touch.getCount() > 0) {
      const auto& touch = M5.Touch.getDetail();
      if (touchTapped(touch) && touch.y >= rowStartY - 4) {
        const int32_t row = (touch.y - rowStartY + 4) / rowHeight;
        if (row >= 0 && static_cast<size_t>(row) < count) {
          selected = static_cast<size_t>(row);
          choose = true;
        }
      }
    }
    if (selected != previous && !choose) {
      display.setEpdMode(lgfx::epd_mode::epd_text);
      display.startWrite();
      drawRow(previous);
      drawRow(selected);
      display.endWrite();
    }
    if (choose) {
      const int result = onChoose(selected);
      if (result == 2) break;
      drawRows(result == 1);
    }
    delay(20);
  }
  display.setEpdMode(originalMode);
}

void showAdaptivePollingManager() {
  runButtonMenu(
      "Adaptive polling", 4,
      [&](size_t row) -> String {
        if (row == 0) {
          return String("Adaptive polling: ") + (adaptiveUpdates ? "ON" : "OFF");
        }
        if (row == 1) return "Slow poll (min): " + String(slowPollMinutes);
        if (row == 2) return "Quick poll (min): " + String(fastPollMinutes);
        return "Back to settings";
      },
      [&](size_t row) -> int {
        if (row == 0) {
          adaptiveUpdates = !adaptiveUpdates;
          adaptiveRefreshIntervalMinutes = 0;
          preferences.putBool("adapt_on", adaptiveUpdates);
          return 0;
        }
        if (row == 1 || row == 2) {
          uint16_t& value = row == 1 ? slowPollMinutes : fastPollMinutes;
          const String edited = editOnDeviceNumber(
              row == 1 ? "Slow poll (not rain), minutes"
                       : "Quick poll (rain or worse), minutes",
              String(value), 4);
          uint16_t parsed = 0;
          if (parseRefreshIntervalMinutes(edited, parsed)) {
            value = parsed;
            preferences.putUShort(row == 1 ? "poll_slow" : "poll_fast", value);
            adaptiveRefreshIntervalMinutes = 0;
          }
          return 1;
        }
        return 2;
      },
      [&](size_t row) -> bool {
        return (row == 1 || row == 2) && !adaptiveUpdates;
      });
}

void showAdvancedMenu() {
  runButtonMenu(
      "Advanced", 3,
      [&](size_t row) -> String {
        if (row == 0) {
          return String("Silent wake refresh: ") + (quietBackgroundSetting ? "ON" : "OFF");
        }
        if (row == 1) {
          return String("No animation loading: ") + (instantLoad ? "ON" : "OFF");
        }
        return "Back to settings";
      },
      [&](size_t row) -> int {
        if (row == 0) {
          quietBackgroundSetting = !quietBackgroundSetting;
          preferences.putBool("quiet_bg", quietBackgroundSetting);
          return 0;
        }
        if (row == 1) {
          instantLoad = !instantLoad;
          preferences.putBool("instant_load", instantLoad);
          return 0;
        }
        return 2;
      },
      nullptr);
}

void showDeviceSettings(bool redrawForecastOnExit = true) {
  enum Setting : uint8_t {
    batterySaverSetting,
    zipSetting,
    weatherApiSetting,
    refreshSetting,
    adaptiveSetting,
    timezoneSetting,
    temperatureSetting,
    simpleModeSetting,
    goodnightTimingSetting,
    nightIconsSetting,
    customizationSetting,
    updatesManagerSetting,
    advancedSetting,
    saveSetting,
    settingCount
  };
  setupStatusScreenDrawn = false;
  Setting selected = batterySaverSetting;
  const bool batterySaverAtEntry = batterySaver;
  bool done = false;
  constexpr int32_t rowStartY = 92;
  constexpr int32_t rowHeight = 58;
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  display.setEpdMode(lgfx::epd_mode::epd_text);
  String rowTexts[settingCount];
  rowTexts[batterySaverSetting] =
      String("Battery saver: ") + (batterySaver ? "ON" : "OFF");
  rowTexts[zipSetting] =
      String("Location: ") +
      (defaultLocationQuery().isEmpty() ? "not set" : defaultLocationQuery());
  rowTexts[weatherApiSetting] =
      String("Weather API: ") + weatherApiName(weatherApiProvider);
  rowTexts[refreshSetting] = "Poll rate (minutes): " + String(refreshIntervalMinutes);
  rowTexts[adaptiveSetting] =
      "Adaptive polling manager";
  rowTexts[timezoneSetting] = String("Clock offset: ") + selectedTimezone;
  rowTexts[temperatureSetting] =
      String("Temperature: ") + (useCelsius ? "Celsius" : "Fahrenheit");
  rowTexts[simpleModeSetting] =
      String("Simple display: ") + (simpleMode ? "ON" : "OFF");
  rowTexts[goodnightTimingSetting] = "Shutdown Period Settings";
  rowTexts[nightIconsSetting] = String("Night icons: ") + (useNightIcons ? "ON" : "OFF");
  rowTexts[customizationSetting] = "Customization";
  rowTexts[updatesManagerSetting] = "Updates manager";
  rowTexts[advancedSetting] = "Advanced";
  rowTexts[saveSetting] = "Save and exit";
  for (uint8_t i = 0; i < settingCount; ++i) {
    if (rowTexts[i].isEmpty()) {
      rowTexts[i] = "(not used)";
    }
  }
  uint8_t cachedSelected = selected;
  auto drawSettingRow = [&](Setting row, bool highlighted) {
    const int32_t y = rowStartY + static_cast<int32_t>(row) * rowHeight;
    const bool greyed = row == refreshSetting && adaptiveUpdates;
    display.startWrite();
    display.fillRect(22, y - 4, display.width() - 44, rowHeight - 6,
                     TFT_WHITE);
    display.setTextColor(TFT_BLACK);
    display.drawRect(22, y - 4, display.width() - 44, rowHeight - 6,
                     highlighted ? TFT_RED : TFT_LIGHTGREY);
    display.setTextColor(greyed ? TFT_LIGHTGREY
                                : highlighted ? TFT_RED : TFT_BLACK);
    display.setTextDatum(textdatum_t::top_left);
    display.setTextSize(2);
    const String shown = String(highlighted ? "> " : "  ") + rowTexts[row];
    display.drawString(shown, 36, y + 12);
    if (row == updatesManagerSetting) {
      const int32_t shownWidth = display.textWidth(shown.c_str());
      display.setTextSize(1);
      display.drawString("[updates os to latest]", 36 + shownWidth + 10,
                         y + 20);
      display.setTextSize(2);
    }
    display.endWrite();
  };
  auto drawSettingsScreen = [&]() {
    display.setEpdMode(lgfx::epd_mode::epd_quality);
    display.startWrite();
    display.clear(TFT_WHITE);
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(3);
    display.drawString("Device settings", display.width() / 2, 20);
    for (uint8_t i = 0; i < settingCount; ++i) {
      drawSettingRow(static_cast<Setting>(i), i == cachedSelected);
    }
    display.setTextColor(TFT_BLACK);
    display.setTextDatum(textdatum_t::top_center);
    display.setTextSize(2);
    display.drawString("A: up  C: down  Tap: select  B: exit",
                       display.width() / 2, display.height() - 44);
    display.setTextDatum(textdatum_t::top_left);
    display.endWrite();
    display.waitDisplay();
    display.setEpdMode(lgfx::epd_mode::epd_text);
  };
  const bool heldFromBoot = bootSettingsRequested;
  if (bootSettingsRequested) {
    ignoreBootCClick = false;
    bootSettingsRequested = false;
  }
  drawSettingsScreen();
  uint8_t upHoldHandled = false;
  uint8_t downHoldHandled = heldFromBoot;
  while (!done) {
    M5.update();
    bool choose = false;
    const bool upClicked = M5.BtnA.wasClicked();
    const bool downClicked = M5.BtnC.wasClicked();
    if (upClicked && !upHoldHandled) {
      const Setting previous = selected;
      selected = static_cast<Setting>(
          (selected + settingCount - 1) % settingCount);
      drawSettingRow(previous, false);
      drawSettingRow(selected, true);
      cachedSelected = selected;
    }
    if (downClicked && !downHoldHandled) {
      const Setting previous = selected;
      selected = static_cast<Setting>((selected + 1) % settingCount);
      drawSettingRow(previous, false);
      drawSettingRow(selected, true);
      cachedSelected = selected;
    }
    if (M5.BtnA.isPressed() && M5.BtnA.pressedFor(350) &&
        !upHoldHandled) {
      upHoldHandled = true;
      const Setting previous = selected;
      selected = static_cast<Setting>(
          (selected + settingCount - 1) % settingCount);
      drawSettingRow(previous, false);
      drawSettingRow(selected, true);
      cachedSelected = selected;
    }
    if (M5.BtnC.isPressed() && M5.BtnC.pressedFor(350) &&
        !downHoldHandled) {
      downHoldHandled = true;
      const Setting previous = selected;
      selected = static_cast<Setting>((selected + 1) % settingCount);
      drawSettingRow(previous, false);
      drawSettingRow(selected, true);
      cachedSelected = selected;
    }
    if (M5.BtnA.wasReleased()) upHoldHandled = false;
    if (M5.BtnC.wasReleased()) downHoldHandled = false;
    if (M5.Touch.getCount() > 0) {
      const auto& touch = M5.Touch.getDetail();
      if (touchTapped(touch)) {
        const int32_t row = (touch.y - rowStartY) / rowHeight;
        if (touch.y >= rowStartY && row >= 0 && row < settingCount) {
          const Setting previous = selected;
          selected = static_cast<Setting>(row);
          choose = true;
          if (previous != selected) {
            drawSettingRow(previous, false);
            drawSettingRow(selected, true);
            cachedSelected = selected;
          }
        }
      }
    }
    if (M5.BtnB.wasClicked()) done = true;
    if (choose) {
      switch (selected) {
        case batterySaverSetting:
          batterySaver = !batterySaver;
          break;
        case zipSetting: {
          bool clearLocation = false;
          const String edited =
              editOnDeviceNumber("Location ZIP code",
                                 defaultLocationQuery(), 5, false, false,
                                 nullptr, &clearLocation);
          if (clearLocation) {
            savedLocationQuery = "";
            savedZipCode = "";
            preferences.remove("zip");
            preferences.putString("location_query", "");
            activeLocationQuery = defaultLocationQuery();
            resolvedLocationQuery = "";
            savedLatitude = NAN;
            savedLongitude = NAN;
            locationLookupPending = !activeLocationQuery.isEmpty();
            preferences.putBool("geocode_pending", locationLookupPending);
            preferences.remove("latitude");
            preferences.remove("longitude");
            forecast.valid = false;
            forecast.periodCount = 0;
            clockUpdatesEnabled = false;
          } else if (edited.length() == 5) {
            savedLocationQuery = edited;
            savedZipCode = "";
            activeLocationQuery = defaultLocationQuery();
            resolvedLocationQuery = "";
            savedLatitude = NAN;
            savedLongitude = NAN;
            locationLookupPending = true;
            preferences.putString("location_query", savedLocationQuery);
            preferences.remove("zip");
            preferences.putBool("geocode_pending", true);
            preferences.remove("latitude");
            preferences.remove("longitude");
          }
          break;
        }
        case weatherApiSetting: {
          const auto currentApi = static_cast<uint8_t>(weatherApiProvider);
          weatherApiProvider = static_cast<WeatherApiProvider>(
              (currentApi + 1) % 4);
          preferences.putString("weather_api",
                                weatherApiPreferenceValue(weatherApiProvider));
          weatherApiChangedInSettings = true;
          break;
        }
        case refreshSetting: {
          if (adaptiveUpdates) break;
          const String edited = editOnDeviceNumber(
              "Poll rate in minutes", String(refreshIntervalMinutes), 4);
          uint16_t parsed = 0;
          if (parseRefreshIntervalMinutes(edited, parsed)) {
            refreshIntervalMinutes = parsed;
            if (!adaptiveUpdates) {
              adaptiveRefreshIntervalMinutes = parsed;
            }
            preferences.putUShort("refresh_minutes", refreshIntervalMinutes);
          }
          break;
        }
        case timezoneSetting: {
          int offsetHours = 0;
          if (selectedTimezone == "auto") {
            selectedTimezone = "-12";
          } else if (parseClockOffsetHours(selectedTimezone, offsetHours)) {
            selectedTimezone = offsetHours >= 14
                ? "auto" : String(offsetHours + 1);
          } else {
            selectedTimezone = "auto";
          }
          preferences.putString("timezone", selectedTimezone);
          break;
        }
        case simpleModeSetting:
          simpleMode = !simpleMode;
          break;
        case temperatureSetting:
          useCelsius = !useCelsius;
          preferences.putBool("celsius", useCelsius);
          break;
        case adaptiveSetting:
          showAdaptivePollingManager();
          break;
        case customizationSetting:
          showCustomizationMenu();
          break;
        case advancedSetting:
          showAdvancedMenu();
          break;
        case nightIconsSetting:
          useNightIcons = !useNightIcons;
          break;
        case goodnightTimingSetting:
          showShutdownPeriodSettings();
          break;
        case updatesManagerSetting:
          showUpdatesManager();
          break;
        case saveSetting:
          done = true;
          break;
        case settingCount:
          break;
      }
    }
    if (choose && !done) {
      rowTexts[batterySaverSetting] =
          String("Battery saver: ") + (batterySaver ? "ON" : "OFF");
      rowTexts[zipSetting] =
          String("Location: ") +
          (defaultLocationQuery().isEmpty() ? "not set" : defaultLocationQuery());
      rowTexts[weatherApiSetting] =
          String("Weather API: ") + weatherApiName(weatherApiProvider);
      rowTexts[refreshSetting] = "Poll rate (minutes): " + String(refreshIntervalMinutes);
      rowTexts[adaptiveSetting] =
          "Adaptive polling manager";
      rowTexts[timezoneSetting] = String("Clock offset: ") + selectedTimezone;
      rowTexts[temperatureSetting] =
          String("Temperature: ") + (useCelsius ? "Celsius" : "Fahrenheit");
      rowTexts[simpleModeSetting] =
          String("Simple display: ") + (simpleMode ? "ON" : "OFF");
      rowTexts[goodnightTimingSetting] = "Shutdown Period Settings";
      rowTexts[nightIconsSetting] = String("Night icons: ") + (useNightIcons ? "ON" : "OFF");
      rowTexts[customizationSetting] = "Customization";
      rowTexts[updatesManagerSetting] = "Updates manager";
      rowTexts[advancedSetting] = "Advanced";
      rowTexts[saveSetting] = "Save and exit";
      const bool openedNumpad =
          selected == zipSetting || selected == goodnightTimingSetting ||
          selected == updatesManagerSetting || selected == adaptiveSetting ||
          selected == customizationSetting || selected == advancedSetting ||
          (selected == refreshSetting && !adaptiveUpdates);
      if (openedNumpad) {
        drawSettingsScreen();
      } else {
        drawSettingRow(refreshSetting, selected == refreshSetting);
        drawSettingRow(adaptiveSetting, selected == adaptiveSetting);
        drawSettingRow(selected, true);
      }
    }
    delay(20);
  }
  display.setEpdMode(originalMode);

  preferences.putBool("battery_saver", batterySaver);
  preferences.putBool("adapt_on", adaptiveUpdates);
  preferences.putBool("simple_mode", simpleMode);
  preferences.putBool("night_icons", useNightIcons);
  preferences.putBool("night_shutdown", nightShutdownEnabled);
  preferences.putString("timezone", selectedTimezone);
  preferences.putUShort("refresh_minutes", refreshIntervalMinutes);
  preferences.putUShort("night_start", nightShutdownStartMinute);
  preferences.putUShort("night_end", nightShutdownEndMinute);
  preferences.putUInt("net_count", savedNetworkCount);
  preferences.putUInt("night_icon_set", nightIconSetSelected);
  preferences.putBool("nic_saved", nightIconChoiceSaved);
  preferences.putBool("nis_saved", nightIconSetChoiceSaved);
  applyPendingCustomization();
  if (batterySaver != batterySaverAtEntry) {
    restartForCustomization("Applying battery saver setting.");
  }
  if (redrawForecastOnExit) {
    showWeekForecast ? drawWeekForecast() : drawForecast();
  }
}

bool hasSavedWifi() {
  for (const String& ssid : savedSsids) {
    if (!ssid.isEmpty()) return true;
  }
  return false;
}

void drawWifiConnectingStatus(const String& status) {
  if (quietBackground) return;
  auto& display = M5.Display;
  const auto originalMode = display.getEpdMode();
  display.setEpdMode(lgfx::epd_mode::epd_text);
  display.setTextColor(TFT_BLACK);
  display.setTextDatum(textdatum_t::top_left);
  display.setTextSize(2);
  if (wifiConnectLineCount == 12) {
    for (uint8_t i = 1; i < wifiConnectLineCount; ++i) {
      wifiConnectLines[i - 1] = wifiConnectLines[i];
    }
    --wifiConnectLineCount;
    display.fillRect(24, 128, display.width() - 48, 620, TFT_WHITE);
    for (uint8_t i = 0; i < wifiConnectLineCount; ++i) {
      display.drawString("> " + wifiConnectLines[i], 32, 136 + i * 48);
    }
  }
  wifiConnectLines[wifiConnectLineCount] = status;
  const int32_t lineY = 136 + wifiConnectLineCount * 48;
  display.fillRect(24, lineY, display.width() - 48, 44, TFT_WHITE);
  display.drawString("> " + status, 32, lineY);
  ++wifiConnectLineCount;
  display.waitDisplay();
  display.setEpdMode(originalMode);
}

bool connectToSavedWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, true);
  delay(100);
  setupStatusScreenDrawn = false;
  showSetupStatus("Scanning for networks...");
  const int16_t scanResult = WiFi.scanNetworks(false, true);
  bool visibleNetworks[kMaximumNetworks] = {};
  if (scanResult >= 0) {
    for (size_t i = 0; i < kMaximumNetworks; ++i) {
      if (savedSsids[i].isEmpty()) continue;
      for (int16_t scanned = 0; scanned < scanResult; ++scanned) {
        if (WiFi.SSID(scanned) == savedSsids[i]) {
          visibleNetworks[i] = true;
          break;
        }
      }
    }
    WiFi.scanDelete();
    bool anyVisibleNetwork = false;
    for (bool visible : visibleNetworks) {
      anyVisibleNetwork = anyVisibleNetwork || visible;
    }
    if (!anyVisibleNetwork) {
      Serial.println("Wi-Fi scan found none of the saved networks.");
      return false;
    }
  } else {
    Serial.printf("Wi-Fi scan failed (%d); trying saved networks anyway.\n",
                  scanResult);
  }

  for (size_t i = 0; i < kMaximumNetworks; ++i) {
    if (savedSsids[i].isEmpty()) continue;
    if (scanResult >= 0 && !visibleNetworks[i]) continue;

    showSetupStatus("Connecting to network " + String(i + 1) + "...");
    WiFi.begin(savedSsids[i].c_str(), savedPasswords[i].c_str());
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - started < 15000) {
      M5.update();
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) break;
    WiFi.disconnect(false, false);
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) return false;
  wifiUnavailable = false;
  selectLocationForConnectedNetwork();

  requestClockSync();
  return true;
}

void refreshWeather(bool automatic = false) {
  weatherApiChangedInSettings = false;
  if (weatherTestMode) {
    drawForecast();
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    selectLocationForConnectedNetwork();
  }
  const bool hadForecast = forecast.valid;
  apiExpired = false;
  const int batteryLevel = M5.Power.getBatteryLevel();
  if (batteryLevel >= 0 && batteryLevel <= 5) {
    showDeadBatteryScreen();
    return;
  }

  if (!clockReady) {
    lastRefreshMs = millis();
    lastWeatherError = "Waiting for network time synchronization.";
    if (!clockWaitScreenDrawn) {
      drawForecast(lastWeatherError);
      clockWaitScreenDrawn = true;
    }
    Serial.println(lastWeatherError);
    return;
  }
  clockWaitScreenDrawn = false;

  if (!hasLocationConfiguration()) {
    clockUpdatesEnabled = false;
    lastRefreshMs = millis();
    lastWeatherError = "No location is configured and public-IP lookup failed.";
    drawLocationNeededScreen();
    Serial.printf("Weather refresh skipped: %s\n", lastWeatherError.c_str());
    return;
  }

  Forecast& latest = forecastScratch;
  if (resolveSavedLocation() && fetchForecast(latest)) {
    const ForecastPeriod& upcoming = latest.periods[0];
    const String condition = lowercase(upcoming.shortForecast);
    const bool rainOrWorse =
        condition.indexOf("rain") >= 0 ||
        condition.indexOf("drizzle") >= 0 ||
        condition.indexOf("shower") >= 0 ||
        condition.indexOf("thunder") >= 0 ||
        condition.indexOf("hail") >= 0 ||
        condition.indexOf("snow") >= 0 ||
        condition.indexOf("flurr") >= 0 ||
        condition.indexOf("sleet") >= 0 ||
        condition.indexOf("ice") >= 0 ||
        condition.indexOf("squall") >= 0 ||
        condition.indexOf("hurricane") >= 0 ||
        condition.indexOf("tropical") >= 0 ||
        condition.indexOf("tornado") >= 0;
    adaptiveRefreshIntervalMinutes = rainOrWorse ? fastPollMinutes : slowPollMinutes;
    forecast = latest;
    const bool darkChanged = applyAutoDarkMode(forecast.periods[0].isDaytime);
    if (darkChanged) {
      if (darkMode) {
        prepareDarkImages();
      } else {
        freeDarkImages();
      }
    }
    clockUpdatesEnabled = true;
    fetchCalendarEvent(latest.updated);
    lastRefreshMs = millis();
    if (showWeekForecast) {
      drawWeekForecast();
    } else if (hadForecast && automatic && !darkChanged) {
      drawForecastContent(true);
      drawForecastFooter();
      drawClock();
    } else {
      drawForecast();
    }
  } else {
    lastRefreshMs = millis();
    if (apiExpired) {
      showErrorAndPowerOff("/apiexpired.png", "NOAA API expired",
                           "Add apiexpired.png to the SD card root, then restart.");
      return;
    }
    quietBackground = false;
    if (!hadForecast) {
      clockUpdatesEnabled = false;
      drawForecast(lastWeatherError);
    } else {
      if (showWeekForecast) {
        drawWeekForecast();
      } else {
        drawForecastFooter("Refresh failed. B: retry.");
      }
      Serial.printf("Weather refresh failed: %s\n", lastWeatherError.c_str());
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.printf("Reset cause: %d\n", static_cast<int>(esp_reset_reason()));
  M5.begin();
  M5.update();
  bootSettingsRequested = M5.BtnC.isPressed();
  ignoreBootCClick = bootSettingsRequested;
  {
    Preferences early;
    early.begin(kPreferencesNamespace, false);
    const uint32_t expectedWake = early.getUInt("wake_at", 0);
    const time_t bootTime = time(nullptr);
    timedWakeBoot = expectedWake != 0 && bootTime >= kValidClockEpoch &&
                    static_cast<uint32_t>(bootTime) + 90 >= expectedWake;
    if (expectedWake != 0) early.putUInt("wake_at", 0);
    quietBackground = timedWakeBoot && early.getBool("battery_saver", false) &&
                      early.getBool("quiet_bg", false) && !bootSettingsRequested;
    darkMode = early.getBool("dark_mode", false);
    early.end();
  }
  M5.Display.setRotation(0);
  M5.Display.setTextDatum(textdatum_t::top_left);
  if (M5.Power.getBatteryLevel() >= 0 &&
      M5.Power.getBatteryLevel() <= 5) {
    showDeadBatteryScreen();
    return;
  }
  sdCardReady = mountSdCard();
  if (!sdCardReady) darkMode = false;
  if (!quietBackground) {
    M5.Display.clear(TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextSize(11);
    const char* bootWords[] = {"Yet", "Another", "Weather", "OS"};
    for (size_t i = 0; i < 4; ++i) {
      M5.Display.drawString(bootWords[i], 24, 140 + static_cast<int32_t>(i) * 120);
    }
    M5.Display.setTextSize(2);
    M5.Display.drawString("Starting...", 24, 660);
    M5.Display.waitDisplay();
  }
  readLocalEnvironment();

  preferences.begin(kPreferencesNamespace, false);
  preferencesReady = true;
  darkPref = preferences.getUChar("dark_pref", preferences.getBool("dark_mode", false) ? 1 : 0);
  if (darkPref > 3) darkPref = 0;
  darkMode = sdCardReady && (darkPref == 1 ||
             (darkPref >= 2 && preferences.getBool("dark_mode", false)));
  for (size_t i = 0; i < 3; ++i) {
    const String suffix = String(i + 1);
    const String ssidKey = String("ssid") + suffix;
    const String passwordKey = String("password") + suffix;
    savedSsids[i] = preferences.getString(ssidKey.c_str(), "");
    savedPasswords[i] = preferences.getString(passwordKey.c_str(), "");
  }
  for (size_t i = kPrimaryNetworks; i < kMaximumNetworks; ++i) {
    const String suffix = String(i + 1);
    const String ssidKey = String("ssid") + suffix;
    const String passwordKey = String("password") + suffix;
    const String locationKey = String("loc") + suffix;
    savedSsids[i] = preferences.getString(ssidKey.c_str(), "");
    savedPasswords[i] = preferences.getString(passwordKey.c_str(), "");
    savedNetworkLocations[i] = preferences.getString(locationKey.c_str(), "");
  }
  if (savedSsids[0].isEmpty()) {
    savedSsids[0] = preferences.getString("ssid", "");
    savedPasswords[0] = preferences.getString("password", "");
  }
  savedLatitude = preferences.getFloat("latitude", NAN);
  savedLongitude = preferences.getFloat("longitude", NAN);
  savedLocationName = preferences.getString("location", "");
  calendarId = preferences.getString("calendar_id", "");
  calendarApiKey = preferences.getString("calendar_key", "");
  savedLocationQuery = preferences.getString("location_query", "");
  savedZipCode = preferences.getString("zip", "");
  savedState = preferences.getString("state", "");
  locationUtcOffsetSeconds = preferences.getInt("utc_offset", 0);
  utcOffsetKnown = preferences.isKey("utc_offset");
  selectedTimezone = preferences.getString("timezone", "auto");
  const String configuredWeatherApi =
      preferences.getString("weather_api", "open-meteo");
  if (!parseWeatherApiProvider(configuredWeatherApi, weatherApiProvider)) {
    weatherApiProvider = WeatherApiProvider::openMeteo;
    preferences.putString("weather_api", "open-meteo");
  }
  if (!validClockOffsetSelection(selectedTimezone)) {
    selectedTimezone = "auto";
    preferences.putString("timezone", selectedTimezone);
  }
  refreshIntervalMinutes = preferences.getUShort(
      "refresh_minutes", kDefaultRefreshIntervalMinutes);
  if (refreshIntervalMinutes < kMinimumRefreshIntervalMinutes ||
      refreshIntervalMinutes > kMaximumRefreshIntervalMinutes) {
    refreshIntervalMinutes = kDefaultRefreshIntervalMinutes;
    preferences.putUShort("refresh_minutes", refreshIntervalMinutes);
  }
  for (size_t i = 0; i < kPrimaryNetworks; ++i) {
    const String locationKey = String("loc") + String(i + 1);
    savedNetworkLocations[i] = preferences.getString(locationKey.c_str(), "");
  }
  locationLookupPending = preferences.getBool("geocode_pending", false);
  batterySaver = preferences.getBool("battery_saver", false);
  quietBackgroundSetting = preferences.getBool("quiet_bg", false);
  instantLoad = preferences.getBool("instant_load", false);
  darkPref = preferences.getUChar("dark_pref", preferences.getBool("dark_mode", false) ? 1 : 0);
  if (darkPref > 3) darkPref = 0;
  showWeekForecast = preferences.getBool("week_view", false);
  simpleMode = preferences.getBool("simple_mode", false);
  nightShutdownEnabled = preferences.getBool("night_shutdown", false);
  useNightIcons = preferences.getBool("night_icons", false);
  nightShutdownStartMinute = preferences.getUShort("night_start", 22 * 60);
  nightShutdownEndMinute = preferences.getUShort("night_end", 6 * 60);
  if (nightShutdownStartMinute >= 1440) nightShutdownStartMinute = 22 * 60;
  if (nightShutdownEndMinute >= 1440) nightShutdownEndMinute = 6 * 60;
  useCelsius = preferences.getBool("celsius", false);
  adaptiveUpdates = preferences.getBool("adapt_on", false);
  slowPollMinutes = preferences.getUShort("poll_slow", 30);
  fastPollMinutes = preferences.getUShort("poll_fast", 15);
  if (slowPollMinutes < 1 || slowPollMinutes > 1440) slowPollMinutes = 30;
  if (fastPollMinutes < 1 || fastPollMinutes > 1440) fastPollMinutes = 15;
  autoUpdates = preferences.getBool("auto_update", true);
  updateRepo = preferences.getString("repo", updateRepo);
  updateBranch = preferences.getString("repo_branch", updateBranch);
  loadDataUsage();
  nightlyUpdates = preferences.getBool("nightly_updates", false);
  flashMode = preferences.getUChar("flash_mode", 0);
  if (flashMode > 2) flashMode = 0;
  cleanupStagedUpdates();
  recoverInterruptedInstall();
  prepareImagesFolder();
  prepareDarkImages();
  loadCustomFont(preferences.getString("font_path", ""));
  clockReady = time(nullptr) >= kValidClockEpoch;
  savedNetworkCount = preferences.getUInt("net_count", 3);
  if (savedNetworkCount < kPrimaryNetworks) savedNetworkCount = kPrimaryNetworks;
  if (savedNetworkCount > kMaxNetworkEntries) savedNetworkCount = kMaxNetworkEntries;
  nightIconSetSelected = preferences.getUInt("night_icon_set", 0);
  nightIconChoiceSaved = preferences.getBool("nic_saved", false);
  nightIconSetChoiceSaved = preferences.getBool("nis_saved", false);
  if (bootSettingsRequested) {
    showDeviceSettings(false);
  }
  if (!weatherApiChangedInSettings && nightShutdownIsActive() &&
      nightShutdownTimeConfirmed()) {
    enterNightShutdownSleep();
    return;
  }
  if (!isfinite(savedLatitude) && !defaultLocationQuery().isEmpty()) {
    locationLookupPending = true;
  }
  if (M5.Power.getBatteryLevel() >= 0 &&
      M5.Power.getBatteryLevel() <= 5) {
    showDeadBatteryScreen();
    return;
  }
  if (!quietBackground) reviewSdImagesOnBoot();

  M5.update();
  if (M5.BtnA.isPressed() || !hasSavedWifi()) {
    startSetupPortal();
  }

  if (!hasSavedWifi()) {
    drawMessage("Setup required",
                "Press A to reopen Wi-Fi setup.");
    return;
  }

  if (!connectToSavedWifi()) {
    quietBackground = false;
    showWeatherUnavailable("Wi-Fi unavailable. Press C to retry, A for setup.");
    lastWifiRetryMs = millis();
    return;
  }

  if (!clockReady) {
    showSetupStatus("Synchronizing time...");
    const uint32_t syncStarted = millis();
    while (!clockReady && millis() - syncStarted < 20000) {
      M5.update();
      updateClockSync();
      delay(50);
    }
  }
  if (clockReady && !weatherApiChangedInSettings &&
      nightShutdownIsActive() && nightShutdownTimeConfirmed()) {
    enterNightShutdownSleep();
    return;
  }
  {
    const uint32_t nowEpoch =
        clockReady ? static_cast<uint32_t>(time(nullptr)) : 0;
    const uint32_t lastPackCheck = preferences.getUInt("pack_chk", 0);
    if (!timedWakeBoot ||
        (nowEpoch != 0 && (nowEpoch < lastPackCheck ||
                           nowEpoch - lastPackCheck >= 6UL * 3600UL))) {
      syncInstalledImagePack();
      if (nowEpoch != 0) preferences.putUInt("pack_chk", nowEpoch);
    }
  }
  saveRecoveryImage();
  checkForFirmwareUpdate();
  refreshWeather();
  if (clockReady && !hasLocationConfiguration()) {
    clockUpdatesEnabled = false;
    drawLocationNeededScreen();
    return;
  }
  enterBatterySaverSleep();
}

void persistSavedNetworks();

void saveSavedNetworkList(String ssids[], String passwords[]) {
  for (size_t i = 0; i < kMaxNetworkEntries && i < kMaximumNetworks; ++i) {
    if (i < savedNetworkCount) {
      savedSsids[i] = ssids[i];
      savedPasswords[i] = passwords[i];
    }
  }
  savedNetworkCount = kMaxNetworkEntries;
  assignNetworkSelection();
  persistSavedNetworks();
}

void setNightIconSet(uint8_t set) {
  if (set > 1) set = 1;
  nightIconSetSelected = set;
  preferences.putUInt("night_icon_set", nightIconSetSelected);
}

void setNightIconChoice(String choice) {
  nightIconChoice = choice;
  nightIconChoiceSaved = true;
  preferences.putString("nic_choice", choice);
}

void setNightIconSetChoice(String choice) {
  nightIconSetChoice = choice;
  nightIconSetChoiceSaved = true;
  preferences.putString("nis_choice", choice);
}

void updateSavedNetworkList() {
  String input = savedNetworkCount < kNetworkDropdownVisible
      ? savedSsids[0] + "\n" + savedSsids[1] + "\n" + savedSsids[2]
      : savedSsids[0] + "\n" + savedSsids[1] + "\n" + savedSsids[2] + "\n...";
  String edited = editOnDeviceNumber("Wi-Fi network list", input, 60, false, false, nullptr, nullptr);
  if (edited.endsWith("...")) {
    edited.remove(edited.length() - 3);
  }
  String nextLine;
  int lineNum = 1;
  for (int i = 0; i < edited.length(); ++i) {
    if (edited[i] == '\n') {
      if (lineNum <= kMaxNetworkEntries) {
        if (lineNum == 1) savedSsids[0] = edited.substring(0, i);
        else if (lineNum == 2) savedSsids[1] = edited.substring(0, i);
        else if (lineNum == 3) savedSsids[2] = edited.substring(0, i);
      }
      lineNum++;
      if (lineNum > kMaxNetworkEntries) break;
    }
  }
  savedNetworkCount = min<size_t>(savedNetworkCount, kMaxNetworkEntries);
  if (savedNetworkCount < kPrimaryNetworks) savedNetworkCount = kPrimaryNetworks;
  assignNetworkSelection();
  persistSavedNetworks();
}

void persistSavedNetworks() {
  preferences.putUInt("net_count", savedNetworkCount);
  for (size_t i = 0; i < savedNetworkCount && i < kMaximumNetworks; ++i) {
    preferences.putString((String("ssid") + String(i + 1)).c_str(), savedSsids[i]);
    preferences.putString((String("password") + String(i + 1)).c_str(), savedPasswords[i]);
    preferences.putString((String("loc") + String(i + 1)).c_str(), savedNetworkLocations[i]);
  }
  preferences.putBool("night_icons", useNightIcons);
  preferences.putBool("night_shutdown", nightShutdownEnabled);
  preferences.putBool("nic_saved", nightIconChoiceSaved);
  preferences.putBool("nis_saved", nightIconSetChoiceSaved);
  preferences.putUInt("night_icon_set", nightIconSetSelected);
}

void loop() {
  static bool settingsHoldHandled = false;
  static bool detailsHoldHandled = false;
  M5.update();
  const bool suppressBootCClick = consumeBootCClick();
  if (!weatherTestMode && !weatherApiChangedInSettings && nightShutdownIsActive() &&
      nightShutdownTimeConfirmed()) {
    enterNightShutdownSleep();
    return;
  }
  if (showWeekForecast && M5.Touch.getCount() > 0) {
    const auto& touch = M5.Touch.getDetail();
    if (touchTapped(touch)) {
      if (showDayDetails) {
        if (touch.y >= 854 && touch.y <= 908 && touch.x >= 92 &&
            touch.x <= M5.Display.width() - 92) {
          showDayDetails = false;
          drawWeekForecast();
        }
      } else if (!wifiUnavailable && forecast.valid) {
        size_t indices[kForecastDays] = {};
        const size_t availableCount = dailyForecastIndices(indices);
        const size_t count =
            min(availableCount, simpleMode ? size_t{3} : kForecastDays);
        constexpr int32_t rowStartY = 128;
        const int32_t rowHeight = simpleMode
            ? (M5.Display.height() - rowStartY - 60) /
                  (count == 0 ? 1 : count)
            : 105;
        if (touch.y >= rowStartY && touch.y < 874 && count > 0) {
          const size_t row =
              static_cast<size_t>((touch.y - rowStartY) / rowHeight);
          if (row < count) {
            selectedForecastIndex = indices[row];
            showDayDetails = true;
            drawWeekForecast();
          }
        }
      }
    }
  }
  if (M5.BtnA.isPressed() && M5.BtnA.pressedFor(1200) &&
      !settingsHoldHandled) {
    settingsHoldHandled = true;
    showDeviceSettings();
  }
  if (M5.BtnC.isPressed() && M5.BtnC.pressedFor(1000) &&
      !ignoreBootCClick && !detailsHoldHandled &&
      showWeekForecast && !wifiUnavailable) {
    detailsHoldHandled = true;
    size_t indices[kForecastDays] = {};
    const size_t count = min(dailyForecastIndices(indices),
                             simpleMode ? size_t{3} : kForecastDays);
    if (count > 0) {
      size_t currentRow = 0;
      while (currentRow < count &&
             indices[currentRow] != selectedForecastIndex) {
        ++currentRow;
      }
      selectedForecastIndex = indices[(currentRow + 1) % count];
      showDayDetails = true;
      drawWeekForecast();
    }
  }
  if (updateClockSync()) {
    if (!weatherApiChangedInSettings && nightShutdownIsActive() &&
        nightShutdownTimeConfirmed()) {
      enterNightShutdownSleep();
      return;
    }
    weatherApiChangedInSettings = false;
    checkForFirmwareUpdate();
    refreshWeather();
    if (!weatherTestMode) enterBatterySaverSleep();
  }
  if (!weatherTestMode && WiFi.status() != WL_CONNECTED && !wifiUnavailable) {
    showWeatherUnavailable("Wi-Fi unavailable. Press C to retry, A for setup.");
    lastWifiRetryMs = millis();
    wifiRetryCount = 0;
  }
  if (!weatherTestMode && wifiUnavailable &&
      millis() - lastWifiRetryMs >= kWifiRetryIntervalMs) {
    lastWifiRetryMs = millis();
    if (connectToSavedWifi()) {
      wifiRetryCount = 0;
      refreshWeather();
      enterBatterySaverSleep();
    } else {
      ++wifiRetryCount;
      showWeatherUnavailable(
          "Wi-Fi unavailable. C: retry now. Retrying every 30 seconds.");
      lastWifiRetryMs = millis();
      if (batterySaver && wifiRetryCount >= 2) {
        wifiRetryCount = 0;
        enterWifiRetrySleep();
      }
    }
  }
  if (M5.BtnA.wasClicked()) {
    if (settingsHoldHandled) {
      settingsHoldHandled = false;
    } else {
      startSetupPortal();
      if (!hasSavedWifi()) {
        drawMessage("Setup required",
                    "Save Wi-Fi settings, then restart.");
        return;
      }
      if (connectToSavedWifi()) {
        refreshWeather();
        if (!hasLocationConfiguration()) {
          clockUpdatesEnabled = false;
          drawLocationNeededScreen();
          return;
        }
        enterBatterySaverSleep();
      } else {
        showWeatherUnavailable("Wi-Fi unavailable. Press C to retry, A for setup.");
        lastWifiRetryMs = millis();
      }
    }
  }
  if (M5.BtnA.wasReleased()) settingsHoldHandled = false;
  if (M5.BtnB.wasClicked()) {
    refreshWeather();
    enterBatterySaverSleep();
  }
  if (!suppressBootCClick && M5.BtnC.wasClicked()) {
    if (detailsHoldHandled) {
      detailsHoldHandled = false;
    } else if (wifiUnavailable) {
      if (connectToSavedWifi()) {
        wifiUnavailable = false;
        wifiRetryCount = 0;
        refreshWeather();
        enterBatterySaverSleep();
      } else {
        showWeatherUnavailable(
            "Wi-Fi unavailable. Press C to retry, A for setup.");
        lastWifiRetryMs = millis();
      }
    } else {
      if (showDayDetails) {
        showDayDetails = false;
      } else {
        showWeekForecast = !showWeekForecast;
        preferences.putBool("week_view", showWeekForecast);
      }
    }
    if (showWeekForecast) {
      drawWeekForecast();
    } else {
      drawForecast();
    }
  }
  if (M5.BtnC.wasReleased()) detailsHoldHandled = false;
  if (clockReady && clockUpdatesEnabled && !weatherTestMode &&
      !wifiUnavailable && forecast.valid && millis() - lastClockCheckMs >= 1000) {
    lastClockCheckMs = millis();
    if (!showWeekForecast && !showDayDetails) drawClock();
  }
  if (!weatherTestMode &&
      millis() - lastSensorReadMs >= kSensorRefreshIntervalMs) {
    const bool sensorRead = readLocalEnvironment();
    lastSensorReadMs = millis();
    if (sensorRead && forecast.valid && !showWeekForecast && !simpleMode) {
      drawLocalEnvironmentLine();
    }
  }
  if (!weatherTestMode && scheduledRefreshIsDue()) {
    refreshWeather(true);
    enterBatterySaverSleep();
  }
  delay(10);
}
