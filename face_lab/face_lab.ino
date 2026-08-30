// face_lab — designing the mouth. Display only.
//
// No I2C, no MPU, no state machine: this exists so the face can be judged and iterated
// without touching the working pet. Cycles all ten expressions, ~3 s each.
//
// Built on face_check's skeleton, which is the known-good display configuration for this
// hardware: direct to the panel, full-screen writes, library-default SPI clock.
// DO NOT introduce Arduino_Canvas here — it corrupts this panel (README gotcha 9).

#include <Arduino_GFX_Library.h>

#define TFT_DC    4
#define TFT_CS    5
#define TFT_SCK  18
#define TFT_MOSI 23
#define TFT_RST  25

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, true /* IPS */);

#define EYE RGB565(255, 208, 70)
#define BG  RGB565(0, 0, 0)
#define LBL RGB565(70, 80, 90)

#define SHOW_LABELS 1
#define HOLD_MS 3000

// Geometry reworked to make room below. Eyes moved up and shrunk; mouth sits at 178.
// Eyes span y 75-141, mouth spans 166-190 — a 25 px gap. At y=190 the round panel's
// half-width is ~95 px, so a mouth up to 60 px wide stays clear of the bezel.
const int EYE_LX = 76, EYE_RX = 164, EYE_CY = 108;
const int EW = 54, EH = 66, ER = 20;
const int MOUTH_Y = 178;

// ---------------------------------------------------------------- primitives

void eyeRect(int cx,int cy,int w,int h,int r,uint16_t c){
  if (h < 3) h = 3;
  if (r > h/2) r = h/2;
  gfx->fillRoundRect(cx-w/2, cy-h/2, w, h, r, c);
}

// Top crescent: an arch (⌒). Used for happy EYES, and for a frowning MOUTH.
void arcUpper(int cx,int cy,int r,int th,uint16_t c){
  if (th < 3) th = 3;
  gfx->fillCircle(cx, cy, r, c);
  gfx->fillCircle(cx, cy + th, r, BG);
}

// Bottom crescent: a smile (⌣). Same trick, punching the hole above instead of below.
void arcLower(int cx,int cy,int r,int th,uint16_t c){
  if (th < 3) th = 3;
  gfx->fillCircle(cx, cy - r + th, r, c);
  gfx->fillCircle(cx, cy - r, r, BG);
}

void thickLine(int x0,int y0,int x1,int y1,int t,uint16_t c){
  for (int i=-t/2;i<=t/2;i++){
    gfx->drawLine(x0+i,y0,x1+i,y1,c);
    gfx->drawLine(x0,y0+i,x1,y1+i,c);
  }
}

void brow(int cx,int y,int hl,int inDy,int outDy,bool left,uint16_t c){
  int xi = left ? cx+hl : cx-hl, xo = left ? cx-hl : cx+hl;
  thickLine(xo, y+outDy, xi, y+inDy, 9, c);
}

void lidWedge(int cx,int cy,int w,int h,int depth,bool left){
  int x0 = cx-w/2-2, x1 = cx+w/2+2, top = cy-h/2-2;
  if (left) gfx->fillTriangle(x0, top, x1, top, x1, top+depth, BG);
  else      gfx->fillTriangle(x0, top, x1, top, x0, top+depth, BG);
}

void heart(int cx,int cy,int sz,uint16_t c){
  gfx->fillCircle(cx-sz/2, cy-sz/3, sz/2, c);
  gfx->fillCircle(cx+sz/2, cy-sz/3, sz/2, c);
  gfx->fillTriangle(cx-sz, cy-sz/6, cx+sz, cy-sz/6, cx, cy+sz, c);
}

// ---------------------------------------------------------------- mouths

void mouthLine(int cx,int cy,int w,int th,uint16_t c){
  gfx->fillRoundRect(cx-w/2, cy-th/2, w, th, th/2, c);
}

void mouthO(int cx,int cy,int rx,int ry,uint16_t c){
  gfx->fillEllipse(cx, cy, rx, ry, c);
}

void mouthWave(int cx,int cy,int w,int amp,int phase,uint16_t c){
  const int N = 8;
  int px = cx-w/2, py = cy;
  for (int i=1;i<=N;i++){
    int x = cx - w/2 + (w*i)/N;
    int y = cy + (int)(amp * sinf(i * 1.6f + phase * 0.01f));
    thickLine(px, py, x, y, 6, c);
    px = x; py = y;
  }
}

