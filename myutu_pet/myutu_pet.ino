// Myutu — the pet.
//
// Display + MPU6050. Faces are driven by real events: tap, double-tap, shake, and an
// idle decay into sleep. Touch (TTP223) and light (LDR) are deferred, so `happy` comes
// from a double tap rather than a pat, and `sleeping` is an idle timeout rather than
// darkness. Both wire in later without touching the state machine.
//
// HARDWARE NOTES, learned the hard way (see README gotchas):
//   * I2C is on 32/33, NOT 21/22. Those sit adjacent to the display's SPI pins on the
//     DOIT header and I2C edges crosstalk into the panel, corrupting its scroll address.
//   * Never toggle RST manually before begin() — it leaves the GC9A01 init incomplete.
//   * No WHO_AM_I check on the MPU: these clones report 0x70/0x72/0x98.
//
// RENDERING: DIRECT TO THE PANEL. Do not reintroduce Arduino_Canvas here.
// It was tried three times — 16-bit offset, 8-bit indexed offset, and 8-bit indexed
// full-size — and every one eventually corrupted the panel's addressing, producing a
// shifted image that wraps around the bottom. Direct drawing has never failed on this
// hardware. Double-buffering is worth revisiting only on the final soldered board.
//
// Flicker is controlled instead by SKIPPING FRAMES THAT WOULD LOOK IDENTICAL. A still
// face costs zero writes and therefore cannot flicker; only genuine motion redraws.

#include <Arduino_GFX_Library.h>
#include <Wire.h>

#define TFT_DC    4
#define TFT_CS    5
#define TFT_SCK  18
#define TFT_MOSI 23
#define TFT_RST  25
#define I2C_SDA  32
#define I2C_SCL  33

// LDR: 3-pin module, digital comparator output only (threshold set by its trimpot).
// D14, not D34 — GPIO34-39 have no internal pull-up, and an open-drain DO would float.
// MEASURED polarity on this module: DO reads HIGH when dark, LOW in room light.
#define LDR_PIN  14
#define DARK_HIGH   1
#define DARK_STABLE_MS 800     // light must hold its state this long before it counts

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, true /* IPS */);

#define BG  RGB565(0, 0, 0)

// One gold for every expression. Mood-shifting colour is implemented below and can be
// switched back on with MOOD_COLOUR 1, but a single colour reads as one creature with
// moods rather than as a status light changing state — the shape carries the emotion.
#define MOOD_COLOUR 0
#define EYE RGB565(255, 208, 70)          // the fallback / neutral amber


const int EYE_LX = 76, EYE_RX = 164, EYE_CY = 115;
const int EW = 58, EH = 76, ER = 22;

// ---------------------------------------------------------------- sensor

#define REG_ACCEL_XOUT_H 0x3B
#define REG_PWR_MGMT_1   0x6B
#define REG_SMPLRT_DIV   0x19
#define REG_CONFIG       0x1A
#define REG_GYRO_CONFIG  0x1B
#define REG_ACCEL_CONFIG 0x1C
#define ACCEL_LSB_PER_G 4096.0f
#define GYRO_LSB_PER_DPS  65.5f

// Measured on this build: resting noise jolt 0.08-0.63 spin 0; real tap/shake jolt 25.2
// spin 230. A 40x gap, so these sit far from the noise without demanding a hard whack.
#define JOLT_TAP        4.0f
#define SPIN_SHAKE    120.0f
#define SPIN_TAP_MAX  100.0f
#define TAP_REFRACTORY  250
#define DOUBLE_TAP_MS   600

#define TILT_HOLD_MS    600     // sustained lean before it counts as curiosity
#define TILT_THRESH     4.5f    // m/s^2 of gravity off-axis, about a 27 degree lean
#define FREEFALL_MAX    4.0f    // total accel this low means it's falling
#define ANGRY_WINDOW   4000     // a second shake this soon after the first = annoyed

#define CURIOUS_HOLD   2000
#define SCARED_HOLD    2000
#define LOVE_HOLD      3000
#define ANGRY_HOLD     2500
#define SURPRISED_HOLD 1500
#define HAPPY_HOLD     2500
#define DIZZY_HOLD     2500
#define SLEEPY_AFTER  20000
#define SLEEPING_AFTER 45000

