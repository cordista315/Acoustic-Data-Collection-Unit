/*
  BLE Turntable Controller — ReSpeaker Edition v2.3.0
  ESP32 + DRV8825 + FLSUN 42SHDC40472-23B (1.8deg/step)

  ── Wiring ────────────────────────────────────────
  GPIO18 → STEP
  GPIO19 → DIR
  GPIO21 → EN     (LOW = enabled)
  GPIO22 → MODE0
  GPIO23 → MODE1
  GPIO5  → MODE2
  RST    → 3.3V
  SLP    → 3.3V

  ── Microstepping ─────────────────────────────────
  MODE0=LOW MODE1=HIGH MODE2=LOW = 1/4 step
  200 full steps x 4 microsteps = 800 steps/rev
  Resolution = 360 / 800 = 0.45deg per microstep

  ── BLE Commands ──────────────────────────────────
  STEP / STEP:N / BACK / BACK:N / GOTO:N
  RESET / ZERO / SWEEP / SCAN:N / STOP
  SPEED:N / DWELL:N / SETSTEP:N
  DIR:CW / DIR:CCW / DIR:AUTO
  STATUS / INFO
*/

#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#define FW_VERSION "2.3.0"

// ── Pins ───────────────────────────────────────────────────────────────────
#define PIN_STEP 18
#define PIN_DIR 19
#define PIN_EN 21
#define PIN_MS1 22
#define PIN_MS2 23
#define PIN_MS3 5

// ── Motor config ───────────────────────────────────────────────────────────
#define MICROSTEPS 4
#define FULL_STEPS_REV 200
#define MICROSTEPS_REV (FULL_STEPS_REV * MICROSTEPS)      // 800 steps/rev
#define DEGREES_PER_STEP (360.0f / (float)MICROSTEPS_REV) // 0.45 deg/step

// All angle tracking is in integer steps.
// TOTAL_STEPS = 800 = one full revolution.
// Target angles are converted to steps once and never back.

// ── BLE UUIDs ─────────────────────────────────────────────────────────────
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ── State ──────────────────────────────────────────────────────────────────
BLECharacteristic *pTxChar;
bool deviceConnected = false;

long currentSteps = 0; // absolute position in steps (wraps at MICROSTEPS_REV)
int stepDelayUs = 800;
int dwellMs = 1000;
int stepIncrSteps = 0; // stepIncrement stored as integer steps
float stepIncrDeg = 2.0f;
int runNumber = 0;
int approachDir = 0;     // 1=CW, -1=CCW, 0=auto
bool holdEnabled = true; // keep motor energized between moves

enum Mode { IDLE, SWEEPING, SCANNING };
Mode currentMode = IDLE;

bool scanWaitingForAck = false;
int scanStepSteps = 0; // scan step size in integer steps
float scanStepDeg = 2.0f;
unsigned long lastSweepTime = 0;

// ── Conversion helpers ────────────────────────────────────────────────────
// Always convert degrees→steps, never steps→degrees for logic
int degreesToSteps(float deg) { return (int)round(deg / DEGREES_PER_STEP); }

// Only for reporting to BLE — never used in motion logic
float currentAngleDeg() {
  long s = ((currentSteps % MICROSTEPS_REV) + MICROSTEPS_REV) % MICROSTEPS_REV;
  return s * DEGREES_PER_STEP;
}

// ── Motor control ──────────────────────────────────────────────────────────
void motorEnable() { digitalWrite(PIN_EN, LOW); }
void motorDisable() { digitalWrite(PIN_EN, HIGH); }

void stepMotor(int steps) {
  if (steps == 0) return;
  digitalWrite(PIN_DIR, steps > 0 ? HIGH : LOW);
  delayMicroseconds(2);
  int absSteps = abs(steps);
  for (int i = 0; i < absSteps; i++) {
    digitalWrite(PIN_STEP, HIGH);
    delayMicroseconds(stepDelayUs);
    digitalWrite(PIN_STEP, LOW);
    delayMicroseconds(stepDelayUs);
  }
  currentSteps += steps;
}

