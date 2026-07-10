#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ------------------------------------------------------------
// Network Configuration
// ------------------------------------------------------------
const char* ssid = "Wokwi-GUEST"; 
const char* password = "";
const char* kioskApiUrl = "http://192.168.1.183:3000/api/hardware/event";
const char* kioskStatusUrl = "http://192.168.1.183:3000/api/hardware/status";
// ------------------------------------------------------------
// Pin Configurations (Dual Hopper Setup)
// ------------------------------------------------------------
const byte COIN_SLOT_PIN   = 27;  
const byte BILL_ACC_PIN    = 14;  

// Hopper 10 Pesos
const byte HOPPER_10_SENS_PIN  = 32;  
const byte HOPPER_10_RELAY_PIN = 13; 

// Hopper 1 Peso
const byte HOPPER_1_SENS_PIN   = 33;
const byte HOPPER_1_RELAY_PIN  = 25;

const byte TEST_DISPENSE_BTN   = 26; 
const byte STATUS_LED_PIN      = 2;  // Built-in LED for status indication

// ------------------------------------------------------------
// Shared Volatile Variables
// ------------------------------------------------------------
volatile int coinPulseCount = 0;
volatile unsigned long lastCoinPulseTime = 0;
volatile unsigned long lastCoinInterruptTimeUs = 0;

volatile int billPulseCount = 0;
volatile unsigned long lastBillPulseTime = 0;
volatile unsigned long lastBillPulseTimeUs = 0;

volatile uint16_t currentHopper10Count = 0;
volatile uint16_t currentHopper1Count = 0;

const unsigned long debounceTimeUs = 50000;
const unsigned long coinTimeout = 600;
const unsigned long billTimeout = 500;
const unsigned long DISPENSE_TIMEOUT = 15000;
const bool SIMULATE_HOPPER_PULSES = true;
const unsigned long SIM_PULSE_INTERVAL_MS = 250;

// Transaction State
int totalCredit = 0;
int itemPrice = 0;
enum TransactionState { IDLE, AWAITING_PAYMENT, CALCULATING_CHANGE, DISPENSING_CHANGE, COMPLETED };
TransactionState currentState = IDLE;

// Change to dispense
int changeDue = 0;
int change10Coins = 0;
int change1Coins = 0;

// ------------------------------------------------------------
// Interrupt Service Routines (ISRs)
// ------------------------------------------------------------
void IRAM_ATTR coinISR() {
  unsigned long nowUs = micros();
  if (nowUs - lastCoinInterruptTimeUs > debounceTimeUs) {
    coinPulseCount++;
    lastCoinPulseTime = nowUs / 1000;
    lastCoinInterruptTimeUs = nowUs;
  }
}

void IRAM_ATTR billISR() {
  unsigned long nowUs = micros();
  if (nowUs - lastBillPulseTimeUs > 20000) {
    billPulseCount++;
    lastBillPulseTime = nowUs / 1000;
    lastBillPulseTimeUs = nowUs;
  }
}

void IRAM_ATTR hopper10ISR() { currentHopper10Count++; }
void IRAM_ATTR hopper1ISR()  { currentHopper1Count++; }

// ------------------------------------------------------------
// Helper Functions
// ------------------------------------------------------------
String getTimestamp() {
  unsigned long now = millis();
  unsigned long seconds = now / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  
  char timestamp[20];
  sprintf(timestamp, "%02lu:%02lu:%02lu", hours % 24, minutes % 60, seconds % 60);
  return String(timestamp);
}

void sendPaymentToKiosk(String deviceType, int amount) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.setTimeout(3000);
    http.begin(kioskApiUrl);
    http.addHeader("Content-Type", "application/json");
    
    // Create JSON payload using ArduinoJson
    StaticJsonDocument<256> doc;
    doc["device"] = deviceType;
    doc["amount"] = amount;
    doc["credit"] = totalCredit;
    doc["timestamp"] = getTimestamp();
    
    String jsonPayload;
    serializeJson(doc, jsonPayload);
    
    int httpResponseCode = http.POST(jsonPayload);
    
    if (httpResponseCode > 0) {
      Serial.printf("[API] Payment sent - Response Code: %d\n", httpResponseCode);
    } else {
      Serial.printf("[API] Send failed: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    
    http.end();
  }
}

