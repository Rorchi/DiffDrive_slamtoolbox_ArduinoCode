#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <stddef.h>

// --- MOTOR VE ENCODER PINLERI ---
const int enc1A = 2;  const int enc1B = 3;
const int enc2A = 18; const int enc2B = 19;
const int PWMA = 5;   const int AIN1 = 7; const int AIN2 = 8;
const int PWMB = 6;   const int BIN1 = 9; const int BIN2 = 10;
const int STBY = 11;

// --- PIL GOSTERGESI VE UYARI PINLERI ---
const uint8_t buzzerPin = 22;
const uint8_t redLedPin = 23;
const uint8_t yellowLedPin = 24;
const uint8_t greenLedPin = 25;
const uint8_t buzzerActiveLevel = LOW;
const uint8_t buzzerInactiveLevel = HIGH;
const unsigned long buzzerMelodyPeriodMs = 20000UL;
const unsigned long buzzerMelodyDurationMs = 3000UL;
const unsigned long ledBlinkPeriodMs = 3000UL;
const unsigned long ledBlinkOnMs = 300UL;

const int encoderLeftSign = 1;
const int encoderRightSign = 1;

volatile long encoder1Count = 0;
volatile long encoder2Count = 0;

// --- BAGIMSIZ SENSOR DURUMLARI ---
Adafruit_INA219 ina219;
bool mpuAvailable = false;
uint8_t mpuAddress = 0;
uint8_t mpuWhoAmI = 0;
bool ina219Available = false;

float accelX_offset = 0.0f;
float accelY_offset = 0.0f;
float accelZ_offset = 0.0f;
float gyroX_offset = 0.0f;
float gyroY_offset = 0.0f;
float gyroZ_offset = 0.0f;

const int imuCalibrationSamples = 500;
const float gravityMs2 = 9.80665f;

// --- 4S 3300 mAh 40C LiPo / INA219 ---
const uint8_t batteryCellCount = 4;
const float batteryCapacityAh = 3.3f;
const float cellFullVoltage = 4.20f;
const float cellEmptyVoltage = 3.00f;
const float batteryFullVoltage = batteryCellCount * cellFullVoltage;
const float batteryEmptyVoltage = batteryCellCount * cellEmptyVoltage;
const float criticalPackVoltage = batteryEmptyVoltage;

// Bu katsayilar referans multimetre/ampermetre ile kalibre edilebilir.
const float voltageCalibrationGain = 1.0f;
const float currentOffsetA = 0.0f;
const float currentCalibrationGain = 1.0f;

const float batteryFilterAlpha = 0.18f;
const float averageCurrentTimeConstantS = 60.0f;
const float restCurrentThresholdA = 0.08f;
const unsigned long restRequiredMs = 30000UL;
const float ocvCorrectionAlpha = 0.002f;
const unsigned long lowTimeConfirmationMs = 10000UL;
const unsigned long criticalVoltageConfirmationMs = 2000UL;
const unsigned long batterySaveIntervalMs = 120000UL;
const float batterySaveSocStep = 0.01f;

float batteryVoltageV = 0.0f;
float dischargeCurrentA = 0.0f;
float batteryCurrentA = 0.0f;  // ROS: desarjda negatif
float batteryPowerW = 0.0f;    // ROS: desarjda negatif
float remainingChargeAh = 0.0f;
float batteryPercentage = 0.0f;
float averageDischargeCurrentA = 0.0f;
float remainingMinutes = -1.0f;
float batteryConfidence = 0.0f;
float previousDischargeCurrentA = 0.0f;
bool batteryFilterInitialized = false;
bool lowBatteryAlarm = false;
unsigned long lastBatteryTime = 0;
unsigned long batteryEstimateRuntimeMs = 0;
unsigned long restStartMs = 0;
unsigned long lowTimeCandidateMs = 0;
unsigned long criticalVoltageCandidateMs = 0;
unsigned long lowBatteryAlarmStartedMs = 0;
unsigned long lastBatterySaveMs = 0;
float lastSavedBatteryPercentage = -1.0f;

