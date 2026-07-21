#include <ArduinoJson.h>

// Forward Declarations for PlatformIO/C++ Compiler
void calculateChange();
void resetTransaction();
void dispenseChange();
void dispenseCoins(String hopperType, byte relayPin, uint16_t targetCoins);
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

// PHYSICAL MANUAL SIMULATION BUTTONS (Optional for hardware testing)
const byte BTN_1_PESO = 33;
const byte BTN_10_PESO = 32;

const byte TEST_DISPENSE_BTN = 26;

// OUTPUTS (Connected to Relay Modules driving the Hopper Motors)
const byte HOPPER_10_RELAY_PIN = 13; // Relay control for 10 Peso Hopper Motor
const byte HOPPER_1_RELAY_PIN = 25;  // Relay control for 1 Peso Hopper Motor
const byte STATUS_LED_PIN = 2;       // Onboard ESP32 Status LED

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
const unsigned long coinTimeout = 1500;     // Wait 1500ms after last pulse — ₱20 coin = 20 pulses @ ~50ms
const unsigned long billTimeout = 250;    // Wait 12s after last pulse — ₱1000 bill = 100 pulses @ ~100ms
const unsigned long DISPENSE_TIMEOUT = 15000;

// Transaction State Machine
int totalCredit = 0;
int itemPrice = 0;
String serialBuffer = "";  // Non-blocking serial receive buffer
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

// Button Monitoring States
bool lastBtn1PesoState = HIGH;
bool lastBtn10PesoState = HIGH;

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
    lastCoinPulseTime = millis();  // Use millis() directly — safe on ESP32 (FreeRTOS)
    lastCoinInterruptTimeUs = nowUs;
  }
}

