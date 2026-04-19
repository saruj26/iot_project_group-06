
#define BLYNK_TEMPLATE_ID "TMPL64YFSAmIT"
#define BLYNK_TEMPLATE_NAME "Smart Fan"
#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <DHTesp.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <ESP32Servo.h>

// ------------------ BLYNK / WIFI ------------------
char auth[] = "lFisIbe9jVm8c4FZGXNRmLiQfPOsa8Uc";

// Wokwi wifi
char ssid[] = "Wokwi-GUEST";
char pass[] = "";

// ------------------ PINS ------------------
#define DHT_PIN 23
#define RELAY_PIN 2
#define MOTION_SENSOR_PIN 4
#define SERVO_PIN 26

// A4988
#define STEP_PIN 18
#define DIR_PIN 19
#define EN_PIN 5

// ------------------ BLYNK VPINS ------------------
#define VPIN_HEATER       V1
#define VPIN_TEMP         V2
#define VPIN_FAN_SPEED    V3
#define VPIN_MODE_SWITCH  V4
#define VPIN_FAN_CONTROL  V5
#define VPIN_DOOR_SWITCH  V6
#define VPIN_DOOR_PIN     V7
#define VPIN_NOTIFY       V8

// ------------------ OBJECTS ------------------
BlynkTimer timer;
DHTesp dht;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo doorServo;

// ------------------ KEYPAD ------------------
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {13, 12, 14, 15};
byte colPins[COLS] = {27, 25, 33, 32};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ------------------ VARIABLES ------------------
bool automaticMode = true;
int manualFanSpeed = 0;
bool motionDetected = false;
bool heaterState = false;
int fanSpeedPercent = 0;
float currentTemperature = 0.0;

String enteredPIN = "";
String correctPIN = "1234";

bool doorOpen = false;
bool doorMoving = false;
unsigned long doorOpenTime = 0;
const unsigned long DOOR_AUTO_CLOSE_TIME = 5000;

unsigned int wrongPasswordAttempts = 0;
unsigned long lastWrongPasswordTime = 0;
const unsigned long PASSWORD_COOLDOWN = 30000;
const int MAX_WRONG_ATTEMPTS = 3;

int stepDelayMicros = 0;
unsigned long lastStepperStep = 0;

// ------------------ NOTIFICATIONS ------------------
void sendDoorOpenNotification() {
  Blynk.virtualWrite(VPIN_NOTIFY, "Door opened");
}

void sendDoorCloseNotification() {
  Blynk.virtualWrite(VPIN_NOTIFY, "Door closed");
}

void sendInvalidPasswordNotification() {
  String message = "Invalid password ";
  message += String(wrongPasswordAttempts);
  message += "/";
  message += String(MAX_WRONG_ATTEMPTS);
  Blynk.virtualWrite(VPIN_NOTIFY, message);
}

void sendSystemUnlockedNotification() {
  Blynk.virtualWrite(VPIN_NOTIFY, "System unlocked");
}

// ------------------ SETUP ------------------
void setup() {
  Serial.begin(115200);

  dht.setup(DHT_PIN, DHTesp::DHT22);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Initializing...");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  pinMode(MOTION_SENSOR_PIN, INPUT);

  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);

  digitalWrite(DIR_PIN, HIGH);
  digitalWrite(EN_PIN, HIGH); // stepper disabled initially

  // Servo
  doorServo.attach(SERVO_PIN);
  doorServo.write(0);

  Blynk.begin(auth, ssid, pass);

  Blynk.virtualWrite(VPIN_MODE_SWITCH, 0);
  Blynk.virtualWrite(VPIN_FAN_CONTROL, 0);
  Blynk.virtualWrite(VPIN_HEATER, 0);
  Blynk.virtualWrite(VPIN_DOOR_SWITCH, 0);
  Blynk.virtualWrite(VPIN_NOTIFY, "System Started");

  timer.setInterval(2000L, updateSensorData);
  timer.setInterval(500L, updateLCD);
  timer.setInterval(100L, checkDoorAutoClose);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Ready");
  delay(1000);
}

// ------------------ SENSOR ------------------
float readTemperature() {
  TempAndHumidity data = dht.getTempAndHumidity();
  if (isnan(data.temperature)) {
    Serial.println("Failed to read temperature!");
    return -999;
  }
  return data.temperature;
}

