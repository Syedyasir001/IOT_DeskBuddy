/*
  DESK BUDDY v15 - two-button control
  NAV button (wire A GPIO18 + wire B GPIO15, or BOOT GPIO0)
    Home : tap = next screen | double tap = open menu | hold = Eyes: pet
           Timer: start/stop focus | other screens: previous screen
    Menu : tap = move cursor | hold = select | double tap = close menu
  ACT button (push button GPIO19 to GND)
    Message showing: tap = dismiss message
    Alarm ringing  : tap = dismiss | double tap = snooze 5 min
    Home : tap = screen action | double tap = open menu | hold = screen action 2
    Menu : tap = select | hold = select | double tap = close menu
  OLED   : 1.3" blue I2C SH1106 (address 0x3C, try 0x3D if blank)
           SDA = GPIO21, SCL = GPIO22
  Buzzer : Casio piezo, + -> GPIO23, - -> GND
  Libraries: "Adafruit SH110X", "Adafruit GFX Library", "Adafruit BusIO"
*/

#define OLED_ADDR 0x3C         // try 0x3D if the screen stays blank

#include <stdarg.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#ifndef WHITE
  #define WHITE 1
#endif
#ifndef BLACK
  #define BLACK 0
#endif

Adafruit_SH1106G display(128, 64, &Wire, -1);
#define OLED_CMD_ON  SH110X_DISPLAYON
#define OLED_CMD_OFF SH110X_DISPLAYOFF

// ===================== CONFIG =====================
const char* STA_SSID = "butter";      // phone hotspot name (2.4 GHz)
const char* STA_PASS = "11223344";    // hotspot password
const char* AP_SSID  = "DeskBuddy";
const char* AP_PASS  = "buddy1234";   // min 8 characters
const char* TZ_INFO  = "IST-5:30";
const char* CITY     = "BENGALURU";
const char* LAT      = "12.9716";
const char* LON      = "77.5946";

const int   PIN_SENSE  = 18;          // NAV wire A
const int   PIN_DRIVE  = 15;          // NAV wire B (held LOW)
const int   PIN_BOOT   = 0;           // BOOT button (NAV backup)
const int   PIN_ACT    = 19;          // ACT push button
const int   PIN_BUZZER = 23;          // Casio piezo
const bool  SOUND_ON     = true;
const bool  CLICK_ON_TAP = true;
const bool  DEBUG_INPUT   = true;      // print every press and gap to Serial

const unsigned long DEBOUNCE_MS          = 30UL;
const unsigned long DOUBLE_GAP_MS        = 250UL;
const unsigned long LONGPRESS_MS         = 600UL;
const unsigned long REACT_MS             = 3000UL;
const unsigned long DISPLAY_OFF_AFTER_MS = 600000UL;
const unsigned long MENU_TIMEOUT_MS      = 25000UL;
const int NIGHT_START_HOUR = 23;
const int NIGHT_END_HOUR   = 6;
const int LONG_BREAK_MIN   = 15;
const int MENU_ROWS        = 5;
const int QUICK_MIN        = 5;
const int SNOOZE_MIN       = 5;
// ==================================================

#define NOTE_C5  523
#define NOTE_E5  659
#define NOTE_G5  784
#define NOTE_A5  880
#define NOTE_C6  1047

enum Mood { M_NORMAL, M_HAPPY, M_SLEEPY, M_ANGRY, M_SURPRISED, M_LOVE };
const char* MOOD_NAMES[] = {"normal", "happy", "sleepy", "angry", "surprised", "love"};
enum TimerState { T_IDLE, T_FOCUS, T_BREAK };
enum UiMode { UI_HOME, UI_MENU };
enum Screen { SCR_CLOCK, SCR_WEATHER, SCR_TIMER, SCR_TASKS, SCR_EVENT, SCR_NOTE, SCR_STATS, SCR_EYES, SCR_COUNT };
enum Sound { SND_NONE, SND_ALARM, SND_FOCUS_DONE, SND_BREAK_OVER, SND_NUDGE, SND_CLICK, SND_STARTUP, SND_TASK_DONE };
enum GestureEv { EV_NONE, EV_TAP, EV_DOUBLE, EV_LONG };

const int MI_FOCUS    = SCR_COUNT;
const int MI_TASKDONE = SCR_COUNT + 1;
const int MI_QUICK    = SCR_COUNT + 2;
const int MI_HOME     = SCR_COUNT + 3;
const int MENU_COUNT  = SCR_COUNT + 4;
const char* MENU_LABELS[MENU_COUNT] = {
  "Clock", "Weather", "Focus timer", "Tasks", "Countdown", "Pinned note", "Focus stats", "Eyes",
  "Start/stop focus", "Done: top task", "Quick 5 min timer", "Back to clock"
};

struct Gesture {
  int pin;
  const char* name;
  bool stable;
  bool rawPrev;
  unsigned long changeAt;
  unsigned long downAt;
  bool longFired;
  int taps;
  unsigned long lastUpAt;
};
struct AlarmItem { uint8_t h; uint8_t m; bool daily; char label[22]; int lastKey; };
struct QuickItem { bool used; unsigned long at; char label[22]; };

Gesture gWire = {PIN_SENSE, "WIRE", false, false, 0, 0, false, 0, 0};
Gesture gBoot = {PIN_BOOT,  "BOOT", false, false, 0, 0, false, 0, 0};
Gesture gAct  = {PIN_ACT,   "ACT",  false, false, 0, 0, false, 0, 0};

// ---------------- globals ----------------
WebServer server(80);
Preferences prefs;
bool oledOK = false;
bool displayOn = true;
bool netStarted = false;
bool nightCached = false;

UiMode uiMode = UI_HOME;
int homeIdx = SCR_CLOCK;
int menuCursor = 0;
int menuTop = 0;
int taskSel = 0;

Mood overrideMood = M_NORMAL;  unsigned long overrideUntil = 0;
Mood reactMood = M_NORMAL;     unsigned long reactUntil = 0;
unsigned long happyUntil = 0, nextHappyAt = 0, lastInteraction = 0;
float lookX = 0, lookY = 0, tgtX = 0, tgtY = 0;
unsigned long nextLookAt = 0, nextBlinkAt = 0, blinkStart = 0;
bool blinking = false;

char msgBuf[101] = "";
const char* msgTitle = "NEW MESSAGE";
unsigned long msgUntil = 0;
String lastMsg = "";

bool alerting = false;
bool invState = false;
unsigned long alertUntil = 0;
String alertText = "";

TimerState tState = T_IDLE;
unsigned long tEnd = 0, tTotal = 0;
int focusMin = 25, breakMin = 5, cycleCount = 0;
bool nextBreakLong = false;
uint32_t totalSessions = 0, totalMinutes = 0;
String logStr = "";
int cachedToday = 0;
int goalSessions = 4;

const int MAX_ALARMS = 5;
AlarmItem alarms[MAX_ALARMS];
int alarmN = 0;
QuickItem quickTimers[3];

const int MAX_TASKS = 8;
String tasks[MAX_TASKS];
int taskN = 0;
uint32_t tasksDone = 0;
char evName[21] = "";
char evDate[11] = "";
String note = "";

int waterMin = 0, stretchMin = 0;
unsigned long nextWaterAt = 0, nextStretchAt = 0;

volatile float wTemp = NAN, wHum = NAN, wAqi = NAN;
volatile int wCode = -1;
volatile unsigned long wUpdated = 0;

