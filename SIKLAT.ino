#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include "RTClib.h"

// ========================
// --- Pin and Constants ---
// ========================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define CLK 34     // Rotary encoder CLK pin
#define DT 35      // Rotary encoder DT pin
#define SW 25      // Rotary encoder Switch pin

#define IN1 14     // Motor driver pins
#define IN2 27
#define IN3 26
#define IN4 33
#define EEP_PIN 32 // Motor enable/EEPROM pin

#define MAX_ALARMS 3

// ========================
// --- Global Variables ---
// ========================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
RTC_DS3231 rtc;

// Time and alarm value buffers
int editValues[5] = {0};     // Holds [year, month, day, hour, minute] for editing
int alarmValues[3] = {0};    // Holds [hour, minute, rampTime] for alarm

// State flags and indices
bool settingInitialized = false;
bool alarmSettingInitialized = false;
bool alarmSet = false;
bool alarmTriggered = false;
volatile int menuIndex = 0;
int lastEncoded = 0;
bool inMenu = false;
bool inSubMenu = false;
bool inAlarmMenu = false;
int setFieldIndex = 0;
int alarmFieldIndex = 0;
bool alarmSettingInProgress = false;

// Button debounce and alarm confirmation
unsigned long lastButtonPress = 0;
const unsigned long buttonDebounce = 200;
bool lastButtonState = HIGH;
bool alarmConfirmed = false; // Tracks whether the alarm has been explicitly confirmed
int lastAlarmMinute = -1;    // Tracks the last minute the alarm triggered

// ========================
// --- Arduino Setup ---
// ========================
void setup() {
  Serial.begin(9600);

  // --- Pin Modes ---
  pinMode(CLK, INPUT);
  pinMode(DT, INPUT);
  pinMode(SW, INPUT_PULLUP);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  pinMode(EEP_PIN, OUTPUT);

  // --- Display and RTC Init ---
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    while (true);
  }

  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (true);
  }

  // If RTC lost power, set it to compile time
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // --- Intro Animation ---
  displayIntro();

  display.clearDisplay();
  display.display();
  delay(2000);
}

// ========================
// --- Main Loop ---
// ========================
void loop() {
  // --- Handle Inputs ---
  handleEncoder();
  handleButtonPress();

  // --- Alarm Handling ---
  // Prioritize alarm popup when triggered
  if (alarmTriggered) {
    showWakeUpScreen();
    return; // Prevent other tasks from running
  }

  // Check for alarm trigger only when alarm is confirmed and set
  if (alarmSet && alarmConfirmed) {
    DateTime now = rtc.now();
    if (now.hour() == alarmValues[0] && now.minute() == alarmValues[1]) {
      if (lastAlarmMinute != now.minute()) { // Trigger only if not already triggered in this minute
        alarmTriggered = true;
        lastAlarmMinute = now.minute();
        startVibration();
        showWakeUpScreen(); // Immediately show WAKE UP screen
        Serial.println("Alarm Triggered: Displaying WAKE UP and Vibrating...");
        return; // Exit loop to prioritize alarm
      }
    } else {
      lastAlarmMinute = -1; // Reset last triggered minute if time moves away
    }
  }

  // --- Main State Machine ---
  if (inSubMenu) {
    setDateTime();
  } else if (inAlarmMenu) {
    setAlarm();
  } else if (inMenu) {
    showMenu();
  } else {
    showClock();
  }
}

// ========================
// --- Intro Animation ---
// ========================
void displayIntro() {
  // Stage 1: Show branding
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor((SCREEN_WIDTH - 84) / 2, 10);
  display.print("PENTAGRAM tech.");

  display.setFont(&FreeMonoBold9pt7b);
  int16_t x, y;
  uint16_t w, h;
  const char* title = "S.I.K.L.A.T";
  display.getTextBounds(title, 0, 0, &x, &y, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 45);
  display.print(title);
  display.setFont(); // Reset to default
  display.display();

  delay(3000);  // Wait to view intro

  // Stage 2: Show "Initializing..." and animation
  display.clearDisplay();
  display.setCursor(30, 30);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.print("Initializing");
  display.display();
  delay(1000);

  // Animation frames
  const char* frames[] = {
    "      (  -_-) zZ ",     // sleepy
    "      (  -_-) ...",    // blinking
    "      (-_-  )    ",
    "      ( -_- )    ",        // eyes half open
    "      ( O_O )    ",         // surprised
    "      ( *_* )    ",         // fully awake
    "      ( ^_^ )    "      // Fully awake
  };

  for (int i = 0; i < 7; i++) {
    display.clearDisplay();
    display.setCursor((SCREEN_WIDTH - w) / 2, 30);
    display.setTextSize(1);
    display.print(frames[i]);
    display.display();
    delay(500);
    // Pause before exiting intro
  }
}