enum BatteryLedState : uint8_t {
  BATTERY_LED_UNKNOWN,
  BATTERY_LED_RED,
  BATTERY_LED_YELLOW,
  BATTERY_LED_GREEN
};

BatteryLedState batteryLedState = BATTERY_LED_UNKNOWN;

struct StoredBatteryState {
  uint32_t magic;
  uint32_t sequence;
  float remainingAh;
  uint16_t checksum;
};

const uint32_t batteryStateMagic = 0x4B43424DUL;
const uint8_t batteryStateSlotCount = 16;
uint8_t batteryStateSlot = 0;
uint32_t batteryStateSequence = 0;

// --- KONTROL VE ZAMANLAMA ---
const int maxSpeed = 450;
float targetSpeedA = 0;
float targetSpeedB = 0;
const float Kp = 0.8f;
int currentPWMA = 0;
int currentPWMB = 0;
long lastE1 = 0;
long lastE2 = 0;

unsigned long lastControlTime = 0;
const int controlInterval = 100;
unsigned long lastCommandTime = 0;
const int watchDogTimeout = 1000;

void setTargetSpeeds(char command);
void encoder1ISR();
void encoder2ISR();
int readI2cRegister(uint8_t address, uint8_t reg);
bool writeI2cRegister(uint8_t address, uint8_t reg, uint8_t value);
bool initImuDirect(uint8_t address);
bool readImuDirect(float &ax, float &ay, float &az,
                   float &gx, float &gy, float &gz);
void calibrateImu();
void readBattery(unsigned long nowMs);
float estimateBatteryPercentage(float voltage);
void updateBatteryIndicators(unsigned long nowMs);
void updateBatteryAlarm(unsigned long nowMs);
void persistBatteryState(unsigned long nowMs, bool force = false);
bool restoreBatteryState(float &restoredRemainingAh);
uint16_t batteryStateChecksum(const StoredBatteryState &state);

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Wire.setClock(100000);
  Wire.setWireTimeout(50000, true);

  pinMode(enc1A, INPUT_PULLUP); pinMode(enc1B, INPUT_PULLUP);
  pinMode(enc2A, INPUT_PULLUP); pinMode(enc2B, INPUT_PULLUP);
  pinMode(PWMA, OUTPUT); pinMode(AIN1, OUTPUT); pinMode(AIN2, OUTPUT);
  pinMode(PWMB, OUTPUT); pinMode(BIN1, OUTPUT); pinMode(BIN2, OUTPUT);
  pinMode(STBY, OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(redLedPin, OUTPUT);
  pinMode(yellowLedPin, OUTPUT);
  pinMode(greenLedPin, OUTPUT);
  digitalWrite(STBY, HIGH);
  digitalWrite(buzzerPin, buzzerInactiveLevel);
  digitalWrite(redLedPin, LOW);
  digitalWrite(yellowLedPin, LOW);
  digitalWrite(greenLedPin, LOW);

  attachInterrupt(digitalPinToInterrupt(enc1A), encoder1ISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(enc2A), encoder2ISR, CHANGE);

  // Kimlik degeri farkli olan uyumlu klonlari da desteklemek icin
  // sensoru dogrudan MPU60x0 register haritasi ile baslat.
  mpuAvailable = initImuDirect(0x68);
  if (mpuAvailable) {
    mpuAddress = 0x68;
  } else {
    mpuAvailable = initImuDirect(0x69);
    if (mpuAvailable) {
      mpuAddress = 0x69;
    }
  }
  if (mpuAvailable) {
    mpuWhoAmI = readI2cRegister(mpuAddress, 0x75);
    calibrateImu();
  }

  // INA219 bulunamazsa yalniz batarya yayini gecersiz sayilir.
  ina219Available = ina219.begin();
  if (ina219Available) {
    ina219.setCalibration_32V_2A();
    const float shuntVoltageV = ina219.getShuntVoltage_mV() / 1000.0f;
    batteryVoltageV =
        (ina219.getBusVoltage_V() + shuntVoltageV) * voltageCalibrationGain;
    dischargeCurrentA =
        ((ina219.getCurrent_mA() / 1000.0f) * currentCalibrationGain) -
        currentOffsetA;
    if (fabs(dischargeCurrentA) < 0.01f) dischargeCurrentA = 0.0f;

    batteryFilterInitialized = true;
    previousDischargeCurrentA = dischargeCurrentA;
    averageDischargeCurrentA = max(dischargeCurrentA, 0.0f);

    const float voltageBasedPercentage =
        estimateBatteryPercentage(batteryVoltageV);
    float restoredRemainingAh = 0.0f;
    const bool stateRestored = restoreBatteryState(restoredRemainingAh);
    const float restoredPercentage = restoredRemainingAh / batteryCapacityAh;

    // Tam dolu paket yeni takildiysa eski EEPROM kaydini kullanma. Diger
    // durumlarda, gerilim tahminiyle makul derecede uyusan sayaci geri yukle.
    if (batteryVoltageV >= batteryFullVoltage - 0.08f) {
      remainingChargeAh = batteryCapacityAh;
      batteryConfidence = 1.0f;
    } else if (stateRestored &&
               fabs(restoredPercentage - voltageBasedPercentage) <= 0.30f) {
      remainingChargeAh = restoredRemainingAh;
      batteryConfidence = 0.75f;
    } else {
      remainingChargeAh = voltageBasedPercentage * batteryCapacityAh;
      batteryConfidence = 0.35f;
    }
    batteryPercentage = remainingChargeAh / batteryCapacityAh;
    lastSavedBatteryPercentage = batteryPercentage;
  }
  lastBatteryTime = millis();
  lastBatterySaveMs = lastBatteryTime;
  updateBatteryIndicators(lastBatteryTime);

  Serial.print("{\"status\":\"baslatildi\",\"imu_ok\":");
  Serial.print(mpuAvailable ? "true" : "false");
  Serial.print(",\"imu_address\":");
  if (mpuAvailable) {
    Serial.print(mpuAddress == 0x68 ? "\"0x68\"" : "\"0x69\"");
  } else {
    Serial.print("null");
  }
  Serial.print(",\"imu_who_am_i\":");
  if (mpuAvailable) {
    Serial.print(mpuWhoAmI);
  } else {
    Serial.print("null");
  }
  Serial.print(",\"battery_ok\":");
  Serial.print(ina219Available ? "true" : "false");
  Serial.println("}");
}