// ---------------- debug ----------------
void dbg(const char* fmt, ...) {
  if (!DEBUG_INPUT) return;
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

// ---------------- display helpers ----------------
bool initOled() { return display.begin(OLED_ADDR, true); }

void setDisplay(bool on) {
  if (on == displayOn) return;
  displayOn = on;
  if (oledOK) display.oled_command(on ? OLED_CMD_ON : OLED_CMD_OFF);
}

// ---------------- sound ----------------
struct ToneStep { uint16_t f; uint16_t ms; };

const ToneStep TUNE_ALARM[] = {
  {NOTE_C5,120},{0,60},{NOTE_E5,120},{0,60},{NOTE_G5,220},{0,300},
  {NOTE_C5,120},{0,60},{NOTE_E5,120},{0,60},{NOTE_G5,220},{0,0}
};
const ToneStep TUNE_FOCUS_DONE[] = {{NOTE_C5,90},{NOTE_E5,90},{NOTE_G5,90},{NOTE_C6,260},{0,0}};
const ToneStep TUNE_BREAK_OVER[] = {{NOTE_G5,180},{0,40},{NOTE_C5,320},{0,0}};
const ToneStep TUNE_NUDGE[]      = {{NOTE_A5,80},{0,70},{NOTE_A5,80},{0,0}};
const ToneStep TUNE_CLICK[]      = {{NOTE_C6,18},{0,0}};
const ToneStep TUNE_STARTUP[]    = {{NOTE_C5,90},{NOTE_G5,90},{NOTE_C6,200},{0,0}};
const ToneStep TUNE_TASK_DONE[]  = {{NOTE_E5,70},{NOTE_G5,70},{NOTE_C6,140},{0,0}};

const ToneStep* curTune = nullptr;
int tuneIdx = 0;
unsigned long toneUntil = 0;
bool tunePlaying = false;
bool tuneLoops = false;

void startSound(Sound s) {
  if (!SOUND_ON) return;
  switch (s) {
    case SND_ALARM:      curTune = TUNE_ALARM;      tuneLoops = true;  break;
    case SND_FOCUS_DONE: curTune = TUNE_FOCUS_DONE; tuneLoops = false; break;
    case SND_BREAK_OVER: curTune = TUNE_BREAK_OVER; tuneLoops = false; break;
    case SND_NUDGE:      curTune = TUNE_NUDGE;      tuneLoops = false; break;
    case SND_CLICK:      curTune = TUNE_CLICK;      tuneLoops = false; break;
    case SND_STARTUP:    curTune = TUNE_STARTUP;    tuneLoops = false; break;
    case SND_TASK_DONE:  curTune = TUNE_TASK_DONE;  tuneLoops = false; break;
    default: return;
  }
  tuneIdx = 0;
  tunePlaying = true;
  toneUntil = 0;
}

void stopSound() {
  tunePlaying = false;
  curTune = nullptr;
  noTone(PIN_BUZZER);
}

void soundUpdate(unsigned long now) {
  if (!tunePlaying || !curTune) return;
  if ((long)(now - toneUntil) < 0) return;
  const ToneStep& t = curTune[tuneIdx];
  if (t.f == 0 && t.ms == 0) {
    if (tuneLoops) { tuneIdx = 0; toneUntil = now + 600; noTone(PIN_BUZZER); }
    else stopSound();
    return;
  }
  if (t.f) tone(PIN_BUZZER, t.f); else noTone(PIN_BUZZER);
  toneUntil = now + t.ms;
  tuneIdx++;
}

void click() { if (CLICK_ON_TAP) startSound(SND_CLICK); }

// ---------------- helpers ----------------
bool timeSynced() { return time(nullptr) > 1700000000; }

String cleanText(const String& in, unsigned int maxLen) {
  String o;
  for (unsigned int i = 0; i < in.length() && o.length() < maxLen; i++) {
    char c = in[i];
    if (c == '"') c = '\'';
    else if (c == '\\' || c == '|') c = '/';
    else if (c < 32 || c > 126) c = '?';
    o += c;
  }
  o.trim();
  return o;
}

bool nightNow() {
  if (!timeSynced()) return false;
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;
  if (NIGHT_START_HOUR > NIGHT_END_HOUR) return (t.tm_hour >= NIGHT_START_HOUR || t.tm_hour < NIGHT_END_HOUR);
  return (t.tm_hour >= NIGHT_START_HOUR && t.tm_hour < NIGHT_END_HOUR);
}

void poke() { lastInteraction = millis(); }

void showMsgT(const String& t, unsigned long ms, const char* title) {
  String c = cleanText(t, 100);
  strncpy(msgBuf, c.c_str(), 100);
  msgBuf[100] = 0;
  msgTitle = title;
  lastMsg = c;
  msgUntil = millis() + ms;
  happyUntil = msgUntil + 2500;
}

void showMsg(const String& t, unsigned long ms) { showMsgT(t, ms, "NEW MESSAGE"); }

bool msgActive() { return (long)(msgUntil - millis()) > 0; }

void dismissMsg() { msgUntil = 0; }

bool wantDisplay(unsigned long now) {
  if (alerting || msgActive()) return true;
  unsigned long idle = now - lastInteraction;
  if (nightCached) return idle < 30000UL;
  if (tState != T_IDLE) return true;
  return idle < DISPLAY_OFF_AFTER_MS;
}

// ---------------- alerts ----------------
void startAlert(const String& text) {
  alerting = true;
  alertUntil = millis() + 30000UL;
  alertText = cleanText(text, 60);
  startSound(SND_ALARM);
}

void stopAlert() {
  alerting = false;
  if (invState) {
    invState = false;
    if (oledOK) display.invertDisplay(false);
  }
  if (curTune == TUNE_ALARM) stopSound();
}

// ---------------- alarms + quick timers ----------------
void saveAlarms() {
  String s;
  for (int i = 0; i < alarmN; i++)
    s += String(alarms[i].h) + "|" + String(alarms[i].m) + "|" + String(alarms[i].daily ? 1 : 0) + "|" + alarms[i].label + "\n";
  prefs.putString("alm", s);
}

void loadAlarms() {
  alarmN = 0;
  String s = prefs.getString("alm", "");
  int start = 0;
  while (start < (int)s.length() && alarmN < MAX_ALARMS) {
    int nl = s.indexOf('\n', start);
    if (nl < 0) break;
    String line = s.substring(start, nl);
    start = nl + 1;
    int p1 = line.indexOf('|');
    int p2 = line.indexOf('|', p1 + 1);
    int p3 = line.indexOf('|', p2 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0) continue;
    AlarmItem& a = alarms[alarmN];
    a.h = line.substring(0, p1).toInt();
    a.m = line.substring(p1 + 1, p2).toInt();
    a.daily = line.substring(p2 + 1, p3).toInt() == 1;
    String lb = line.substring(p3 + 1);
    strncpy(a.label, lb.c_str(), 21);
    a.label[21] = 0;
    a.lastKey = -1;
    alarmN++;
  }
}

void deleteAlarm(int i) {
  if (i < 0 || i >= alarmN) return;
  for (int j = i; j < alarmN - 1; j++) alarms[j] = alarms[j + 1];
  alarmN--;
  saveAlarms();
}

void checkAlarms() {
  if (alarmN == 0 || !timeSynced()) return;
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  int key = t.tm_year * 400 + t.tm_yday;
  for (int i = 0; i < alarmN; i++) {
    if (alarms[i].h == t.tm_hour && alarms[i].m == t.tm_min && alarms[i].lastKey != key) {
      alarms[i].lastKey = key;
      startAlert(alarms[i].label[0] ? String(alarms[i].label) : String("Reminder"));
      if (!alarms[i].daily) { deleteAlarm(i); i--; }
    }
  }
}

int minutesToNextAlarm() {
  if (alarmN == 0 || !timeSynced()) return -1;
  struct tm t;
  if (!getLocalTime(&t, 0)) return -1;
  int nowMin = t.tm_hour * 60 + t.tm_min;
  int best = -1;
  for (int i = 0; i < alarmN; i++) {
    int diff = (alarms[i].h * 60 + alarms[i].m) - nowMin;
    if (diff <= 0) diff += 1440;
    if (best < 0 || diff < best) best = diff;
  }
  return best;
}

bool addQuick(int minutes, const String& label) {
  for (int i = 0; i < 3; i++) {
    if (!quickTimers[i].used) {
      quickTimers[i].used = true;
      quickTimers[i].at = millis() + (unsigned long)minutes * 60000UL;
      String l = cleanText(label, 21);
      strncpy(quickTimers[i].label, l.c_str(), 21);
      quickTimers[i].label[21] = 0;
      return true;
    }
  }
  return false;
}

void checkQuick() {
  unsigned long now = millis();
  for (int i = 0; i < 3; i++) {
    if (quickTimers[i].used && (long)(now - quickTimers[i].at) >= 0) {
      quickTimers[i].used = false;
      startAlert(quickTimers[i].label[0] ? String(quickTimers[i].label) : String("Timer done"));
    }
  }
}

// ---------------- tasks ----------------
void saveTasks() {
  String s;
  for (int i = 0; i < taskN; i++) s += tasks[i] + "\n";
  prefs.putString("tsk", s);
  prefs.putUInt("tdn", tasksDone);
}

void loadTasks() {
  taskN = 0;
  String s = prefs.getString("tsk", "");
  tasksDone = prefs.getUInt("tdn", 0);
  int start = 0;
  while (start < (int)s.length() && taskN < MAX_TASKS) {
    int nl = s.indexOf('\n', start);
    if (nl < 0) break;
    String line = s.substring(start, nl);
    start = nl + 1;
    if (line.length()) tasks[taskN++] = line;
  }
}

bool addTask(const String& t) {
  String c = cleanText(t, 28);
  if (c.length() == 0 || taskN >= MAX_TASKS) return false;
  tasks[taskN++] = c;
  saveTasks();
  return true;
}

void removeTask(int i) {
  if (i < 0 || i >= taskN) return;
  for (int j = i; j < taskN - 1; j++) tasks[j] = tasks[j + 1];
  taskN--;
  if (taskSel >= taskN) taskSel = taskN > 0 ? taskN - 1 : 0;
}

void completeTask(int i) {
  if (i < 0 || i >= taskN) return;
  removeTask(i);
  tasksDone++;
  saveTasks();
  startSound(SND_TASK_DONE);
  reactMood = M_LOVE;
  reactUntil = millis() + 2500;
}

void deleteTask(int i) {
  if (i < 0 || i >= taskN) return;
  removeTask(i);
  saveTasks();
}

// ---------------- countdown ----------------
void saveEvent() {
  prefs.putString("evn", evName);
  prefs.putString("evd", evDate);
}

int daysUntilEvent(bool& ok) {
  ok = false;
  if (!evDate[0] || !timeSynced()) return 0;
  int y, m, d;
  if (sscanf(evDate, "%d-%d-%d", &y, &m, &d) != 3) return 0;
  struct tm e;
  memset(&e, 0, sizeof e);
  e.tm_year = y - 1900;
  e.tm_mon = m - 1;
  e.tm_mday = d;
  e.tm_isdst = -1;
  time_t et = mktime(&e);
  time_t n = time(nullptr);
  struct tm t;
  localtime_r(&n, &t);
  t.tm_hour = 0;
  t.tm_min = 0;
  t.tm_sec = 0;
  t.tm_isdst = -1;
  time_t tt = mktime(&t);
  ok = true;
  return (int)floor(difftime(et, tt) / 86400.0 + 0.5);
}

// ---------------- pomodoro ----------------
int todayCount() {
  if (!timeSynced()) return 0;
  time_t n = time(nullptr);
  struct tm now;
  localtime_r(&n, &now);
  int cnt = 0, start = 0;
  while (start < (int)logStr.length()) {
    int nl = logStr.indexOf('\n', start);
    if (nl < 0) break;
    uint32_t ep = strtoul(logStr.substring(start, nl).c_str(), NULL, 10);
    if (ep > 1700000000UL) {
      time_t t = ep;
      struct tm tt;
      localtime_r(&t, &tt);
      if (tt.tm_yday == now.tm_yday && tt.tm_year == now.tm_year) cnt++;
    }
    start = nl + 1;
  }
  return cnt;
}

void logSession() {
  totalSessions++;
  totalMinutes += focusMin;
  uint32_t ep = timeSynced() ? (uint32_t)time(nullptr) : 0;
  logStr += String(ep) + "," + String(focusMin) + "\n";
  int lines = 0;
  for (unsigned int i = 0; i < logStr.length(); i++) if (logStr[i] == '\n') lines++;
  while (lines > 30) {
    int nl = logStr.indexOf('\n');
    logStr.remove(0, nl + 1);
    lines--;
  }
  prefs.putUInt("ses", totalSessions);
  prefs.putUInt("min", totalMinutes);
  prefs.putString("log", logStr);
  cachedToday = todayCount();
}

void startTimer(TimerState s) {
  bool lb = (s == T_BREAK) && nextBreakLong;
  nextBreakLong = false;
  int mins = (s == T_FOCUS) ? focusMin : (lb ? LONG_BREAK_MIN : breakMin);
  tState = s;
  tTotal = (unsigned long)mins * 60000UL;
  tEnd = millis() + tTotal;
}

void stopTimer() { tState = T_IDLE; }

void toggleFocus() {
  if (tState == T_IDLE) startTimer(T_FOCUS); else stopTimer();
}

void timerTick() {
  if (tState == T_IDLE) return;
  if ((long)(millis() - tEnd) < 0) return;
  if (tState == T_FOCUS) {
    logSession();
    cycleCount++;
    bool goalHit = goalSessions > 0 && cachedToday == goalSessions;
    nextBreakLong = (cycleCount % 4 == 0);
    const char* txt = goalHit ? "Daily goal reached! Awesome work." :
                      (nextBreakLong ? "Session done! Take a long break." : "Great job! Session logged.");
    showMsgT(txt, 7000, "FOCUS DONE");
    reactMood = M_LOVE;
    reactUntil = millis() + 6000;
    startSound(SND_FOCUS_DONE);
    startTimer(T_BREAK);
  } else {
    tState = T_IDLE;
    showMsgT("Break over! Ready for another round?", 6000, "BREAK OVER");
    startSound(SND_BREAK_OVER);
  }
}

// ---------------- menu ----------------
void keepCursorVisible() {
  if (menuCursor < menuTop) menuTop = menuCursor;
  if (menuCursor >= menuTop + MENU_ROWS) menuTop = menuCursor - MENU_ROWS + 1;
}

void openMenu() {
  dismissMsg();
  uiMode = UI_MENU;
  menuCursor = homeIdx;
  menuTop = 0;
  keepCursorVisible();
  poke();
  startSound(SND_NUDGE);
}

void goHome() { uiMode = UI_HOME; }

void petBuddy() {
  reactMood = M_LOVE;
  reactUntil = millis() + REACT_MS;
  startSound(SND_TASK_DONE);
}

void startQuickTimer() {
  if (addQuick(QUICK_MIN, "Quick break")) {
    showMsgT(String(QUICK_MIN) + " min timer started", 2000, "TIMER");
    startSound(SND_NUDGE);
  } else {
    showMsgT("All 3 timers in use", 2000, "TIMER");
  }
}

void selectMenu() {
  if (menuCursor < SCR_COUNT) {
    homeIdx = menuCursor;
    goHome();
    startSound(SND_NUDGE);
  } else if (menuCursor == MI_FOCUS) {
    toggleFocus();
    goHome();
    startSound(SND_NUDGE);
  } else if (menuCursor == MI_TASKDONE) {
    if (taskN > 0) completeTask(taskSel);
    else showMsgT("No tasks to finish", 2500, "TASKS");
    goHome();
  } else if (menuCursor == MI_QUICK) {
    goHome();
    startQuickTimer();
  } else if (menuCursor == MI_HOME) {
    homeIdx = SCR_CLOCK;
    goHome();
  }
}

// ---------------- NAV button (wires + BOOT) ----------------
void handleNav(GestureEv ev) {
  poke();

  if (alerting) { stopAlert(); click(); return; }

  if (ev == EV_DOUBLE) {
    if (uiMode == UI_MENU) goHome();
    else openMenu();
    return;
  }

  if (uiMode == UI_HOME) {
    // NAV keeps working over a message; it simply clears the message too.
    if (ev == EV_TAP) {
      dismissMsg();
      homeIdx = (homeIdx + 1) % SCR_COUNT;
      click();
    } else if (ev == EV_LONG) {
      dismissMsg();
      if (homeIdx == SCR_EYES)       petBuddy();
      else if (homeIdx == SCR_TIMER) { toggleFocus(); startSound(SND_NUDGE); }
      else {
        homeIdx = (homeIdx + SCR_COUNT - 1) % SCR_COUNT;
        click();
      }
    }
  } else {
    if (ev == EV_TAP) {
      menuCursor = (menuCursor + 1) % MENU_COUNT;
      keepCursorVisible();
      click();
    } else if (ev == EV_LONG) {
      selectMenu();
    }
  }
}

// ---------------- ACT button (push button GPIO19) ----------------
void actTap() {
  switch (homeIdx) {
    case SCR_CLOCK: {
      int m = minutesToNextAlarm();
      if (m < 0) showMsgT("No alarm set. Add one in the app.", 3000, "NEXT ALARM");
      else showMsgT("Next alarm in " + String(m / 60) + "h " + String(m % 60) + "m", 4000, "NEXT ALARM");
      break;
    }
    case SCR_WEATHER: {
      if (wUpdated == 0) { showMsgT("No weather yet. Check WiFi.", 3000, "WEATHER"); break; }
      String s = String((int)roundf(wTemp)) + " C, humidity " +
                 (isnan(wHum) ? String("--") : String((int)wHum)) + "%, AQI " +
                 (isnan(wAqi) ? String("--") : String((int)wAqi)) + " " + String(aqiName(wAqi));
      showMsgT(s, 6000, "WEATHER");
      break;
    }
    case SCR_TIMER:
      toggleFocus();
      startSound(SND_NUDGE);
      break;
    case SCR_TASKS:
      if (taskN > 0) { taskSel = (taskSel + 1) % taskN; click(); }
      else showMsgT("No tasks. Add some in the app.", 3000, "TASKS");
      break;
    case SCR_EVENT: {
      bool ok = false;
      int days = daysUntilEvent(ok);
      if (!ok || !evName[0]) showMsgT("No countdown set yet", 3000, "COUNTDOWN");
      else if (days > 0) showMsgT(String(evName) + ": " + String(days) + " days left", 5000, "COUNTDOWN");
      else if (days == 0) showMsgT(String(evName) + " is TODAY!", 5000, "COUNTDOWN");
      else showMsgT(String(evName) + " has passed", 5000, "COUNTDOWN");
      break;
    }
    case SCR_NOTE:
      if (note.length()) showMsgT(note, 6000, "PINNED NOTE");
      else showMsgT("No pinned note", 2500, "PINNED NOTE");
      break;
    case SCR_STATS:
      showMsgT("Today " + String(cachedToday) + "/" + String(goalSessions) +
               " sessions. Total " + String((int)totalSessions) + ".", 5000, "FOCUS STATS");
      break;
    case SCR_EYES:
      petBuddy();
      break;
    default:
      click();
      break;
  }
}

void actHold() {
  switch (homeIdx) {
    case SCR_CLOCK:
      startQuickTimer();
      break;
    case SCR_WEATHER:
      showMsgT("Weather refreshes every 15 min", 3000, "WEATHER");
      break;
    case SCR_TIMER:
      if (tState == T_BREAK) {
        tState = T_IDLE;
        showMsgT("Break skipped", 2000, "TIMER");
        startSound(SND_NUDGE);
      } else if (tState == T_FOCUS) {
        stopTimer();
        showMsgT("Focus cancelled", 2000, "TIMER");
        startSound(SND_NUDGE);
      } else {
        showMsgT("Tap to start focus", 2000, "TIMER");
      }
      break;
    case SCR_TASKS:
      if (taskN > 0) completeTask(taskSel);
      else showMsgT("No tasks to finish", 2000, "TASKS");
      break;
    case SCR_EVENT:
      showMsgT("Set the date in the app", 3000, "COUNTDOWN");
      break;
    case SCR_NOTE:
      showMsgT("Edit the note in the app", 3000, "PINNED NOTE");
      break;
    case SCR_STATS: {
      int left = goalSessions - cachedToday;
      if (goalSessions <= 0) showMsgT("No daily goal set", 2500, "GOAL");
      else if (left > 0) showMsgT(String(left) + " more session(s) to hit your goal", 4000, "GOAL");
      else showMsgT("Goal reached today!", 3000, "GOAL");
      break;
    }
    case SCR_EYES:
      overrideMood = (Mood)random(1, 6);
      overrideUntil = millis() + 30000UL;
      startSound(SND_NUDGE);
      break;
    default:
      break;
  }
}

void handleAct(GestureEv ev) {
  poke();

  // Alarm ringing: tap dismisses, double tap snoozes.
  if (alerting) {
    if (ev == EV_DOUBLE) {
      addQuick(SNOOZE_MIN, alertText);
      showMsgT("Snoozed " + String(SNOOZE_MIN) + " min", 2000, "SNOOZE");
    }
    stopAlert();
    click();
    return;
  }

  // Message showing: a single tap dismisses it right away.
  if (msgActive() && ev == EV_TAP) {
    dismissMsg();
    click();
    return;
  }

  // Double tap always toggles the menu.
  if (ev == EV_DOUBLE) {
    if (uiMode == UI_MENU) goHome();
    else openMenu();
    return;
  }

  if (uiMode == UI_MENU) {
    if (ev == EV_TAP || ev == EV_LONG) selectMenu();
    return;
  }

  // Hold while a message is showing also dismisses it, then runs the hold action.
  dismissMsg();
  if (ev == EV_TAP) actTap();
  else if (ev == EV_LONG) actHold();
}

// ---------------- gesture detector (shared by all buttons) ----------------
GestureEv gestureUpdate(Gesture& g, unsigned long now) {
  bool raw = (digitalRead(g.pin) == LOW);
  if (raw != g.rawPrev) { g.rawPrev = raw; g.changeAt = now; }

  if (raw != g.stable && now - g.changeAt >= DEBOUNCE_MS) {
    g.stable = raw;
    if (g.stable) {                              // press down
      g.downAt = now;
      g.longFired = false;
      if (g.taps == 1 && now - g.lastUpAt > DOUBLE_GAP_MS) {
        dbg("%s: gap %lu ms too long, single tap", g.name, now - g.lastUpAt);
        g.taps = 0;
        return EV_TAP;
      }
    } else {                                     // release
      if (g.longFired) {
        g.taps = 0;
      } else {
        g.taps++;
        if (g.taps == 1) {
          g.lastUpAt = now;
          dbg("%s: release 1", g.name);
        } else {
          dbg("%s: release 2, gap %lu ms -> DOUBLE", g.name, now - g.lastUpAt);
          g.taps = 0;
          return EV_DOUBLE;
        }
      }
    }
  }

  if (g.stable && !g.longFired && now - g.downAt >= LONGPRESS_MS) {
    g.longFired = true;
    g.taps = 0;
    return EV_LONG;
  }

  if (!g.stable && g.taps == 1 && now - g.lastUpAt > DOUBLE_GAP_MS) {
    g.taps = 0;
    return EV_TAP;
  }
  return EV_NONE;
}
// ---------------- weather ----------------
float jsonNum(const String& s, const char* key, int from) {
  String k = String("\"") + key + "\":";
  int i = s.indexOf(k, from);
  if (i < 0) return NAN;
  i += k.length();
  if (s.startsWith("null", i)) return NAN;
  return s.substring(i, i + 12).toFloat();
}

bool httpsGet(const String& url, String& body) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code == 200) body = http.getString();
  http.end();
  return code == 200;
}

