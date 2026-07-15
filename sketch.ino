#include <WiFi.h>
#include <HTTPClient.h>
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
// Network Configuration (UPDATE THESE FOR YOUR ACTUAL ENVIRONMENT)
// ------------------------------------------------------------
const char *ssid = "OLTEK Corp";
const char *password = "hayahAI2026";

// Local server address — update only if your server machine's IP changes.
// The ESP32 talks directly to the server over LAN; it does NOT need the
// Cloudflare tunnel URL (that's only for browser access from outside).
const char *SERVER_HOST = "http://192.168.1.134:3000"; // <-- set to your server PC's local IP
const char *kioskApiUrl = "http://192.168.1.134:3000/api/hardware/event";
const char *kioskStatusUrl = "http://192.168.1.134:3000/api/hardware/status";

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

void sendPaymentToKiosk(String deviceType, int amount)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;
    http.setTimeout(3000); // 3-second network timeout limit
    http.begin(kioskApiUrl);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["device"] = deviceType;
    doc["amount"] = amount;
    doc["credit"] = totalCredit;
    doc["timestamp"] = getTimestamp();

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    int httpResponseCode = http.POST(jsonPayload);
    if (httpResponseCode > 0)
    {
      Serial.printf("[API] Payment sent - Response Code: %d\n", httpResponseCode);
    }
    else
    {
      Serial.printf("[API] Send failed: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    http.end();
  }
}

void sendStatusToKiosk(String status, int credit, int change)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;
    http.setTimeout(3000);
    http.begin(kioskStatusUrl);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["status"] = status;
    doc["credit"] = credit;
    doc["changeDue"] = change;
    doc["timestamp"] = getTimestamp();

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    int httpResponseCode = http.POST(jsonPayload);
    if (httpResponseCode > 0)
    {
      Serial.printf("[API] Status sent - Response Code: %d\n", httpResponseCode);
    }
    else
    {
      Serial.printf("[API] Status send failed: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    http.end();
  }
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
  Serial.printf("WiFi Status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
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

  // Network Initialization
  Serial.printf("[WiFi] Establishing link to network: %s\n", ssid);
  WiFi.begin(ssid, password);

  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 30)
  {
    delay(1000);
    Serial.print(".");
    wifiAttempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\n[WiFi] Connection Authenticated!");
    Serial.printf("[WiFi] Local Station IP Address: %s\n", WiFi.localIP().toString().c_str());
    digitalWrite(STATUS_LED_PIN, HIGH); // Solid light indicates active connection
  }
  else
  {
    Serial.println("\n[WiFi] Critical Error: Connection Time Out!");
    // Blink error signal on hardware
    for (int i = 0; i < 5; i++)
    {
      digitalWrite(STATUS_LED_PIN, HIGH);
      delay(100);
      digitalWrite(STATUS_LED_PIN, LOW);
      delay(100);
    }
  }

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