void loop() {
  if (Serial.available() > 0) {
    const char cmd = Serial.read();
    setTargetSpeeds(cmd);
    lastCommandTime = millis();
  }

  if (millis() - lastCommandTime > watchDogTimeout) {
    targetSpeedA = 0;
    targetSpeedB = 0;
  }

  if (millis() - lastControlTime < controlInterval) {
    return;
  }

  const unsigned long nowMs = millis();
  lastControlTime = nowMs;

  noInterrupts();
  const long e1 = encoder1Count;
  const long e2 = encoder2Count;
  interrupts();

  const long encLeftForOdom = encoderLeftSign * e1;
  const long encRightForOdom = encoderRightSign * e2;
  const long currentSpeedA = abs(e1 - lastE1);
  const long currentSpeedB = abs(e2 - lastE2);
  lastE1 = e1;
  lastE2 = e2;

  if (targetSpeedA > 0 || targetSpeedB > 0) {
    currentPWMA += (int)((targetSpeedA - currentSpeedA) * Kp);
    currentPWMB += (int)((targetSpeedB - currentSpeedB) * Kp);
  } else {
    currentPWMA = 0;
    currentPWMB = 0;
  }

  currentPWMA = constrain(currentPWMA, 0, 255);
  currentPWMB = constrain(currentPWMB, 0, 255);
  analogWrite(PWMA, currentPWMA);
  analogWrite(PWMB, currentPWMB);

  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  float gx = 0.0f, gy = 0.0f, gz = 0.0f;
  if (mpuAvailable) {
    if (readImuDirect(ax, ay, az, gx, gy, gz)) {
      ax -= accelX_offset;
      ay -= accelY_offset;
      az -= accelZ_offset;
      gx -= gyroX_offset;
      gy -= gyroY_offset;
      gz -= gyroZ_offset;
    } else {
      mpuAvailable = false;
    }
  }

  if (ina219Available) {
    readBattery(nowMs);
  }
  updateBatteryIndicators(nowMs);

  Serial.print("{\"t_ms\":"); Serial.print(nowMs);
  Serial.print(",\"imu_ok\":"); Serial.print(mpuAvailable ? "true" : "false");
  Serial.print(",\"ax\":"); Serial.print(ax, 6);
  Serial.print(",\"ay\":"); Serial.print(ay, 6);
  Serial.print(",\"az\":"); Serial.print(az, 6);
  Serial.print(",\"gx\":"); Serial.print(gx, 6);
  Serial.print(",\"gy\":"); Serial.print(gy, 6);
  Serial.print(",\"gz\":"); Serial.print(gz, 6);
  Serial.print(",\"enc_l\":"); Serial.print(encLeftForOdom);
  Serial.print(",\"enc_r\":"); Serial.print(encRightForOdom);
  Serial.print(",\"battery_ok\":"); Serial.print(ina219Available ? "true" : "false");
  Serial.print(",\"voltage\":"); Serial.print(batteryVoltageV, 3);
  Serial.print(",\"current\":"); Serial.print(batteryCurrentA, 3);
  Serial.print(",\"power\":"); Serial.print(batteryPowerW, 3);
  Serial.print(",\"charge\":"); Serial.print(remainingChargeAh, 4);
  Serial.print(",\"capacity\":"); Serial.print(batteryCapacityAh, 2);
  Serial.print(",\"percentage\":"); Serial.print(batteryPercentage, 4);
  Serial.print(",\"cell_voltage_avg\":");
  Serial.print(batteryVoltageV / batteryCellCount, 3);
  Serial.print(",\"average_discharge_current\":");
  Serial.print(averageDischargeCurrentA, 3);
  Serial.print(",\"remaining_minutes\":");
  if (remainingMinutes >= 0.0f) Serial.print(remainingMinutes, 1);
  else Serial.print("null");
  Serial.print(",\"battery_confidence\":");
  Serial.print(batteryConfidence, 3);
  Serial.print(",\"low_battery\":");
  Serial.print(lowBatteryAlarm ? "true" : "false");
  Serial.println("}");
}