void weatherTask(void*) {
  for (;;) {
    bool ok = false;
    if (strlen(STA_SSID) && WiFi.status() == WL_CONNECTED) {
      String b;
      String u1 = String("https://api.open-meteo.com/v1/forecast?latitude=") + LAT + "&longitude=" + LON +
                  "&current=temperature_2m,relative_humidity_2m,weather_code&timezone=auto";
      if (httpsGet(u1, b)) {
        int c = b.indexOf("\"current\":");
        if (c >= 0) {
          float t = jsonNum(b, "temperature_2m", c);
          float h = jsonNum(b, "relative_humidity_2m", c);
          float w = jsonNum(b, "weather_code", c);
          if (!isnan(t)) {
            wTemp = t; wHum = h; wCode = isnan(w) ? -1 : (int)w;
            wUpdated = millis();
            ok = true;
          }
        }
      }
      b = "";
      String u2 = String("https://air-quality-api.open-meteo.com/v1/air-quality?latitude=") + LAT + "&longitude=" + LON +
                  "&current=us_aqi&timezone=auto";
      if (httpsGet(u2, b)) {
        int c = b.indexOf("\"current\":");
        if (c >= 0) wAqi = jsonNum(b, "us_aqi", c);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(ok ? 900000UL : 20000UL));
  }
}

const char* wxName(int c) {
  if (c < 0) return "--";
  if (c == 0) return "Clear";
  if (c <= 2) return "P.Cloudy";
  if (c == 3) return "Overcast";
  if (c == 45 || c == 48) return "Fog";
  if (c <= 57) return "Drizzle";
  if (c <= 67) return "Rain";
  if (c <= 77) return "Snow";
  if (c <= 82) return "Showers";
  if (c >= 95) return "Storm";
  return "Cloudy";
}

const char* aqiName(float a) {
  if (isnan(a)) return "--";
  if (a <= 50) return "Good";
  if (a <= 100) return "Moderate";
  if (a <= 150) return "Sensitive";
  if (a <= 200) return "Unhealthy";
  return "Hazardous";
}

// ---------------- mood ----------------
Mood currentMood() {
  unsigned long now = millis();
  if ((long)(overrideUntil - now) > 0) return overrideMood;
  if ((long)(reactUntil - now) > 0) return reactMood;
  if (now - lastInteraction > 90000UL) return M_SLEEPY;
  if ((long)(happyUntil - now) > 0) return M_HAPPY;
  return M_NORMAL;
}

// ---------------- drawing: eyes ----------------
void updateEyes(unsigned long now) {
  if (now >= nextLookAt) {
    tgtX = random(-8, 9);
    tgtY = random(-4, 5);
    nextLookAt = now + random(2500, 5500);
  }
  lookX += (tgtX - lookX) * 0.25f;
  lookY += (tgtY - lookY) * 0.25f;
  if (!blinking && now >= nextBlinkAt) { blinking = true; blinkStart = now; }
}

float eyeOpenness(unsigned long now) {
  if (!blinking) return 1.0f;
  unsigned long e = now - blinkStart;
  if (e < 80) return 1.0f - 0.9f * e / 80.0f;
  if (e < 180) return 0.1f + 0.9f * (e - 80) / 100.0f;
  blinking = false;
  nextBlinkAt = now + random(2500, 6000);
  return 1.0f;
}

void drawHeart(int cx, int cy, int r) {
  display.fillCircle(cx - r, cy - r / 2, r, WHITE);
  display.fillCircle(cx + r, cy - r / 2, r, WHITE);
  display.fillTriangle(cx - 2 * r + 1, cy - r / 2 + 3, cx + 2 * r - 1, cy - r / 2 + 3, cx, cy + 2 * r + 2, WHITE);
}

void drawEye(int cx, int cy, int w, int h, int r, float open, bool left, Mood m) {
  int hh = max(4, (int)(h * open));
  int x = cx - w / 2 + (int)lookX;
  int y = cy - hh / 2 + (int)lookY;
  display.fillRoundRect(x, y, w, hh, min(r, hh / 2), WHITE);
  if (open < 0.5f) return;
  switch (m) {
    case M_HAPPY:
      display.fillCircle(cx + (int)lookX, y + hh + 4, (int)(w * 0.62f), BLACK);
      break;
    case M_SLEEPY:
      display.fillRect(x - 1, y - 1, w + 2, hh * 55 / 100, BLACK);
      break;
    case M_ANGRY:
      if (left) display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x + w + 1, y + hh / 2, BLACK);
      else      display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x - 1, y + hh / 2, BLACK);
      break;
    case M_SURPRISED:
      display.fillCircle(cx + (int)lookX / 2, cy + (int)lookY / 2, 5, BLACK);
      break;
    default: break;
  }
}

