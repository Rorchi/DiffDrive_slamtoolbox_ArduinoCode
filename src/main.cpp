#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

// --- MOTOR VE ENCODER PINLERI ---
const int enc1A = 2;  const int enc1B = 3;
const int enc2A = 18; const int enc2B = 19;
const int PWMA = 5;   const int AIN1 = 7; const int AIN2 = 8;
const int PWMB = 6;   const int BIN1 = 9; const int BIN2 = 10;
const int STBY = 11;

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

// --- 4S 7000 mAh LiPo / INA219 ---
const float batteryCapacityAh = 7.0f;
const float batteryFullVoltage = 16.8f;
const float batteryEmptyVoltage = 14.0f;
const float currentOffsetA = 0.0f;

float batteryVoltageV = 0.0f;
float dischargeCurrentA = 0.0f;
float batteryCurrentA = 0.0f;  // ROS: desarjda negatif
float batteryPowerW = 0.0f;    // ROS: desarjda negatif
float remainingChargeAh = 0.0f;
float batteryPercentage = 0.0f;
unsigned long lastBatteryTime = 0;

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
  digitalWrite(STBY, HIGH);

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
    batteryVoltageV = ina219.getBusVoltage_V() + shuntVoltageV;
    batteryPercentage = estimateBatteryPercentage(batteryVoltageV);
    remainingChargeAh = batteryPercentage * batteryCapacityAh;
  }
  lastBatteryTime = millis();

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
  const float result =
      (voltage - batteryEmptyVoltage) /
      (batteryFullVoltage - batteryEmptyVoltage);
  return constrain(result, 0.0f, 1.0f);
}

void readBattery(unsigned long nowMs) {
  const float shuntVoltageV = ina219.getShuntVoltage_mV() / 1000.0f;
  batteryVoltageV = ina219.getBusVoltage_V() + shuntVoltageV;
  dischargeCurrentA = (ina219.getCurrent_mA() / 1000.0f) - currentOffsetA;
  if (fabs(dischargeCurrentA) < 0.01f) dischargeCurrentA = 0.0f;

  if (lastBatteryTime != 0) {
    const float elapsedHours = (nowMs - lastBatteryTime) / 3600000.0f;
    remainingChargeAh -= dischargeCurrentA * elapsedHours;
    remainingChargeAh = constrain(remainingChargeAh, 0.0f, batteryCapacityAh);
  }
  lastBatteryTime = nowMs;

  batteryPercentage = remainingChargeAh / batteryCapacityAh;
  batteryCurrentA = -dischargeCurrentA;
  batteryPowerW = batteryVoltageV * batteryCurrentA;
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