int readI2cRegister(uint8_t address, uint8_t reg) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(address, static_cast<uint8_t>(1)) != 1) return -1;
  return Wire.read();
}

bool writeI2cRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool initImuDirect(uint8_t address) {
  Wire.beginTransmission(address);
  if (Wire.endTransmission() != 0) return false;

  // Uyku modundan cik, +-2 g, +-250 derece/s ve yaklasik 20 Hz DLPF.
  if (!writeI2cRegister(address, 0x6B, 0x00)) return false;
  delay(100);
  if (!writeI2cRegister(address, 0x19, 0x09)) return false;
  if (!writeI2cRegister(address, 0x1A, 0x04)) return false;
  if (!writeI2cRegister(address, 0x1B, 0x00)) return false;
  if (!writeI2cRegister(address, 0x1C, 0x00)) return false;
  return true;
}

bool readImuDirect(float &ax, float &ay, float &az,
                   float &gx, float &gy, float &gz) {
  Wire.beginTransmission(mpuAddress);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(mpuAddress, static_cast<uint8_t>(14)) != 14) return false;

  const int16_t rawAx = (static_cast<int16_t>(Wire.read()) << 8) | Wire.read();
  const int16_t rawAy = (static_cast<int16_t>(Wire.read()) << 8) | Wire.read();
  const int16_t rawAz = (static_cast<int16_t>(Wire.read()) << 8) | Wire.read();
  Wire.read(); Wire.read();  // Sicaklik bu projede kullanilmiyor.
  const int16_t rawGx = (static_cast<int16_t>(Wire.read()) << 8) | Wire.read();
  const int16_t rawGy = (static_cast<int16_t>(Wire.read()) << 8) | Wire.read();
  const int16_t rawGz = (static_cast<int16_t>(Wire.read()) << 8) | Wire.read();

  ax = (rawAx / 16384.0f) * gravityMs2;
  ay = (rawAy / 16384.0f) * gravityMs2;
  az = (rawAz / 16384.0f) * gravityMs2;
  const float gyroScale = DEG_TO_RAD / 131.0f;
  gx = rawGx * gyroScale;
  gy = rawGy * gyroScale;
  gz = rawGz * gyroScale;
  return true;
}