void drawFace(Mood m) {
  unsigned long now = millis();
  updateEyes(now);
  float open = eyeOpenness(now);
  const int LCX = 38, RCX = 90, CY = 30;
  display.clearDisplay();
  switch (m) {
    case M_LOVE: {
      int r = 9 + (int)(2 * sinf(now / 130.0f));
      drawHeart(LCX, CY, r);
      drawHeart(RCX, CY, r);
      break;
    }
    case M_HAPPY:
      drawEye(LCX, CY, 34, 34, 12, open, true, m);
      drawEye(RCX, CY, 34, 34, 12, open, false, m);
      break;
    case M_SLEEPY:
      drawEye(LCX, CY, 34, 30, 10, open, true, m);
      drawEye(RCX, CY, 34, 30, 10, open, false, m);
      display.setTextColor(WHITE);
      display.setTextSize(2);
      display.setCursor(110, 0);
      display.print("z");
      break;
    case M_ANGRY:
      drawEye(LCX, CY, 34, 36, 8, open, true, m);
      drawEye(RCX, CY, 34, 36, 8, open, false, m);
      break;
    case M_SURPRISED:
      drawEye(LCX, CY, 30, 46, 14, open, true, m);
      drawEye(RCX, CY, 30, 46, 14, open, false, m);
      break;
    default:
      drawEye(LCX, CY, 34, 40, 10, open, true, m);
      drawEye(RCX, CY, 34, 40, 10, open, false, m);
      break;
  }
  display.display();
}

