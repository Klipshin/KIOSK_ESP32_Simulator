#include <ArduinoJson.h>

// Forward Declarations for PlatformIO/C++ Compiler
void calculateChange();
void resetTransaction();
void dispenseChange();
bool dispenseCoins(String hopperType, byte relayPin, byte sensorPin, uint16_t targetCoins, byte motorSpeed);
void sendPaymentToKiosk(String deviceType, int amount);
void sendStatusToKiosk(String status, int credit, int change);
void addCredit(int value, String source);
void decodeCoins(int pulses);
void decodeBills(int pulses);
void setItemPrice(int price);
void printStatus();
void processSerialCommand(String command);

// ------------------------------------------------------------
// Communication: USB Serial (EVT: JSON protocol)
// The ESP32 sends events to the Node.js server via USB serial.
// Prefix "EVT:" marks machine-readable JSON; other lines are human debug logs.
// The server also writes commands back: SET_PRICE:<n>, RESET, STATUS
// ------------------------------------------------------------

// ------------------------------------------------------------
// Pin Configurations (Physical ESP32 Setup)
// ------------------------------------------------------------
// INPUTS (Hardware Interrupt Pins)
const byte COIN_SLOT_PIN = 27; // Signal line from Multi-Coin Selector (Pulse Mode)
const byte BILL_ACC_PIN = 14;  // Signal line from Bill Acceptor (Pulse Mode)

// HARDWARE TEST PIN CONFIGURATION (Hoppers & Sensors - REMAPPED FOR ESP32 HARDWARE SAFETY)
const byte HOPPER_1_SENSOR_PIN = 21; // 1 Peso Hopper Sensor (Input pin - REQUIRES external pull-up resistor!)
const byte HOPPER_1_RELAY_PIN = 19;  // 1 Peso Hopper Motor Relay (Output pin - GPIO 25 avoided as it is a DAC pin!)
const byte HOPPER_10_RELAY_PIN = 13; // 10 Peso Hopper Motor Relay (Output pin - changed from 32 due to XTAL caps)
const byte HOPPER_10_SENSOR_PIN = 4; // 10 Peso Hopper Sensor (Input pin - changed from 33 due to XTAL caps)

// PWM Speed for Hoppers (0 to 255) - increase if coins are sticking mid-dispense
const byte HOPPER_1_SPEED = 220;  // ~86% speed for 1 Peso Hopper (SSR-25)
const byte HOPPER_10_SPEED = 220; // ~86% speed for 10 Peso Hopper (SSR-40)

const byte TEST_DISPENSE_BTN = 26;
const byte STATUS_LED_PIN = 2; // Onboard ESP32 Status LED

// ------------------------------------------------------------
// Shared Volatile Variables (Interrupt Protected)
// ------------------------------------------------------------
volatile int coinPulseCount = 0;
volatile unsigned long lastCoinPulseTime = 0;
volatile unsigned long lastCoinInterruptTimeUs = 0;

volatile int billPulseCount = 0;
volatile unsigned long lastBillPulseTime = 0;
volatile unsigned long lastBillPulseTimeUs = 0;

// Hardware Debounce constraints
const unsigned long debounceTimeUs = 40000; // 40ms filter for hardware switch bouncing
const unsigned long coinTimeout = 600;      // Wait 600ms after last pulse to aggregate coin value
const unsigned long billTimeout = 500;      // Wait 500ms after last pulse to aggregate bill value
const unsigned long DISPENSE_TIMEOUT = 15000;

// Transaction State Machine
int totalCredit = 0;
int itemPrice = 0;
enum TransactionState
{
  IDLE,
  AWAITING_PAYMENT,
  CALCULATING_CHANGE,
  DISPENSING_CHANGE,
  COMPLETED
};
TransactionState currentState = IDLE;

// Change Management Variables
int changeDue = 0;
int change10Coins = 0;
int change1Coins = 0;

// ------------------------------------------------------------
// Interrupt Service Routines (ISRs)
// ------------------------------------------------------------
void IRAM_ATTR coinISR()
{
  unsigned long nowUs = micros();
  // Hardware noise filter
  if (nowUs - lastCoinInterruptTimeUs > debounceTimeUs)
  {
    coinPulseCount++;
    lastCoinPulseTime = nowUs / 1000;
    lastCoinInterruptTimeUs = nowUs;
  }
}