void calibrateImu() {
  float axSum = 0.0f, aySum = 0.0f, azSum = 0.0f;
  float gxSum = 0.0f, gySum = 0.0f, gzSum = 0.0f;

  for (int i = 0; i < imuCalibrationSamples; i++) {
    float ax, ay, az, gx, gy, gz;
    if (!readImuDirect(ax, ay, az, gx, gy, gz)) {
      mpuAvailable = false;
      return;
    }
    axSum += ax;
    aySum += ay;
    azSum += az;
    gxSum += gx;
    gySum += gy;
    gzSum += gz;
    delay(5);
  }

  accelX_offset = axSum / imuCalibrationSamples;
  accelY_offset = aySum / imuCalibrationSamples;
  accelZ_offset = (azSum / imuCalibrationSamples) - gravityMs2;
  gyroX_offset = gxSum / imuCalibrationSamples;
  gyroY_offset = gySum / imuCalibrationSamples;
  gyroZ_offset = gzSum / imuCalibrationSamples;
}

float estimateBatteryPercentage(float voltage) {
  // Dinlenme gerilimi icin genel 4.20 V LiPo OCV egrisi. Uc noktalar
  // kullanicinin belirledigi 3.00 V ve 4.20 V sinirlaridir.
  static const float cellVoltages[] = {
      3.00f, 3.30f, 3.50f, 3.60f, 3.68f, 3.73f, 3.77f,
      3.80f, 3.84f, 3.89f, 3.96f, 4.05f, 4.12f, 4.20f};
  static const float percentages[] = {
      0.00f, 0.01f, 0.03f, 0.06f, 0.12f, 0.20f, 0.30f,
      0.40f, 0.50f, 0.60f, 0.70f, 0.80f, 0.90f, 1.00f};
  const uint8_t pointCount = sizeof(cellVoltages) / sizeof(cellVoltages[0]);
  const float cellVoltage = voltage / batteryCellCount;

  if (cellVoltage <= cellVoltages[0]) return 0.0f;
  if (cellVoltage >= cellVoltages[pointCount - 1]) return 1.0f;

  for (uint8_t i = 1; i < pointCount; ++i) {
    if (cellVoltage <= cellVoltages[i]) {
      const float fraction =
          (cellVoltage - cellVoltages[i - 1]) /
          (cellVoltages[i] - cellVoltages[i - 1]);
      return percentages[i - 1] +
             fraction * (percentages[i] - percentages[i - 1]);
    }
  }
  return 1.0f;
}