// ========================
// --- Alarm Popup ---
// ========================
void showWakeUpScreen() {
  static bool popupDisplayed = false;

  if (!popupDisplayed) {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor((SCREEN_WIDTH - 90) / 2, 20);
    display.print("WAKE UP");

    display.setTextSize(1);
    display.setCursor((SCREEN_WIDTH - 40) / 2, 45);
    display.print("Close");

    display.display();
    popupDisplayed = true;
    Serial.println("Displaying WAKE UP Popup");
  }

  // Stop alarm and vibration when button is pressed
  if (digitalRead(SW) == LOW && millis() - lastButtonPress > buttonDebounce) {
    lastButtonPress = millis();
    alarmTriggered = false;
    stopVibration();
    popupDisplayed = false; // Reset popup flag for the next alarm
    Serial.println("WAKE UP Popup Closed by User");
  }
}

// ========================
// --- Rotary Encoder ---
// ========================
void handleEncoder() {
  static int lastState = 0;
  static unsigned long lastDebounceTime = 0;
  const unsigned long debounceDelay = 5; // 5ms debounce time for rotary encoder

  int currentState = (digitalRead(CLK) << 1) | digitalRead(DT);

  // Ignore changes that occur within the debounce delay
  if (millis() - lastDebounceTime > debounceDelay) {
    if (currentState != lastState) {
      // Detect the direction of rotation
      if ((lastState == 0b00 && currentState == 0b01) ||
          (lastState == 0b01 && currentState == 0b11) ||
          (lastState == 0b11 && currentState == 0b10) ||
          (lastState == 0b10 && currentState == 0b00)) {
        handleScroll(1); // Forward rotation
      } else if ((lastState == 0b00 && currentState == 0b10) ||
                 (lastState == 0b10 && currentState == 0b11) ||
                 (lastState == 0b11 && currentState == 0b01) ||
                 (lastState == 0b01 && currentState == 0b00)) {
        handleScroll(-1); // Reverse rotation
      }
      lastDebounceTime = millis(); // Update debounce time
    }
    lastState = currentState; // Update the last state
  }
}

// Handles scroll action in various menus
void handleScroll(int direction) {
  // If not in any menu, enter the main menu
  if (!inMenu && !inSubMenu && !inAlarmMenu) {
    inMenu = true;
    menuIndex = 0;
  } else if (inSubMenu) {
    // Editing date/time fields
    editValues[setFieldIndex] += direction;
    if (setFieldIndex == 0) { // Year
      if (editValues[0] < 2000) editValues[0] = 2099;
      if (editValues[0] > 2099) editValues[0] = 2000;
    } else if (setFieldIndex == 1) { // Month
      if (editValues[1] < 1) editValues[1] = 12;
      if (editValues[1] > 12) editValues[1] = 1;
    } else if (setFieldIndex == 2) { // Day (with leap year check)
      int maxDay = 31;
      if (editValues[1] == 2) {
        bool isLeap = (editValues[0] % 4 == 0 && (editValues[0] % 100 != 0 || editValues[0] % 400 == 0));
        maxDay = isLeap ? 29 : 28;
      } else if (editValues[1] == 4 || editValues[1] == 6 || editValues[1] == 9 || editValues[1] == 11) {
        maxDay = 30;
      }
      if (editValues[2] < 1) editValues[2] = maxDay;
      if (editValues[2] > maxDay) editValues[2] = 1;
    } else if (setFieldIndex == 3) { // Hour
      if (editValues[3] < 0) editValues[3] = 23;
      if (editValues[3] > 23) editValues[3] = 0;
    } else if (setFieldIndex == 4) { // Minute
      if (editValues[4] < 0) editValues[4] = 59;
      if (editValues[4] > 59) editValues[4] = 0;
    }
  } else if (inAlarmMenu) {
    // Editing alarm fields
    alarmValues[alarmFieldIndex] += direction;
    if (alarmFieldIndex == 0) { // Hour
      if (alarmValues[0] < 0) alarmValues[0] = 23;
      if (alarmValues[0] > 23) alarmValues[0] = 0;
    } else if (alarmFieldIndex == 1) { // Minute
      if (alarmValues[1] < 0) alarmValues[1] = 59;
      if (alarmValues[1] > 59) alarmValues[1] = 0;
    } else if (alarmFieldIndex == 2) { // Ramp time
      if (alarmValues[2] < 0) alarmValues[2] = 0;
      if (alarmValues[2] > 60) alarmValues[2] = 60;
    }
  } else {
    // Main menu navigation
    menuIndex = (menuIndex + direction + 3) % 3;
  }
}

