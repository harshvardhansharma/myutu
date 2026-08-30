// Myutu — expression showcase.
// Six emotions drawn with eyes alone, cycling every 4 seconds.
//
// Cut down from twelve to the six the state machine can actually reach: every face here
// has a real trigger in the product (tap, pat, shake, idle decay, darkness). Expressions
// with no way to be caused are dead weight — a pet with six faces you see constantly
// reads as more coherent than one with twelve you never do.
//
// Flicker fix: a full 240x240 16-bit framebuffer is 115 KB contiguous, which a plain
// ESP32-WROOM with no PSRAM won't allocate. But the eyes only occupy a band, and
// Arduino_Canvas takes a Y offset — so a 240x140 canvas at y=40 is 67 KB, allocates
// fine, and double-buffers the only part of the screen that actually moves.

#include <Arduino_GFX_Library.h>

#define TFT_DC    4
#define TFT_CS    5
#define TFT_SCK  18
#define TFT_MOSI 23
#define TFT_RST  25

#define CANVAS_Y  40
#define CANVAS_H 140

// SPI clock. The library defaults to ~40 MHz, which breadboard jumper wires can't carry
// cleanly — corrupted pixels show up as vertical streaks. 20 MHz is still far faster
// than this animation needs. Raise it once the build moves to soldered short wires.
#define SPI_HZ 20000000

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED /* MISO */);
Arduino_GFX *panel = new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, true /* IPS */);
Arduino_Canvas *gfx = new Arduino_Canvas(240, CANVAS_H, panel, 0, CANVAS_Y);

// Warm amber. Yellow holds the most contrast against black after white, but unlike
// white it reads as something lit from inside rather than as an appliance.
#define EYE RGB565(255, 208,  70)
#define BG  RGB565(  0,   0,   0)
#define LBL RGB565( 70,  80,  90)

#define SHOW_LABELS 1

// Bigger than the first pass — on a 240 px round panel, small eyes read as vague.
const int EYE_LX = 76, EYE_RX = 164;
const int EYE_CY = 115 - CANVAS_Y;      // canvas-local; low enough to leave brow room
const int EW = 58, EH = 76, ER = 22;

enum Emotion { NEUTRAL, SURPRISED, HAPPY, DIZZY, SLEEPY, SLEEPING, EMOTION_COUNT };

const char *NAMES[] = { "neutral", "surprised", "happy", "dizzy", "sleepy", "sleeping" };

// What causes each face, so the mapping to Step 5 is obvious from the showcase itself.
const char *CAUSES[] = { "idle, awake", "tap", "pat", "shake", "idle decay", "dark / long idle" };

// ---------------------------------------------------------------- primitives

void eyeRect(int cx, int cy, int w, int h, int r, uint16_t c) {
  if (h < 4) h = 4;
  if (r > h / 2) r = h / 2;
  gfx->fillRoundRect(cx - w / 2, cy - h / 2, w, h, r, c);
}

// An arc opening downward: a disc with a second disc punched out below it. This is what
// makes a happy eye read as happy instead of as a squashed rectangle.
void eyeArc(int cx, int cy, int r, int thickness, uint16_t c) {
  gfx->fillCircle(cx, cy, r, c);
  gfx->fillCircle(cx, cy + thickness, r, BG);
}

void thickLine(int x0, int y0, int x1, int y1, int t, uint16_t c) {
  for (int i = -t / 2; i <= t / 2; i++) {
    gfx->drawLine(x0 + i, y0, x1 + i, y1, c);
    gfx->drawLine(x0, y0 + i, x1, y1 + i, c);
  }
}

// A brow is one thick bar; its angle does all the work. innerDy/outerDy drop the end
// nearest the nose and the end nearest the ear, so the same call mirrors correctly for
// either eye — down toward the nose is a scowl, down toward the ear is fatigue.
void brow(int cx, int y, int halfLen, int innerDy, int outerDy, bool isLeft, uint16_t c) {
  int xi = isLeft ? cx + halfLen : cx - halfLen;
  int xo = isLeft ? cx - halfLen : cx + halfLen;
  thickLine(xo, y + outerDy, xi, y + innerDy, 9, c);
}

// ---------------------------------------------------------------- expressions