void readBattery(unsigned long nowMs) {
  const float shuntVoltageV = ina219.getShuntVoltage_mV() / 1000.0f;
  const float measuredVoltage =
      (ina219.getBusVoltage_V() + shuntVoltageV) * voltageCalibrationGain;
  float measuredCurrent =
      ((ina219.getCurrent_mA() / 1000.0f) * currentCalibrationGain) -
      currentOffsetA;
  if (fabs(measuredCurrent) < 0.01f) measuredCurrent = 0.0f;

  // INA219 32 V / 2 A ayari disindaki veya bozuk ornekleri hesaba katma.
  if (!isfinite(measuredVoltage) || !isfinite(measuredCurrent) ||
      measuredVoltage < 6.0f || measuredVoltage > 26.0f ||
      fabs(measuredCurrent) > 2.1f) {
    return;
  }

  if (!batteryFilterInitialized) {
    batteryVoltageV = measuredVoltage;
    dischargeCurrentA = measuredCurrent;
    previousDischargeCurrentA = measuredCurrent;
    averageDischargeCurrentA = max(measuredCurrent, 0.0f);
    batteryFilterInitialized = true;
  } else {
    batteryVoltageV += batteryFilterAlpha * (measuredVoltage - batteryVoltageV);
    dischargeCurrentA += batteryFilterAlpha * (measuredCurrent - dischargeCurrentA);
  }

  const unsigned long elapsedMs = nowMs - lastBatteryTime;
  const float elapsedSeconds = elapsedMs / 1000.0f;
  if (lastBatteryTime != 0 && elapsedMs <= 2000UL) {
    const float elapsedHours = elapsedSeconds / 3600.0f;
    const float trapezoidCurrent =
        0.5f * (previousDischargeCurrentA + dischargeCurrentA);
    remainingChargeAh -= trapezoidCurrent * elapsedHours;
    remainingChargeAh = constrain(remainingChargeAh, 0.0f, batteryCapacityAh);

    const float positiveCurrent = max(dischargeCurrentA, 0.0f);
    const float averageAlpha =
        elapsedSeconds / (averageCurrentTimeConstantS + elapsedSeconds);
    averageDischargeCurrentA +=
        averageAlpha * (positiveCurrent - averageDischargeCurrentA);

    if (batteryEstimateRuntimeMs < 60000UL) {
      batteryEstimateRuntimeMs =
          min(60000UL, batteryEstimateRuntimeMs + elapsedMs);
    }
  }
  lastBatteryTime = nowMs;
  previousDischargeCurrentA = dischargeCurrentA;

  // Gerilim sadece pil yeterince dinlendiginde coulomb sayacini yavasca
  // duzeltir. Motor yukundeki gerilim cokmeleri yuzdeyi aniden degistirmez.
  if (fabs(dischargeCurrentA) <= restCurrentThresholdA) {
    if (restStartMs == 0) restStartMs = nowMs;
    if (nowMs - restStartMs >= restRequiredMs) {
      const float ocvChargeAh =
          estimateBatteryPercentage(batteryVoltageV) * batteryCapacityAh;
      remainingChargeAh +=
          ocvCorrectionAlpha * (ocvChargeAh - remainingChargeAh);
      batteryConfidence = max(batteryConfidence, 0.90f);
    }
  } else {
    restStartMs = 0;
  }

  batteryPercentage = remainingChargeAh / batteryCapacityAh;
  batteryCurrentA = -dischargeCurrentA;
  batteryPowerW = batteryVoltageV * batteryCurrentA;

  if (averageDischargeCurrentA >= 0.05f &&
      batteryEstimateRuntimeMs >= 10000UL) {
    remainingMinutes =
        (remainingChargeAh / averageDischargeCurrentA) * 60.0f;
  } else {
    remainingMinutes = -1.0f;
  }

  const float runtimeConfidence =
      0.35f + 0.65f * (batteryEstimateRuntimeMs / 60000.0f);
  batteryConfidence = max(batteryConfidence, min(runtimeConfidence, 1.0f));

  updateBatteryAlarm(nowMs);
  persistBatteryState(nowMs);
}

