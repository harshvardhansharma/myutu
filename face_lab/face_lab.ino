// face_lab — designing the face. Display only.
//
// Cycles all ten expressions so the design can be judged without touching the pet.
//
// RENDERING: TFT_eSPI with a full-screen SPRITE. The whole frame is composed in RAM and
// pushed in one operation, so the glass never shows a half-drawn frame — that
// erase-then-redraw flash is the flicker, and this removes it rather than shrinking it.
//
// Why not Arduino_GFX, which the rest of the project uses: its Arduino_Canvas allocates
// fine and fillScreen reaches the panel, but flush() never lands — a black screen. Tested
// 16-bit and 8-bit indexed, offset and full-size, with and without its DMA bus, and in a
// sketch with no I2C at all. TFT_eSPI's sprite is a different implementation entirely.
//
// Pins are configured in the LIBRARY, not here: ~/Documents/Arduino/libraries/TFT_eSPI/
// User_Setup.h (stock file preserved as User_Setup.h.orig). SPI is set to 40 MHz there,
// not the 80 MHz the tutorials suggest — 80 needs sub-10 cm wiring and already produced
// tearing on this breadboard.
//
// A full-screen 16-bit sprite is 115 KB and DOES NOT ALLOCATE on this ESP32 — tried, and
// createSprite returned null. So this uses a BAND sprite covering only the rows the face
// occupies: 240 x 172 at y=28, about 82 KB. Everything is drawn in sprite-local
// coordinates (global y minus SPR_Y), and the label lives outside the band, drawn
// straight to the panel when the face changes.

#include <TFT_eSPI.h>

TFT_eSPI  tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
bool spriteOk = false;

#define C565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))

#define CORE  C565(255,255,255)
#define HALO1 C565( 96,122,172)
#define HALO2 C565( 52, 70,110)
#define HALO3 C565( 26, 36, 62)
#define HALO4 C565( 12, 17, 32)
#define BG    C565(  0,  0,  0)
#define LBL   C565( 60, 68, 80)

#define SHOW_LABELS 1
#define HOLD_MS 3000

#define SPR_Y  28              // where the band sits on the panel
#define SPR_H 172              // 240 x 172 x 2 bytes = 82,560

// Sprite-local. Global position is these plus SPR_Y.
const int EYE_LX = 76, EYE_RX = 164, EYE_CY = 108 - SPR_Y;
const int EW = 54, EH = 66, ER = 20;
const int MOUTH_Y = 170 - SPR_Y;

// ---------------------------------------------------------------- life

float fOpen = 1.0f;
int   fSacX = 0, fSacY = 0;

float ease(float t){ if(t<0)t=0; if(t>1)t=1; return t*t*(3.0f-2.0f*t); }

unsigned long blinkAt = 0;
void updateBlink(unsigned long now){
  if (now > blinkAt) blinkAt = now + random(2600, 5200);
  long until = (long)(blinkAt - now);
  fOpen = 1.0f;
  if (until < 240) {
    float p = (240 - until) / 240.0f;
    fOpen = 1.0f - ease(1.0f - fabsf(p - 0.5f) * 2.0f);
  }
}

// Eyes dart and hold — they do not glide. A slow pan reads as a camera.
int sacFX=0, sacFY=0, sacTX=0, sacTY=0;
unsigned long sacStart=0, sacNext=0;
bool sacMoving=false;
void updateSaccade(unsigned long now){
  const unsigned long DART = 90;
  if (sacMoving) {
    float k = ease((float)(now - sacStart) / DART);
    fSacX = sacFX + (int)((sacTX - sacFX) * k);
    fSacY = sacFY + (int)((sacTY - sacFY) * k);
    if (now - sacStart >= DART) {
      sacMoving = false; fSacX = sacTX; fSacY = sacTY;
      sacNext = now + random(1400, 3600);
    }
  } else if (now > sacNext) {
    sacFX = fSacX; sacFY = fSacY;
    if (fSacX || fSacY) { sacTX = 0; sacTY = 0; }
    else { sacTX = random(-8, 9); sacTY = random(-3, 4); }
    sacStart = now; sacMoving = true;
  }
}

// ---------------------------------------------------------------- primitives
// Each takes `g`, how far to grow, so the glow is built from concentric passes.

