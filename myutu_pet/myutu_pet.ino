// Myutu — the pet. BROWSE MODE.
//
// One tap advances to the next expression, and it stays there until tapped again.
// Deliberately simple: this is for reviewing all thirteen faces on the real glass
// without having to perform the right gesture to reach each one.
//
// The full gesture state machine — tap/double/triple, shake escalation, tilt, free fall,
// carried, idle decay — is written and lives in git history; restore it when the design
// is settled. See the README's state-machine table for the mapping.
//
// Thirteen expressions driven by real accelerometer gestures. Rendering is lifted whole
// from face_lab, which is the design source of truth alongside faces/index.html.
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
#include <Wire.h>

TFT_eSPI  tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
bool spriteOk = false;

#define C565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))

#define CORE  C565(255,255,255)
#define HALO1 C565( 96,122,172)
#define HALO2 C565( 52, 70,110)
#define HALO3 C565( 26, 36, 62)
#define HALO4 C565( 12, 17, 32)
#define BG    0            // palette index, not a colour
#define BG_RGB C565(0, 0, 0)   // for the panel itself
#define LBL   3            // palette index (a dim halo) for the debug label

// A second palette, so one element can be a different colour and still get the same
// five-pass bloom. shapes() is told WHICH LAYER it is drawing rather than a raw colour,
// and picks from whichever palette that element wants.
#define RCORE C565(255,  56,  40)      // bright red
#define RHAL1 C565(168,  30,  20)
#define RHAL2 C565( 98,  17,  12)
#define RHAL3 C565( 52,   9,   6)
#define RHAL4 C565( 24,   4,   3)

#define BCORE C565( 96, 186, 255)      // tear blue
#define BHAL1 C565( 58, 116, 170)
#define BHAL2 C565( 33,  68, 102)
#define BHAL3 C565( 17,  36,  55)
#define BHAL4 C565(  8,  17,  27)

// 4-BIT PALETTE SPRITE. A 16-bit band sprite is 82 KB and will not allocate once the
// I2C stack is also on the heap — createSprite returns null and the sketch halts to a
// black screen. At 4 bits it is 240 x 172 / 2 = 20.6 KB, a quarter of the cost, and
// because the face uses EXACTLY 16 colours the palette is lossless: nothing is
// quantised, unlike an 8-bit sprite which would band these dark halos badly.
//
// Drawing colours are now PALETTE INDICES (0-15), not RGB565 values.
const uint16_t SPR_PALETTE[16] = {
  0x0000,                                   // 0  background
  HALO4, HALO3, HALO2, HALO1, CORE,         // 1-5   white face
  RHAL4, RHAL3, RHAL2, RHAL1, RCORE,        // 6-10  red, angry's sparks
  BHAL4, BHAL3, BHAL2, BHAL1, BCORE         // 11-15 blue, crying's tears
};

const uint8_t PAL_WHITE[5] = { 1,  2,  3,  4,  5  };
const uint8_t PAL_RED[5]   = { 6,  7,  8,  9,  10 };
const uint8_t PAL_BLUE[5]  = { 11, 12, 13, 14, 15 };
const int      PAL_GROW[5]  = {     7,     5,     3,     1,    0  };

#define SHOW_LABELS 0     // no debug labels on the pet

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

// Overshoots past 1 and settles back. Eyes that spring open read as alive; eyes that
// arrive exactly on target and stop read as a slideshow.
float easeOutBack(float t){
  if (t<0) t=0; if (t>1) t=1;
  const float c1 = 1.70158f, c3 = c1 + 1.0f;
  float u = t - 1.0f;
  return 1.0f + c3*u*u*u + c1*u*u;
}

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

void eyeRect(int cx,int cy,int w,int h,int r,int g,uint8_t c){
  w += 2*g; h += 2*g; r += g;
  if (h < 3) h = 3;
  if (r > h/2) r = h/2;
  spr.fillRoundRect(cx-w/2, cy-h/2, w, h, r, c);
}

void arcUpper(int cx,int cy,int r,int th,int g,uint8_t c){   // ⌒ arch
  r += g; th += 2*g;
  if (th < 3) th = 3;
  spr.fillCircle(cx, cy, r, c);
  spr.fillCircle(cx, cy + th, r, BG);
}