void drawFace(int e, float open, unsigned long t) {
  gfx->fillScreen(BG);                       // canvas-local: clears the band only

  int h   = (int)(EH * open);
  int bob = (int)(2.0f * sinf(t / 900.0f));  // idle breathing
  int ey  = EYE_CY + bob;

  switch (e) {
    case NEUTRAL:
      eyeRect(EYE_LX, ey, EW, h, ER, EYE);
      eyeRect(EYE_RX, ey, EW, h, ER, EYE);
      break;

    case SURPRISED: {
      // Wide, round and lifted. Roundness plus size is what separates shock from resting.
      int r = (int)(40 * open) + 2;
      gfx->fillCircle(EYE_LX, ey - 4, r, EYE);
      gfx->fillCircle(EYE_RX, ey - 4, r, EYE);
      if (open > 0.5f) {
        brow(EYE_LX, ey - 48, 26, 3, 0, true,  EYE);
        brow(EYE_RX, ey - 48, 26, 3, 0, false, EYE);
      }
      break;
    }

    case HAPPY:
      if (open > 0.5f) {
        eyeArc(EYE_LX, ey + 12, 35, 24, EYE);
        eyeArc(EYE_RX, ey + 12, 35, 24, EYE);
        brow(EYE_LX, ey - 40, 24, 5, -2, true,  EYE);
        brow(EYE_RX, ey - 40, 24, 5, -2, false, EYE);
      } else {
        eyeRect(EYE_LX, ey, EW, h, ER, EYE);
        eyeRect(EYE_RX, ey, EW, h, ER, EYE);
      }
      break;

    case DIZZY: {
      // Counter-phase wobble: the two eyes swing opposite ways, which reads as
      // disorientation rather than as the whole face sliding around.
      int wob = (int)(4.0f * sinf(t / 150.0f));
      int s = 26;
      thickLine(EYE_LX - s + wob, ey - s, EYE_LX + s + wob, ey + s, 9, EYE);
      thickLine(EYE_LX + s + wob, ey - s, EYE_LX - s + wob, ey + s, 9, EYE);
      thickLine(EYE_RX - s - wob, ey - s, EYE_RX + s - wob, ey + s, 9, EYE);
      thickLine(EYE_RX + s - wob, ey - s, EYE_RX - s - wob, ey + s, 9, EYE);
      brow(EYE_LX, ey - 44, 24,  wob, -wob, true,  EYE);
      brow(EYE_RX, ey - 44, 24, -wob,  wob, false, EYE);
      break;
    }

    case SLEEPY: {
      // Half-lidded and sitting low. The lid is a black bar over a full eye, not a
      // shorter eye — the flat top edge is what makes it read as a lid.
      eyeRect(EYE_LX, ey + 10, EW, h, ER, EYE);
      eyeRect(EYE_RX, ey + 10, EW, h, ER, EYE);
      if (open > 0.5f) {
        int lid = ey + 10 - EH / 2 - 2;
        gfx->fillRect(EYE_LX - EW / 2 - 2, lid, EW + 4, 42, BG);
        gfx->fillRect(EYE_RX - EW / 2 - 2, lid, EW + 4, 42, BG);
        brow(EYE_LX, ey - 34, 24, -2, 9, true,  EYE);
        brow(EYE_RX, ey - 34, 24, -2, 9, false, EYE);
      }
      break;
    }

    case SLEEPING: {
      // Slower, deeper bob than the idle breath. This is the one face that should look
      // like nothing is happening on purpose.
      int sb = (int)(4.0f * sinf(t / 1400.0f));
      eyeRect(EYE_LX, ey + sb + 8, 60, 10, 5, EYE);
      eyeRect(EYE_RX, ey + sb + 8, 60, 10, 5, EYE);
      break;
    }
  }

  gfx->flush();
}

// Labels live outside the canvas band, so they're drawn straight to the panel and only
// when they change — redrawing static text every frame is what makes text flicker.
void drawLabel(int e) {
#if SHOW_LABELS
  panel->fillRect(0, 186, 240, 40, BG);
  panel->setTextColor(LBL);
  panel->setTextSize(2);
  panel->setCursor(120 - (int)strlen(NAMES[e]) * 6, 188);
  panel->print(NAMES[e]);
  panel->setTextSize(1);
  panel->setCursor(120 - (int)strlen(CAUSES[e]) * 3, 210);
  panel->print(CAUSES[e]);
#endif
}

// ---------------------------------------------------------------- runtime

const unsigned long HOLD_MS  = 4000;
const unsigned long BLINK_AT = 2600;
const unsigned long BLINK_MS = 260;

int current = 0;
unsigned long phaseStart = 0;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nMyutu expression showcase");

  if (!gfx->begin(SPI_HZ)) {
    Serial.println("canvas begin() FAILED — allocation too large or display wiring");
    while (true) delay(1000);
  }
  panel->fillScreen(BG);
  Serial.printf("ok — %d expressions, %lu ms each, canvas %dx%d, SPI %d MHz\n",
                (int)EMOTION_COUNT, HOLD_MS, 240, CANVAS_H, SPI_HZ / 1000000);

  // Paint the whole panel twice. Anything outside the canvas band is only ever written
  // here, so a single corrupted pass would leave stale pixels on screen forever.
  panel->fillScreen(BG);
  delay(20);
  panel->fillScreen(BG);

  drawLabel(current);
  phaseStart = millis();
}

void loop() {
  unsigned long now = millis();
  unsigned long t = now - phaseStart;

  if (t >= HOLD_MS) {
    current = (current + 1) % EMOTION_COUNT;
    phaseStart = now;
    t = 0;
    drawLabel(current);
    Serial.printf("-> %s (%s)\n", NAMES[current], CAUSES[current]);
  }

  // One blink per expression, late in its hold, so each face is seen still first.
  float open = 1.0f;
  if (t >= BLINK_AT && t < BLINK_AT + BLINK_MS) {
    float p = (float)(t - BLINK_AT) / BLINK_MS;
    open = fabsf(p - 0.5f) * 2.0f;
  }

  drawFace(current, open, now);
  delay(25);
}