// ---------------- drawing: screens ----------------
String ipString() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
}

void drawWrapped(const String& s, int x, int y0, int perLine, int maxLines, int lineH) {
  int len = s.length();
  for (int l = 0; l < maxLines; l++) {
    int i = l * perLine;
    if (i >= len) break;
    display.setCursor(x, y0 + l * lineH);
    display.print(s.substring(i, min(i + perLine, len)));
  }
}

void drawClock() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("DESK BUDDY");
  if (timeSynced()) {
    struct tm t;
    getLocalTime(&t, 0);
    char b[8], d[24];
    strftime(b, sizeof b, "%H:%M", &t);
    strftime(d, sizeof d, "%a %d %b %Y", &t);
    display.setTextSize(3);
    display.setCursor(19, 14);
    display.print(b);
    display.setTextSize(1);
    display.setCursor(19, 42);
    display.print(d);
  } else {
    display.setTextSize(2);
    display.setCursor(19, 18);
    display.print("--:--");
    display.setTextSize(1);
    display.setCursor(4, 42);
    display.print("No clock: needs WiFi");
  }
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("NAV:next 2x:menu");
  display.display();
}

void drawMenu() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("MENU");
  display.setCursor(96, 0);
  display.print(menuCursor + 1);
  display.print("/");
  display.print(MENU_COUNT);
  display.drawFastHLine(0, 8, 128, WHITE);
  for (int r = 0; r < MENU_ROWS; r++) {
    int idx = menuTop + r;
    if (idx >= MENU_COUNT) break;
    int rowY = 10 + r * 11;
    bool sel = (idx == menuCursor);
    if (sel) display.fillRect(0, rowY, 128, 10, WHITE);
    display.setTextColor(sel ? BLACK : WHITE);
    display.setCursor(4, rowY + 1);
    display.print(MENU_LABELS[idx]);
  }
  display.setTextColor(WHITE);
  display.setCursor(0, 56);
  display.print("NAV:move ACT:pick");
  display.display();
}

void drawTimer() {
  long left = (long)(tEnd - millis());
  if (left < 0) left = 0;
  int s = left / 1000;
  char buf[8];
  snprintf(buf, sizeof buf, "%02d:%02d", s / 60, s % 60);
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(tState == T_FOCUS ? "FOCUS" : "BREAK");
  display.setCursor(56, 0);
  display.print("ACT:pause/go");
  display.setTextSize(3);
  display.setCursor(19, 16);
  display.print(buf);
  display.drawRect(4, 48, 120, 9, WHITE);
  int fill = tTotal ? (int)((tTotal - (unsigned long)left) * 116UL / tTotal) : 0;
  display.fillRect(6, 50, fill, 5, WHITE);
  display.display();
}

void drawTimerIdle() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("FOCUS TIMER");
  display.setCursor(0, 16);
  display.print("Focus: ");
  display.print(focusMin);
  display.print(" min");
  display.setCursor(0, 26);
  display.print("Break: ");
  display.print(breakMin);
  display.print(" min");
  display.setCursor(0, 44);
  display.print("ACT tap: start");
  display.setCursor(0, 56);
  display.print("NAV hold: start");
  display.display();
}

void drawWeather() {
  float t = wTemp, a = wAqi, h = wHum;
  int c = wCode;
  unsigned long up = wUpdated;
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(CITY);
  if (up == 0) {
    display.setCursor(0, 26);
    display.print(strlen(STA_SSID) ? "Fetching weather..." : "Offline: add WiFi");
    display.display();
    return;
  }
  display.setTextSize(3);
  display.setCursor(0, 12);
  display.print((int)roundf(t));
  display.write(248);
  display.print("C");
  display.setTextSize(1);
  display.setCursor(80, 14);
  display.print(wxName(c));
  display.setCursor(80, 26);
  display.print("Hum ");
  if (isnan(h)) display.print("--"); else display.print((int)h);
  display.print("%");
  display.setCursor(0, 42);
  display.print("AQI ");
  if (isnan(a)) display.print("--"); else display.print((int)a);
  display.print(" ");
  display.print(aqiName(a));
  display.setCursor(0, 56);
  display.print("updated ");
  display.print((millis() - up) / 60000UL);
  display.print("m ago");
  display.display();
}

void drawTasks() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("TASKS");
  if (taskN > 0) {
    display.setCursor(96, 0);
    display.print(taskSel + 1);
    display.print("/");
    display.print(taskN);
  }
  if (taskN == 0) {
    display.setCursor(0, 24);
    display.print("All done! Add more");
    display.setCursor(0, 34);
    display.print("from the dashboard.");
  } else {
    int top = taskSel >= 4 ? taskSel - 3 : 0;
    for (int r = 0; r < 4; r++) {
      int i = top + r;
      if (i >= taskN) break;
      bool sel = (i == taskSel);
      if (sel) display.fillRect(0, 11 + r * 10, 128, 10, WHITE);
      display.setTextColor(sel ? BLACK : WHITE);
      display.setCursor(3, 12 + r * 10);
      String t = tasks[i];
      if (t.length() > 20) t = t.substring(0, 20);
      display.print(t);
    }
    display.setTextColor(WHITE);
  }
  display.setCursor(0, 56);
  display.print("ACT:next hold:done");
  display.display();
}

void drawEvent() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("COUNTDOWN");
  display.setCursor(0, 12);
  display.print(evName[0] ? String(evName) : String("Event"));
  bool ok = false;
  int days = daysUntilEvent(ok);
  if (!ok) {
    display.setCursor(0, 34);
    display.print(timeSynced() ? "Set a date in the app" : "Needs WiFi for date");
  } else if (days > 0) {
    String n = String(days);
    display.setTextSize(3);
    display.setCursor(0, 28);
    display.print(n);
    display.setTextSize(1);
    display.setCursor((int)n.length() * 18 + 6, 42);
    display.print(days == 1 ? "day left" : "days left");
  } else if (days == 0) {
    display.setTextSize(3);
    display.setCursor(0, 28);
    display.print("TODAY!");
  } else {
    display.setTextSize(2);
    display.setCursor(0, 28);
    display.print("Passed");
    display.setTextSize(1);
    display.setCursor(0, 48);
    display.print(-days);
    display.print(" days ago");
  }
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("ACT:days left");
  display.display();
}

void drawNote() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("NOTE");
  display.setCursor(64, 0);
  display.print("ACT:read");
  display.drawFastHLine(0, 10, 128, WHITE);
  drawWrapped(note, 0, 15, 21, 5, 10);
  display.display();
}

void drawStats() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("FOCUS STATS");
  display.setCursor(0, 12);
  display.print("TODAY");
  display.setCursor(68, 12);
  display.print("TOTAL");
  display.setTextSize(3);
  display.setCursor(0, 22);
  display.print(cachedToday);
  display.setCursor(68, 22);
  display.print((int)totalSessions);
  display.setTextSize(1);
  if (goalSessions > 0) {
    display.setCursor(40, 40);
    display.print("/");
    display.print(goalSessions);
    display.drawRect(0, 48, 62, 5, WHITE);
    int w = (int)((long)min(cachedToday, goalSessions) * 58 / goalSessions);
    display.fillRect(2, 49, w, 3, WHITE);
  }
  display.setCursor(0, 56);
  display.print("F ");
  display.print((int)(totalMinutes / 60));
  display.print("h");
  display.print((int)(totalMinutes % 60));
  display.print("m  Tasks ");
  display.print((int)tasksDone);
  display.display();
}