void updateBatteryAlarm(unsigned long nowMs) {
  const bool lowTime =
      remainingMinutes >= 0.0f && remainingMinutes <= 5.0f;
  if (lowTime) {
    if (lowTimeCandidateMs == 0) lowTimeCandidateMs = nowMs;
  } else if (remainingMinutes < 0.0f || remainingMinutes > 6.0f) {
    lowTimeCandidateMs = 0;
  }

  const bool criticalVoltage = batteryVoltageV <= criticalPackVoltage;
  if (criticalVoltage) {
    if (criticalVoltageCandidateMs == 0) criticalVoltageCandidateMs = nowMs;
  } else if (batteryVoltageV >= criticalPackVoltage + 0.40f) {
    criticalVoltageCandidateMs = 0;
  }

  const bool timeConfirmed =
      lowTimeCandidateMs != 0 &&
      nowMs - lowTimeCandidateMs >= lowTimeConfirmationMs;
  const bool voltageConfirmed =
      criticalVoltageCandidateMs != 0 &&
      nowMs - criticalVoltageCandidateMs >= criticalVoltageConfirmationMs;

  if (!lowBatteryAlarm &&
      (timeConfirmed || voltageConfirmed || batteryPercentage <= 0.03f)) {
    lowBatteryAlarm = true;
    lowBatteryAlarmStartedMs = nowMs;
  } else if ((remainingMinutes < 0.0f || remainingMinutes > 6.0f) &&
             batteryPercentage > 0.05f &&
             batteryVoltageV > criticalPackVoltage + 0.40f) {
    lowBatteryAlarm = false;
    lowBatteryAlarmStartedMs = 0;
  }
}

void updateBatteryIndicators(unsigned long nowMs) {
  if (!ina219Available) {
    const bool errorPulse = (nowMs % ledBlinkPeriodMs) < ledBlinkOnMs;
    digitalWrite(redLedPin, errorPulse ? HIGH : LOW);
    digitalWrite(yellowLedPin, LOW);
    digitalWrite(greenLedPin, LOW);
    noTone(buzzerPin);
    digitalWrite(buzzerPin, buzzerInactiveLevel);
    batteryLedState = BATTERY_LED_UNKNOWN;
    return;
  }

  if (batteryLedState == BATTERY_LED_UNKNOWN) {
    if (batteryPercentage >= 0.80f) batteryLedState = BATTERY_LED_GREEN;
    else if (batteryPercentage >= 0.50f) batteryLedState = BATTERY_LED_YELLOW;
    else batteryLedState = BATTERY_LED_RED;
  } else if (batteryLedState == BATTERY_LED_GREEN && batteryPercentage < 0.78f) {
    batteryLedState =
        batteryPercentage < 0.48f ? BATTERY_LED_RED : BATTERY_LED_YELLOW;
  } else if (batteryLedState == BATTERY_LED_YELLOW) {
    if (batteryPercentage >= 0.82f) batteryLedState = BATTERY_LED_GREEN;
    else if (batteryPercentage < 0.48f) batteryLedState = BATTERY_LED_RED;
  } else if (batteryLedState == BATTERY_LED_RED && batteryPercentage >= 0.52f) {
    batteryLedState =
        batteryPercentage >= 0.82f ? BATTERY_LED_GREEN : BATTERY_LED_YELLOW;
  }

  const bool ledPulse = (nowMs % ledBlinkPeriodMs) < ledBlinkOnMs;
  digitalWrite(redLedPin,
               batteryLedState == BATTERY_LED_RED && ledPulse ? HIGH : LOW);
  digitalWrite(yellowLedPin,
               batteryLedState == BATTERY_LED_YELLOW && ledPulse ? HIGH : LOW);
  digitalWrite(greenLedPin,
               batteryLedState == BATTERY_LED_GREEN && ledPulse ? HIGH : LOW);

  // LOW-tetiklemeli aktif buzzer alarm basladiginda ve daha sonra her 20
  // saniyede bir, 3 saniyelik bildirim ritmi calar.
  if (lowBatteryAlarm) {
    const unsigned long cycleTime =
        (nowMs - lowBatteryAlarmStartedMs) % buzzerMelodyPeriodMs;
    bool buzzerOn = false;
    if (cycleTime < buzzerMelodyDurationMs) {
      buzzerOn =
          cycleTime < 140UL ||
          (cycleTime >= 230UL && cycleTime < 370UL) ||
          (cycleTime >= 460UL && cycleTime < 740UL) ||
          (cycleTime >= 900UL && cycleTime < 1080UL) ||
          (cycleTime >= 1160UL && cycleTime < 1340UL) ||
          (cycleTime >= 1420UL && cycleTime < 1760UL) ||
          (cycleTime >= 1940UL && cycleTime < 2140UL) ||
          (cycleTime >= 2240UL && cycleTime < 2440UL) ||
          (cycleTime >= 2540UL && cycleTime < 2920UL);
    }
    digitalWrite(buzzerPin,
                 buzzerOn ? buzzerActiveLevel : buzzerInactiveLevel);
  } else {
    noTone(buzzerPin);
    digitalWrite(buzzerPin, buzzerInactiveLevel);
  }
}