// ── Move by integer steps ─────────────────────────────────────────────────
void moveBySteps(int steps) {
  stepMotor(steps);
}

// ── Move to absolute target in integer steps ───────────────────────────────
// Everything is integer — no float conversion in logic path
void moveToSteps(int targetSteps) {
  // Normalize target into 0..MICROSTEPS_REV-1
  targetSteps = ((targetSteps % MICROSTEPS_REV) + MICROSTEPS_REV) % MICROSTEPS_REV;
  int current = (int)(((currentSteps % MICROSTEPS_REV) + MICROSTEPS_REV) % MICROSTEPS_REV);

  int delta = targetSteps - current;

  // Shortest path
  if (delta >  MICROSTEPS_REV / 2) delta -= MICROSTEPS_REV;
  if (delta < -MICROSTEPS_REV / 2) delta += MICROSTEPS_REV;

  // Integer deadband — if delta is 0 steps, do nothing at all
  if (delta == 0) return;

  // Approach direction override
  if (approachDir == 1 && delta < 0) {
    int overshoot = degreesToSteps(5.0f);
    stepMotor(delta - overshoot);
    stepMotor(overshoot);
  } else if (approachDir == -1 && delta > 0) {
    int overshoot = degreesToSteps(5.0f);
    stepMotor(delta + overshoot);
    stepMotor(-overshoot);
  } else {
    stepMotor(delta);
  }
}

void moveToAngle(float deg) {
  moveToSteps(degreesToSteps(deg));
}

void resetPosition() {
  moveToSteps(0);
  currentSteps = 0;
}

// ── BLE helpers ───────────────────────────────────────────────────────────
void sendBLE(String msg) {
  if (deviceConnected) {
    pTxChar->setValue(msg.c_str());
    pTxChar->notify();
  }
  Serial.println(">> " + msg);
}

String buildMeta(float stepDeg) {
  return "READY:" + String(currentAngleDeg(), 2) +
         ",TS:"   + String(millis()) +
         ",STEP:" + String(stepDeg, 2) +
         ",RUN:"  + String(runNumber);
}

void stopAll() {
  currentMode       = IDLE;
  scanWaitingForAck = false;
  motorDisable();
  sendBLE("STOPPED:" + String(currentAngleDeg(), 2));
}

// ── BLE server callbacks ───────────────────────────────────────────────────
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    deviceConnected = true;
    sendBLE("READY,FW:v" + String(FW_VERSION));
  }
  void onDisconnect(BLEServer *pServer) {
    deviceConnected = false;
    currentMode = IDLE;
    scanWaitingForAck = false;
    motorDisable();
    pServer->startAdvertising();
  }
};

// ── Command parser ─────────────────────────────────────────────────────────
class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) {
    String cmd = String(pChar->getValue().c_str());
    cmd.trim();
    cmd.toUpperCase();
    Serial.println("<< " + cmd);

    // ── ACK ───────────────────────────────────────────────────────────────
    if (cmd == "ACK") {
      if (currentMode == SCANNING && scanWaitingForAck) {
        scanWaitingForAck = false;
        int current = (int)(((currentSteps % MICROSTEPS_REV) + MICROSTEPS_REV) % MICROSTEPS_REV);
        int next    = current + scanStepSteps;
        if (next >= MICROSTEPS_REV) {
          currentMode = IDLE;
          motorDisable();
          sendBLE("SCAN_DONE,RUN:" + String(runNumber));
        } else {
          motorEnable();
          moveToSteps(next);
          motorDisable();
          scanWaitingForAck = true;
          sendBLE(buildMeta(scanStepDeg));
        }
      } else {
        sendBLE("ERR:NO_SCAN_ACTIVE");
      }
      return;
    }

    // ── STOP ──────────────────────────────────────────────────────────────
    if (cmd == "STOP") { stopAll(); return; }

    // ── SCAN:N ────────────────────────────────────────────────────────────
    if (cmd.startsWith("SCAN:")) {
      float sz = cmd.substring(5).toFloat();
      if (sz <= 0 || sz > 180) { sendBLE("ERR:SCAN_STEP_RANGE_0.1-180"); return; }
      scanStepDeg   = sz;
      scanStepSteps = degreesToSteps(sz);
      currentMode   = SCANNING;
      scanWaitingForAck = false;
      runNumber++;
      motorEnable();
      moveToSteps(0);
      motorDisable();
      scanWaitingForAck = true;
      sendBLE(buildMeta(scanStepDeg));
      return;
    }