#define TRANSITION_MS  420      // full close-then-open when the expression changes

// Frame rate is bounded by the flush, not by this delay: 240x240x16 bits is ~23 ms at
// 40 MHz, so this free-runs at roughly 40 fps.
//
// 80 MHz was tried and is WORSE, not better. It pushes frames faster than the panel's
// own ~60 Hz refresh, so the glass shows half-old/half-new frames — visible as heavy
// flicker. This module is the 7-pin variant with no tearing-effect pin broken out, so
// writes cannot be synchronised to its scan-out. Staying under the panel's refresh rate
// is the fix, not more bandwidth.
#define SPI_HZ  40000000
#define FRAME_MS        25      // ~40 fps ceiling; skipped frames cost nothing

uint8_t addr = 0x68;
bool mpuOk = false;
float gravityBase = 9.81f, gxBias = 0, gyBias = 0, gzBias = 0;

// Every face has a real trigger — nothing here is decorative. The accelerometer alone
// distinguishes far more than tap/shake: tilt, free-fall, and repeat gestures are all
// separable, so each new expression is something the pet can genuinely detect.
enum { NEUTRAL, SURPRISED, HAPPY, DIZZY, SLEEPY, SLEEPING, CURIOUS, SCARED, LOVE, ANGRY };
const char *NAMES[] = {"neutral","surprised","happy","dizzy","sleepy","sleeping",
                       "curious","scared","love","angry"};

int shown = NEUTRAL, target = NEUTRAL;

// Saccade state. A dart takes ~90 ms, then the eyes hold for 1.2-3.5 s before the next.
int sacX = 0, sacY = 0, sacFromX = 0, sacFromY = 0, sacToX = 0, sacToY = 0;
unsigned long sacStart = 0, sacNext = 0;
bool sacMoving = false;
bool transitioning = false;
unsigned long transStart = 0;
unsigned long holdUntil = 0, lastInteraction = 0, lastTapAt = 0, lastEventAt = 0, blinkAt = 0;
unsigned long tiltSince = 0, lastShakeAt = 0, freefallSince = 0;
bool isDark = false, lastRawDark = false;
unsigned long darkChangedAt = 0;
int tapCount = 0, tiltDir = 0;

// ---------------------------------------------------------------- i2c

bool writeReg(uint8_t r, uint8_t v) {
  Wire.beginTransmission(addr); Wire.write(r); Wire.write(v);
  return Wire.endTransmission() == 0;
}
bool readRegs(uint8_t r, uint8_t *b, uint8_t n) {
  Wire.beginTransmission(addr); Wire.write(r);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) b[i] = Wire.read();
  return true;
}
bool present(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }

bool initMPU() {
  if      (present(0x68)) addr = 0x68;
  else if (present(0x69)) addr = 0x69;
  else return false;
  if (!writeReg(REG_PWR_MGMT_1, 0x00)) return false;   // wake; it boots asleep
  delay(100);
  writeReg(REG_SMPLRT_DIV, 0x04);
  writeReg(REG_CONFIG, 0x03);
  writeReg(REG_GYRO_CONFIG, 0x08);
  writeReg(REG_ACCEL_CONFIG, 0x10);
  delay(50);
  return true;
}

void calibrate() {
  const int N = 100;
  float magSum = 0, gx = 0, gy = 0, gz = 0; int taken = 0;
  for (int i = 0; i < N; i++) {
    uint8_t b[14];
    if (!readRegs(REG_ACCEL_XOUT_H, b, 14)) { delay(5); continue; }
    float ax = (int16_t)((b[0]<<8)|b[1]) / ACCEL_LSB_PER_G * 9.81f;
    float ay = (int16_t)((b[2]<<8)|b[3]) / ACCEL_LSB_PER_G * 9.81f;
    float az = (int16_t)((b[4]<<8)|b[5]) / ACCEL_LSB_PER_G * 9.81f;
    magSum += sqrtf(ax*ax + ay*ay + az*az);
    gx += (int16_t)((b[8]<<8)|b[9]);
    gy += (int16_t)((b[10]<<8)|b[11]);
    gz += (int16_t)((b[12]<<8)|b[13]);
    taken++; delay(5);
  }
  if (taken < 10) return;
  gravityBase = magSum/taken; gxBias = gx/taken; gyBias = gy/taken; gzBias = gz/taken;
}