void IRAM_ATTR billISR() {
  unsigned long nowUs = micros();

  if (nowUs - lastBillPulseTimeUs > 50000) { // 50ms
    billPulseCount++;
    lastBillPulseTime = millis();
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
  doc["type"]      = "payment";
  doc["device"]    = deviceType;
  doc["amount"]    = amount;
  doc["credit"]    = totalCredit;
  doc["timestamp"] = getTimestamp();

  Serial.print("EVT:");
  serializeJson(doc, Serial);
  Serial.println();
}

// Emit a status event to the server via USB serial.
void sendStatusToKiosk(String status, int credit, int change)
{
  JsonDocument doc;
  doc["type"]      = "status";
  doc["status"]    = status;
  doc["credit"]    = credit;
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

  if (currentState == AWAITING_PAYMENT && totalCredit >= itemPrice)
  {
    calculateChange();
  }
  else if (currentState == AWAITING_PAYMENT)
  {
    Serial.printf("[STATUS] Still need PHP %d more\n", itemPrice - totalCredit);
  }
}

// ------------------------------------------------------------
// COIN/BILL PARSING LOGIC
// ------------------------------------------------------------
void decodeCoins(int pulses)
{
  // Linear mapping: 1 pulse = ₱1.  Total value = pulse count.
  int coinValue = pulses;
  Serial.printf("\n[COIN DETECTED] %d pulses → ₱%d coin inserted\n", pulses, coinValue);
  addCredit(coinValue, "coin_slot");
}

void decodeBills(int pulses)
{
  // Linear mapping: 1 pulse = ₱10.  (₱20=2p, ₱50=5p, ₱100=10p, ₱500=50p, ₱1000=100p)
  int billValue = pulses * 10;
  Serial.printf("\n[BILL DETECTED] %d pulses → ₱%d bill inserted\n", pulses, billValue);
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
  Serial.println("\n[DISPENSE] Commencing physical change process...");

  if (change10Coins > 0)
  {
    Serial.printf("[DISPENSE] Activating ₱10 Hopper for %d coins\n", change10Coins);
    dispenseCoins("10 PESO", HOPPER_10_RELAY_PIN, change10Coins);
  }

  if (change10Coins > 0 && change1Coins > 0)
  {
    delay(800); // Guard window between motor activations to control current draw
  }

  if (change1Coins > 0)
  {
    Serial.printf("[DISPENSE] Activating ₱1 Hopper for %d coins\n", change1Coins);
    dispenseCoins("1 PESO", HOPPER_1_RELAY_PIN, change1Coins);
  }

  Serial.println("[DISPENSE] Change dispensing cycle finished.");
  currentState = COMPLETED;
  sendStatusToKiosk("completed", totalCredit, changeDue);

  delay(2000);
  resetTransaction();
}

void dispenseCoins(String hopperType, byte relayPin, uint16_t targetCoins)
{
  Serial.printf("\n[Hopper %s] Motor engaged... Target: %d\n", hopperType.c_str(), targetCoins);

  digitalWrite(relayPin, HIGH); // Pull Relay Pin High to start motor
  unsigned long startTime = millis();
  uint16_t counted = 0;

  while (true)
  {
    // Hardware Simulation Block (Every 250ms counts as one dispensed coin)
    if (millis() - startTime > (counted * 250UL))
    {
      if (counted < targetCoins)
      {
        counted++;
        Serial.printf("[Hopper %s] Counted: %d / %d\n", hopperType.c_str(), counted, targetCoins);
      }
    }

    if (counted >= targetCoins)
    {
      Serial.printf("[Hopper %s] Target reached successfully.\n", hopperType.c_str());
      break;
    }

    // Safety timeout: prevents motor burn out if hopper runs out of coins
    if (millis() - startTime > 20000UL)
    {
      Serial.printf("[CRITICAL ERROR] Hopper %s Safety Timeout exceeded!\n", hopperType.c_str());
      sendStatusToKiosk("hardware_error_jam", totalCredit, changeDue);
      break;
    }

    delay(10);
  }
  digitalWrite(relayPin, LOW); // Cut motor relay line
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
  pinMode(BTN_1_PESO, INPUT_PULLUP);
  pinMode(BTN_10_PESO, INPUT_PULLUP);
  pinMode(TEST_DISPENSE_BTN, INPUT_PULLUP);

  // Setup output relay driving pins
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(HOPPER_10_RELAY_PIN, OUTPUT);
  pinMode(HOPPER_1_RELAY_PIN, OUTPUT);

  // Set default stable states (Relays off)
  digitalWrite(HOPPER_10_RELAY_PIN, LOW);
  digitalWrite(HOPPER_1_RELAY_PIN, LOW);
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

  // Non-blocking serial read — avoids up to 1s hang from readStringUntil()
  while (Serial.available() > 0)
  {
    char c = Serial.read();
    if (c == '\n')
    {
      processSerialCommand(serialBuffer);
      serialBuffer = "";
    }
    else if (c != '\r')
    {
      serialBuffer += c;
    }
  }

  // --- MANUAL HARDWARE BUTTON SIMULATIONS ---
  bool btn1PesoState = digitalRead(BTN_1_PESO);
  if (lastBtn1PesoState == HIGH && btn1PesoState == LOW)
  {
    delay(50);
    if (digitalRead(BTN_1_PESO) == LOW)
    {
      if (currentState == AWAITING_PAYMENT || currentState == IDLE)
      {
        addCredit(1, "manual_1peso");
      }
    }
  }
  lastBtn1PesoState = btn1PesoState;

  bool btn10PesoState = digitalRead(BTN_10_PESO);
  if (lastBtn10PesoState == HIGH && btn10PesoState == LOW)
  {
    delay(50);
    if (digitalRead(BTN_10_PESO) == LOW)
    {
      if (currentState == AWAITING_PAYMENT || currentState == IDLE)
      {
        addCredit(10, "manual_10peso");
      }
    }
  }
  lastBtn10PesoState = btn10PesoState;

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
      dispenseCoins("10 PESO", HOPPER_10_RELAY_PIN, 1);
      delay(500);
      dispenseCoins("1 PESO", HOPPER_1_RELAY_PIN, 2);
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