    // ── SWEEP ─────────────────────────────────────────────────────────────
    if (cmd == "SWEEP") {
      currentMode = SWEEPING;
      runNumber++;
      motorEnable();
      moveToSteps(0);
      lastSweepTime = millis();
      sendBLE("SWEEP_START,RUN:" + String(runNumber));
      return;
    }

    // ── RESET ─────────────────────────────────────────────────────────────
    if (cmd == "RESET") {
      currentMode = IDLE;
      scanWaitingForAck = false;
      motorEnable();
      resetPosition();
      motorDisable();
      sendBLE("ANGLE:0.00");
      return;
    }

    // ── ZERO ──────────────────────────────────────────────────────────────
    if (cmd == "ZERO") {
      currentSteps = 0;
      sendBLE("ZEROED");
      return;
    }

    // ── STATUS ────────────────────────────────────────────────────────────
    if (cmd == "STATUS") {
      sendBLE("ANGLE:"  + String(currentAngleDeg(), 2) +
              ",STEP:"  + String(stepIncrDeg, 2) +
              ",SPEED:" + String(stepDelayUs) +
              ",DWELL:" + String(dwellMs) +
              ",RUN:"   + String(runNumber) +
              ",MODE:"  + String(currentMode == IDLE ? "IDLE" : currentMode == SWEEPING ? "SWEEP" : "SCAN") +
              ",DIR:"   + String(approachDir == 0 ? "AUTO" : approachDir == 1 ? "CW" : "CCW") +
              ",HOLD:"  + String(holdEnabled ? "ON" : "OFF"));
      return;
    }

    // ── INFO ──────────────────────────────────────────────────────────────
    if (cmd == "INFO") {
      sendBLE("FW:"          + String(FW_VERSION) +
              ",MOTOR:42SHDC40472-23B" +
              ",STEPS_REV:"  + String(MICROSTEPS_REV) +
              ",DEG_STEP:"   + String(DEGREES_PER_STEP, 4) +
              ",MICROSTEPS:" + String(MICROSTEPS));
      return;
    }

    // ── HOLD ─────────────────────────────────────────────────────────────
    if (cmd == "HOLD:ON")  { holdEnabled = true;  motorEnable();  sendBLE("HOLD:ON");  return; }
    if (cmd == "HOLD:OFF") { holdEnabled = false; motorDisable(); sendBLE("HOLD:OFF"); return; }

    // ── DIR ───────────────────────────────────────────────────────────────
    if (cmd == "DIR:CW")   { approachDir =  1; sendBLE("DIR_SET:CW");   return; }
    if (cmd == "DIR:CCW")  { approachDir = -1; sendBLE("DIR_SET:CCW");  return; }
    if (cmd == "DIR:AUTO") { approachDir =  0; sendBLE("DIR_SET:AUTO"); return; }

    // ── Movement (IDLE only) ──────────────────────────────────────────────
    if (currentMode != IDLE) { sendBLE("ERR:BUSY,SEND_STOP_FIRST"); return; }

    motorEnable();