void IRAM_ATTR billISR()
{
  unsigned long nowUs = micros();
  if (nowUs - lastBillPulseTimeUs > 20000)
  { // 20ms separation check for fast bill pulses
    billPulseCount++;
    lastBillPulseTime = nowUs / 1000;
    lastBillPulseTimeUs = nowUs;
  }
}

// ------------------------------------------------------------
// Helper Functions
// ------------------------------------------------------------
String getTimestamp()
{
  unsigned long now = millis();
  unsigned long seconds = now / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;

  char timestamp[20];
  sprintf(timestamp, "%02lu:%02lu:%02lu", hours % 24, minutes % 60, seconds % 60);
  return String(timestamp);
}

// Emit a payment event to the server via USB serial.
void sendPaymentToKiosk(String deviceType, int amount)
{
  JsonDocument doc;
  doc["type"] = "payment";
  doc["device"] = deviceType;
  doc["amount"] = amount;
  doc["credit"] = totalCredit;
  doc["timestamp"] = getTimestamp();

  Serial.print("EVT:");
  serializeJson(doc, Serial);
  Serial.println();
}

// Emit a status event to the server via USB serial.
void sendStatusToKiosk(String status, int credit, int change)
{
  JsonDocument doc;
  doc["type"] = "status";
  doc["status"] = status;
  doc["credit"] = credit;
  doc["changeDue"] = change;
  doc["timestamp"] = getTimestamp();

  Serial.print("EVT:");
  serializeJson(doc, Serial);
  Serial.println();
}

void addCredit(int value, String source)
{
  totalCredit += value;
  Serial.printf("\n>>> ACCEPTED %s: PHP %d | Total Credit: PHP %d <<<\n",
                source.c_str(), value, totalCredit);

  sendPaymentToKiosk(source, value);

  if (currentState == AWAITING_PAYMENT)
  {
    if (totalCredit >= itemPrice)
    {
      Serial.printf("[STATUS] Ready to Pay! Total Credit: PHP %d | Item Price: PHP %d\n", totalCredit, itemPrice);
    }
    else
    {
      Serial.printf("[STATUS] Still need PHP %d more\n", itemPrice - totalCredit);
    }
  }
}

// ------------------------------------------------------------
// COIN/BILL PARSING LOGIC
// ------------------------------------------------------------
void decodeCoins(int pulses)
{
  // Matches typical configuration profiles: 1 pulse = ₱1, 5 pulses = ₱5, etc.
  int remaining = pulses;
  while (remaining > 0)
  {
    if (remaining >= 20)
    {
      addCredit(20, "coin_slot");
      remaining -= 20;
    }
    else if (remaining >= 10)
    {
      addCredit(10, "coin_slot");
      remaining -= 10;
    }
    else if (remaining >= 5)
    {
      addCredit(5, "coin_slot");
      remaining -= 5;
    }
    else
    {
      addCredit(1, "coin_slot");
      remaining -= 1;
    }
  }
}

void decodeBills(int pulses)
{
  int billValue = 0;
  String billDesc = "";

  if (pulses == 1)
  {
    billValue = 50;
    billDesc = "₱50 bill";
  }
  else if (pulses == 2)
  {
    billValue = 100;
    billDesc = "₱100 bill";
  }
  else if (pulses == 5)
  {
    billValue = 500;
    billDesc = "₱500 bill";
  }
  else if (pulses == 10)
  {
    billValue = 1000;
    billDesc = "₱1000 bill";
  }
  else
  {
    billValue = pulses * 10;
    billDesc = String(pulses) + " pulses (₱" + String(billValue) + ")";
  }

  Serial.printf("\n[BILL DETECTED] %s inserted\n", billDesc.c_str());
  addCredit(billValue, "bill_acceptor");
}

// ------------------------------------------------------------
// CHANGE CALCULATING & PHYSICAL DISPENSING
// ------------------------------------------------------------
void calculateChange()
{
  if (totalCredit < itemPrice)
  {
    Serial.println("[ERROR] Insufficient funds!");
    currentState = AWAITING_PAYMENT;
    sendStatusToKiosk("insufficient_funds", totalCredit, 0);
    return;
  }

  changeDue = totalCredit - itemPrice;

  if (changeDue == 0)
  {
    Serial.println("\n[TRANSACTION] Exact payment! No change needed.");
    currentState = COMPLETED;
    sendStatusToKiosk("completed", totalCredit, 0);
    resetTransaction();
    return;
  }

  Serial.printf("\n[CHANGE CALCULATION] Change due: PHP %d\n", changeDue);

  change10Coins = changeDue / 10;
  change1Coins = changeDue % 10;

  Serial.printf("[CHANGE] Dispensing: %d × ₱10 + %d × ₱1 = ₱%d\n",
                change10Coins, change1Coins, changeDue);

  currentState = DISPENSING_CHANGE;
  sendStatusToKiosk("dispensing_change", totalCredit, changeDue);

  dispenseChange();
}