void sendStatusToKiosk(String status, int credit, int change) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.setTimeout(3000);
    http.begin(kioskStatusUrl);
    http.addHeader("Content-Type", "application/json");
    
    StaticJsonDocument<256> doc;
    doc["status"] = status;
    doc["credit"] = credit;
    doc["changeDue"] = change;
    doc["timestamp"] = getTimestamp();
    
    String jsonPayload;
    serializeJson(doc, jsonPayload);
    
    int httpResponseCode = http.POST(jsonPayload);
    
    if (httpResponseCode > 0) {
      Serial.printf("[API] Status sent - Response Code: %d\n", httpResponseCode);
    } else {
      Serial.printf("[API] Status send failed: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    
    http.end();
  }
}

void addCredit(int value, String source) {
  totalCredit += value;
  Serial.printf("\n>>> ACCEPTED %s: PHP %d | Total Credit: PHP %d <<<\n", 
                source.c_str(), value, totalCredit);
  
  sendPaymentToKiosk(source, value);
  
  // Check if we have enough credit
  if (currentState == AWAITING_PAYMENT && totalCredit >= itemPrice) {
    calculateChange();
  } else if (currentState == AWAITING_PAYMENT) {
    Serial.printf("[STATUS] Still need PHP %d more\n", itemPrice - totalCredit);
  }
}

// ------------------------------------------------------------
// COIN ACCEPTANCE LOGIC (Input Side)
// Accepts ALL standard PHP coin denominations
// ------------------------------------------------------------
void decodeCoins(int pulses) {
  // Each pulse = 1 coin inserted sequentially
  // We process largest denomination first for efficiency
  
  int remaining = pulses;
  
  while (remaining > 0) {
    if (remaining >= 20) {
      addCredit(20, "coin_slot");
      remaining -= 20;
    } else if (remaining >= 10) {
      addCredit(10, "coin_slot");
      remaining -= 10;
    } else if (remaining >= 5) {
      addCredit(5, "coin_slot");
      remaining -= 5;
    } else {
      addCredit(1, "coin_slot");
      remaining -= 1;
    }
  }
}

void decodeBills(int pulses) {
  // Bill denominations based on pulse count:
  // 1 pulse = ₱50, 2 pulses = ₱100, 5 pulses = ₱500, 10 pulses = ₱1000
  // This is a simulation mapping - adjust based on your hardware
  
  int billValue = 0;
  String billDesc = "";
  
  if (pulses == 1) {
    billValue = 50;
    billDesc = "₱50 bill";
  } else if (pulses == 2) {
    billValue = 100;
    billDesc = "₱100 bill";
  } else if (pulses == 5) {
    billValue = 500;
    billDesc = "₱500 bill";
  } else if (pulses == 10) {
    billValue = 1000;
    billDesc = "₱1000 bill";
  } else {
    // Default: treat as ₱10 per pulse for unknown patterns
    billValue = pulses * 10;
    billDesc = String(pulses) + " pulses (₱" + String(billValue) + ")";
  }
  
  Serial.printf("\n[BILL DETECTED] %s inserted\n", billDesc.c_str());
  addCredit(billValue, "bill_acceptor");
}