void eyeRect(int cx,int cy,int w,int h,int r,int g,uint16_t c){
  w += 2*g; h += 2*g; r += g;
  if (h < 3) h = 3;
  if (r > h/2) r = h/2;
  spr.fillRoundRect(cx-w/2, cy-h/2, w, h, r, c);
}

void arcUpper(int cx,int cy,int r,int th,int g,uint16_t c){   // ⌒ arch
  r += g; th += 2*g;
  if (th < 3) th = 3;
  spr.fillCircle(cx, cy, r, c);
  spr.fillCircle(cx, cy + th, r, BG);
}

void arcLower(int cx,int cy,int r,int th,int g,uint16_t c){   // ⌣ smile
  r += g; th += 2*g;
  if (th < 3) th = 3;
  spr.fillCircle(cx, cy - r + th, r, c);
  spr.fillCircle(cx, cy - r, r, BG);
}

void thickLine(int x0,int y0,int x1,int y1,int t,uint16_t c){
  for (int i=-t/2;i<=t/2;i++){
    spr.drawLine(x0+i,y0,x1+i,y1,c);
    spr.drawLine(x0,y0+i,x1,y1+i,c);
  }
}

void brow(int cx,int y,int hl,int inDy,int outDy,bool left,int g,uint16_t c){
  int xi = left ? cx+hl : cx-hl, xo = left ? cx-hl : cx+hl;
  thickLine(xo, y+outDy, xi, y+inDy, 9 + 2*g, c);
}

// Hard-cornered quad — anger lives in the eye's own geometry, not in a wedge on top.
void eyeQuad(int cx,int cy,int w,int h,int innerDrop,bool left,int g,uint16_t c){
  w += 2*g; h += 2*g;
  int xo = left ? cx-w/2 : cx+w/2;
  int xi = left ? cx+w/2 : cx-w/2;
  int yt = cy-h/2, yb = cy+h/2;
  spr.fillTriangle(xo, yt, xi, yt+innerDrop, xi, yb, c);
  spr.fillTriangle(xo, yt, xi, yb,           xo, yb, c);
}

void spark(int x,int y,int r,int g,uint16_t c){
  r += g;
  thickLine(x-r, y, x+r, y, 3 + 2*g, c);
  thickLine(x, y-r, x, y+r, 3 + 2*g, c);
}

void heart(int cx,int cy,int sz,int g,uint16_t c){
  sz += g;
  spr.fillCircle(cx-sz/2, cy-sz/3, sz/2, c);
  spr.fillCircle(cx+sz/2, cy-sz/3, sz/2, c);
  spr.fillTriangle(cx-sz, cy-sz/6, cx+sz, cy-sz/6, cx, cy+sz, c);
}

// ---------------------------------------------------------------- mouths
// Small on purpose: in the reference faces the mouth is a fraction of an eye's width.

void mouthLine(int cx,int cy,int w,int th,int g,uint16_t c){
  w += 2*g; th += 2*g;
  spr.fillRoundRect(cx-w/2, cy-th/2, w, th, th/2, c);
}
void mouthO(int cx,int cy,int rx,int ry,int g,uint16_t c){
  spr.fillEllipse(cx, cy, rx+g, ry+g, c);
}
void mouthWave(int cx,int cy,int w,int amp,int phase,int g,uint16_t c){
  const int N = 8;
  int px = cx-w/2, py = cy;
  for (int i=1;i<=N;i++){
    int x = cx - w/2 + (w*i)/N;
    int y = cy + (int)(amp * sinf(i * 1.6f + phase * 0.01f));
    thickLine(px, py, x, y, 6 + 2*g, c);
    px = x; py = y;
  }
}
void mouthW(int cx,int cy,int w,int g,uint16_t c){            // the cat-style ω
  arcLower(cx - w/4, cy, w/4, 6, g, c);
  arcLower(cx + w/4, cy, w/4, 6, g, c);
}

// ---------------------------------------------------------------- faces

enum { NEUTRAL, SURPRISED, HAPPY, DIZZY, SLEEPY, SLEEPING, CURIOUS, SCARED, LOVE, ANGRY, FACE_COUNT };
const char *NAMES[] = {"neutral","surprised","happy","dizzy","sleepy","sleeping",
                       "curious","scared","love","angry"};