// ---------------------------------------------------------------- drawing

// Smoothstep. Linear motion is what makes animation look mechanical; easing every
// transition and blink is most of the difference between "a screen" and "a creature".
float ease(float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  return t * t * (3.0f - 2.0f * t);
}

// Overshoots past 1 and settles back. Eyes that spring open read as alive; eyes that
// arrive exactly on target and stop read as a slideshow. This is the single biggest
// contributor to the animation feeling organic.
float easeOutBack(float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  const float c1 = 1.70158f, c3 = c1 + 1.0f;
  float u = t - 1.0f;
  return 1.0f + c3 * u * u * u + c1 * u * u;
}

void eyeRect(int cx,int cy,int w,int h,int r,uint16_t c){
  if (h < 3) h = 3;
  if (r > h/2) r = h/2;
  gfx->fillRoundRect(cx-w/2, cy-h/2, w, h, r, c);
}
// A downward-opening arc: a disc with a second disc punched out below it.
void eyeArc(int cx,int cy,int r,int th,uint16_t c){
  if (th < 3) th = 3;
  gfx->fillCircle(cx, cy, r, c);
  gfx->fillCircle(cx, cy + th, r, BG);
}
void thickLine(int x0,int y0,int x1,int y1,int t,uint16_t c){
  for (int i=-t/2;i<=t/2;i++){
    gfx->drawLine(x0+i,y0,x1+i,y1,c);
    gfx->drawLine(x0,y0+i,x1,y1+i,c);
  }
}
// The angle carries the emotion, so one call mirrors correctly for either eye.
void brow(int cx,int y,int hl,int inDy,int outDy,bool left,uint16_t c){
  int xi = left ? cx+hl : cx-hl, xo = left ? cx-hl : cx+hl;
  thickLine(xo, y+outDy, xi, y+inDy, 9, c);
}

// Repairs the panel's vertical scroll start address (GC9A01 command 0x37 / VSCRSADD).
//
// The recurring "image shifted up and wrapping around the bottom" fault is exactly what
// a non-zero scroll address looks like. It has appeared with a canvas and without one,
// after minutes of correct operation, which points at the panel's register being
// disturbed by something environmental rather than at the drawing code. Rather than keep
// hunting the cause, the register is simply re-zeroed before every frame — three bytes
// over SPI, far cheaper than the 57,600-pixel frame that follows it, and it makes the
// corruption self-healing within one frame instead of permanent.
void resetScroll() {
  bus->beginWrite();
  bus->writeCommand(0x37);
  bus->write(0x00);
  bus->write(0x00);
  bus->endWrite();
}

void heart(int cx, int cy, int sz, uint16_t c) {
  gfx->fillCircle(cx - sz/2, cy - sz/3, sz/2, c);
  gfx->fillCircle(cx + sz/2, cy - sz/3, sz/2, c);
  gfx->fillTriangle(cx - sz, cy - sz/6, cx + sz, cy - sz/6, cx, cy + sz, c);
}

// Slices a wedge off the top of an eye. The lid angling down toward the nose is anger;
// this is the whole expression, which is why no brow is needed on top of it.
void lidWedge(int cx, int cy, int w, int h, int depth, bool left) {
  int x0 = cx - w/2 - 2, x1 = cx + w/2 + 2, top = cy - h/2 - 2;
  if (left) gfx->fillTriangle(x0, top, x1, top, x1, top + depth, BG);
  else      gfx->fillTriangle(x0, top, x1, top, x0, top + depth, BG);
}

uint16_t faceColour(int e) {
#if !MOOD_COLOUR
  return EYE;
#else
  switch (e) {
    case SURPRISED: return RGB565(130, 235, 255);   // bright cyan — alert, cold shock
    case HAPPY:     return RGB565(255, 232, 120);   // warm gold, brighter than neutral
    case LOVE:      return RGB565(255, 110, 150);   // pink
    case ANGRY:     return RGB565(255,  85,  55);   // red-orange
    case DIZZY:     return RGB565(195, 140, 255);   // violet
    case SCARED:    return RGB565(205, 205, 255);   // washed-out lilac, drained of colour
    case CURIOUS:   return RGB565(130, 240, 190);   // mint
    case SLEEPY:    return RGB565(150, 180, 225);   // cooling off
    case SLEEPING:  return RGB565( 80, 120, 190);   // dim blue, lowest energy
    default:        return EYE;                     // neutral amber
  }
#endif
}