void dispenseChange()
{
  Serial.println("\n[DISPENSE] Commencing parallel change dispensing...");

  // --- Parallel Hopper State Machine ---
  // Both hoppers run simultaneously in a shared polling loop.

  // Hopper state tracking
  uint16_t counted10 = 0;
  uint16_t counted1 = 0;
  bool done10 = (change10Coins == 0);
  bool done1 = (change1Coins == 0);
  bool error10 = false;
  bool error1 = false;

  bool idleState10 = digitalRead(HOPPER_10_SENSOR_PIN);
  bool idleState1 = digitalRead(HOPPER_1_SENSOR_PIN);
  bool lastState10 = idleState10;
  bool lastState1 = idleState1;

  unsigned long lastCount10 = millis();
  unsigned long lastCount1 = millis();
  unsigned long lastDebug = 0;

  unsigned long hopperStartTime = millis(); // used to stagger ₱1 hopper startup
  bool started1 = done1;                    // if ₱1 isn't needed, treat it as already started

  // Start ₱10 hopper immediately
  if (!done10)
  {
    Serial.printf("[DISPENSE] Activating ₱10 Hopper for %d coins (speed: %d)\n", change10Coins, HOPPER_10_SPEED);
    Serial.printf("[Hopper 10 PESO] Idle sensor state: %s\n", idleState10 == HIGH ? "HIGH" : "LOW");
    analogWrite(HOPPER_10_RELAY_PIN, HOPPER_10_SPEED);
  }
  // ₱1 hopper will be started inside the loop after a 500ms stagger
  // so the ₱10 sensor is polled continuously during that window.
  if (done1)
  {
    Serial.println("[DISPENSE] No ₱1 coins needed, skipping ₱1 Hopper.");
  }

  // Shared polling loop — exits when both hoppers are done or errored
  while (!done10 || !done1)
  {
    unsigned long now = millis();

    // --- Staggered ₱1 hopper startup (inside loop so ₱10 is always monitored) ---
    if (!started1 && (now - hopperStartTime >= 500UL))
    {
      started1 = true;
      Serial.printf("[DISPENSE] Activating ₱1 Hopper for %d coins (speed: %d)\n", change1Coins, HOPPER_1_SPEED);
      Serial.printf("[Hopper 1 PESO] Idle sensor state: %s\n", idleState1 == HIGH ? "HIGH" : "LOW");
      analogWrite(HOPPER_1_RELAY_PIN, HOPPER_1_SPEED);
      lastCount1 = now; // reset timeout baseline to the moment the motor actually starts
    }

    bool cur10 = digitalRead(HOPPER_10_SENSOR_PIN);
    bool cur1 = digitalRead(HOPPER_1_SENSOR_PIN);

    // --- 10 Peso Hopper counting ---
    if (!done10)
    {
      if (lastState10 == idleState10 && cur10 != idleState10)
      {
        counted10++;
        lastCount10 = now;
        Serial.printf("[Hopper 10 PESO] Counted: %d / %d\n", counted10, change10Coins);
        delay(20); // debounce
        if (counted10 >= change10Coins)
        {
          analogWrite(HOPPER_10_RELAY_PIN, 0);
          done10 = true;
          Serial.println("[Hopper 10 PESO] Target reached. Motor stopped.");
        }
      }
      lastState10 = cur10;

      if (now - lastCount10 > 5000UL)
      {
        analogWrite(HOPPER_10_RELAY_PIN, 0);
        done10 = true;

        // --- Fallback: convert remaining ₱10 coins to ₱1 coins ---
        int remaining10 = change10Coins - counted10;
        if (remaining10 > 0)
        {
          int extra1 = remaining10 * 10;
          change1Coins += extra1; // add the deficit to the ₱1 target
          Serial.printf("[FALLBACK] ₱10 hopper empty! %d coin(s) short — adding %d ₱1 coins to compensate.\n",
                        remaining10, extra1);

          if (done1)
          {
            // ₱1 hopper already completed its previous run (motor stopped) — restart it
            done1 = false;
            lastState1 = digitalRead(HOPPER_1_SENSOR_PIN);
            idleState1 = lastState1;
            lastCount1 = now;
            started1 = true;
            Serial.printf("[FALLBACK] Restarting ₱1 Hopper for %d total coins (speed: %d)\n",
                          change1Coins, HOPPER_1_SPEED);
            analogWrite(HOPPER_1_RELAY_PIN, HOPPER_1_SPEED);
          }
          else if (!started1)
          {
            // Stagger hasn't fired yet — start ₱1 immediately now
            started1 = true;
            lastCount1 = now;
            Serial.printf("[FALLBACK] Activating ₱1 Hopper for %d total coins (speed: %d)\n",
                          change1Coins, HOPPER_1_SPEED);
            analogWrite(HOPPER_1_RELAY_PIN, HOPPER_1_SPEED);
          }
          else
          {
            // Motor is still running — reset timeout so extra coins don't cause a false jam
            lastCount1 = now;
            Serial.printf("[FALLBACK] ₱1 Hopper still running. New target: %d coins.\n", change1Coins);
          }
        }
        else
        {
          // Timed out with 0 remaining — shouldn't normally happen, treat as error
          error10 = true;
          Serial.println("[CRITICAL ERROR] Hopper 10 PESO Safety Timeout!");
          sendStatusToKiosk("hardware_error_jam", totalCredit, changeDue);
        }
      }
    }

    // --- 1 Peso Hopper counting (only after motor has been started) ---
    if (!done1 && started1)
    {
      if (lastState1 == idleState1 && cur1 != idleState1)
      {
        counted1++;
        lastCount1 = now;
        Serial.printf("[Hopper 1 PESO] Counted: %d / %d\n", counted1, change1Coins);
        delay(20); // debounce
        if (counted1 >= change1Coins)
        {
          analogWrite(HOPPER_1_RELAY_PIN, 0);
          done1 = true;
          Serial.println("[Hopper 1 PESO] Target reached. Motor stopped.");
        }
      }
      lastState1 = cur1;

      if (now - lastCount1 > 5000UL)
      {
        analogWrite(HOPPER_1_RELAY_PIN, 0);
        error1 = true;
        done1 = true;
        Serial.println("[CRITICAL ERROR] Hopper 1 PESO Safety Timeout!");
        sendStatusToKiosk("hardware_error_jam", totalCredit, changeDue);
      }
    }

    // Periodic debug print for both sensors
    if (now - lastDebug > 500)
    {
      if (!done10)
        Serial.printf("[DEBUG] Hopper 10 PESO pin %d: %s (counted: %d)\n", HOPPER_10_SENSOR_PIN, cur10 == HIGH ? "HIGH" : "LOW", counted10);
      if (!done1)
        Serial.printf("[DEBUG] Hopper  1 PESO pin %d: %s (counted: %d)\n", HOPPER_1_SENSOR_PIN, cur1 == HIGH ? "HIGH" : "LOW", counted1);
      lastDebug = now;
    }

    delay(5);
  }

  // Safety: ensure both motors are stopped
  analogWrite(HOPPER_10_RELAY_PIN, 0);
  analogWrite(HOPPER_1_RELAY_PIN, 0);

  bool success = !error10 && !error1;
  if (success)
  {
    Serial.println("[DISPENSE] Both hoppers finished. Dispensing complete!");
    currentState = COMPLETED;
    sendStatusToKiosk("completed", totalCredit, changeDue);
  }
  else
  {
    Serial.println("[DISPENSE] One or more hoppers had an error. Aborted.");
  }

  delay(2000);
  resetTransaction();
}