void shapes(int e, unsigned long t, int g, uint16_t c) {
  int ey = EYE_CY + fSacY;
  int my = MOUTH_Y;                     // the mouth doesn't follow the gaze
  int lx = EYE_LX + fSacX, rx = EYE_RX + fSacX;
  int eh = (int)(EH * fOpen);
  bool wide = fOpen > 0.35f;

  switch (e) {
    case NEUTRAL:
      eyeRect(lx, ey, EW, eh, ER, g, c);
      eyeRect(rx, ey, EW, eh, ER, g, c);
      mouthLine(120, my, 18, 5, g, c);
      break;

    case SURPRISED: {
      int pr = 38 + (int)(1.5f * sinf(t / 420.0f));   // a held breath, not a freeze
      spr.fillEllipse(lx, ey-6, pr+g, (int)(pr*fOpen)+g, c);
      spr.fillEllipse(rx, ey-6, pr+g, (int)(pr*fOpen)+g, c);
      if (wide) {
        brow(lx, ey-64, 26, 3, 0, true,  g, c);
        brow(rx, ey-64, 26, 3, 0, false, g, c);
      }
      mouthO(120, my, 9, 11, g, c);
      break;
    }

    case HAPPY: {
      int wig = (int)(2.0f * sinf(t / 380.0f));
      arcUpper(lx, ey+10, 32, (int)(22*fOpen), g, c);
      arcUpper(rx, ey+10, 32, (int)(22*fOpen), g, c);
      brow(lx, ey-42+wig, 24, 5, -2, true,  g, c);
      brow(rx, ey-42-wig, 24, 5, -2, false, g, c);
      mouthW(120+wig, my-2, 30, g, c);
      break;
    }

    case DIZZY: {
      int ox = (int)(5.0f * sinf(t/170.0f));
      int oy = (int)(4.0f * cosf(t/170.0f));
      int sz = 24;
      thickLine(lx-sz+ox, ey-sz+oy, lx+sz+ox, ey+sz+oy, 9+2*g, c);
      thickLine(lx+sz+ox, ey-sz+oy, lx-sz+ox, ey+sz+oy, 9+2*g, c);
      thickLine(rx-sz-ox, ey-sz-oy, rx+sz-ox, ey+sz-oy, 9+2*g, c);
      thickLine(rx+sz-ox, ey-sz-oy, rx-sz-ox, ey+sz-oy, 9+2*g, c);
      mouthWave(120, my, 32, 4, (int)t, g, c);
      break;
    }

    case SLEEPY: {
      float droop = 0.62f + 0.38f * (0.5f + 0.5f * sinf(t / 2400.0f));
      eyeRect(lx, ey+8, EW, (int)(EH*0.85f*droop*fOpen), ER, g, c);
      eyeRect(rx, ey+8, EW, (int)(EH*0.85f*droop*fOpen), ER, g, c);
      spr.fillRect(lx-EW/2-2-g, ey+8-EH/2-2-g, EW+4+2*g, 38+g, BG);
      spr.fillRect(rx-EW/2-2-g, ey+8-EH/2-2-g, EW+4+2*g, 38+g, BG);
      brow(lx, ey-36, 24, -2, 9, true,  g, c);
      brow(rx, ey-36, 24, -2, 9, false, g, c);
      mouthLine(120, my, 16, 6, g, c);
      break;
    }

    case SLEEPING: {
      int sb = (int)(4.0f * sinf(t/1400.0f));
      eyeRect(lx, ey+sb+6, 46, 8, 4, g, c);
      eyeRect(rx, ey+sb+6, 46, 8, 4, g, c);
      int br = 4 + (int)(2.0f * sinf(t/1400.0f));
      mouthO(120, my+sb, br, br, g, c);
      break;
    }

    case CURIOUS: {
      float ph = sinf(t / 1500.0f);          // actively searching, not posing
      int look = (int)(12.0f * ph);
      int tilt = (int)(8.0f * ph);
      eyeRect(lx+look, ey-tilt, EW, eh, ER, g, c);
      eyeRect(rx+look, ey+tilt, (int)(EW*0.78f), (int)(eh*0.78f), ER-4, g, c);
      if (wide) brow(lx+look, ey-tilt-40, 24, 8, -3, true, g, c);
      mouthO(120+look, my, 7, 9, g, c);
      break;
    }

    case SCARED: {
      int tr = (int)(2.0f * sinf(t/120.0f));
      spr.fillEllipse(lx+tr, ey-12, 22+g, (int)(28*fOpen)+g, c);
      spr.fillEllipse(rx-tr, ey-12, 22+g, (int)(28*fOpen)+g, c);
      if (wide) {
        brow(lx, ey-60, 22, 10, -4, true,  g, c);
        brow(rx, ey-60, 22, 10, -4, false, g, c);
      }
      mouthWave(120, my, 26, 3, (int)(t*1.5f), g, c);
      break;
    }

    case LOVE: {
      arcUpper(lx, ey+12, 31, (int)(19*fOpen), g, c);
      arcUpper(rx, ey+12, 31, (int)(19*fOpen), g, c);
      arcLower(120, my-6, 14, 6, g, c);
      for (int i=0;i<2;i++){
        float p2 = fmodf((t/2200.0f) + i*0.5f, 1.0f);
        heart(184 + (int)(10*sinf(p2*6.283f)), (88 - SPR_Y) - (int)(48*p2), 6 + (int)(p2*6), g, c);
      }
      break;
    }

    case ANGRY: {
      eyeQuad(lx, ey, EW, (int)(EH*0.72f*fOpen), 26, true,  g, c);
      eyeQuad(rx, ey, EW, (int)(EH*0.72f*fOpen), 26, false, g, c);
      arcUpper(120, my+6, 16, 6, g, c);
      int ss = 7 + (int)(2.0f * sinf(t/130.0f));
      spark(196, 62 - SPR_Y, ss, g, c);
      spark(52, 88 - SPR_Y, ss-2, g, c);
      break;
    }
  }
}