// ------------------------------------------------------------
// CHANGE CALCULATION LOGIC (Output Side)
// CAN ONLY DISPENSE: ₱1 and ₱10 coins (2 hoppers max)
// Handles ₱5 gap by smart rounding
// ------------------------------------------------------------
void calculateChange() {
  if (totalCredit < itemPrice) {
    Serial.println("[ERROR] Insufficient funds!");
    currentState = AWAITING_PAYMENT;
    sendStatusToKiosk("insufficient_funds", totalCredit, 0);
    return;
  }
  
  changeDue = totalCredit - itemPrice;
  
  if (changeDue == 0) {
    Serial.println("\n[TRANSACTION] Exact payment! No change needed.");
    currentState = COMPLETED;
    sendStatusToKiosk("completed", totalCredit, 0);
    resetTransaction();
    return;
  }
  
  Serial.printf("\n[CHANGE CALCULATION] Raw change due: PHP %d\n", changeDue);
  
  // SMART ROUNDING FOR 2-HOPPER LIMITATION
  // Since we only have 1 and ₱10 hoppers, we cannot dispense ₱5 coins
  // Strategy: Round DOWN to nearest ₱10 + ₱1 combination
  // Example: ₱7 change → dispense ₱5 worth? NO → dispense ₱0? 
  // Better strategy: Always round DOWN to avoid losing money
  // ₱7 → give ₱5? Can't. Give ₱0? User loses ₱7.
  // BEST STRATEGY: Round DOWN to nearest dispensable amount
  // But that's unfair to user. 
  // REAL KIOSK STRATEGY: Store ₱5 internally, give equivalent in ₱1 coins
  // OR: Round UP and absorb loss (not recommended)
  // SAFEST SIMULATION: Convert everything to ₱1 equivalent when ₱5 exists
  
  int actualDispenseAmount = changeDue;
  
  // If change contains ₱5 component, convert to ₱1 coins
  // e.g., ₱17 = 1×₱10 + 7×₱1 (no problem)
  // e.g., ₱25 = 2×₱10 + 5×₱1 (we give 5×₱1 instead of 1×₱5)
  // This works because we HAVE ₱1 hopper!
  
  change10Coins = actualDispenseAmount / 10;
  change1Coins = actualDispenseAmount % 10;
  
  // NOTE: Since we have ₱1 hopper, we can ALWAYS make exact change
  // The only limitation is hopper capacity, not denomination
  // ₱5 change = 5×₱1 coins ✓
  // ₱15 change = 1×₱10 + 5×1 ✓
  // ₱25 change = 2×₱10 + 5×₱1 ✓
  
  Serial.printf("[CHANGE] Dispensing: %d × ₱10 + %d × ₱1 = ₱%d\n", 
                change10Coins, change1Coins, actualDispenseAmount);
  
  currentState = DISPENSING_CHANGE;
  sendStatusToKiosk("dispensing_change", totalCredit, actualDispenseAmount);
  
  dispenseChange();
}

void dispenseChange() {
  Serial.println("\n[DISPENSE] Starting change dispensing...");
  
  // Dispense ₱10 coins first
  if (change10Coins > 0) {
    Serial.printf("[DISPENSE] Dispensing %d x ₱10 coins\n", change10Coins);
    dispenseCoins("10 PESO", HOPPER_10_RELAY_PIN, currentHopper10Count, change10Coins);
  }
  
  // Small delay between hopper operations
  if (change10Coins > 0 && change1Coins > 0) {
    delay(500);
  }
  
  // Dispense ₱1 coins
  if (change1Coins > 0) {
    Serial.printf("[DISPENSE] Dispensing %d x ₱1 coins\n", change1Coins);
    dispenseCoins("1 PESO", HOPPER_1_RELAY_PIN, currentHopper1Count, change1Coins);
  }
  
  Serial.println("[DISPENSE] Change dispensing complete!");
  currentState = COMPLETED;
  sendStatusToKiosk("completed", totalCredit, changeDue);
  
  // Reset after short delay
  delay(2000);
  resetTransaction();
}