bool dispenseCoins(String hopperType, byte relayPin, byte sensorPin, uint16_t targetCoins, byte motorSpeed)
{
  Serial.printf("\n[Hopper %s] Motor engaged... Target: %d\n", hopperType.c_str(), targetCoins);

  // Determine the sensor's idle state before starting the motor
  bool idleState = digitalRead(sensorPin);
  Serial.printf("[Hopper %s] Detected sensor idle state: %s\n",
                hopperType.c_str(), idleState == HIGH ? "HIGH" : "LOW");

  analogWrite(relayPin, motorSpeed); // Engage motor via SSR at configured PWM speed
  unsigned long lastCountTime = millis();
  unsigned long lastDebugPrint = 0;
  uint16_t counted = 0;
  bool lastSensorState = idleState;
  bool success = true;

  while (counted < targetCoins)
  {
    bool currentSensorState = digitalRead(sensorPin);

    // Detect departure from the idle state (transition from idleState to activeState)
    if (lastSensorState == idleState && currentSensorState != idleState)
    {
      counted++;
      lastCountTime = millis(); // Reset timeout since we detected a coin successfully
      Serial.printf("[Hopper %s] Counted: %d / %d\n", hopperType.c_str(), counted, targetCoins);
      delay(20); // Shorter debounce filter to prevent missing rapid coin drops
    }
    lastSensorState = currentSensorState;

    // Print sensor state periodically for debug visibility
    if (millis() - lastDebugPrint > 500)
    {
      Serial.printf("[DEBUG] Hopper %s sensor pin %d state: %s (counted: %d)\n",
                    hopperType.c_str(), sensorPin, currentSensorState == HIGH ? "HIGH" : "LOW", counted);
      lastDebugPrint = millis();
    }

    // Safety timeout: prevent motor burnout if hopper runs out of coins or jam occurs.
    // Allow 5 seconds of inactivity before shutting off.
    if (millis() - lastCountTime > 5000UL)
    {
      Serial.printf("[CRITICAL ERROR] Hopper %s Safety Timeout exceeded! No coin detected for 5 seconds.\n", hopperType.c_str());
      sendStatusToKiosk("hardware_error_jam", totalCredit, changeDue);
      success = false;
      break;
    }

    delay(5);
  }
  analogWrite(relayPin, 0); // Cut motor line
  return success;
}

