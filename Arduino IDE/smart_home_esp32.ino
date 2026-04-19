#define BLYNK_TEMPLATE_ID "TMPL64YFSAmIT"
#define BLYNK_TEMPLATE_NAME "Smart Fan"
#define BLYNK_PRINT Serial
char auth[] = "lFisIbe9jVm8c4FZGXNRmLiQfPOsa8Uc";

char ssid[] = "Dialog 4G 2611"; // Name
char pass[] = "D4TBF1542QR"; // Password

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h> 
#include <Keypad.h>
#include <ESP32Servo.h>

// ------------------ DEFINES ------------------
#define DHTPIN 23 
#define DHTTYPE DHT11

#define HEATER_PIN 2  // Heater pin
#define MOTION_SENSOR_PIN 4  // Motion sensor 

// L298 Motor Driver
#define ENA 5   
#define IN1 18  
#define IN2 19

#define SERVO_PIN 26

#define VPIN_HEATER V1  // Heater
#define VPIN_TEMP V2  // Temperature
#define VPIN_FAN_SPEED V3  // Fan speed gauge
#define VPIN_MODE_SWITCH V4  // Mode (0=Auto, 1=Manual)
#define VPIN_FAN_CONTROL V5    // Fan speed control slider
#define VPIN_DOOR_SWITCH V6  // Manual door switch
#define VPIN_DOOR_PIN V7  // Blynk app password input
#define VPIN_NOTIFY V8    // Notification widget

// ------------------ OBJECTS ------------------
BlynkTimer timer;
DHT dht(DHTPIN, DHTTYPE);
Servo doorServo;
LiquidCrystal_I2C lcd(0x27, 16, 2);

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
bool automaticMode = true;  // Start in automatic mode
int manualFanSpeed = 0;     // Fan speed in manual mode (0-100%)
bool motionDetected = false; // Motion sensor state
bool heaterState = false;    // Heater on/off
int fanSpeedPercent = 0;     // Current fan speed percentage
float currentTemperature = 0.0;

String enteredPIN = "";
String correctPIN = "1234";
bool doorOpen = false;
bool doorMoving = false;  // Prevent overlapping servo commands
unsigned long doorOpenTime = 0;  // Track when door was opened
const unsigned long DOOR_AUTO_CLOSE_TIME = 5000; // 5 seconds

// Variables for invalid password attempts
unsigned int wrongPasswordAttempts = 0;
unsigned long lastWrongPasswordTime = 0;
const unsigned long PASSWORD_COOLDOWN = 30000; // 30 seconds cooldown
const int MAX_WRONG_ATTEMPTS = 3;

// ------------------ NOTIFICATION FUNCTIONS ------------------
void sendDoorOpenNotification() {
  String message = "🚪 Door opened via ";
  if (automaticMode) {
    message += "Auto mode";
  } else {
    message += "Manual mode";
  }
  Blynk.virtualWrite(V8, message);
  Blynk.logEvent("door_open", message);
}

void sendDoorCloseNotification() {
  String message = "🚪 Door closed";
  Blynk.virtualWrite(V8, message);
  Blynk.logEvent("door_close", message);
}

void sendInvalidPasswordNotification() {
  String message = "⚠️ Invalid password attempt! ";
  message += String(wrongPasswordAttempts);
  message += "/";
  message += String(MAX_WRONG_ATTEMPTS);
  message += " attempts";
  
  Blynk.virtualWrite(V8, message);
  Blynk.logEvent("security_alert", message);
  
  if (wrongPasswordAttempts >= MAX_WRONG_ATTEMPTS) {
    String lockoutMessage = "🔒 System locked! Too many wrong attempts. Try again in 30 seconds.";
    Blynk.virtualWrite(V8, lockoutMessage);
    Blynk.logEvent("system_locked", lockoutMessage);
  }
}

void sendSystemUnlockedNotification() {
  String message = "✅ System unlocked. You can try password again.";
  Blynk.virtualWrite(V8, message);
  Blynk.logEvent("system_unlocked", message);
}

