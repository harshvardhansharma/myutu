// Step 2 — MPU6050 bring-up.
// Goal: confirm the accelerometer responds, and measure the real magnitudes of a tap
// and a shake on THIS build, so Step 5's thresholds are measured rather than guessed.
//
// No Adafruit library. Their begin() reads WHO_AM_I and refuses to start unless it
// returns exactly 0x68 — and the clone chips these breakout boards ship with commonly
// report 0x70, 0x72 or 0x98. The silicon is fine and the register map is identical, so
// we talk to the registers directly and skip the ID check. This also drops a dependency.
//
// Prints continuously and never halts: a sketch stuck in while(true) is indistinguishable
// from a dead board on a serial monitor attached later.

#include <Wire.h>

#define I2C_SDA 21
#define I2C_SCL 22

// Register map (identical on every MPU6050 and its clones).
#define REG_SMPLRT_DIV   0x19
#define REG_CONFIG       0x1A
#define REG_GYRO_CONFIG  0x1B
#define REG_ACCEL_CONFIG 0x1C
#define REG_ACCEL_XOUT_H 0x3B
#define REG_PWR_MGMT_1   0x6B
#define REG_WHO_AM_I     0x75

// +-8g leaves headroom for a shake without clipping; a firm tap can exceed +-4g.
#define ACCEL_LSB_PER_G  4096.0f   // AFS_SEL = 2
#define GYRO_LSB_PER_DPS   65.5f   // FS_SEL  = 1  (+-500 deg/s)

uint8_t addr = 0x68;
bool mpuOk = false;

// Measured at startup rather than assumed. These parts ship with a percent or two of
// scale error and a real gyro offset — this one reads 10.07 m/s^2 for gravity and 4.3
// deg/s while sitting still. Left uncorrected, that bias lands directly in every
// threshold measured in Step 5.
float gravityBase = 9.81f;
float gxBias = 0, gyBias = 0, gzBias = 0;

const unsigned long WINDOW_MS = 2000;
unsigned long windowStart = 0;
float peakJolt = 0, peakSpin = 0;

// Session maxima, never reset. The 2 s window is for watching live; these are so a
// tap or a shake can be performed now and read off the port minutes later.
float maxJolt = 0, maxSpin = 0;

bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool readRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

bool present(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

void scanI2C() {
  Serial.println("Scanning I2C bus...");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (present(a)) { Serial.printf("  device found at 0x%02X\n", a); found++; }
  }
  if (!found) Serial.println("  nothing on the bus — check SDA(21), SCL(22), VCC, GND");
}

bool initMPU() {
  if      (present(0x68)) addr = 0x68;
  else if (present(0x69)) addr = 0x69;
  else return false;

  uint8_t who = 0;
  readRegs(REG_WHO_AM_I, &who, 1);
  Serial.printf("Found device at 0x%02X, WHO_AM_I = 0x%02X", addr, who);
  if (who != 0x68) Serial.print("  (clone — genuine parts report 0x68; harmless)");
  Serial.println();

  // Wake it: the chip boots in sleep mode and reads all zeroes until this is cleared.
  if (!writeReg(REG_PWR_MGMT_1, 0x00)) return false;
  delay(100);
  writeReg(REG_SMPLRT_DIV,   0x04);   // 200 Hz sample rate
  writeReg(REG_CONFIG,       0x03);   // DLPF 44 Hz — trims hand tremor, keeps taps
  writeReg(REG_GYRO_CONFIG,  0x08);   // +-500 deg/s
  writeReg(REG_ACCEL_CONFIG, 0x10);   // +-8 g
  delay(50);
  return true;
}