void dispenseCoins(String hopperType, byte relayPin, volatile uint16_t &countVar, uint16_t targetCoins) {
  Serial.printf("\n[Hopper %s] Spinning motor... Target: %d coins\n", hopperType.c_str(), targetCoins);

  countVar = 0;
  digitalWrite(relayPin, HIGH); 
  unsigned long startTime = millis();
  
  // Wokwi-optimized pulse simulation
  while (true) {
    // Force-increment counter every 100ms regardless of interrupt state
    if (millis() - startTime > (countVar * 100UL)) {
      noInterrupts();
      if (countVar < targetCoins) {
        countVar++;
        Serial.printf("[Hopper %s] Counted: %d / %d\n", hopperType.c_str(), countVar, targetCoins);
      }
      interrupts();
    }

    // Success condition
    if (countVar >= targetCoins) {
      Serial.printf("[Hopper %s] ✅ Target reached.\n", hopperType.c_str());
      break;
    }

    // Timeout extended to 30s for Wokwi stability
    if (millis() - startTime > 30000UL) {
      Serial.printf("[ERROR] Hopper %s Jammed or Empty! (Timeout)\n", hopperType.c_str());
      break;
    }
    
    delay(10); // Prevent watchdog trigger
  }
  digitalWrite(relayPin, LOW); 
}

void resetTransaction() {
  Serial.println("\n[RESET] Transaction cleared. Ready for next customer.");
  totalCredit = 0;
  changeDue = 0;
  change10Coins = 0;
  change1Coins = 0;
  currentState = IDLE;
  sendStatusToKiosk("idle", 0, 0);
}

void setItemPrice(int price) {
  itemPrice = price;
  totalCredit = 0;
  currentState = AWAITING_PAYMENT;
  Serial.printf("\n[CONFIG] Item price set to PHP %d\n", itemPrice);
  Serial.printf("[STATUS] Waiting for payment...\n");
  sendStatusToKiosk("awaiting_payment", 0, 0);
}

void printStatus() {
  Serial.println("\n=== [SYSTEM STATUS] ===");
  Serial.printf("State: ");
  switch(currentState) {
    case IDLE: Serial.println("IDLE"); break;
    case AWAITING_PAYMENT: Serial.println("AWAITING_PAYMENT"); break;
    case CALCULATING_CHANGE: Serial.println("CALCULATING_CHANGE"); break;
    case DISPENSING_CHANGE: Serial.println("DISPENSING_CHANGE"); break;
    case COMPLETED: Serial.println("COMPLETED"); break;
  }
  Serial.printf("Item Price: PHP %d\n", itemPrice);
  Serial.printf("Total Credit: PHP %d\n", totalCredit);
  Serial.printf("Change Due: PHP %d\n", changeDue);
  Serial.printf("WiFi Status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  Serial.println("======================\n");
}

void processSerialCommand(String command) {
  command.trim();
  command.toUpperCase();
  
  if (command.startsWith("SET_PRICE:")) {
    String priceStr = command.substring(10);
    int price = priceStr.toInt();
    if (price > 0) {
      setItemPrice(price);
    } else {
      Serial.println("[ERROR] Invalid price value");
    }
  } else if (command == "STATUS") {
    printStatus();
  } else if (command == "RESET") {
    resetTransaction();
  } else if (command.startsWith("SET_URL:")) {
    String newUrl = command.substring(8);
    // Note: Can't actually change const char* at runtime easily
    // This is just for demonstration
    Serial.printf("[INFO] URL update requested: %s\n", newUrl.c_str());
    Serial.println("[INFO] Please update kioskApiUrl in code and reflash");
  } else if (command == "HELP") {
    Serial.println("\n=== [AVAILABLE COMMANDS] ===");
    Serial.println("SET_PRICE:<amount>  - Set item price (e.g., SET_PRICE:150)");
    Serial.println("STATUS              - Show current transaction status");
    Serial.println("RESET               - Clear current transaction");
    Serial.println("SET_URL:<url>       - Request URL update (requires reflash)");
    Serial.println("HELP                - Show this help message");
    Serial.println("===========================\n");
  } else {
    Serial.println("[ERROR] Unknown command. Type HELP for available commands.");
  }
}

// ------------------------------------------------------------
// Setup & Main Loop
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[BOOT] Kiosk Simulator starting...");

  // Initialize pins
  pinMode(COIN_SLOT_PIN, INPUT_PULLUP);
  pinMode(BILL_ACC_PIN, INPUT_PULLUP);
  pinMode(HOPPER_10_SENS_PIN, INPUT_PULLUP);
  pinMode(HOPPER_1_SENS_PIN, INPUT_PULLUP);
  pinMode(TEST_DISPENSE_BTN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);

  pinMode(HOPPER_10_RELAY_PIN, OUTPUT);
  pinMode(HOPPER_1_RELAY_PIN, OUTPUT);
  digitalWrite(HOPPER_10_RELAY_PIN, LOW);
  digitalWrite(HOPPER_1_RELAY_PIN, LOW);
  digitalWrite(STATUS_LED_PIN, LOW);

  // Attach interrupts
  attachInterrupt(digitalPinToInterrupt(COIN_SLOT_PIN), coinISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BILL_ACC_PIN), billISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(HOPPER_10_SENS_PIN), hopper10ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(HOPPER_1_SENS_PIN), hopper1ISR, FALLING);

  // Connect to WiFi
  Serial.print("[WiFi] Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 40) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected!");
    Serial.printf("[WiFi] IP Address: %s\n", WiFi.localIP().toString().c_str());
    digitalWrite(STATUS_LED_PIN, HIGH);  // LED on when connected
  } else {
    Serial.println("\n[WiFi] Connection failed!");
    digitalWrite(STATUS_LED_PIN, LOW);
  }
  
  Serial.println("[BOOT] Dual Hopper system armed.");
  Serial.println("[BOOT] Type HELP for available commands.\n");
}