// The cat-style ω. Reads cuter than a plain smile at this size.
void mouthW(int cx,int cy,int w,uint16_t c){
  arcLower(cx - w/4, cy, w/4, 6, c);
  arcLower(cx + w/4, cy, w/4, 6, c);
}

// ---------------------------------------------------------------- faces

enum { NEUTRAL, SURPRISED, HAPPY, DIZZY, SLEEPY, SLEEPING, CURIOUS, SCARED, LOVE, ANGRY, FACE_COUNT };
const char *NAMES[] = {"neutral","surprised","happy","dizzy","sleepy","sleeping",
                       "curious","scared","love","angry"};

void draw(int e, unsigned long t) {
  uint16_t col = EYE;
  gfx->fillScreen(BG);

  int bob = (int)(2.0f * sinf(t / 1500.0f));
  int ey  = EYE_CY + bob;
  int my  = MOUTH_Y + bob;
  int lx  = EYE_LX, rx = EYE_RX;

  switch (e) {
    case NEUTRAL:
      eyeRect(lx, ey, EW, EH, ER, col);
      eyeRect(rx, ey, EW, EH, ER, col);
      mouthLine(120, my, 26, 6, col);              // calm, not blank
      break;

    case SURPRISED:
      gfx->fillEllipse(lx, ey-6, 38, 38, col);
      gfx->fillEllipse(rx, ey-6, 38, 38, col);
      brow(lx, ey-58, 26, 3, 0, true,  col);
      brow(rx, ey-58, 26, 3, 0, false, col);
      mouthO(120, my, 13, 16, col);                // the classic round shock
      break;

    case HAPPY:
      arcUpper(lx, ey+10, 32, 22, col);
      arcUpper(rx, ey+10, 32, 22, col);
      brow(lx, ey-42, 24, 5, -2, true,  col);
      brow(rx, ey-42, 24, 5, -2, false, col);
      mouthW(120, my-4, 46, col);                  // cat mouth
      break;

    case DIZZY: {
      int ox = (int)(5.0f * sinf(t/170.0f));
      int oy = (int)(4.0f * cosf(t/170.0f));
      int sz = 24;
      thickLine(lx-sz+ox, ey-sz+oy, lx+sz+ox, ey+sz+oy, 9, col);
      thickLine(lx+sz+ox, ey-sz+oy, lx-sz+ox, ey+sz+oy, 9, col);
      thickLine(rx-sz-ox, ey-sz-oy, rx+sz-ox, ey+sz-oy, 9, col);
      thickLine(rx+sz-ox, ey-sz-oy, rx-sz-ox, ey+sz-oy, 9, col);
      mouthWave(120, my, 46, 5, (int)t, col);      // woozy
      break;
    }

    case SLEEPY:
      eyeRect(lx, ey+8, EW, (int)(EH*0.85f), ER, col);
      eyeRect(rx, ey+8, EW, (int)(EH*0.85f), ER, col);
      gfx->fillRect(lx-EW/2-2, ey+8-EH/2-2, EW+4, 38, BG);
      gfx->fillRect(rx-EW/2-2, ey+8-EH/2-2, EW+4, 38, BG);
      brow(lx, ey-36, 24, -2, 9, true,  col);
      brow(rx, ey-36, 24, -2, 9, false, col);
      mouthLine(120, my, 20, 8, col);              // slack
      break;

    case SLEEPING: {
      int sb = (int)(4.0f * sinf(t/1400.0f));
      eyeRect(lx, ey+sb+6, 46, 8, 4, col);
      eyeRect(rx, ey+sb+6, 46, 8, 4, col);
      int br = 6 + (int)(3.0f * sinf(t/1400.0f));  // snoring, in time with the breath
      mouthO(120, my+sb, br, br, col);
      gfx->setTextColor(col);
      for (int i=0;i<3;i++){
        float ph = fmodf((t/2600.0f) + i*0.333f, 1.0f);
        gfx->setTextSize(1 + (int)(ph*2.5f));
        gfx->setCursor(150 + (int)(20*ph), 88 - (int)(50*ph));
        gfx->print('Z');
      }
      gfx->setTextSize(1);
      break;
    }

    case CURIOUS: {
      // Asymmetry is the whole expression — EMO's principle. The mouth is offset too.
      int look = 10;
      eyeRect(lx+look, ey-7, EW, EH, ER, col);
      eyeRect(rx+look, ey+7, (int)(EW*0.78f), (int)(EH*0.78f), ER-4, col);
      brow(lx+look, ey-46, 24, 8, -3, true, col);
      mouthO(120+look, my, 9, 11, col);
      break;
    }

    case SCARED: {
      int tr = (int)(2.0f * sinf(t/120.0f));
      gfx->fillEllipse(lx+tr, ey-12, 22, 28, col);
      gfx->fillEllipse(rx-tr, ey-12, 22, 28, col);
      brow(lx, ey-56, 22, 10, -4, true,  col);
      brow(rx, ey-56, 22, 10, -4, false, col);
      mouthWave(120, my, 34, 3, (int)(t*1.5f), col); // trembling, faster than dizzy
      break;
    }

    case LOVE: {
      arcUpper(lx, ey+12, 31, 19, col);
      arcUpper(rx, ey+12, 31, 19, col);
      arcLower(120, my-8, 20, 7, col);             // soft smile; hearts carry the rest
      for (int i=0;i<2;i++){
        float ph = fmodf((t/2200.0f) + i*0.5f, 1.0f);
        heart(184 + (int)(10*sinf(ph*6.283f)), 88 - (int)(48*ph), 6 + (int)(ph*6), col);
      }
      break;
    }

    case ANGRY:
      eyeRect(lx, ey, EW, EH, ER, col);
      eyeRect(rx, ey, EW, EH, ER, col);
      lidWedge(lx, ey, EW, EH, 32, true);
      lidWedge(rx, ey, EW, EH, 32, false);
      arcUpper(120, my+8, 22, 7, col);             // frown
      break;
  }

#if SHOW_LABELS
  gfx->setTextColor(LBL);
  gfx->setTextSize(1);
  gfx->setCursor(120 - (int)strlen(NAMES[e]) * 3, 212);
  gfx->print(NAMES[e]);
#endif
}