    if (cmd == "STEP") {
      moveBySteps(stepIncrSteps);
      sendBLE("ANGLE:" + String(currentAngleDeg(), 2));

    } else if (cmd == "BACK") {
      moveBySteps(-stepIncrSteps);
      sendBLE("ANGLE:" + String(currentAngleDeg(), 2));

    } else if (cmd.startsWith("STEP:")) {
      float deg = cmd.substring(5).toFloat();
      if (deg > 0 && deg <= 360) {
        stepIncrDeg   = deg;
        stepIncrSteps = degreesToSteps(deg);
        moveBySteps(stepIncrSteps);
        sendBLE("ANGLE:" + String(currentAngleDeg(), 2));
      } else sendBLE("ERR:INVALID_STEP");

    } else if (cmd.startsWith("BACK:")) {
      float deg = cmd.substring(5).toFloat();
      if (deg > 0 && deg <= 360) {
        moveBySteps(-degreesToSteps(deg));
        sendBLE("ANGLE:" + String(currentAngleDeg(), 2));
      } else sendBLE("ERR:INVALID_STEP");

    } else if (cmd.startsWith("GOTO:")) {
      float target = cmd.substring(5).toFloat();
      if (target >= 0 && target <= 360) {
        moveToAngle(target);
        sendBLE("ANGLE:" + String(currentAngleDeg(), 2));
      } else sendBLE("ERR:ANGLE_RANGE_0-360");

    } else if (cmd.startsWith("SETSTEP:")) {
      float deg = cmd.substring(8).toFloat();
      if (deg > 0 && deg <= 180) {
        stepIncrDeg   = deg;
        stepIncrSteps = degreesToSteps(deg);
        sendBLE("STEP_SET:" + String(stepIncrDeg, 2));
      } else sendBLE("ERR:SETSTEP_RANGE_0.1-180");

    } else if (cmd.startsWith("SPEED:")) {
      int spd = cmd.substring(6).toInt();
      if (spd >= 100 && spd <= 10000) { stepDelayUs = spd; sendBLE("SPEED_SET:" + String(stepDelayUs)); }
      else sendBLE("ERR:SPEED_RANGE_100-10000");

    } else if (cmd.startsWith("DWELL:")) {
      int dw = cmd.substring(6).toInt();
      if (dw >= 100 && dw <= 60000) { dwellMs = dw; sendBLE("DWELL_SET:" + String(dwellMs)); }
      else sendBLE("ERR:DWELL_RANGE_100-60000");

    } else {
      sendBLE("ERR:UNKNOWN_CMD");
    }
    // Respect hold setting after manual commands
    if (!holdEnabled) motorDisable();
    // Motor stays ENABLED after manual commands so it holds
    // its detent and doesn't jerk on re-energization.
    // It will be disabled after SWEEP/SCAN completes or on disconnect.
  }
};

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(PIN_STEP, OUTPUT);
  pinMode(PIN_DIR,  OUTPUT);
  pinMode(PIN_EN,   OUTPUT);
  pinMode(PIN_MS1,  OUTPUT);
  pinMode(PIN_MS2,  OUTPUT);
  pinMode(PIN_MS3,  OUTPUT);

  // 1/4 step: MODE0=LOW MODE1=HIGH MODE2=LOW
  digitalWrite(PIN_MS1, LOW);
  digitalWrite(PIN_MS2, HIGH);
  digitalWrite(PIN_MS3, LOW);

  // Initialise step increment
  stepIncrSteps = degreesToSteps(stepIncrDeg);

  motorDisable();
  Serial.println("Turntable FW v" + String(FW_VERSION) +
                 " — 0.45deg/step, 800 steps/rev");

  BLEDevice::init("Turntable");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxChar = pService->createCharacteristic(
    CHAR_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  pTxChar->addDescriptor(new BLE2902());

  BLECharacteristic *pRxChar = pService->createCharacteristic(
    CHAR_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRxChar->setCallbacks(new RxCallbacks());

  pService->start();
  pServer->getAdvertising()->start();
  Serial.println("BLE advertising as 'Turntable'");
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
  if (currentMode == SWEEPING) {
    unsigned long now = millis();
    if (now - lastSweepTime >= (unsigned long)dwellMs) {
      lastSweepTime = now;
      int current = (int)(((currentSteps % MICROSTEPS_REV) + MICROSTEPS_REV) % MICROSTEPS_REV);
      if (current + stepIncrSteps >= MICROSTEPS_REV) {
        currentMode = IDLE;
        motorDisable();
        sendBLE("SWEEP_DONE,RUN:" + String(runNumber));
      } else {
        motorEnable();
        moveBySteps(stepIncrSteps);
        motorDisable();
        sendBLE("ANGLE:" + String(currentAngleDeg(), 2) +
                ",TS:"   + String(millis()) +
                ",RUN:"  + String(runNumber));
      }
    }
  }
}