// ------------------ SETUP ------------------
void setup() {
  Serial.begin(115200);

  // ========== Initialize servo FIRST ==========
  Serial.println("Initializing servo...");
  
  // Allocate timer for servo BEFORE anything else
  ESP32PWM::allocateTimer(0);
  
  // Simple servo initialization
  doorServo.attach(SERVO_PIN);
  delay(100);
  doorServo.write(0);  // Start at closed position
  delay(500);
  Serial.println("Servo initialized at 0 degrees");

  // Initialize Blynk and sensors
  Blynk.begin(auth, ssid, pass);
  dht.begin();

  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Initializing...");

  // Configure motor control pins
  pinMode(ENA, OUTPUT);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);

  // Ensure motor is stopped at startup
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  analogWrite(ENA, 0);  // Motor OFF

  // Configure other pins
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);

  pinMode(MOTION_SENSOR_PIN, INPUT);

  // Set initial Blynk states
  Blynk.virtualWrite(VPIN_MODE_SWITCH, automaticMode ? 0 : 1);  // 0=Auto, 1=Manual
  Blynk.virtualWrite(VPIN_FAN_CONTROL, manualFanSpeed);
  Blynk.virtualWrite(VPIN_HEATER, 0);
  Blynk.virtualWrite(VPIN_DOOR_SWITCH, 0);

  // Send initial notification
  Blynk.virtualWrite(V8, "✅ System Started Successfully");
  Blynk.logEvent("system_start", "Smart Fan System Started");

  // Send data to Blynk every 2 seconds
  timer.setInterval(2000L, updateSensorData);
  
  // Update LCD every 500ms
  timer.setInterval(500L, updateLCD);
  
  // Check door auto-close every 100ms
  timer.setInterval(100L, checkDoorAutoClose);
  
  lcd.clear();
  lcd.print("System Ready");
  delay(2000);
}

// ------------------ SENSOR FUNCTIONS ------------------
float readTemperature() {
  float temp = dht.readTemperature();
  if (isnan(temp)) {
    Serial.println("Failed to read temperature!");
    return -999;
  }
  return temp;
}

// ------------------ FAN CONTROL ------------------
void controlFan() {
  int pwmValue = 0;
  
  if (automaticMode) {
    // Automatic mode logic
    if (motionDetected) {
      if (currentTemperature > 35) {
        fanSpeedPercent = 100;
      } else if (currentTemperature > 30) {
        fanSpeedPercent = 75;
      } else if (currentTemperature > 25) {
        fanSpeedPercent = 50;
      } else if (currentTemperature > 20) {
        fanSpeedPercent = 25;
      } else {
        fanSpeedPercent = 0;
      }
    } else {
      // No motion in auto mode - fan off
      fanSpeedPercent = 0;
    }
  } else {
    // Manual mode - use manualFanSpeed directly
    fanSpeedPercent = manualFanSpeed;
  }

  // Convert percentage to PWM (0-255)
  pwmValue = map(fanSpeedPercent, 0, 100, 0, 255);
  
  // Control motor
  if (fanSpeedPercent > 0) {
    digitalWrite(IN1, HIGH);
    digitalWrite(IN2, LOW);
    analogWrite(ENA, pwmValue);
  } else {
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
    analogWrite(ENA, 0);
  }

  // Send fan speed to Blynk gauge
  Blynk.virtualWrite(VPIN_FAN_SPEED, fanSpeedPercent);
  
  Serial.print("Fan Speed: ");
  Serial.print(fanSpeedPercent);
  Serial.println("%");
}

// ------------------ HEATER CONTROL ------------------
void controlHeater() {
  if (automaticMode) {
    // Auto mode: Heater ON when temp < 20°C AND motion detected
    if (currentTemperature < 20 && motionDetected) {
      digitalWrite(HEATER_PIN, HIGH);
      heaterState = true;
    } else {
      digitalWrite(HEATER_PIN, LOW);
      heaterState = false;
    }
  }
  // In manual mode, heater is controlled by Blynk app
  
  Blynk.virtualWrite(VPIN_HEATER, heaterState ? 1 : 0);
}

// ------------------ UPDATE SENSOR DATA ------------------
void updateSensorData() {
  currentTemperature = readTemperature();
  
  if (currentTemperature == -999) {
    return; // Skip if sensor read failed
  }

  // Send temperature to Blynk
  Blynk.virtualWrite(VPIN_TEMP, currentTemperature);
  
  Serial.print("Temperature: ");
  Serial.print(currentTemperature);
  Serial.println(" °C");

  // Update fan and heater based on mode
  controlFan();
  controlHeater();
}

// ------------------ UPDATE LCD ------------------
void updateLCD() {
  lcd.clear();
  
  // Line 1: Temperature
  lcd.setCursor(0, 0);
  lcd.print("Temp:");
  lcd.print(currentTemperature, 1);
  lcd.print("C");
  
  // Line 2: Fan Speed and Mode
  lcd.setCursor(0, 1);
  lcd.print("Fan:");
  lcd.print(fanSpeedPercent);
  lcd.print("% ");
  lcd.print(automaticMode ? "Auto" : "Man");
  
  // If door is open, show on second line
  if (doorOpen) {
    lcd.setCursor(12, 1);
    lcd.print("OPEN");
  }
}

