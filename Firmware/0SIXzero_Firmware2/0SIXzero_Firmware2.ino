#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

const char* WIFI_SSID = "MY_WIFI_NAME"; //2.4 GHz network only 
const char* WIFI_PASS = "MY_WIFI_PASSWORD";
const char* TZ_INFO = "NZST-12NZDT,M9.5.0,M4.1.0/3"; //NZ with daylight saving
bool ntpStarted = false;

const int PIN_SNOOZE = D0; 
const int PIN_SET    = D1;
const int PIN_MINUS  = D2; 
const int PIN_PLUS   = D3; 

const uint32_t HOLD_MS = 2000;

const int PIN_DC = D4;
const int PIN_CS = D5;
const int PIN_BL = D6;
const int PIN_SCL = D9;
const int PIN_SDA =D10;
const int PIN_RST = D8;

class MyST7789 : public Adafruit_ST7789 {
public: 
 MyST7789(SPIClass* spi, int8_t cs, int8_t dc, int8_t rst)
 : Adafruit_ST7789(spi, cs, dc, rst){}
 void setOffsets(uint8_t col, uint8_t row) {
  _colstart = _colstart2 = col;
  _rowstart = _rowstart2 = row;
 }
};

MyST7789 tft(&SPI, PIN_CS, PIN_DC, PIN_RST);
GFXcanvas16* canvas = nullptr;

enum Ev { EV_NONE, EV_PRESS, EV_SHORT, EV_HOLD };

struct Btn {
  uint8_t  pin;
  bool     down, raw, holdFired;
  uint32_t changed, downAt;
  Btn(uint8_t p) : pin(p), down(false), raw(false), holdFired(false), changed(0), downAt(0) {}
};

Btn bSnooze(PIN_SNOOZE), bSet(PIN_SET), bMinus(PIN_MINUS), bPlus(PIN_PLUS);
Ev poll(Btn& b) {
  bool r = (digitalRead(b.pin) == LOW);
  uint32_t now = millis();
  if (r != b.raw) { b.raw = r; b.changed = now; }
  Ev ev = EV_NONE;
  if ((now - b.changed) > 25 && r != b.down) {
 b.down = r;
    if (r) { b.downAt = now; b.holdFired = false; ev = EV_PRESS; }
    else if (!b.holdFired) ev = EV_SHORT;
  }
  if (b.down && !b.holdFired && (now - b.downAt) >= HOLD_MS) {
    b.holdFired = true;
    ev = EV_HOLD;
  }
  return ev;
}

const int PIN_BUZZ = D7;
const bool BUZZER_ACTIVE = false;  // change if it was active buzzer, to true 
const int BUZZER_HZ = 2700;

void buzzTone(int f) {
  #if ESP_ARDUINO_VERSION_MAJOR >=3 
  ledcWriteTone(PIN_BUZZ, f);
  #else 
  ledcWriteTone(0, f);
  #endif
}
void buzzInit() { 
  if (BUZZER_ACTIVE) { 
    pinMode(PIN_BUZZ, OUTPUT);
    digitalWrite(PIN_BUZZ, LOW);
  } else { 
  #if ESP_ARDUINO_VERSION_MAJOR >=3
      ledcAttach(PIN_BUZZ, 2000, 8 );
  #else 
  ledcSetup(0, 2000 ,8);
  ledcAttachPin(PIN_BUZZ, 0); 
  #endif
  buzzTone(0);
  }
}
void buzzOff() { 
  if (BUZZER_ACTIVE) digitalWrite(PIN_BUZZ, LOW);
  else buzzTone(0);
}

int      alarmH = 7, alarmM = 0;
bool     alarmOn = true;           // true for now, for testing
const int SNOOZE_MINUTES = 9;
bool     ringing = false, snoozed = false;
time_t   snoozeUntil = 0;
uint32_t ringStart = 0;
long     lastTrigger = -1;

void startRinging() { ringing = true; snoozed = false; ringStart = millis(); Serial.println("ringing"); }
void stopAlarm()    { ringing = false; snoozed = false; buzzOff(); Serial.println("stopped"); }

// four short beeps, a pause, repeat. After 30 s it gets faster.
void alarmSound() {
  bool fast = (millis() - ringStart) > 30000;
  uint32_t cycle = fast ? 500 : 2000;
  uint32_t m = millis() % cycle;
  bool on = fast ? ((m % 125) < 60) : (m < 1000 && (m % 250) < 125);
  if (BUZZER_ACTIVE) digitalWrite(PIN_BUZZ, on ? HIGH : LOW);
  else buzzTone(on ? BUZZER_HZ : 0);
}

enum Mode {CLOCK, SET_HOUR, SET_MIN, SET_ON};
Mode mode = CLOCK; 
uint32_t lastInput = 0;
Preferences prefs; 