// `open` (0..1) scales every face's vertical extent, so a blink or a transition reads
// as the eyes closing rather than as one face being swapped for another.
void drawFace(int e, float open, unsigned long t) {
  resetScroll();
  gfx->fillScreen(BG);
  uint16_t col = faceColour(e);

  int bob = (int)(2.0f * sinf(t / 1500.0f));

  // Saccades, not drift. Real eyes dart and hold; they do not glide. Anki's animators
  // called this out as what sells a synthetic eye as alive — a slow pan reads as a
  // camera panning, a quick flick and hold reads as a creature choosing where to look.
  int dx = sacX, dy = sacY;

  // Squash and stretch: as the eyes close they widen, as they spring open they narrow.
  // Volume looks conserved, which is what stops a blink reading as a shape being scaled.
  float o  = (open > 1.0f) ? 1.0f : open;
  float sx = 1.0f + 0.22f * (1.0f - o);

  int ey  = EYE_CY + bob + dy;
  int lx  = EYE_LX + dx, rx = EYE_RX + dx;
  int h   = (int)(EH * open);
  int w   = (int)(EW * sx);
  bool wide = open > 0.35f;         // brows disappear once the eyes are nearly shut

  switch (e) {
    case NEUTRAL:
      eyeRect(lx, ey, w, h, ER, col);
      eyeRect(rx, ey, w, h, ER, col);
      break;

    case SURPRISED:
      // An ellipse, not a circle, so it can squash smoothly through a blink.
      // Vector's idiom: eyes morph from rounded rectangles toward circles for surprise.
      // Bigger AND rounder AND higher — all three, or it just reads as "awake".
      gfx->fillEllipse(lx, ey-8, (int)(42*sx), (int)(42*open)+2, col);
      gfx->fillEllipse(rx, ey-8, (int)(42*sx), (int)(42*open)+2, col);
      if (wide) {
        brow(lx, ey-64, 26, 3, 0, true,  col);
        brow(rx, ey-64, 26, 3, 0, false, col);
      }
      break;

    case HAPPY:
      eyeArc(lx, ey+12, (int)(35*sx), (int)(24*open), col);
      eyeArc(rx, ey+12, (int)(35*sx), (int)(24*open), col);
      if (wide) {
        brow(lx, ey-40, 24, 5, -2, true,  col);
        brow(rx, ey-40, 24, 5, -2, false, col);
      }
      break;

    case DIZZY: {
      int ox = (int)(5.0f * sinf(t/170.0f));      // orbit, counter-phase between eyes
      int oy = (int)(4.0f * cosf(t/170.0f));
      int sz = (int)(26*sx), sy = (int)(26*open);
      // No brows here — an X already says "not okay", and brows on top of it read as a
      // second, contradictory signal. The eyes orbit in opposite directions instead,
      // which is what actually sells dizziness rather than mere annoyance.
      thickLine(lx-sz+ox, ey-sy+oy, lx+sz+ox, ey+sy+oy, 9, col);
      thickLine(lx+sz+ox, ey-sy+oy, lx-sz+ox, ey+sy+oy, 9, col);
      thickLine(rx-sz-ox, ey-sy-oy, rx+sz-ox, ey+sy-oy, 9, col);
      thickLine(rx+sz-ox, ey-sy-oy, rx-sz-ox, ey+sy-oy, 9, col);
      break;
    }

    case SLEEPY: {
      // The lid is a black bar over a full eye, not a shorter eye — the flat top edge
      // is what makes it read as a lid rather than as a small eye.
      int sh = (int)(h * 0.9f);
      eyeRect(lx, ey+10, w, sh, ER, col);
      eyeRect(rx, ey+10, w, sh, ER, col);
      if (wide) {
        int lid = ey + 10 - EH/2 - 2;
        gfx->fillRect(lx-w/2-2, lid, w+4, 42, BG);
        gfx->fillRect(rx-w/2-2, lid, w+4, 42, BG);
        brow(lx, ey-34, 24, -2, 9, true,  col);
        brow(rx, ey-34, 24, -2, 9, false, col);
      }
      break;
    }

    case CURIOUS: {
      // Asymmetry is what reads as curiosity: one eye larger, and the pair tipped like a
      // head tilting. They also look INTO the lean, so it tracks what you're doing.
      int look = tiltDir * 10;
      eyeRect(lx + look, ey - 7, w, h, ER, col);
      eyeRect(rx + look, ey + 7, (int)(w*0.78f), (int)(h*0.78f), ER-4, col);
      if (wide) brow(lx + look, ey - 46, 24, 8, -3, true, col);
      break;
    }

    case SCARED: {
      // Small, high, and trembling. Shrinking the eyes is what separates fear from
      // surprise — surprise opens them wide, fear pulls them back and away.
      int tr = (int)(2.0f * sinf(t / 45.0f));       // fast tremble
      gfx->fillEllipse(lx + tr, ey - 14, (int)(24*sx), (int)(30*open)+2, col);
      gfx->fillEllipse(rx - tr, ey - 14, (int)(24*sx), (int)(30*open)+2, col);
      if (wide) {
        brow(lx, ey - 60, 22, 10, -4, true,  col);   // inner ends driven up
        brow(rx, ey - 60, 22, 10, -4, false, col);
      }
      break;
    }

    case LOVE: {
      // Squinting with joy, not heart-shaped eyes. Every other emotion here is carried
      // by eye SHAPE, so eyes that turn into objects break the rule the face runs on.
      // The hearts stay as a small accent above, which is the only thing separating
      // this from `happy` now that the decorative extras are gone.
      eyeArc(lx, ey + 15, (int)(34*sx), (int)(20*open), col);
      eyeArc(rx, ey + 15, (int)(34*sx), (int)(20*open), col);
      if (wide) {
        for (int i = 0; i < 2; i++) {
          float ph = fmodf((t / 2200.0f) + i * 0.5f, 1.0f);
          heart(184 + (int)(10 * sinf(ph * 6.283f)),
                92 - (int)(50 * ph),
                6 + (int)(ph * 6), col);
        }
      }
      break;
    }

    case ANGRY:
      eyeRect(lx, ey, w, h, ER, col);
      eyeRect(rx, ey, w, h, ER, col);
      if (wide) {
        lidWedge(lx, ey, w, h, 34, true);
        lidWedge(rx, ey, w, h, 34, false);
      }
      break;

    case SLEEPING: {
      // Slower, deeper bob than the idle breath: the one face that should look like
      // nothing is happening on purpose.
      int sb = (int)(4.0f * sinf(t/1400.0f));
      // Two small flat lines for shut eyes, plus Z's drifting up from the top right.
      eyeRect(lx, ey+sb+6, 46, 7, 3, col);
      eyeRect(rx, ey+sb+6, 46, 7, 3, col);

      // Three Z's on a staggered loop: each rises, grows, and restarts. Growing as they
      // climb reads as drifting away from the sleeper rather than merely sliding.
      gfx->setTextColor(col);
      for (int i = 0; i < 3; i++) {
        float ph = fmodf((t / 2600.0f) + i * 0.333f, 1.0f);
        gfx->setTextSize(1 + (int)(ph * 2.5f));
        gfx->setCursor(150 + (int)(20 * ph), 92 - (int)(52 * ph));
        gfx->print('Z');
      }
      gfx->setTextSize(1);
      break;
    }
  }
}