// ------------------ DOOR AUTO-CLOSE CHECK ------------------
void checkDoorAutoClose() {
  if (doorOpen && doorOpenTime > 0) {
    unsigned long currentTime = millis();
    if (currentTime - doorOpenTime >= DOOR_AUTO_CLOSE_TIME) {
      closeDoor();
      Serial.println("Door auto-closed after 5 seconds");
    }
  }
}

// ------------------ CHECK PASSWORD COOLDOWN ------------------
void checkPasswordCooldown() {
  if (wrongPasswordAttempts >= MAX_WRONG_ATTEMPTS) {
    unsigned long currentTime = millis();
    if (currentTime - lastWrongPasswordTime >= PASSWORD_COOLDOWN) {
      wrongPasswordAttempts = 0;
      sendSystemUnlockedNotification();
      Serial.println("Password cooldown reset. System unlocked.");
    }
  }
}

// ------------------ BLYNK CALLBACKS ------------------
BLYNK_WRITE(VPIN_MODE_SWITCH) {
  automaticMode = (param.asInt() == 0);  // 0 = Auto, 1 = Manual
  
  if (automaticMode) {
    Serial.println("Mode: Automatic");
    Blynk.virtualWrite(V8, "Mode changed to: Automatic");
    // Reset manual controls when switching to auto
    manualFanSpeed = 0;
    Blynk.virtualWrite(VPIN_FAN_CONTROL, 0);
  } else {
    Serial.println("Mode: Manual");
    Blynk.virtualWrite(V8, "Mode changed to: Manual");
  }
}

BLYNK_WRITE(VPIN_FAN_CONTROL) {
  manualFanSpeed = param.asInt();  // Get value 0-100 from slider
  
  if (!automaticMode) {
    Serial.print("Manual Fan Speed Set: ");
    Serial.print(manualFanSpeed);
    Serial.println("%");
    
    // Immediately update fan in manual mode
    controlFan();
  }
}

BLYNK_WRITE(VPIN_HEATER) {
  int buttonState = param.asInt(); 
  
  // Heater control only in manual mode
  if (!automaticMode) {
    heaterState = (buttonState == 1);
    digitalWrite(HEATER_PIN, heaterState ? HIGH : LOW);
    
    Serial.print("Manual Heater: ");
    Serial.println(heaterState ? "ON" : "OFF");
  }
}

BLYNK_WRITE(VPIN_DOOR_SWITCH) {
  // Manual door control from Blynk - FIXED VERSION
  int switchState = param.asInt();
  Serial.print("Door switch state: ");
  Serial.println(switchState);
  
  if (switchState == 1 && !doorOpen) {
    // Door should open - check password
    Blynk.virtualWrite(V8, "Enter password to open door...");
    // Don't open directly - wait for password
    Blynk.virtualWrite(VPIN_DOOR_SWITCH, 0); // Reset switch
  } else if (switchState == 0 && doorOpen) {
    // Door should close
    closeDoor();
  }
}

BLYNK_WRITE(VPIN_DOOR_PIN) {
  String inputPIN = param.asStr();
  Serial.print("Blynk PIN entered: ");
  Serial.println(inputPIN);
  
  // Check if system is locked
  checkPasswordCooldown();
  
  if (wrongPasswordAttempts >= MAX_WRONG_ATTEMPTS) {
    Blynk.virtualWrite(V8, "🔒 System locked! Try again in 30 seconds.");
    Blynk.virtualWrite(VPIN_DOOR_PIN, ""); // Clear the input field
    return;
  }
  
  if (inputPIN == correctPIN) {
    // Correct password
    wrongPasswordAttempts = 0; // Reset attempts
    
    if (!doorOpen) {
      openDoor();
      sendDoorOpenNotification();
    } else {
      closeDoor();
      sendDoorCloseNotification();
    }
    
    Blynk.virtualWrite(VPIN_DOOR_PIN, ""); // Clear the input field
    Blynk.virtualWrite(VPIN_DOOR_SWITCH, doorOpen ? 1 : 0); // Update switch
  } else {
    // Wrong password
    wrongPasswordAttempts++;
    lastWrongPasswordTime = millis();
    
    Serial.print("Wrong PIN! Attempt ");
    Serial.print(wrongPasswordAttempts);
    Serial.print("/");
    Serial.println(MAX_WRONG_ATTEMPTS);
    
    sendInvalidPasswordNotification();
    Blynk.virtualWrite(VPIN_DOOR_PIN, ""); // Clear the input field
    Blynk.virtualWrite(VPIN_DOOR_SWITCH, 0); // Reset switch to closed position
  }
}