void resetTransaction()
{
  Serial.println("\n[RESET] Transaction reset complete. Listening for terminal commands...");
  totalCredit = 0;
  changeDue = 0;
  change10Coins = 0;
  change1Coins = 0;
  currentState = IDLE;
  sendStatusToKiosk("idle", 0, 0);
}

void setItemPrice(int price)
{
  itemPrice = price;
  totalCredit = 0;
  currentState = AWAITING_PAYMENT;
  Serial.printf("\n[CONFIG] Target purchase price locked to PHP %d\n", itemPrice);
  sendStatusToKiosk("awaiting_payment", 0, 0);
}

void printStatus()
{
  Serial.println("\n=== [SYSTEM STATUS] ===");
  Serial.printf("State: ");
  switch (currentState)
  {
  case IDLE:
    Serial.println("IDLE");
    break;
  case AWAITING_PAYMENT:
    Serial.println("AWAITING_PAYMENT");
    break;
  case CALCULATING_CHANGE:
    Serial.println("CALCULATING_CHANGE");
    break;
  case DISPENSING_CHANGE:
    Serial.println("DISPENSING_CHANGE");
    break;
  case COMPLETED:
    Serial.println("COMPLETED");
    break;
  }
  Serial.printf("Item Price: PHP %d\n", itemPrice);
  Serial.printf("Total Credit: PHP %d\n", totalCredit);
  Serial.printf("Change Due: PHP %d\n", changeDue);
  Serial.println("Connection: USB Serial");
  Serial.println("======================\n");
}

void processSerialCommand(String command)
{
  command.trim();
  command.toUpperCase();

  if (command.startsWith("SET_PRICE:"))
  {
    String priceStr = command.substring(10);
    int price = priceStr.toInt();
    if (price > 0)
    {
      setItemPrice(price);
    }
    else
    {
      Serial.println("[ERROR] Invalid price entry.");
    }
  }
  else if (command.startsWith("ADD_CREDIT:"))
  {
    String creditStr = command.substring(11);
    int credit = creditStr.toInt();
    if (credit > 0)
    {
      addCredit(credit, "simulated_web");
    }
    else
    {
      Serial.println("[ERROR] Invalid credit entry.");
    }
  }
  else if (command.startsWith("DISPENSE:"))
  {
    String amountStr = command.substring(9);
    int amount = amountStr.toInt();
    if (amount >= 0)
    {
      changeDue = amount;
      change10Coins = changeDue / 10;
      change1Coins = changeDue % 10;
      Serial.printf("\n[COMMAND] Dispense requested: PHP %d\n", changeDue);
      Serial.printf("[CHANGE] Dispensing: %d × ₱10 + %d × ₱1 = ₱%d\n",
                    change10Coins, change1Coins, changeDue);
      currentState = DISPENSING_CHANGE;
      sendStatusToKiosk("dispensing_change", totalCredit, changeDue);
      dispenseChange();
    }
    else
    {
      Serial.println("[ERROR] Invalid dispense amount.");
    }
  }
  else if (command == "STATUS")
  {
    printStatus();
  }
  else if (command == "RESET")
  {
    resetTransaction();
  }
  else if (command == "HELP")
  {
    Serial.println("\n=== [AVAILABLE COMMANDS] ===");
    Serial.println("SET_PRICE:<amount>  - Set item price (e.g., SET_PRICE:150)");
    Serial.println("ADD_CREDIT:<amount> - Add simulated payment credit (e.g., ADD_CREDIT:50)");
    Serial.println("STATUS              - Show current transaction status");
    Serial.println("RESET               - Clear current transaction");
    Serial.println("HELP                - Show this help message");
    Serial.println("===========================\n");
  }
  else
  {
    Serial.println("[ERROR] Unknown command string.");
  }
}