void arcLower(int cx,int cy,int r,int th,int g,uint8_t c){   // ⌣ smile
  r += g; th += 2*g;
  if (th < 3) th = 3;
  spr.fillCircle(cx, cy - r + th, r, c);
  spr.fillCircle(cx, cy - r, r, BG);
}

// A genuinely solid bar: a filled quad along the line, plus round caps.
//
// The previous version stacked 1 px lines offset in BOTH x and y. On anything diagonal
// that leaves visible striping between the strokes — the "small lines" in the brows —
// and makes the effective width vary with angle. Filling the quad once fixes both.
void thickLine(int x0,int y0,int x1,int y1,int t,uint8_t c){
  float dx = (float)(x1-x0), dy = (float)(y1-y0);
  float len = sqrtf(dx*dx + dy*dy);
  if (len < 0.01f) { spr.fillCircle(x0, y0, t/2, c); return; }
  float px = -dy/len * (t/2.0f), py = dx/len * (t/2.0f);
  spr.fillTriangle(x0+px, y0+py, x1+px, y1+py, x1-px, y1-py, c);
  spr.fillTriangle(x0+px, y0+py, x1-px, y1-py, x0-px, y0-py, c);
  spr.fillCircle(x0, y0, t/2, c);          // round caps, so joined segments don't notch
  spr.fillCircle(x1, y1, t/2, c);
}

void brow(int cx,int y,int hl,int inDy,int outDy,bool left,int g,uint8_t c){
  int xi = left ? cx+hl : cx-hl, xo = left ? cx-hl : cx+hl;
  thickLine(xo, y+outDy, xi, y+inDy, 9 + 2*g, c);
}

// Hard-cornered quad — anger lives in the eye's own geometry, not in a wedge on top.
void eyeQuad(int cx,int cy,int w,int h,int innerDrop,bool left,int g,uint8_t c){
  w += 2*g; h += 2*g;
  int xo = left ? cx-w/2 : cx+w/2;
  int xi = left ? cx+w/2 : cx-w/2;
  int yt = cy-h/2, yb = cy+h/2;
  spr.fillTriangle(xo, yt, xi, yt+innerDrop, xi, yb, c);
  spr.fillTriangle(xo, yt, xi, yb,           xo, yb, c);
}

// Four-pointed star: two crossed diamonds, one tall and one wide. The pinched waist is
// what makes it read as a spark rather than as a plus sign drawn with thick lines.
// Isosceles trapezium, built from two triangles. Pass wTop < wBot for the inverted form
// used by `angry` — narrow above, flaring below.
void trapezium(int cx,int cy,int wTop,int wBot,int h,int g,uint8_t c){
  int wt = wTop + 2*g, wb = wBot + 2*g, hh = h + 2*g;
  int yt = cy - hh/2, yb = cy + hh/2;
  spr.fillTriangle(cx-wt/2, yt, cx+wt/2, yt, cx+wb/2, yb, c);
  spr.fillTriangle(cx-wt/2, yt, cx+wb/2, yb, cx-wb/2, yb, c);
}

// Teardrop: round below, pointed above. Does duty as both the anime sweat drop and a
// falling tear — the same shape reads as either depending on where it sits.
void teardrop(int x,int y,int r,int g,uint8_t c){
  r += g;
  spr.fillCircle(x, y, r, c);
  spr.fillTriangle(x-r, y-r/2, x+r, y-r/2, x, y-r*3, c);
}

// Four separate tapered spikes radiating from an EMPTY centre — not a solid star. The
// gaps between the points are what make it read as a spark of light rather than a blob.
void spark(int x,int y,int r,int g,uint8_t c){
  r += g;
  int inner = r/3 + 1;          // where each spike begins: the hollow middle
  int w     = r/4 + 1;          // half-width of a spike at its base
  spr.fillTriangle(x-w, y-inner, x+w, y-inner, x,   y-r, c);   // up
  spr.fillTriangle(x-w, y+inner, x+w, y+inner, x,   y+r, c);   // down
  spr.fillTriangle(x-inner, y-w, x-inner, y+w, x-r, y,   c);   // left
  spr.fillTriangle(x+inner, y-w, x+inner, y+w, x+r, y,   c);   // right
}