// ------------------ FAN CONTROL ------------------
void controlFan() {
  if (automaticMode) {
    if (motionDetected) {
      if (currentTemperature > 35) fanSpeedPercent = 100;
      else if (currentTemperature > 30) fanSpeedPercent = 75;
      else if (currentTemperature > 25) fanSpeedPercent = 50;
      else if (currentTemperature > 20) fanSpeedPercent = 25;
      else fanSpeedPercent = 0;
    } else {
      fanSpeedPercent = 0;
    }
  } else {
    fanSpeedPercent = manualFanSpeed;
  }

  if (fanSpeedPercent == 0) {
    digitalWrite(EN_PIN, HIGH);
    stepDelayMicros = 0;
  } else {
    digitalWrite(EN_PIN, LOW);

    if (fanSpeedPercent <= 25) stepDelayMicros = 3000;
    else if (fanSpeedPercent <= 50) stepDelayMicros = 2000;
    else if (fanSpeedPercent <= 75) stepDelayMicros = 1200;
    else stepDelayMicros = 700;
  }

  Blynk.virtualWrite(VPIN_FAN_SPEED, fanSpeedPercent);
}

// ------------------ HEATER CONTROL ------------------
void controlHeater() {
  if (automaticMode) {
    heaterState = (currentTemperature < 20 && motionDetected);
  }

  digitalWrite(RELAY_PIN, heaterState ? HIGH : LOW);
  Blynk.virtualWrite(VPIN_HEATER, heaterState ? 1 : 0);
}

// ------------------ UPDATE SENSOR DATA ------------------
void updateSensorData() {
  currentTemperature = readTemperature();
  if (currentTemperature == -999) return;

  Blynk.virtualWrite(VPIN_TEMP, currentTemperature);

  Serial.print("Temperature: ");
  Serial.print(currentTemperature);
  Serial.print(" C | Motion: ");
  Serial.println(motionDetected ? "YES" : "NO");

  controlFan();
  controlHeater();
}

// ------------------ RUN STEPPER ------------------
void runStepperFan() {
  if (stepDelayMicros <= 0) return;

  unsigned long now = micros();
  if (now - lastStepperStep >= (unsigned long)stepDelayMicros) {
    lastStepperStep = now;
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(5);
    digitalWrite(STEP_PIN, LOW);
  }
}

// ------------------ LCD ------------------
void updateLCD() {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("Temp:");
  lcd.print(currentTemperature, 1);
  lcd.print("C");

  lcd.setCursor(0, 1);
  lcd.print("Fan:");
  lcd.print(fanSpeedPercent);
  lcd.print("%");

  if (doorOpen) {
    lcd.setCursor(12, 1);
    lcd.print("OPEN");
  } else if (heaterState) {
    lcd.setCursor(12, 1);
    lcd.print("HEAT");
  } else {
    lcd.setCursor(12, 1);
    lcd.print("CLSD");
  }
}

// ------------------ PASSWORD ------------------
void checkPasswordCooldown() {
  if (wrongPasswordAttempts >= MAX_WRONG_ATTEMPTS) {
    if (millis() - lastWrongPasswordTime >= PASSWORD_COOLDOWN) {
      wrongPasswordAttempts = 0;
      sendSystemUnlockedNotification();
    }
  }
}

// ------------------ DOOR ------------------
void openDoor() {
  if (!doorOpen && !doorMoving) {
    doorMoving = true;

    int oldFanSpeed = fanSpeedPercent;
    digitalWrite(EN_PIN, HIGH);
    stepDelayMicros = 0;

    doorServo.write(90);
    delay(800);

    doorOpen = true;
    doorOpenTime = millis();
    doorMoving = false;

    if (oldFanSpeed > 0) {
      fanSpeedPercent = oldFanSpeed;
      controlFan();
    }

    Serial.println("Door OPENED");
  }
}

void closeDoor() {
  if (doorOpen && !doorMoving) {
    doorMoving = true;

    int oldFanSpeed = fanSpeedPercent;
    digitalWrite(EN_PIN, HIGH);
    stepDelayMicros = 0;

    doorServo.write(0);
    delay(800);

    doorOpen = false;
    doorOpenTime = 0;
    doorMoving = false;

    if (oldFanSpeed > 0) {
      fanSpeedPercent = oldFanSpeed;
      controlFan();
    }

    Serial.println("Door CLOSED");
  }
}

void checkDoorAutoClose() {
  if (doorOpen && doorOpenTime > 0) {
    if (millis() - doorOpenTime >= DOOR_AUTO_CLOSE_TIME) {
      closeDoor();
      sendDoorCloseNotification();
    }
  }
}