// ------------------ DOOR FUNCTIONS ------------------
void openDoor() {
  if (!doorOpen && !doorMoving) {
    doorMoving = true;
    Serial.println("OPENING DOOR...");
    
    // IMPORTANT: Stop fan while moving servo to reduce power load
    int tempFanSpeed = fanSpeedPercent;
    fanSpeedPercent = 0;
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
    analogWrite(ENA, 0);
    
    // Move servo to open position
    doorServo.write(90);
    delay(800);  // Give enough time for servo to move
    
    doorOpen = true;
    doorOpenTime = millis();
    doorMoving = false;
    
    // Restore fan speed if needed
    if (tempFanSpeed > 0) {
      fanSpeedPercent = tempFanSpeed;
      controlFan();
    }
    
    Serial.println("Door OPENED");
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Door Opened");
    delay(300);
  }
}

void closeDoor() {
  if (doorOpen && !doorMoving) {
    doorMoving = true;
    Serial.println("CLOSING DOOR...");
    
    // IMPORTANT: Stop fan while moving servo to reduce power load
    int tempFanSpeed = fanSpeedPercent;
    fanSpeedPercent = 0;
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
    analogWrite(ENA, 0);
    
    // Move servo to closed position
    doorServo.write(0);
    delay(800);  // Give enough time for servo to move
    
    doorOpen = false;
    doorOpenTime = 0;
    doorMoving = false;
    
    // Restore fan speed if needed
    if (tempFanSpeed > 0) {
      fanSpeedPercent = tempFanSpeed;
      controlFan();
    }
    
    Serial.println("Door CLOSED");
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Door Closed");
    delay(300);
  }
}

// ------------------ KEYPAD HANDLING ------------------
void handleKeypad() {
  char key = keypad.getKey();
  
  if (key) {
    Serial.print("Key pressed: ");
    Serial.println(key);
    
    // Check if system is locked
    checkPasswordCooldown();
    
    if (key == '#') {
      // Enter key - check password
      Serial.print("Checking PIN: ");
      Serial.println(enteredPIN);
      
      if (wrongPasswordAttempts >= MAX_WRONG_ATTEMPTS) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("System Locked!");
        lcd.setCursor(0, 1);
        lcd.print("Wait 30 sec");
        delay(2000);
        enteredPIN = "";
        return;
      }
      
      if (enteredPIN == correctPIN) {
        // Correct password
        wrongPasswordAttempts = 0; // Reset attempts
        
        Serial.println("Correct PIN - Toggling Door");
        if (doorOpen) {
          closeDoor();
          sendDoorCloseNotification();
        } else {
          openDoor();
          sendDoorOpenNotification();
        }
      } else {
        // Wrong password
        wrongPasswordAttempts++;
        lastWrongPasswordTime = millis();
        
        Serial.print("Wrong PIN! Attempt ");
        Serial.print(wrongPasswordAttempts);
        Serial.print("/");
        Serial.println(MAX_WRONG_ATTEMPTS);
        
        sendInvalidPasswordNotification();
        
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Wrong PIN!");
        lcd.setCursor(0, 1);
        lcd.print("Try:");
        lcd.print(wrongPasswordAttempts);
        lcd.print("/");
        lcd.print(MAX_WRONG_ATTEMPTS);
        delay(2000);
      }
      enteredPIN = "";
      
    } else if (key == '*') {
      // Clear key
      enteredPIN = "";
      Serial.println("PIN cleared");
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("PIN cleared");
      delay(1000);
      
    } else if (key >= '0' && key <= '9') {
      // Number key
      if (enteredPIN.length() < 4) {
        enteredPIN += key;
        Serial.print("Current PIN: ");
        Serial.println(enteredPIN);
        
        // Show asterisks on LCD
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("PIN: ");
        for (int i = 0; i < enteredPIN.length(); i++) {
          lcd.print("*");
        }
      } else {
        Serial.println("PIN too long!");
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("PIN too long!");
        delay(1000);
      }
    }
  }
}

// ------------------ MOTION SENSOR ------------------
void checkMotionSensor() {
  bool newMotion = digitalRead(MOTION_SENSOR_PIN);
  
  if (newMotion != motionDetected) {
    motionDetected = newMotion;
    Serial.print("Motion: ");
    Serial.println(motionDetected ? "DETECTED" : "NO MOTION");
    
    // In auto mode, update controls when motion changes
    if (automaticMode) {
      controlFan();
      controlHeater();
    }
  }
}

// ------------------ MAIN LOOP ------------------
void loop() {
  Blynk.run();  
  timer.run();  
  
  checkMotionSensor();
  handleKeypad();
  
  // Check password cooldown periodically
  static unsigned long lastCooldownCheck = 0;
  if (millis() - lastCooldownCheck > 1000) {
    checkPasswordCooldown();
    lastCooldownCheck = millis();
  }
  
  // Optional: Add a small delay to prevent overwhelming the ESP32
  delay(10);
}