// ========================
// --- Button Press ---
// ========================
void handleButtonPress() {
  bool buttonPressed = digitalRead(SW) == LOW;
  if (buttonPressed && millis() - lastButtonPress > buttonDebounce) {
    lastButtonPress = millis();

    if (alarmTriggered) {
      // Stop the alarm and vibration
      alarmTriggered = false;
      stopVibration();
      Serial.println("Alarm Stopped by User");
    } else if (inMenu && !inSubMenu && !inAlarmMenu) {
      // Main menu selection
      if (menuIndex == 0) {
        inSubMenu = true;
        settingInitialized = false;
        setFieldIndex = 0;
      } else if (menuIndex == 1) {
        inAlarmMenu = true;
        alarmSettingInitialized = false;
        alarmFieldIndex = 0;
        alarmConfirmed = false; // Reset confirmation during setup
      } else {
        inMenu = false;
      }
    } else if (inSubMenu) {
      // Step through date/time fields
      setFieldIndex++;
      if (setFieldIndex >= 5) {
        rtc.adjust(DateTime(editValues[0], editValues[1], editValues[2], editValues[3], editValues[4]));
        setFieldIndex = 0;
        inSubMenu = false;
      }
    } else if (inAlarmMenu) {
      // Step through alarm fields
      alarmFieldIndex++;
      if (alarmFieldIndex >= 3) {
        alarmSet = true;
        alarmConfirmed = true; // Confirm the alarm after setup is complete
        alarmSettingInProgress = false;
        inAlarmMenu = false;
        Serial.println("Alarm Confirmed by User");
      }
    }
  }
}

// ========================
// --- Clock Display ---
// ========================
void showClock() {
  DateTime now = rtc.now();

  display.clearDisplay();
  // --- Time (Large Font) ---
  char timeStr[10];
  int hour = now.hour();
  String period = "AM";
  if (hour >= 12) period = "PM";
  if (hour > 12) hour -= 12;
  if (hour == 0) hour = 12;

  sprintf(timeStr, "%02d:%02d", hour, now.minute());

  display.setFont(&FreeMonoBold9pt7b);
  int16_t x, y;
  uint16_t w, h;
  display.getTextBounds(timeStr, 0, 0, &x, &y, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 3, 30);
  display.setTextColor(SSD1306_WHITE);
  display.print(timeStr);

  // --- AM/PM ---
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextSize(1);
  display.setCursor((SCREEN_WIDTH - w) / 3 + w + 6, 30); // next to time
  display.print(period);

  // --- Separator/Line ---
  display.setFont();
  display.setTextSize(1);
  display.setCursor((SCREEN_WIDTH - w) / 2, 30);
  display.print("_________");

  // --- Date (Small Under Time) ---
  char dateStr[20];
  sprintf(dateStr, "%02d/%02d/%04d", now.month(), now.day(), now.year());
  display.setCursor((SCREEN_WIDTH - strlen(dateStr) * 6) / 2, 40);  // centered
  display.print(dateStr);

  // --- Menu Placeholder ---
  display.setCursor(0, 0);
  display.print("> Menu");

  display.drawLine(0, 56, 0, 63, SSD1306_WHITE); // Vertical separator

  // --- Alarm status ---
  if (alarmSet) {
    display.setCursor(5, 56);
    display.print("Alarm: ");
    int alarmHour = alarmValues[0];
    String alarmPeriod = "AM";
    if (alarmHour >= 12) alarmPeriod = "PM";
    if (alarmHour > 12) alarmHour -= 12;
    if (alarmHour == 0) alarmHour = 12;
    display.print(alarmHour);
    display.print(":");
    if (alarmValues[1] < 10) display.print("0");
    display.print(alarmValues[1]);
    display.print(" ");
    display.print(alarmPeriod);
  } else {
    display.setCursor(5, 56);
    display.print("No Alarm Set");
  }
  display.display();
}