// ------------------ BLYNK CALLBACKS ------------------
BLYNK_WRITE(VPIN_MODE_SWITCH) {
  automaticMode = (param.asInt() == 0);

  if (automaticMode) {
    manualFanSpeed = 0;
    Blynk.virtualWrite(VPIN_FAN_CONTROL, 0);
    Blynk.virtualWrite(VPIN_NOTIFY, "Mode: Automatic");
  } else {
    Blynk.virtualWrite(VPIN_NOTIFY, "Mode: Manual");
  }
}

BLYNK_WRITE(VPIN_FAN_CONTROL) {
  manualFanSpeed = param.asInt();
  if (!automaticMode) controlFan();
}

BLYNK_WRITE(VPIN_HEATER) {
  int buttonState = param.asInt();

  if (!automaticMode) {
    heaterState = (buttonState == 1);
    digitalWrite(RELAY_PIN, heaterState ? HIGH : LOW);
  }
}

BLYNK_WRITE(VPIN_DOOR_SWITCH) {
  int switchState = param.asInt();

  if (switchState == 1 && !doorOpen) {
    Blynk.virtualWrite(VPIN_NOTIFY, "Enter password to open door");
    Blynk.virtualWrite(VPIN_DOOR_SWITCH, 0);
  } else if (switchState == 0 && doorOpen) {
    closeDoor();
    sendDoorCloseNotification();
  }
}

BLYNK_WRITE(VPIN_DOOR_PIN) {
  String inputPIN = param.asStr();

  checkPasswordCooldown();

  if (wrongPasswordAttempts >= MAX_WRONG_ATTEMPTS) {
    Blynk.virtualWrite(VPIN_NOTIFY, "System locked. Try again later");
    Blynk.virtualWrite(VPIN_DOOR_PIN, "");
    return;
  }

  if (inputPIN == correctPIN) {
    wrongPasswordAttempts = 0;

    if (!doorOpen) {
      openDoor();
      sendDoorOpenNotification();
    } else {
      closeDoor();
      sendDoorCloseNotification();
    }

    Blynk.virtualWrite(VPIN_DOOR_PIN, "");
    Blynk.virtualWrite(VPIN_DOOR_SWITCH, doorOpen ? 1 : 0);
  } else {
    wrongPasswordAttempts++;
    lastWrongPasswordTime = millis();

    sendInvalidPasswordNotification();
    Blynk.virtualWrite(VPIN_DOOR_PIN, "");
    Blynk.virtualWrite(VPIN_DOOR_SWITCH, 0);
  }
}

// ------------------ KEYPAD ------------------
void handleKeypad() {
  char key = keypad.getKey();
  if (!key) return;

  checkPasswordCooldown();

  if (key == '#') {
    if (wrongPasswordAttempts >= MAX_WRONG_ATTEMPTS) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("System Locked");
      lcd.setCursor(0, 1);
      lcd.print("Wait 30 sec");
      delay(1500);
      enteredPIN = "";
      return;
    }

    if (enteredPIN == correctPIN) {
      wrongPasswordAttempts = 0;

      if (doorOpen) {
        closeDoor();
        sendDoorCloseNotification();
      } else {
        openDoor();
        sendDoorOpenNotification();
      }
    } else {
      wrongPasswordAttempts++;
      lastWrongPasswordTime = millis();
      sendInvalidPasswordNotification();

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Wrong PIN");
      delay(1000);
    }
    enteredPIN = "";
  } else if (key == '*') {
    enteredPIN = "";
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("PIN cleared");
    delay(500);
  } else if (key >= '0' && key <= '9') {
    if (enteredPIN.length() < 4) {
      enteredPIN += key;
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("PIN: ");
      for (int i = 0; i < enteredPIN.length(); i++) {
        lcd.print("*");
      }
    }
  }
}

// ------------------ MOTION ------------------
void checkMotionSensor() {
  bool newMotion = digitalRead(MOTION_SENSOR_PIN);

  if (newMotion != motionDetected) {
    motionDetected = newMotion;
    Serial.print("Motion: ");
    Serial.println(motionDetected ? "DETECTED" : "NO MOTION");

    if (automaticMode) {
      controlFan();
      controlHeater();
    }
  }
}

// ------------------ LOOP ------------------
void loop() {
  Blynk.run();
  timer.run();

  checkMotionSensor();
  handleKeypad();
  runStepperFan();

  static unsigned long lastCooldownCheck = 0;
  if (millis() - lastCooldownCheck > 1000) {
    checkPasswordCooldown();
    lastCooldownCheck = millis();
  }

  delay(2);
}