// Averages the resting state. The board must be still and flat for this second.
void calibrate() {
  const int N = 150;
  float magSum = 0, gx = 0, gy = 0, gz = 0;
  int taken = 0;
  for (int i = 0; i < N; i++) {
    uint8_t b[14];
    if (!readRegs(REG_ACCEL_XOUT_H, b, 14)) { delay(5); continue; }
    float ax = (int16_t)((b[0] << 8) | b[1]) / ACCEL_LSB_PER_G * 9.81f;
    float ay = (int16_t)((b[2] << 8) | b[3]) / ACCEL_LSB_PER_G * 9.81f;
    float az = (int16_t)((b[4] << 8) | b[5]) / ACCEL_LSB_PER_G * 9.81f;
    magSum += sqrtf(ax * ax + ay * ay + az * az);
    gx += (int16_t)((b[8]  << 8) | b[9]);
    gy += (int16_t)((b[10] << 8) | b[11]);
    gz += (int16_t)((b[12] << 8) | b[13]);
    taken++;
    delay(5);
  }
  if (taken < 10) return;
  gravityBase = magSum / taken;
  gxBias = gx / taken;
  gyBias = gy / taken;
  gzBias = gz / taken;
  Serial.printf("Calibrated: resting magnitude %.2f m/s^2 (ideal 9.81), gyro bias %.0f/%.0f/%.0f\n",
                gravityBase, gxBias, gyBias, gzBias);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nStep 2: MPU6050 bring-up (direct register access)");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);              // 100 kHz — breadboard-friendly

  mpuOk = initMPU();
  if (mpuOk) {
    Serial.println("MPU6050 ok");
    Serial.println("Hold still — calibrating for 1s...");
    calibrate();
    Serial.println();
    Serial.println("Tilt the board, then tap it, then shake it.");
    Serial.println("Watch PEAK jolt: that number is the tap/shake threshold for Step 5.\n");
  }
  windowStart = millis();
}

void loop() {
  if (!mpuOk) {
    Serial.println("\nMPU6050 not initialising.");
    scanI2C();
    delay(2000);
    mpuOk = initMPU();
    if (mpuOk) Serial.println("MPU6050 ok — starting readings");
    return;
  }

  uint8_t b[14];
  if (!readRegs(REG_ACCEL_XOUT_H, b, 14)) {
    Serial.println("read failed — bus glitch, retrying");
    mpuOk = false;
    return;
  }

  int16_t axr = (b[0]  << 8) | b[1];
  int16_t ayr = (b[2]  << 8) | b[3];
  int16_t azr = (b[4]  << 8) | b[5];
  int16_t gxr = (b[8]  << 8) | b[9];
  int16_t gyr = (b[10] << 8) | b[11];
  int16_t gzr = (b[12] << 8) | b[13];

  float ax = axr / ACCEL_LSB_PER_G * 9.81f;
  float ay = ayr / ACCEL_LSB_PER_G * 9.81f;
  float az = azr / ACCEL_LSB_PER_G * 9.81f;

  // At rest the total magnitude sits near 9.81 (gravity alone), so the deviation from
  // that is what a tap or a shake actually produces.
  float mag  = sqrtf(ax * ax + ay * ay + az * az);
  float jolt = fabsf(mag - gravityBase);
  float cx = gxr - gxBias, cy = gyr - gyBias, cz = gzr - gzBias;
  float spin = sqrtf(cx * cx + cy * cy + cz * cz) / GYRO_LSB_PER_DPS;

  if (jolt > peakJolt) peakJolt = jolt;
  if (spin > peakSpin) peakSpin = spin;
  if (jolt > maxJolt)  maxJolt  = jolt;
  if (spin > maxSpin)  maxSpin  = spin;

  Serial.printf("ax %6.2f  ay %6.2f  az %6.2f  | jolt %5.2f  spin %6.1f\n",
                ax, ay, az, jolt, spin);

  if (millis() - windowStart >= WINDOW_MS) {
    Serial.printf(">>> PEAK  jolt %.2f  spin %.0f   |   SESSION MAX  jolt %.2f m/s^2  spin %.0f deg/s\n",
                  peakJolt, peakSpin, maxJolt, maxSpin);
    peakJolt = peakSpin = 0;
    windowStart = millis();
  }

  delay(100);
}