// Repairs the panel's vertical scroll address. See README gotcha 14 — this display
// intermittently drifts into a shifted, wrapping image, and re-zeroing the register each
// frame makes that self-healing within one frame instead of permanent.
void resetScroll() {
  bus->beginWrite();
  bus->writeCommand(0x37);
  bus->write(0x00);
  bus->write(0x00);
  bus->endWrite();
}

// Everything that can make one frame differ from the last, packed into one integer.
// If it hasn't changed, the frame would be pixel-identical and pushing it buys nothing
// but a tearing artefact — every flush is a full-panel write racing the display's own
// refresh. A still face costs zero writes and therefore cannot flicker.
long frameSig(int e, unsigned long t) {
  long s = (long)e * 10000000L;
  s += ((int)(2.0f * sinf(t / 1500.0f)) + 4) * 100000L;
  switch (e) {
    case DIZZY:
      s += ((int)(5.0f * sinf(t/170.0f)) + 8) * 1000L
         + ((int)(4.0f * cosf(t/170.0f)) + 8) * 30L;
      break;
    case SLEEPING:
      s += ((int)(4.0f * sinf(t/1400.0f)) + 8) * 1000L
         + (int)(fmodf(t/2600.0f, 1.0f) * 30);
      break;
    case SCARED:
      s += ((int)(2.0f * sinf(t/120.0f)) + 4) * 1000L
         + (int)(fmodf(t/900.0f, 1.0f) * 20);
      break;
    case LOVE:
      s += (int)(fmodf(t/2200.0f, 1.0f) * 40);
      break;
  }
  return s;
}

// ---------------------------------------------------------------- runtime

int current = 0;
unsigned long phaseStart = 0;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nface_lab — eyes + mouth");
  if (!gfx->begin()) { Serial.println("display begin() FAILED"); while (true) delay(1000); }
  gfx->fillScreen(BG);
  phaseStart = millis();
}

void loop() {
  unsigned long now = millis();
  if (now - phaseStart >= HOLD_MS) {
    current = (current + 1) % FACE_COUNT;
    phaseStart = now;
    Serial.printf("-> %s\n", NAMES[current]);
  }
  long sig = frameSig(current, now);
  static long lastSig = -1;
  if (sig != lastSig) {
    lastSig = sig;
    resetScroll();
    draw(current, now);
  }
  delay(25);
}