// ---------------------------------------------------------------- events

// Darting motion: fast out, then hold. Returns to centre roughly every other move so
// the eyes don't wander further and further off.
void updateSaccade(unsigned long now) {
  const unsigned long DART_MS = 90;
  if (sacMoving) {
    float k = ease((float)(now - sacStart) / DART_MS);
    sacX = sacFromX + (int)((sacToX - sacFromX) * k);
    sacY = sacFromY + (int)((sacToY - sacFromY) * k);
    if (now - sacStart >= DART_MS) {
      sacMoving = false;
      sacX = sacToX; sacY = sacToY;
      sacNext = now + random(1200, 3500);
    }
  } else if (now > sacNext) {
    sacFromX = sacX; sacFromY = sacY;
    if (sacX != 0 || sacY != 0) { sacToX = 0; sacToY = 0; }   // return to centre
    else { sacToX = random(-9, 10); sacToY = random(-3, 4); }
    sacStart = now;
    sacMoving = true;
  }
}

void enter(int s, unsigned long hold) {
  holdUntil = millis() + hold;
  if (s == target) return;
  Serial.printf("-> %s\n", NAMES[s]);
  target = s;
  transitioning = true;
  transStart = millis();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nMyutu — pet");

  if (!gfx->begin(SPI_HZ)) {
    Serial.println("display begin() FAILED");
    while (true) delay(1000);
  }
  gfx->fillScreen(BG);

  pinMode(LDR_PIN, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  mpuOk = initMPU();
  Serial.println(mpuOk ? "MPU ok — hold still, calibrating" : "MPU NOT FOUND");
  if (mpuOk) calibrate();

  lastInteraction = millis();
  Serial.println("running");
}

void loop() {
  unsigned long now = millis();
  static int i2cFails = 0;
  updateSaccade(now);

  // I2C self-recovery. A stalled bus would otherwise freeze the pet permanently.
  if (i2cFails > 20) {
    i2cFails = 0;
    Wire.end(); delay(10);
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);
    mpuOk = initMPU();
    Serial.printf("i2c recovered=%d\n", (int)mpuOk);
  }

  if (mpuOk) {
    uint8_t b[14];
    if (!readRegs(REG_ACCEL_XOUT_H, b, 14)) i2cFails++;
    else {
      i2cFails = 0;
      float ax = (int16_t)((b[0]<<8)|b[1]) / ACCEL_LSB_PER_G * 9.81f;
      float ay = (int16_t)((b[2]<<8)|b[3]) / ACCEL_LSB_PER_G * 9.81f;
      float az = (int16_t)((b[4]<<8)|b[5]) / ACCEL_LSB_PER_G * 9.81f;
      float cx = (int16_t)((b[8]<<8)|b[9])   - gxBias;
      float cy = (int16_t)((b[10]<<8)|b[11]) - gyBias;
      float cz = (int16_t)((b[12]<<8)|b[13]) - gzBias;
      float jolt = fabsf(sqrtf(ax*ax + ay*ay + az*az) - gravityBase);
      float spin = sqrtf(cx*cx + cy*cy + cz*cz) / GYRO_LSB_PER_DPS;

      float mag = sqrtf(ax*ax + ay*ay + az*az);

      // Free fall: gravity briefly stops registering because everything is accelerating
      // together. Needs ~80 ms to avoid firing on a sharp knock.
      if (mag < FREEFALL_MAX) {
        if (!freefallSince) freefallSince = now;
        if (now - freefallSince > 80) {
          lastEventAt = lastInteraction = now;
          enter(SCARED, SCARED_HOLD);
        }
      } else freefallSince = 0;

      if (now - lastEventAt > TAP_REFRACTORY) {
        if (spin > SPIN_SHAKE) {
          lastEventAt = lastInteraction = now;
          // Shaken again so soon after the last one: it stops being funny.
          enter((now - lastShakeAt < ANGRY_WINDOW) ? ANGRY : DIZZY,
                (now - lastShakeAt < ANGRY_WINDOW) ? ANGRY_HOLD : DIZZY_HOLD);
          lastShakeAt = now;
        } else if (jolt > JOLT_TAP && spin < SPIN_TAP_MAX) {
          lastEventAt = lastInteraction = now;
          tapCount = (now - lastTapAt < DOUBLE_TAP_MS) ? tapCount + 1 : 1;
          lastTapAt = now;
          if      (tapCount >= 3) enter(LOVE,      LOVE_HOLD);
          else if (tapCount == 2) enter(HAPPY,     HAPPY_HOLD);
          else                    enter(SURPRISED, SURPRISED_HOLD);
        }
      }

      // A held lean, not a knock — so it only counts while the pet is otherwise still.
      float lean = sqrtf(ax*ax + ay*ay);
      if (lean > TILT_THRESH && jolt < 2.0f) {
        if (!tiltSince) tiltSince = now;
        if (now - tiltSince > TILT_HOLD_MS && now > holdUntil) {
          tiltDir = (ax > 0) ? 1 : -1;
          lastInteraction = now;
          enter(CURIOUS, CURIOUS_HOLD);
        }
      } else tiltSince = 0;
    }
  }

  // Darkness. Debounced in time so a shadow crossing the desk, or a flickering bulb,
  // can't put the pet to sleep — it has to actually get dark and stay dark.
  bool rawDark = (digitalRead(LDR_PIN) == DARK_HIGH);
  if (rawDark != lastRawDark) { lastRawDark = rawDark; darkChangedAt = now; }
  if (rawDark != isDark && now - darkChangedAt >= DARK_STABLE_MS) {
    isDark = rawDark;
    Serial.printf("light: %s\n", isDark ? "dark — going to sleep" : "bright — waking");
    if (!isDark) {                       // lights came back on: wake with a start
      lastInteraction = now;
      enter(SURPRISED, SURPRISED_HOLD);
    }
  }

  if (now > holdUntil) {
    // Darkness overrides the idle timer entirely — the room going dark puts it to sleep
    // immediately rather than 45 s later. Events still surface while dark (a tap in a
    // dark room gets a reaction), then it settles straight back to sleep.
    if (isDark) { if (target != SLEEPING) enter(SLEEPING, 0); }
    else {
      unsigned long idle = now - lastInteraction;
      if      (idle > SLEEPING_AFTER) enter(SLEEPING, 0);
      else if (idle > SLEEPY_AFTER)   enter(SLEEPY, 0);
      else                            enter(NEUTRAL, 0);
    }
  }

  // Expressions change by blinking through: the eyes ease shut on the old face, swap at
  // the closed point, then ease open on the new one. Snapping between two shapes reads
  // as a screen refresh; closing the eyes to change your mind reads as a creature.
  float open = 1.0f;
  if (transitioning) {
    float half = TRANSITION_MS / 2.0f;
    float e = now - transStart;
    if (e < half) {
      open = 1.0f - ease(e / half);
    } else if (e < TRANSITION_MS) {
      shown = target;
      open = easeOutBack((e - half) / half);   // springs past 1, settles back
    } else {
      shown = target;
      transitioning = false;
      blinkAt = now + random(3000, 6000);
    }
  } else {
    // Spontaneous blink while idle, also eased.
    if (now > blinkAt) blinkAt = now + random(3000, 6000);
    long untilBlink = (long)(blinkAt - now);
    if (untilBlink < 240 && shown != SLEEPING) {
      float p = (240 - untilBlink) / 240.0f;
      open = 1.0f - ease(1.0f - fabsf(p - 0.5f) * 2.0f);
    }
  }

  // Only push a frame that would actually look different. Every flush is a full-panel
  // write racing the display's own refresh, so an identical frame costs a tearing
  // artefact and buys nothing. While a face is still, this drops to zero writes.
  int qOpen = (int)(open * 60);
  int qBob  = (int)(2.0f * sinf(now / 1500.0f));
  int qDx   = sacX * 100 + sacY;
  int qWob  = (shown == DIZZY) ? (int)(5.0f*sinf(now/170.0f))*10 + (int)(4.0f*cosf(now/170.0f)) : 0;
  int qSb   = (shown == SLEEPING)
                ? (int)(4.0f * sinf(now / 1400.0f)) * 100 + (int)(fmodf(now / 2600.0f, 1.0f) * 40)
                : 0;

  int qAcc = 0;                                   // animated accent marks per face
  switch (shown) {
    case HAPPY:   qAcc = (int)(2.0f * sinf(now / 220.0f)); break;
    case CURIOUS: qAcc = (int)(3.0f * sinf(now / 500.0f)); break;
    case SCARED:  qAcc = (int)(6.0f * sinf(now / 400.0f)); break;
    case ANGRY:   qAcc = (int)(2.0f * sinf(now / 130.0f)); break;
    case LOVE:    qAcc = (int)(fmodf(now / 2200.0f, 1.0f) * 40); break;
  }

  static int pShown = -1, pOpen = -999, pBob = -999, pDx = -999, pWob = -999, pSb = -999, pAcc = -999;
  if (shown != pShown || qOpen != pOpen || qBob != pBob ||
      qDx != pDx || qWob != pWob || qSb != pSb || qAcc != pAcc) {
    pShown = shown; pOpen = qOpen; pBob = qBob; pDx = qDx; pWob = qWob; pSb = qSb; pAcc = qAcc;
    drawFace(shown, open, now);
  }
  delay(FRAME_MS);
}