void loadSettings() { 
 prefs.begin("sixzero", true);
 alarmH = prefs.getInt("h", 7);
 alarmM = prefs.getInt("m", 0);
 alarmOn = prefs.getBool("on", false);
 prefs.end();
} 

void saveSettings() {
  prefs.begin("sixzero", false);
  prefs.putInt("h", alarmH);
  prefs.putInt("m", alarmM);
  prefs.putBool("on", alarmOn);
  prefs.end(); 
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_SNOOZE, INPUT_PULLUP);
  pinMode(PIN_SET,    INPUT_PULLUP);
  pinMode(PIN_MINUS,  INPUT_PULLUP);
  pinMode(PIN_PLUS,   INPUT_PULLUP);

  loadSettings();
  buzzInit();

  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, LOW);            // LOW = backlight ON on this module
  SPI.begin(PIN_SCL, -1, PIN_SDA, -1);  // clock, no MISO, data
  tft.init(76, 284);
  tft.setOffsets(82, 18);
  tft.invertDisplay(false);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  canvas = new GFXcanvas16(tft.width(), tft.height());

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastInput = millis();
}

 void drawSimple(const tm& t, bool timeValid) {
  canvas->fillScreen(0x0000);
  canvas->setTextSize(6);
  canvas->setTextColor(0x03DF); // Bright blue 
  canvas->setCursor(6, 14);
  char buf[8];
  if (timeValid) snprintf(buf, sizeof buf, "%02d:%02d", t.tm_hour, t.tm_min);
  else snprintf(buf, sizeof buf, "--:--");
  canvas->print(buf);
  tft.drawRGBBitmap(0, 0, canvas->getBuffer(), canvas->width(), canvas->height());
}

void loop() {
  uint32_t nowMs = millis();

  // Wi-Fi time 
  if (!ntpStarted && WiFi.status() == WL_CONNECTED) {
    configTzTime(TZ_INFO, "pool.ntp.org", "time.cloudflare.com");
    ntpStarted = true;
  }
  time_t nowT = time(nullptr);
  bool timeValid = (nowT > 1700000000);
  struct tm t;
  localtime_r(&nowT, &t);

  // buttons 
  Ev eSn = poll(bSnooze), eSet = poll(bSet), eMinus = poll(bMinus), ePlus = poll(bPlus);
  if (eSn || eSet || eMinus || ePlus) lastInput = nowMs;

  if (ringing) {
    // ringing: snooze or stop 
    if (eSn == EV_SHORT) {
      ringing = false;
      snoozed = true;
      snoozeUntil = nowT + SNOOZE_MINUTES * 60;
      buzzOff();
    } else if (eSn == EV_HOLD) {
      stopAlarm();
    }
  } else if (snoozed && eSn == EV_HOLD) {
    stopAlarm();
  } else {
    // not ringing: set the alarm 
    switch (mode) {
      case CLOCK:
        if (eSet == EV_SHORT) mode = SET_HOUR;
        break;
      case SET_HOUR:
        if (ePlus == EV_PRESS)  alarmH = (alarmH + 1) % 24;
        if (eMinus == EV_PRESS) alarmH = (alarmH + 23) % 24;
        if (eSet == EV_SHORT)   mode = SET_MIN;
        break;
      case SET_MIN:
        if (ePlus == EV_PRESS)  alarmM = (alarmM + 1) % 60;
        if (eMinus == EV_PRESS) alarmM = (alarmM + 59) % 60;
        if (eSet == EV_SHORT)   mode = SET_ON;
        break;
      case SET_ON:
        if (ePlus == EV_PRESS || eMinus == EV_PRESS) alarmOn = !alarmOn;
        if (eSet == EV_SHORT) {
          saveSettings();
          lastTrigger = -1;
          if (!alarmOn) stopAlarm();
          mode = CLOCK;
        }
        break;
    }
    // leave set mode after 15 s of no input
    if (mode != CLOCK && (nowMs - lastInput) > 15000) {
      saveSettings();
      if (!alarmOn) stopAlarm();
      mode = CLOCK;
    }
  }

  // alarm trigger 
  if (timeValid && alarmOn && !ringing) {
    long key = (long)t.tm_yday * 1440 + t.tm_hour * 60 + t.tm_min;
    if (!snoozed && t.tm_hour == alarmH && t.tm_min == alarmM && key != lastTrigger) {
      lastTrigger = key;
      startRinging();
    }
    if (snoozed && nowT >= snoozeUntil) startRinging();
  }

  // give up after 10 minutes
  if (ringing && (nowMs - ringStart) > 10UL * 60UL * 1000UL) stopAlarm();

  // sound
  if (ringing) alarmSound();

  static uint32_t lastDraw = 0;
  if (nowMs - lastDraw >= 200) {
    lastDraw = nowMs;
    drawSimple(t, timeValid);
  }
}