void draw(int e, unsigned long t) {
  spr.fillSprite(BG);                  // clearing RAM, not the glass

  // Five concentric passes build the bloom. All of it happens in memory, so the gradient
  // is now effectively free — only the single push touches SPI.
  shapes(e, t, 7, HALO4);
  shapes(e, t, 5, HALO3);
  shapes(e, t, 3, HALO2);
  shapes(e, t, 1, HALO1);
  shapes(e, t, 0, CORE);

  if (e == SLEEPING) {
    spr.setTextColor(CORE);
    for (int i=0;i<3;i++){
      float ph = fmodf((t/2600.0f) + i*0.333f, 1.0f);
      spr.setTextSize(1 + (int)(ph*2.5f));
      spr.setCursor(150 + (int)(20*ph), (88 - SPR_Y) - (int)(50*ph));
      spr.print('Z');
    }
    spr.setTextSize(1);
  }

  spr.pushSprite(0, SPR_Y);            // one operation, the only SPI traffic
}

#if SHOW_LABELS
// Outside the sprite band, so it goes straight to the panel and only on a change.
void drawLabel(int e) {
  tft.fillRect(60, 206, 120, 14, BG);
  tft.setTextColor(LBL);
  tft.setTextSize(1);
  tft.setCursor(120 - (int)strlen(NAMES[e]) * 3, 212);
  tft.print(NAMES[e]);
}
#endif

// ---------------------------------------------------------------- runtime

int current = 0;
unsigned long phaseStart = 0;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nface_lab — TFT_eSPI sprite");

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(BG);

  spr.setColorDepth(16);
  spriteOk = (spr.createSprite(240, SPR_H) != nullptr);
  Serial.printf("sprite 240x%d (%d KB): %s   free heap %u\n",
                SPR_H, (240*SPR_H*2)/1024, spriteOk ? "ok" : "FAILED", ESP.getFreeHeap());
  if (!spriteOk) { Serial.println("halting"); while (true) delay(1000); }

#if SHOW_LABELS
  drawLabel(0);
#endif
  phaseStart = millis();
}

void loop() {
  unsigned long now = millis();
  if (now - phaseStart >= HOLD_MS) {
    current = (current + 1) % FACE_COUNT;
    phaseStart = now;
    Serial.printf("-> %s\n", NAMES[current]);
#if SHOW_LABELS
    drawLabel(current);
#endif
  }
  updateBlink(now);
  updateSaccade(now);
  draw(current, now);
  delay(16);                           // ~60 fps ceiling
}