void heart(int cx,int cy,int sz,int g,uint8_t c){
  sz += g;
  spr.fillCircle(cx-sz/2, cy-sz/3, sz/2, c);
  spr.fillCircle(cx+sz/2, cy-sz/3, sz/2, c);
  spr.fillTriangle(cx-sz, cy-sz/6, cx+sz, cy-sz/6, cx, cy+sz, c);
}

// ---------------------------------------------------------------- mouths
// Small on purpose: in the reference faces the mouth is a fraction of an eye's width.

void mouthLine(int cx,int cy,int w,int th,int g,uint8_t c){
  w += 2*g; th += 2*g;
  spr.fillRoundRect(cx-w/2, cy-th/2, w, th, th/2, c);
}
void mouthO(int cx,int cy,int rx,int ry,int g,uint8_t c){
  spr.fillEllipse(cx, cy, rx+g, ry+g, c);
}
void mouthWave(int cx,int cy,int w,int amp,int phase,int g,uint8_t c){
  const int N = 8;
  int px = cx-w/2, py = cy;
  for (int i=1;i<=N;i++){
    int x = cx - w/2 + (w*i)/N;
    int y = cy + (int)(amp * sinf(i * 1.6f + phase * 0.01f));
    thickLine(px, py, x, y, 6 + 2*g, c);
    px = x; py = y;
  }
}
void mouthW(int cx,int cy,int w,int g,uint8_t c){            // the cat-style ω
  arcLower(cx - w/4, cy, w/4, 6, g, c);
  arcLower(cx + w/4, cy, w/4, 6, g, c);
}

// ---------------------------------------------------------------- faces

enum { NEUTRAL, SURPRISED, HAPPY, DIZZY, SLEEPY, SLEEPING, CURIOUS, SCARED, LOVE, ANGRY,
       EATING, NERVOUS, CRYING, FACE_COUNT };
const char *NAMES[] = {"neutral","surprised","happy","dizzy","sleepy","sleeping",
                       "curious","scared","love","angry","eating","nervous","crying"};