void loop() {
  unsigned long currentMillis = millis();
  static bool coinReceiving = false;
  static bool lastTestBtnState = HIGH;

  // Process serial commands
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    processSerialCommand(command);
  }

  // Process coin pulses
  if (coinPulseCount > 0) coinReceiving = true;
  if (coinReceiving && (currentMillis - lastCoinPulseTime > coinTimeout)) {
    noInterrupts();
    int totalCoinPulses = coinPulseCount;
    coinPulseCount = 0;
    interrupts();
    
    if (currentState == AWAITING_PAYMENT || currentState == IDLE) {
      decodeCoins(totalCoinPulses);
    } else {
      Serial.println("[IGNORED] Coins inserted but not in payment state");
    }
    coinReceiving = false;
  }

  // Process bill pulses
  if (billPulseCount > 0 && (currentMillis - lastBillPulseTime > billTimeout)) {
    noInterrupts();
    int currentBillPulses = billPulseCount;
    billPulseCount = 0;
    interrupts();
    
    if (currentState == AWAITING_PAYMENT || currentState == IDLE) {
      decodeBills(currentBillPulses);
    } else {
      Serial.println("[IGNORED] Bill inserted but not in payment state");
    }
  }

  // Test dispense button (manual test mode)
  bool testBtnState = digitalRead(TEST_DISPENSE_BTN);
if (lastTestBtnState == HIGH && testBtnState == LOW && currentState == IDLE) {
  delay(50);
  if (digitalRead(TEST_DISPENSE_BTN) == LOW) {
    Serial.println("\n[TEST MODE] Manual dispense triggered");
    dispenseCoins("10 PESO", HOPPER_10_RELAY_PIN, currentHopper10Count, 1);
    delay(500);
    dispenseCoins("1 PESO", HOPPER_1_RELAY_PIN, currentHopper1Count, 2);
    Serial.println("[TEST MODE] Manual dispense complete");
  }
}
lastTestBtnState = testBtnState;
  
  // Blink LED slowly when idle, fast when processing
  if (currentState == IDLE || currentState == AWAITING_PAYMENT) {
    static unsigned long lastBlink = 0;
    if (currentMillis - lastBlink > 1000) {
      digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
      lastBlink = currentMillis;
    }
  }
}