void drawMessage() {
  display.clearDisplay();
  display.drawRect(0, 0, 128, 64, WHITE);
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(4, 3);
  display.print(msgTitle);
  display.drawFastHLine(0, 13, 128, WHITE);
  drawWrapped(String(msgBuf), 4, 17, 20, 5, 9);
  display.display();
}

void drawAlert() {
  bool inv = ((millis() / 450) % 2) == 0;
  if (inv != invState) { invState = inv; display.invertDisplay(inv); }
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(31, 2);
  display.print("* REMINDER *");
  display.drawFastHLine(0, 12, 128, WHITE);
  drawWrapped(alertText, 4, 18, 20, 3, 10);
  display.setCursor(0, 56);
  display.print("ACT:dismiss 2x:snooze");
  display.display();
}

// Priority: alarm > menu > message > pet reaction > mood override > home screen
void render() {
  unsigned long now = millis();
  if (alerting) { drawAlert(); return; }
  if (uiMode == UI_MENU) { drawMenu(); return; }
  if (msgActive()) { drawMessage(); return; }
  if ((long)(reactUntil - now) > 0) { drawFace(reactMood); return; }
  if ((long)(overrideUntil - now) > 0) { drawFace(currentMood()); return; }
  switch (homeIdx) {
    case SCR_CLOCK:   drawClock(); break;
    case SCR_WEATHER: drawWeather(); break;
    case SCR_TIMER:   if (tState != T_IDLE) drawTimer(); else drawTimerIdle(); break;
    case SCR_TASKS:   drawTasks(); break;
    case SCR_EVENT:   drawEvent(); break;
    case SCR_NOTE:    drawNote(); break;
    case SCR_STATS:   drawStats(); break;
    case SCR_EYES:    drawFace(currentMood()); break;
    default:          drawClock(); break;
  }
}