uint16_t batteryStateChecksum(const StoredBatteryState &state) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&state);
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < offsetof(StoredBatteryState, checksum); ++i) {
    crc ^= static_cast<uint16_t>(bytes[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
  }
  return crc;
}

bool restoreBatteryState(float &restoredRemainingAh) {
  bool found = false;
  StoredBatteryState best = {};
  for (uint8_t slot = 0; slot < batteryStateSlotCount; ++slot) {
    StoredBatteryState candidate = {};
    EEPROM.get(slot * sizeof(StoredBatteryState), candidate);
    const bool valid =
        candidate.magic == batteryStateMagic &&
        candidate.checksum == batteryStateChecksum(candidate) &&
        isfinite(candidate.remainingAh) && candidate.remainingAh >= 0.0f &&
        candidate.remainingAh <= batteryCapacityAh;
    if (valid && (!found ||
                  static_cast<int32_t>(candidate.sequence - best.sequence) > 0)) {
      best = candidate;
      batteryStateSlot = slot;
      found = true;
    }
  }

  if (!found) return false;
  batteryStateSequence = best.sequence;
  restoredRemainingAh = best.remainingAh;
  return true;
}

void persistBatteryState(unsigned long nowMs, bool force) {
  if (!force) {
    if (nowMs - lastBatterySaveMs < batterySaveIntervalMs) return;
    if (lastSavedBatteryPercentage >= 0.0f &&
        fabs(batteryPercentage - lastSavedBatteryPercentage) <
            batterySaveSocStep) {
      return;
    }
  }

  batteryStateSlot = (batteryStateSlot + 1) % batteryStateSlotCount;
  StoredBatteryState state = {};
  state.magic = batteryStateMagic;
  state.sequence = ++batteryStateSequence;
  state.remainingAh = remainingChargeAh;
  state.checksum = batteryStateChecksum(state);
  EEPROM.put(batteryStateSlot * sizeof(StoredBatteryState), state);
  lastBatterySaveMs = nowMs;
  lastSavedBatteryPercentage = batteryPercentage;
}

void encoder1ISR() {
  if (digitalRead(enc1A) == digitalRead(enc1B)) encoder1Count++;
  else encoder1Count--;
}

void encoder2ISR() {
  if (digitalRead(enc2A) != digitalRead(enc2B)) encoder2Count++;
  else encoder2Count--;
}

void setTargetSpeeds(char command) {
  switch (command) {
    case 'W':
      digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW);
      digitalWrite(BIN1, LOW);  digitalWrite(BIN2, HIGH);
      targetSpeedA = maxSpeed; targetSpeedB = maxSpeed;
      break;
    case 'X':
      digitalWrite(AIN1, LOW);  digitalWrite(AIN2, HIGH);
      digitalWrite(BIN1, HIGH); digitalWrite(BIN2, LOW);
      targetSpeedA = maxSpeed; targetSpeedB = maxSpeed;
      break;
    case 'A':
      digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH);
      digitalWrite(BIN1, LOW); digitalWrite(BIN2, HIGH);
      targetSpeedA = maxSpeed * 0.7f; targetSpeedB = maxSpeed * 0.7f;
      break;
    case 'D':
      digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW);
      digitalWrite(BIN1, HIGH); digitalWrite(BIN2, LOW);
      targetSpeedA = maxSpeed * 0.7f; targetSpeedB = maxSpeed * 0.7f;
      break;
    case 'S':
    default:
      targetSpeedA = 0; targetSpeedB = 0;
      digitalWrite(AIN1, LOW); digitalWrite(AIN2, LOW);
      digitalWrite(BIN1, LOW); digitalWrite(BIN2, LOW);
      break;
  }
}
