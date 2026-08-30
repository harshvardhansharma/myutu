// Geometry check — display only. No I2C, no change-detection, no partial writes.
// Every frame is a full fillScreen followed by shapes, which is exactly what
// step1_display_test does and is the only pattern proven correct on this panel.
//
// Reference marks are drawn on purpose: a circle hugging the bezel and a crosshair at
// the exact centre. If the crosshair is not in the middle of the glass, the fault is
// panel addressing, not face layout — and that is measurable rather than guessable.

#include <Arduino_GFX_Library.h>

#define TFT_DC    4
#define TFT_CS    5
#define TFT_SCK  18
#define TFT_MOSI 23
#define TFT_RST  25

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, true /* IPS */);

#define EYE  RGB565(255, 208, 70)
#define MARK RGB565(0, 120, 90)
#define BG   RGB565(0, 0, 0)

const int EYE_LX = 76, EYE_RX = 164, EYE_CY = 115;
const int EW = 58, EH = 76, ER = 22;

void eyeRect(int cx,int cy,int w,int h,int r,uint16_t c){
  if (h < 4) h = 4;
  if (r > h/2) r = h/2;
  gfx->fillRoundRect(cx-w/2, cy-h/2, w, h, r, c);
}
void eyeArc(int cx,int cy,int r,int th,uint16_t c){
  gfx->fillCircle(cx, cy, r, c);
  gfx->fillCircle(cx, cy+th, r, BG);
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

const char *NAMES[] = {"neutral","surprised","happy","dizzy","sleepy","sleeping"};

void draw(int e) {
  gfx->fillScreen(BG);

  // Reference geometry, drawn first so the eyes sit on top of it.
  gfx->drawCircle(120, 120, 119, MARK);   // should hug the glass all the way round
  gfx->drawFastHLine(100, 120, 40, MARK); // crosshair at the true centre
  gfx->drawFastVLine(120, 100, 40, MARK);

  int ey = EYE_CY;
  switch (e) {
    case 0:
      eyeRect(EYE_LX, ey, EW, EH, ER, EYE);
      eyeRect(EYE_RX, ey, EW, EH, ER, EYE);
      break;
    case 1:
      gfx->fillCircle(EYE_LX, ey-4, 42, EYE);
      gfx->fillCircle(EYE_RX, ey-4, 42, EYE);
      brow(EYE_LX, ey-48, 26, 3, 0, true,  EYE);
      brow(EYE_RX, ey-48, 26, 3, 0, false, EYE);
      break;
    case 2:
      eyeArc(EYE_LX, ey+12, 35, 24, EYE);
      eyeArc(EYE_RX, ey+12, 35, 24, EYE);
      brow(EYE_LX, ey-40, 24, 5, -2, true,  EYE);
      brow(EYE_RX, ey-40, 24, 5, -2, false, EYE);
      break;
    case 3: {
      int sz = 26;
      thickLine(EYE_LX-sz, ey-sz, EYE_LX+sz, ey+sz, 9, EYE);
      thickLine(EYE_LX+sz, ey-sz, EYE_LX-sz, ey+sz, 9, EYE);
      thickLine(EYE_RX-sz, ey-sz, EYE_RX+sz, ey+sz, 9, EYE);
      thickLine(EYE_RX+sz, ey-sz, EYE_RX-sz, ey+sz, 9, EYE);
      break;
    }
    case 4:
      eyeRect(EYE_LX, ey+10, EW, EH, ER, EYE);
      eyeRect(EYE_RX, ey+10, EW, EH, ER, EYE);
      gfx->fillRect(EYE_LX-EW/2-2, ey+10-EH/2-2, EW+4, 42, BG);
      gfx->fillRect(EYE_RX-EW/2-2, ey+10-EH/2-2, EW+4, 42, BG);
      brow(EYE_LX, ey-34, 24, -2, 9, true,  EYE);
      brow(EYE_RX, ey-34, 24, -2, 9, false, EYE);
      break;
    case 5:
      eyeRect(EYE_LX, ey+8, 60, 10, 5, EYE);
      eyeRect(EYE_RX, ey+8, 60, 10, 5, EYE);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nface_check — display only, full-screen writes");
  if (!gfx->begin()) { Serial.println("begin() FAILED"); while (true) delay(1000); }
  gfx->fillScreen(BG);
}

void loop() {
  for (int e = 0; e < 6; e++) {
    Serial.printf("showing %s\n", NAMES[e]);
    draw(e);
    delay(3000);
  }
}