// ---------------- web dashboard ----------------
const char PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desk Buddy</title>
<style>
:root{color-scheme:dark}
body{margin:0;padding:14px;font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0}
h1{margin:4px 0 2px;font-size:22px;text-align:center}
#status{text-align:center;color:#94a3b8;font-size:13px;margin-bottom:12px}
.grid{display:grid;gap:12px;max-width:520px;margin:0 auto}
.card{background:#1e293b;border-radius:14px;padding:14px}
.card h2{margin:0 0 10px;font-size:12px;letter-spacing:1px;color:#94a3b8;text-transform:uppercase}
button{background:#334155;color:#e2e8f0;border:0;border-radius:10px;padding:10px 12px;font-size:15px;margin:3px}
button.p{background:#0ea5e9;color:#04202e;font-weight:600}
button.d{background:#7f1d1d}
button.g{background:#166534}
input{background:#0f172a;color:#e2e8f0;border:1px solid #334155;border-radius:10px;padding:10px;font-size:15px;margin:3px 0;box-sizing:border-box}
.w{width:100%}
.num{width:64px}
.big{font-size:34px;font-weight:700}
.small{color:#94a3b8;font-size:13px}
.item{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:6px 0;border-top:1px solid #334155}
a{color:#38bdf8}
</style></head><body>
<h1>Desk Buddy</h1><div id="status">Connecting...</div>
<div class="grid">

<div class="card"><h2>Mood (lasts 30 s)</h2>
<button onclick="mood('happy')">&#128522; Happy</button>
<button onclick="mood('sleepy')">&#128564; Sleepy</button>
<button onclick="mood('angry')">&#128544; Angry</button>
<button onclick="mood('surprised')">&#128558; Surprised</button>
<button onclick="mood('love')">&#128525; Love</button>
<button onclick="mood('auto')">Auto</button></div>

<div class="card"><h2>Focus timer</h2>
<div class="big" id="tim">Idle</div>
<div class="small" style="margin:6px 0">Focus <input class="num" id="fmin" type="number" min="1" max="120"> min &nbsp; Break <input class="num" id="bmin" type="number" min="1" max="60"> min</div>
<button class="p" onclick="timer('focus')">Start focus</button>
<button onclick="timer('break')">Start break</button>
<button class="d" onclick="timer('stop')">Stop</button></div>

<div class="card"><h2>Reminders</h2>
<div class="small" id="sync"></div>
<div class="small" style="margin-top:8px">Remind me in:</div>
<input class="w" id="ql" maxlength="21" placeholder="What for? (optional)">
<button onclick="quick(5)">5 min</button><button onclick="quick(10)">10 min</button><button onclick="quick(30)">30 min</button>
<div class="small" id="qrun"></div>
<div class="small" style="margin-top:10px">Alarm at a time:</div>
<input id="at" type="time"> <input id="al" maxlength="21" placeholder="Label" style="width:45%">
<label class="small"><input id="ad" type="checkbox"> daily</label>
<button class="p" onclick="addAlarm()">Add alarm</button>
<div id="alist"></div></div>

<div class="card"><h2>To-do list</h2>
<input class="w" id="tk" maxlength="28" placeholder="New task">
<button class="p" onclick="addTask()">Add task</button>
<div id="tlist"></div></div>

<div class="card"><h2>Countdown</h2>
<input class="w" id="en" maxlength="20" placeholder="Event (e.g. IoT exam)">
<input class="w" id="ed" type="date">
<button class="p" onclick="setEvent()">Save</button><button onclick="clrEvent()">Clear</button>
<div class="small" id="evs"></div></div>

<div class="card"><h2>Pinned note</h2>
<input class="w" id="nt" maxlength="90" placeholder="Sticky note for the screen">
<button class="p" onclick="setNote()">Pin</button><button onclick="clrNote()">Clear</button></div>

<div class="card"><h2>Send a message to the screen</h2>
<input class="w" id="msg" maxlength="100" placeholder="Hello from my phone!">
<button class="p" onclick="sendMsg()">Send</button></div>

<div class="card"><h2>Settings</h2>
<div class="small">Water reminder every <input class="num" id="sw" type="number" min="0" max="240"> min (0 = off)</div>
<div class="small">Stretch reminder every <input class="num" id="ss" type="number" min="0" max="240"> min (0 = off)</div>
<div class="small">Daily focus goal <input class="num" id="sg" type="number" min="0" max="12"> sessions</div>
<button class="p" onclick="saveSet()">Save settings</button></div>

<div class="card"><h2>Weather</h2><div class="big" id="wx">--</div><div class="small" id="wx2"></div></div>

<div class="card"><h2>Focus log</h2>
<div class="small" id="st"></div><div class="small" id="lg" style="margin-top:8px"></div>
<p class="small"><a href="/log.csv">Download CSV</a> &nbsp; <a href="#" onclick="resetStats();return false">Reset stats</a></p></div>
</div>
<script>
const $=id=>document.getElementById(id);
const esc=s=>String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
let inited=false;
const q=(p,o)=>p+'?'+Object.entries(o).map(([k,v])=>k+'='+encodeURIComponent(v)).join('&');
const call=p=>fetch(p).then(tick);
const mood=m=>call(q('/api/mood',{m}));
function sendMsg(){const t=$('msg').value.trim();if(!t)return;call(q('/api/msg',{t}));$('msg').value=''}
const timer=c=>call(q('/api/timer',{cmd:c,f:$('fmin').value,b:$('bmin').value}));
function quick(m){call(q('/api/quick',{m:m,l:$('ql').value.trim()}));$('ql').value=''}
function addAlarm(){const t=$('at').value;if(!t){alert('Pick a time first');return}
  call(q('/api/alarm',{cmd:'add',t:t,l:$('al').value.trim(),d:$('ad').checked?1:0}));$('al').value=''}
const delAlarm=i=>call(q('/api/alarm',{cmd:'del',i:i}));
function addTask(){const t=$('tk').value.trim();if(!t)return;call(q('/api/task',{cmd:'add',t:t}));$('tk').value=''}
const taskDo=(c,i)=>call(q('/api/task',{cmd:c,i:i}));
const setEvent=()=>call(q('/api/event',{n:$('en').value.trim(),d:$('ed').value}));
const clrEvent=()=>{$('en').value='';$('ed').value='';call(q('/api/event',{n:'',d:''}))};
const setNote=()=>call(q('/api/note',{t:$('nt').value.trim()}));
const clrNote=()=>{$('nt').value='';call(q('/api/note',{t:''}))};
const saveSet=()=>call(q('/api/settings',{w:$('sw').value||0,s:$('ss').value||0,g:$('sg').value||0}));
function resetStats(){if(confirm('Clear all focus stats?'))call('/api/reset')}
const fmt=s=>String(Math.floor(s/60)).padStart(2,'0')+':'+String(s%60).padStart(2,'0');
async function tick(){
  try{
    const d=await (await fetch('/api/state')).json();
    if(!inited){
      $('fmin').value=d.fmin;$('bmin').value=d.bmin;$('sw').value=d.water;$('ss').value=d.stretch;$('sg').value=d.goal;
      $('en').value=d.evn;$('ed').value=d.evd;$('nt').value=d.note;inited=true}
    $('status').textContent='Mood: '+d.mood+'  |  '+(d.online?'online':'offline')+'  |  '+d.ip;
    $('tim').textContent=d.timer==='idle'?'Idle':d.timer.toUpperCase()+' '+fmt(d.left);
    $('sync').textContent=d.synced?'Clock synced - alarms active':'Clock not synced - alarms need the hotspot WiFi (quick timers still work)';
    $('qrun').textContent=d.quick.length?'Running: '+d.quick.map(m=>m+' min').join(', '):'';
    $('alist').innerHTML=d.alarms.map((a,i)=>'<div class="item"><span>'+a[0]+(a[2]?' (daily) ':' ')+esc(a[1])+'</span><button class="d" onclick="delAlarm('+i+')">&#10005;</button></div>').join('');
    $('tlist').innerHTML=d.tasks.map((t,i)=>'<div class="item"><span>'+esc(t)+'</span><span><button class="g" onclick="taskDo(\'done\','+i+')">&#10003;</button><button class="d" onclick="taskDo(\'del\','+i+')">&#10005;</button></span></div>').join('')||'<div class="small">No tasks - nice!</div>';
    $('evs').textContent=d.evDays===null?(d.evd?'Needs the hotspot WiFi to count days':''):(d.evDays>0?d.evDays+' day(s) left':d.evDays===0?'Today!':'Passed '+(-d.evDays)+' day(s) ago');
    $('wx').textContent=d.temp===null?'--':d.temp+'\u00b0C  '+d.cond;
    $('wx2').textContent=d.temp===null?'Needs the hotspot WiFi (see sketch config)':'Humidity '+d.hum+'%  |  AQI '+(d.aqi===null?'--':d.aqi+' ('+d.aqiName+')')+'  |  updated '+d.wAge+' min ago';
    $('st').textContent='Today: '+d.today+'/'+d.goal+' sessions  |  Total: '+d.total+' sessions, '+d.totalMin+' min  |  Tasks done: '+d.tasksDone;
    $('lg').innerHTML=d.log.slice().reverse().map(([ep,m])=>(ep?new Date(ep*1000).toLocaleString([],{dateStyle:'medium',timeStyle:'short'}):'time not synced')+' - '+m+' min').join('<br>')||'No sessions yet';
  }catch(e){$('status').textContent='Reconnecting...'}
}
tick();setInterval(tick,2000);
</script></body></html>
)rawliteral";

String jsonEsc(const String& s) {
  String o;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

void appendLogJson(String& j, int maxN) {
  j += "[";
  int lines = 0;
  for (unsigned int i = 0; i < logStr.length(); i++) if (logStr[i] == '\n') lines++;
  int skip = lines > maxN ? lines - maxN : 0;
  int start = 0, idx = 0;
  bool first = true;
  while (start < (int)logStr.length()) {
    int nl = logStr.indexOf('\n', start);
    if (nl < 0) break;
    if (idx >= skip) {
      String line = logStr.substring(start, nl);
      int cm = line.indexOf(',');
      if (cm > 0) {
        if (!first) j += ",";
        first = false;
        j += "[" + line.substring(0, cm) + "," + line.substring(cm + 1) + "]";
      }
    }
    idx++;
    start = nl + 1;
  }
  j += "]";
}

void handleRoot() { server.send(200, "text/html", PAGE); }

void handleState() {
  unsigned long now = millis();
  int left = 0;
  if (tState != T_IDLE) { long l = (long)(tEnd - now); left = l > 0 ? (int)(l / 1000) : 0; }
  float t = wTemp, h = wHum, a = wAqi;
  int c = wCode;
  unsigned long up = wUpdated;
  String j;
  j.reserve(3800);
  j += "{\"mood\":\"";
  j += MOOD_NAMES[currentMood()];
  j += "\"";
  j += ",\"timer\":\"";
  j += (tState == T_IDLE ? "idle" : (tState == T_FOCUS ? "focus" : "break"));
  j += "\"";
  j += ",\"left\":" + String(left);
  j += ",\"fmin\":" + String(focusMin);
  j += ",\"bmin\":" + String(breakMin);
  j += ",\"temp\":" + (isnan(t) ? String("null") : String(t, 1));
  j += ",\"hum\":" + (isnan(h) ? String("null") : String((int)h));
  j += ",\"cond\":\"" + String(wxName(c)) + "\"";
  j += ",\"aqi\":" + (isnan(a) ? String("null") : String((int)a));
  j += ",\"aqiName\":\"" + String(aqiName(a)) + "\"";
  j += ",\"wAge\":" + String(up ? (long)((now - up) / 60000UL) : -1L);
  j += ",\"today\":" + String(cachedToday);
  j += ",\"total\":" + String((int)totalSessions);
  j += ",\"totalMin\":" + String((int)totalMinutes);
  j += ",\"tasksDone\":" + String((int)tasksDone);
  j += ",\"goal\":" + String(goalSessions);
  j += ",\"water\":" + String(waterMin);
  j += ",\"stretch\":" + String(stretchMin);
  j += ",\"online\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  j += ",\"synced\":" + String(timeSynced() ? "true" : "false");
  j += ",\"ip\":\"" + ipString() + "\"";
  j += ",\"msg\":\"" + jsonEsc(lastMsg) + "\"";

  j += ",\"alarms\":[";
  for (int i = 0; i < alarmN; i++) {
    if (i) j += ",";
    char b[8];
    snprintf(b, sizeof b, "%02d:%02d", alarms[i].h, alarms[i].m);
    j += "[\"";
    j += b;
    j += "\",\"";
    j += alarms[i].label;
    j += "\",";
    j += (alarms[i].daily ? "1" : "0");
    j += "]";
  }
  j += "]";

  j += ",\"quick\":[";
  bool first = true;
  for (int i = 0; i < 3; i++) {
    if (!quickTimers[i].used) continue;
    if (!first) j += ",";
    first = false;
    long l = (long)(quickTimers[i].at - now);
    j += String(l > 0 ? (l + 59999L) / 60000L : 0L);
  }
  j += "]";

  j += ",\"tasks\":[";
  for (int i = 0; i < taskN; i++) {
    if (i) j += ",";
    j += "\"" + jsonEsc(tasks[i]) + "\"";
  }
  j += "]";

  bool ok = false;
  int days = daysUntilEvent(ok);
  j += ",\"evn\":\"" + jsonEsc(String(evName)) + "\"";
  j += ",\"evd\":\"" + String(evDate) + "\"";
  j += ",\"evDays\":" + (ok ? String(days) : String("null"));
  j += ",\"note\":\"" + jsonEsc(note) + "\"";
  j += ",\"log\":";
  appendLogJson(j, 8);
  j += "}";
  server.send(200, "application/json", j);
}

void handleMood() {
  String m = server.arg("m");
  if (m == "auto") overrideUntil = 0;
  else {
    for (int i = 0; i < 6; i++) {
      if (m == MOOD_NAMES[i]) { overrideMood = (Mood)i; overrideUntil = millis() + 30000UL; }
    }
  }
  poke();
  server.send(200, "text/plain", "ok");
}

void handleMsg() {
  String t = server.arg("t");
  if (t.length() == 0) { server.send(400, "text/plain", "empty"); return; }
  showMsg(t, 8000);
  startSound(SND_NUDGE);
  poke();
  server.send(200, "text/plain", "ok");
}

void handleTimer() {
  String c = server.arg("cmd");
  if (server.hasArg("f")) { int f = server.arg("f").toInt(); if (f >= 1 && f <= 120) focusMin = f; }
  if (server.hasArg("b")) { int b = server.arg("b").toInt(); if (b >= 1 && b <= 60) breakMin = b; }
  prefs.putInt("fm", focusMin);
  prefs.putInt("bm", breakMin);
  if (c == "focus") startTimer(T_FOCUS);
  else if (c == "break") startTimer(T_BREAK);
  else if (c == "stop") stopTimer();
  poke();
  server.send(200, "text/plain", "ok");
}

void handleQuick() {
  int m = server.arg("m").toInt();
  if (m < 1 || m > 240 || !addQuick(m, server.arg("l"))) { server.send(400, "text/plain", "bad"); return; }
  server.send(200, "text/plain", "ok");
}

void handleAlarm() {
  String c = server.arg("cmd");
  if (c == "add") {
    int h, m;
    if (sscanf(server.arg("t").c_str(), "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60 && alarmN < MAX_ALARMS) {
      AlarmItem& a = alarms[alarmN];
      a.h = h;
      a.m = m;
      a.daily = server.arg("d") == "1";
      String l = cleanText(server.arg("l"), 21);
      strncpy(a.label, l.c_str(), 21);
      a.label[21] = 0;
      a.lastKey = -1;
      alarmN++;
      saveAlarms();
    } else {
      server.send(400, "text/plain", "bad");
      return;
    }
  } else if (c == "del") {
    deleteAlarm(server.arg("i").toInt());
  }
  server.send(200, "text/plain", "ok");
}

void handleTask() {
  String c = server.arg("cmd");
  if (c == "add") {
    if (!addTask(server.arg("t"))) { server.send(400, "text/plain", "bad"); return; }
  } else if (c == "done") {
    completeTask(server.arg("i").toInt());
  } else if (c == "del") {
    deleteTask(server.arg("i").toInt());
  }
  server.send(200, "text/plain", "ok");
}

void handleEvent() {
  String n = cleanText(server.arg("n"), 20);
  String d = server.arg("d");
  if (n.length() == 0 && d.length() == 0) {
    evName[0] = 0;
    evDate[0] = 0;
  } else {
    bool valid = (d.length() == 10);
    for (unsigned int i = 0; valid && i < d.length(); i++) {
      char ch = d[i];
      if (!((ch >= '0' && ch <= '9') || ch == '-')) valid = false;
    }
    int y, m, dd;
    if (valid && sscanf(d.c_str(), "%d-%d-%d", &y, &m, &dd) == 3 && y > 2000 && m >= 1 && m <= 12 && dd >= 1 && dd <= 31) {
      strncpy(evDate, d.c_str(), 10);
      evDate[10] = 0;
    } else {
      evDate[0] = 0;
    }
    strncpy(evName, n.c_str(), 20);
    evName[20] = 0;
  }
  saveEvent();
  server.send(200, "text/plain", "ok");
}

void handleNote() {
  note = cleanText(server.arg("t"), 90);
  prefs.putString("note", note);
  server.send(200, "text/plain", "ok");
}

void handleSettings() {
  int w = server.arg("w").toInt();
  int s = server.arg("s").toInt();
  int g = server.arg("g").toInt();
  waterMin = constrain(w, 0, 240);
  stretchMin = constrain(s, 0, 240);
  goalSessions = constrain(g, 0, 12);
  prefs.putInt("wm", waterMin);
  prefs.putInt("sm", stretchMin);
  prefs.putInt("gl", goalSessions);
  unsigned long now = millis();
  nextWaterAt = waterMin ? now + (unsigned long)waterMin * 60000UL : 0;
  nextStretchAt = stretchMin ? now + (unsigned long)stretchMin * 60000UL : 0;
  server.send(200, "text/plain", "ok");
}

void handleReset() {
  totalSessions = 0;
  totalMinutes = 0;
  logStr = "";
  cachedToday = 0;
  prefs.putUInt("ses", 0);
  prefs.putUInt("min", 0);
  prefs.putString("log", "");
  server.send(200, "text/plain", "ok");
}

void handleCsv() {
  String out = "session_time,focus_minutes\n";
  int start = 0;
  while (start < (int)logStr.length()) {
    int nl = logStr.indexOf('\n', start);
    if (nl < 0) break;
    String line = logStr.substring(start, nl);
    int cm = line.indexOf(',');
    if (cm > 0) {
      uint32_t ep = strtoul(line.substring(0, cm).c_str(), NULL, 10);
      if (ep > 1700000000UL) {
        time_t t = ep;
        struct tm tt;
        localtime_r(&t, &tt);
        char b[24];
        strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tt);
        out += b;
      } else {
        out += "unsynced";
      }
      out += "," + line.substring(cm + 1) + "\n";
    }
    start = nl + 1;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=focus_log.csv");
  server.send(200, "text/csv", out);
}

// ---------------- setup / loop ----------------
void splash(const char* l1, const char* l2) {
  if (!oledOK) return;
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(6, 8);
  display.print(l1);
  display.setTextSize(1);
  display.setCursor(6, 36);
  display.print(l2);
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nDesk Buddy v15 starting");

  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_DRIVE, OUTPUT);
  digitalWrite(PIN_DRIVE, LOW);
  pinMode(PIN_SENSE, INPUT_PULLUP);
  pinMode(PIN_ACT, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  Wire.begin(21, 22);
  Wire.setClock(400000);
  oledOK = initOled();
  if (!oledOK) Serial.println("OLED not found - check wiring (SDA=21, SCL=22) and address");
  display.setRotation(0);
  display.cp437(true);

  splash("Desk Buddy", "Starting...");

  prefs.begin("buddy", false);
  focusMin = prefs.getInt("fm", 25);
  breakMin = prefs.getInt("bm", 5);
  goalSessions = prefs.getInt("gl", 4);
  waterMin = prefs.getInt("wm", 0);
  stretchMin = prefs.getInt("sm", 0);
  totalSessions = prefs.getUInt("ses", 0);
  totalMinutes = prefs.getUInt("min", 0);
  logStr = prefs.getString("log", "");
  note = prefs.getString("note", "");
  strncpy(evName, prefs.getString("evn", "").c_str(), 20);
  evName[20] = 0;
  strncpy(evDate, prefs.getString("evd", "").c_str(), 10);
  evDate[10] = 0;
  loadAlarms();
  loadTasks();
  for (int i = 0; i < 3; i++) quickTimers[i].used = false;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  if (strlen(STA_SSID)) WiFi.begin(STA_SSID, STA_PASS);

  server.on("/", handleRoot);
  server.on("/api/state", handleState);
  server.on("/api/mood", handleMood);
  server.on("/api/msg", handleMsg);
  server.on("/api/timer", handleTimer);
  server.on("/api/quick", handleQuick);
  server.on("/api/alarm", handleAlarm);
  server.on("/api/task", handleTask);
  server.on("/api/event", handleEvent);
  server.on("/api/note", handleNote);
  server.on("/api/settings", handleSettings);
  server.on("/api/reset", handleReset);
  server.on("/log.csv", handleCsv);
  server.begin();

  xTaskCreate(weatherTask, "wx", 12288, NULL, 1, NULL);

  unsigned long now = millis();
  lastInteraction = now;
  nextBlinkAt = now + 2000;
  nextLookAt = now + 1500;
  nextHappyAt = now + random(20000, 35000);
  nextWaterAt = waterMin ? now + (unsigned long)waterMin * 60000UL : 0;
  nextStretchAt = stretchMin ? now + (unsigned long)stretchMin * 60000UL : 0;
  cachedToday = todayCount();

  splash("Ready!", "WiFi: DeskBuddy");
  if (oledOK) {
    display.setCursor(6, 48);
    display.print("Open 192.168.4.1");
    display.display();
  }
  delay(3000);
  startSound(SND_STARTUP);
  Serial.println("Dashboard: http://192.168.4.1");
}

void loop() {
  unsigned long now = millis();
  server.handleClient();

  if (strlen(STA_SSID) && WiFi.status() == WL_CONNECTED && !netStarted) {
    configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
    MDNS.begin("deskbuddy");
    MDNS.addService("http", "tcp", 80);
    netStarted = true;
  }

  // NAV: two-wire switch and BOOT button
  GestureEv ev = gestureUpdate(gWire, now);
  if (ev != EV_NONE) handleNav(ev);
  ev = gestureUpdate(gBoot, now);
  if (ev != EV_NONE) handleNav(ev);

  // ACT: push button on GPIO19
  ev = gestureUpdate(gAct, now);
  if (ev != EV_NONE) handleAct(ev);

  timerTick();
  soundUpdate(now);

  static unsigned long lastSec = 0;
  if (now - lastSec >= 1000) {
    lastSec = now;
    checkAlarms();
    checkQuick();
  }

  static unsigned long lastSlow = 0;
  if (now - lastSlow >= 5000) {
    lastSlow = now;
    cachedToday = todayCount();
    nightCached = nightNow();
  }

  if (uiMode == UI_MENU && now - lastInteraction > MENU_TIMEOUT_MS) goHome();

  if (waterMin > 0 && nextWaterAt && (long)(now - nextWaterAt) >= 0) {
    if (tState == T_FOCUS || nightCached) nextWaterAt = now + 300000UL;
    else {
      showMsgT("Time to drink some water!", 7000, "REMINDER");
      startSound(SND_NUDGE);
      nextWaterAt = now + (unsigned long)waterMin * 60000UL;
    }
  }
  if (stretchMin > 0 && nextStretchAt && (long)(now - nextStretchAt) >= 0) {
    if (tState == T_FOCUS || nightCached) nextStretchAt = now + 300000UL;
    else {
      showMsgT("Stand up and stretch!", 7000, "REMINDER");
      startSound(SND_NUDGE);
      nextStretchAt = now + (unsigned long)stretchMin * 60000UL;
    }
  }

  if (alerting && (long)(now - alertUntil) >= 0) stopAlert();

  if (now >= nextHappyAt) {
    happyUntil = now + 3500;
    nextHappyAt = now + random(25000, 45000);
  }

  setDisplay(wantDisplay(now));

  static unsigned long lastFrame = 0;
  if (oledOK && displayOn && now - lastFrame >= 40) {
    lastFrame = now;
    render();
  }
}