void shapes(int e, unsigned long t, int li) {
  int     g = PAL_GROW[li];
  uint8_t c = PAL_WHITE[li];
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
      // Fat, heavily rounded eyes and a tiny smile. NO BROWS — the reference has none,
      // and without them the face reads as content rather than performing. The bulge
      // comes from a big corner radius on a wide-ish rect: rounder reads younger, and
      // the smaller the mouth the cuter it gets.
      eyeRect(lx, ey, 50, (int)(64*fOpen), 22, g, c);
      eyeRect(rx, ey, 50, (int)(64*fOpen), 22, g, c);
      arcLower(120, my-2, 10, 4, g, c);            // barely there, and cuter for it
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
      // Looks left, then right — and the asymmetry travels with it: whichever side it's
      // looking toward gets the bigger eye and the raised brow.
      //
      // The swap is driven by |ph| rather than switched at the crossover: at the middle
      // of the sweep both eyes are the same size, so there's no pop when the sides trade.
      // The brow only appears once the lean is pronounced, for the same reason.
      float ph = sinf(t / 1800.0f);
      float a  = fabsf(ph);                  // 0 mid-sweep, 1 at the extremes
      bool  leftBig = (ph >= 0);
      int   look = (int)(12.0f * ph);
      int   tt   = (int)(8.0f * a);
      float sh   = 1.0f - 0.26f * a;         // the far eye shrinks as the lean deepens

      int lw = leftBig ? EW : (int)(EW*sh);
      int lh = leftBig ? eh : (int)(eh*sh);
      int rw = leftBig ? (int)(EW*sh) : EW;
      int rh = leftBig ? (int)(eh*sh) : eh;
      int ly = leftBig ? ey-tt : ey+tt;
      int ry = leftBig ? ey+tt : ey-tt;

      eyeRect(lx+look, ly, lw, lh, leftBig ? ER : ER-4, g, c);
      eyeRect(rx+look, ry, rw, rh, leftBig ? ER-4 : ER, g, c);
      if (wide && a > 0.35f) {
        if (leftBig) brow(lx+look, ly-48, 24, 8, -3, true,  g, c);
        else         brow(rx+look, ry-48, 24, 8, -3, false, g, c);
      }
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
      // Hearts drift up from the right corner on a staggered loop, exactly like the Z's
      // on `sleeping` — same timing, same rise, same growth. Growing as they climb reads
      // as floating away from the face rather than merely sliding.
      for (int i=0;i<3;i++){
        float ph = fmodf((t/2600.0f) + i*0.333f, 1.0f);
        heart(176 + (int)(16*ph), (104 - SPR_Y) - (int)(54*ph), 5 + (int)(ph*7), g, c);
      }
      break;
    }

    case NERVOUS: {
      // The sweat drop IS the expression — in the chibi vocabulary it's what separates
      // nervous from merely awake. Everything else stays understated: eyes very slightly
      // mismatched, and an uneasy waver of a mouth.
      eyeRect(lx, ey, EW, eh, ER, g, c);
      eyeRect(rx, ey, (int)(EW*0.92f), (int)(eh*0.92f), ER, g, c);
      mouthWave(120, my, 24, 3, (int)(t*0.8f), g, c);
      int sd = (int)(5.0f * sinf(t/700.0f));       // slides, never quite falls
      teardrop(186, (62 - SPR_Y) + sd, 9, g, c);
      break;
    }

    case CRYING: {
      // Wide eyes and tears that actually fall and repeat. No mouth — the tears carry it.
      eyeRect(lx, ey, EW, eh, ER, g, c);
      eyeRect(rx, ey, EW, eh, ER, g, c);
      // Two continuous streams rather than separate drops — waterworks, not drips.
      // Built from overlapping rounded segments that narrow toward the bottom and
      // wobble side to side, so the flow reads as moving even though nothing falls.
      uint8_t bc = PAL_BLUE[li];                  // streams in blue, eyes stay white
      for (int side=0; side<2; side++){
        int sx  = (side==0) ? lx-15 : rx+15;
        int top = ey + 28;
        int bot = SPR_H - 4;
        for (int yy = top; yy < bot; yy += 5){
          float f  = (float)(yy - top) / (float)(bot - top);
          int   w  = 10 - (int)(5.0f * f);         // tapering as it runs down
          int   wob = (int)(3.0f * sinf(yy * 0.16f + t / 150.0f));
          spr.fillRoundRect(sx + wob - w/2 - g, yy - g, w + 2*g, 7 + 2*g, w/2 + g, bc);
        }
      }
      break;
    }

    case EATING: {
      // Plain wide-awake eyes, and a small mouth chewing off to one side. The asymmetry
      // is deliberate — a centred mouth reads as talking, an offset one as chewing.
      eyeRect(lx, ey, EW, eh, ER, g, c);
      eyeRect(rx, ey, EW, eh, ER, g, c);
      float ch = fabsf(sinf(t / 190.0f));           // fast, uneven chew
      spr.fillRoundRect(100-g, my - (int)(4 + 7*ch) - g,
                        22+2*g, (int)(8 + 14*ch)+2*g, 4+g, c);
      break;
    }

    case ANGRY: {
      eyeQuad(lx, ey, EW, (int)(EH*0.72f*fOpen), 26, true,  g, c);
      eyeQuad(rx, ey, EW, (int)(EH*0.72f*fOpen), 26, false, g, c);
      // Heavy brows, longer and thicker than elsewhere, driven down toward the nose and
      // low enough to touch the eyes. Attached rather than floating: a joined brow reads
      // as a scowl, a separated one as mere surprise.
      if (wide) {
        thickLine(lx-32, ey-36, lx+32, ey-18, 14+2*g, c);
        thickLine(rx+32, ey-36, rx-32, ey-18, 14+2*g, c);
      }
      // One solid trapezium, inverted — narrow at the top, flaring wider at the bottom.
      // Straight edges agree with the angular eyes in a way a curved frown never did.
      trapezium(120, ey+47, 46, 76, 16, g, c);
      // Sparks in red, and pulsing harder than the rest of the face moves — anger is the
      // one expression that should feel like it has a pulse of its own.
      uint8_t rc = PAL_RED[li];
      int ss = 11 + (int)(4.0f * sinf(t/160.0f));
      spark(198, 58 - SPR_Y, ss, g, rc);           // one mark only, upper right
      break;
    }
  }
}