// ------------------------------------------------------------
// Core Setup Loop
// ------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println("[BOOT] Initializing Hardware Layer...");

  // Setup input lines with internal pullup structures
  pinMode(COIN_SLOT_PIN, INPUT_PULLUP);
  pinMode(BILL_ACC_PIN, INPUT_PULLUP);
  pinMode(HOPPER_1_SENSOR_PIN, INPUT_PULLUP);
  pinMode(HOPPER_10_SENSOR_PIN, INPUT_PULLUP);
  pinMode(TEST_DISPENSE_BTN, INPUT_PULLUP);

  // Setup output relay driving pins
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(HOPPER_10_RELAY_PIN, OUTPUT);
  pinMode(HOPPER_1_RELAY_PIN, OUTPUT);

  // Configure slow PWM frequency on ESP32 for the Fotek Solid State Relays (150Hz)
  analogWriteFrequency(150);
  // Set default stable states (Motors off)
  analogWrite(HOPPER_10_RELAY_PIN, 0);
  analogWrite(HOPPER_1_RELAY_PIN, 0);
  digitalWrite(STATUS_LED_PIN, LOW);

  // Link physical hardware interrupt routines
  attachInterrupt(digitalPinToInterrupt(COIN_SLOT_PIN), coinISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BILL_ACC_PIN), billISR, FALLING);

  // USB Serial bridge ready — server reads EVT: JSON lines on this port
  digitalWrite(STATUS_LED_PIN, HIGH); // Solid on = ready
  Serial.println("[BOOT] USB Serial bridge active. Streaming events to server.");
  Serial.println("[BOOT] Physical system initialization finished. Awaiting instructions.");
}

void loop()
{
  unsigned long currentMillis = millis();
  static bool coinReceiving = false;
  static bool lastTestBtnState = HIGH;

  if (Serial.available() > 0)
  {
    String command = Serial.readStringUntil('\n');
    processSerialCommand(command);
  }

  // --- PULSE TRACKING ENGINE ---
  if (coinPulseCount > 0)
    coinReceiving = true;
  if (coinReceiving && (currentMillis - lastCoinPulseTime > coinTimeout))
  {
    noInterrupts(); // Enter atomic window to fetch counts securely
    int totalCoinPulses = coinPulseCount;
    coinPulseCount = 0;
    interrupts();

    if (currentState == AWAITING_PAYMENT || currentState == IDLE)
    {
      decodeCoins(totalCoinPulses);
    }
    coinReceiving = false;
  }

  if (billPulseCount > 0 && (currentMillis - lastBillPulseTime > billTimeout))
  {
    noInterrupts();
    int currentBillPulses = billPulseCount;
    billPulseCount = 0;
    interrupts();

    if (currentState == AWAITING_PAYMENT || currentState == IDLE)
    {
      decodeBills(currentBillPulses);
    }
  }

  // Manual Test Cycle Engine
  bool testBtnState = digitalRead(TEST_DISPENSE_BTN);
  if (lastTestBtnState == HIGH && testBtnState == LOW && currentState == IDLE)
  {
    delay(50);
    if (digitalRead(TEST_DISPENSE_BTN) == LOW)
    {
      Serial.println("\n[TEST MODE] Beginning test dispense sequence...");
      dispenseCoins("10 PESO", HOPPER_10_RELAY_PIN, HOPPER_10_SENSOR_PIN, 1, HOPPER_10_SPEED);
      delay(500);
      dispenseCoins("1 PESO", HOPPER_1_RELAY_PIN, HOPPER_1_SENSOR_PIN, 2, HOPPER_1_SPEED);
    }
  }
  lastTestBtnState = testBtnState;

  // LED Status Heartbeat Indicator
  if (currentState == IDLE || currentState == AWAITING_PAYMENT)
  {
    static unsigned long lastBlink = 0;
    if (currentMillis - lastBlink > 1000)
    {
      digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
      lastBlink = currentMillis;
    }
  }
}