// ========================
// --- Menu Display ---
// ========================
void showMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  String options[] = {"Set Date/Time", "Set Alarm", "Back"};
  for (int i = 0; i < 3; i++) {
    display.setCursor(0, i * 12);
    if (i == menuIndex) display.print("> ");
    else display.print("  ");
    display.print(options[i]);
  }
  display.display();
}

// ========================
// --- Date/Time Setup ---
// ========================
void setDateTime() {
  if (!settingInitialized) {
    DateTime now = rtc.now();
    editValues[0] = now.year();
    editValues[1] = now.month();
    editValues[2] = now.day();
    editValues[3] = now.hour();
    editValues[4] = now.minute();
    settingInitialized = true;
  }
  display.clearDisplay();
  String fields[] = {"Year", "Month", "Day", "Hour", "Minute"};
  for (int i = 0; i < 5; i++) {
    display.setCursor(0, i * 12);
    if (i == setFieldIndex) display.print("> ");
    else display.print("  ");
    display.print(fields[i]);
    display.print(": ");
    display.print(editValues[i]);
  }
  display.display();
}

// ========================
// --- Alarm Setup ---
// ========================
void setAlarm() {
  if (!alarmSettingInitialized) {
    alarmValues[0] = 6;  // Default hour
    alarmValues[1] = 30; // Default minute
    alarmValues[2] = 10; // Default ramp time (seconds)
    alarmSettingInitialized = true;
    alarmSettingInProgress = true;
  }
  display.clearDisplay();
  String fields[] = {"Hour", "Minute", "Ramp (s)"};
  for (int i = 0; i < 3; i++) {
    display.setCursor(0, i * 12);
    if (i == alarmFieldIndex) display.print("> ");
    else display.print("  ");
    display.print(fields[i]);
    display.print(": ");
    if (i == 0) { // Display alarm hour in 12-hour format
      int alarmHour = alarmValues[0];
      String alarmPeriod = "AM";
      if (alarmHour >= 12) alarmPeriod = "PM";
      if (alarmHour > 12) alarmHour -= 12;
      if (alarmHour == 0) alarmHour = 12;
      display.print(alarmHour);
      display.print(" ");
      display.print(alarmPeriod);
    } else {
      display.print(alarmValues[i]);
    }
  }
  display.display();
}

// ========================
// --- Motor Functions ---
// ========================
// Gradually increase vibration motor speed for alarm
void startVibration() {
  if (!alarmTriggered) return;

  int rampTime = alarmValues[2];
  for (int i = 0; i <= rampTime; i++) {
    int motorSpeed = map(i, 0, rampTime, 0, 255);
    controlMotors(motorSpeed);
    delay(1000);
  }
}

// Stop vibration
void stopVibration() {
  controlMotors(0);
}

// Motor driver logic for vibration
void controlMotors(int motorSpeed) {
  digitalWrite(EEP_PIN, HIGH);
  if (motorSpeed > 0) {
    digitalWrite(IN1, HIGH);
    digitalWrite(IN2, LOW);
    digitalWrite(IN3, HIGH);
    digitalWrite(IN4, LOW);
  } else {
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
    digitalWrite(IN3, LOW);
    digitalWrite(IN4, LOW);
  }
}