void draw(int e, unsigned long t) {
  spr.fillSprite(BG);                  // clearing RAM, not the glass

  // Five concentric passes build the bloom. All of it happens in memory, so the gradient
  // is now effectively free — only the single push touches SPI.
  for (int li = 0; li < 5; li++) shapes(e, t, li);

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


// ---------------------------------------------------------------- sensor
//
// I2C on 32/33, NOT 21/22 — those sit adjacent to the display's SPI pins on the DOIT
// header and crosstalk corrupts the panel (README gotcha 14).
// No WHO_AM_I check: these clone chips report 0x70/0x72/0x98 and Adafruit's library
// refuses to start on that alone. The register map is identical.

#define I2C_SDA 32
#define I2C_SCL 33

#define REG_ACCEL_XOUT_H 0x3B
#define REG_PWR_MGMT_1   0x6B
#define REG_SMPLRT_DIV   0x19
#define REG_CONFIG       0x1A
#define REG_GYRO_CONFIG  0x1B
#define REG_ACCEL_CONFIG 0x1C
#define ACCEL_LSB_PER_G 4096.0f
#define GYRO_LSB_PER_DPS  65.5f

// Measured on this build: resting noise jolt 0.08-0.63, spin 0; a real tap/shake is
// jolt 25.2, spin 230. A 40x gap, so these sit far from the noise floor.
#define JOLT_TAP        4.0f
#define SPIN_SHAKE    120.0f
#define SPIN_TAP_MAX  100.0f
#define TAP_REFRACTORY  250
#define DOUBLE_TAP_MS   600
#define TILT_THRESH     4.5f     // ~27 degrees of lean
#define TILT_HOLD_MS    600
#define FREEFALL_MAX    4.0f
#define SHAKE_WINDOW   4000      // repeat shakes inside this escalate
#define HELD_LO         0.8f     // sustained motion between these two reads as "carried"
#define HELD_HI         4.0f
#define HELD_MS        1500

#define SURPRISED_HOLD 1500
#define HAPPY_HOLD     2500
#define LOVE_HOLD      3000
#define DIZZY_HOLD     2500
#define ANGRY_HOLD     2500
#define CRYING_HOLD    3500
#define CURIOUS_HOLD   2000
#define SCARED_HOLD    2000
#define NERVOUS_HOLD   2500
#define EATING_HOLD    4000
#define SLEEPY_AFTER  20000
#define SLEEPING_AFTER 45000
#define TRANSITION_MS   420

uint8_t addr = 0x68;
bool mpuOk = false;
float gravityBase = 9.81f, gxBias = 0, gyBias = 0, gzBias = 0;

bool writeReg(uint8_t r, uint8_t v){
  Wire.beginTransmission(addr); Wire.write(r); Wire.write(v);
  return Wire.endTransmission() == 0;
}
bool readRegs(uint8_t r, uint8_t *b, uint8_t n){
  Wire.beginTransmission(addr); Wire.write(r);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)n) != n) return false;
  for (uint8_t i=0;i<n;i++) b[i] = Wire.read();
  return true;
}
bool present(uint8_t a){ Wire.beginTransmission(a); return Wire.endTransmission()==0; }

bool initMPU(){
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

// Measures its own resting state. These parts have percent-level scale error and a real
// gyro offset; left uncorrected that bias lands straight in every threshold.
void calibrate(){
  const int N = 100;
  float magSum=0, gx=0, gy=0, gz=0; int taken=0;
  for (int i=0;i<N;i++){
    uint8_t b[14];
    if (!readRegs(REG_ACCEL_XOUT_H, b, 14)) { delay(5); continue; }
    float ax = (int16_t)((b[0]<<8)|b[1]) / ACCEL_LSB_PER_G * 9.81f;
    float ay = (int16_t)((b[2]<<8)|b[3]) / ACCEL_LSB_PER_G * 9.81f;
    float az = (int16_t)((b[4]<<8)|b[5]) / ACCEL_LSB_PER_G * 9.81f;
    magSum += sqrtf(ax*ax+ay*ay+az*az);
    gx += (int16_t)((b[8]<<8)|b[9]);
    gy += (int16_t)((b[10]<<8)|b[11]);
    gz += (int16_t)((b[12]<<8)|b[13]);
    taken++; delay(5);
  }
  if (taken < 10) return;
  gravityBase = magSum/taken; gxBias = gx/taken; gyBias = gy/taken; gzBias = gz/taken;
}

// ---------------------------------------------------------------- state

int shown = NEUTRAL, target = NEUTRAL;
bool transitioning = false;
unsigned long transStart = 0;
unsigned long holdUntil = 0, lastInteraction = 0, lastTapAt = 0, lastEventAt = 0;
unsigned long tiltSince = 0, freefallSince = 0, heldSince = 0, lastShakeAt = 0;
unsigned long nextSnack = 0;
int tapCount = 0, shakeCount = 0, tiltDir = 0;

// Expressions change at the CLOSED point of a blink rather than snapping between two
// shapes. Snapping reads as a screen refreshing; closing your eyes to change your mind
// reads as a creature deciding something.
void enter(int s, unsigned long hold){
  holdUntil = millis() + hold;
  if (s == target) return;
  Serial.printf("-> %s\n", NAMES[s]);
  target = s;
  transitioning = true;
  transStart = millis();
}
// ---------------------------------------------------------------- runtime

void setup(){
  Serial.begin(115200);
  delay(300);
  Serial.println("\nMyutu");

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(BG_RGB);

  spr.setColorDepth(4);
  spriteOk = (spr.createSprite(240, SPR_H) != nullptr);
  if (spriteOk) spr.createPalette((uint16_t *)SPR_PALETTE, 16);
  Serial.printf("sprite 240x%d 4-bit (%d KB): %s   free heap %u\n",
                SPR_H, (240*SPR_H/2)/1024, spriteOk ? "ok" : "FAILED", ESP.getFreeHeap());
  if (!spriteOk) { Serial.println("halting"); while (true) delay(1000); }

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  mpuOk = initMPU();
  Serial.println(mpuOk ? "MPU ok - hold still, calibrating" : "MPU NOT FOUND");
  if (mpuOk) calibrate();

  lastInteraction = millis();
  nextSnack = millis() + random(60000, 150000);
  Serial.println("running");
}

void loop(){
  unsigned long now = millis();
  static int i2cFails = 0;

  // I2C self-recovery: a stalled bus would otherwise freeze the pet permanently.
  if (i2cFails > 20) {
    i2cFails = 0;
    Wire.end(); delay(10);
    Wire.begin(I2C_SDA, I2C_SCL); Wire.setClock(100000);
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

      // One tap, one step forward. The spin test keeps a shake from counting as a tap,
      // and the refractory window stops a single impact ringing into several.
      if (now - lastEventAt > TAP_REFRACTORY && jolt > JOLT_TAP && spin < SPIN_TAP_MAX) {
        lastEventAt = now;
        int next = (target + 1) % FACE_COUNT;
        enter(next, 0);
      }
    }
  }

  // Blink drives eye height. During a transition it runs a full close-then-open, and the
  // face swaps at the closed point.
  updateSaccade(now);
  if (transitioning) {
    float half = TRANSITION_MS / 2.0f;
    float e = now - transStart;
    if (e < half)                 fOpen = 1.0f - ease(e / half);
    else if (e < TRANSITION_MS) { shown = target; fOpen = easeOutBack((e - half) / half); }
    else { shown = target; transitioning = false; fOpen = 1.0f; blinkAt = now + random(2600, 5200); }
  } else {
    updateBlink(now);
  }

  draw(shown, now);

  // Heartbeat. In browse mode nothing prints unless tapped, so a silent port would look
  // identical to a halted sketch — which is exactly the trap README gotcha 8 warns about.
  static unsigned long lastBeat = 0;
  if (now - lastBeat >= 3000) {
    lastBeat = now;
    Serial.printf("alive t=%lus  showing %s  mpu=%d  heap %u\n",
                  now/1000, NAMES[shown], (int)mpuOk, ESP.getFreeHeap());
  }

  delay(16);
}
