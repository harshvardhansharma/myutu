// Step 1 — GC9A01 display bring-up.
// Goal: prove wiring, SPI, rotation and the IPS flag before any pet logic exists.
// USB power only. Do not connect the LiPo yet.

#include <Arduino_GFX_Library.h>

// 7-pin module: no BLK pin, backlight is hardwired on.
// GPIO16/17 are not broken out on this board, so DC and RST moved to 4 and 25.
#define TFT_DC    4
#define TFT_CS    5
#define TFT_SCK  18
#define TFT_MOSI 23
#define TFT_RST  25

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED /* MISO */);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, true /* IPS */);

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Step 1: GC9A01 bring-up");

  if (!gfx->begin()) {
    Serial.println("gfx->begin() FAILED — check SCK/MOSI/CS/DC/RST wiring and 3V3 power");
    return;
  }
  Serial.println("gfx->begin() ok");

  // 1. Solid colour flood — confirms the panel is alive and colours aren't swapped.
  //    If red shows as blue, flip the IPS flag in the constructor.
  uint16_t colors[] = {RGB565_RED, RGB565_GREEN, RGB565_BLUE, RGB565_WHITE};
  const char *names[] = {"RED", "GREEN", "BLUE", "WHITE"};
  for (int i = 0; i < 4; i++) {
    Serial.printf("fill %s\n", names[i]);
    gfx->fillScreen(colors[i]);
    delay(600);
  }

  // 2. Orientation test — confirms rotation and that the round bezel isn't clipping.
  //    "TOP" must read upright at the top of the circle. If not, try rotation 1-3.
  gfx->fillScreen(RGB565_BLACK);
  gfx->drawCircle(120, 120, 119, RGB565_WHITE);   // should hug the glass edge all the way round
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(90, 30);
  gfx->print("TOP");
  gfx->setCursor(78, 190);
  gfx->print("BOTTOM");
  delay(2000);
}

void loop() {
  // 3. Blinking eyes — the first thing that looks like a pet, and a live refresh test.
  static bool open = true;

  gfx->fillScreen(RGB565_BLACK);
  if (open) {
    gfx->fillCircle(80, 105, 22, RGB565_WHITE);
    gfx->fillCircle(160, 105, 22, RGB565_WHITE);
    gfx->fillCircle(80, 105, 9, RGB565_BLACK);
    gfx->fillCircle(160, 105, 9, RGB565_BLACK);
  } else {
    gfx->fillRoundRect(58, 100, 44, 8, 4, RGB565_WHITE);
    gfx->fillRoundRect(138, 100, 44, 8, 4, RGB565_WHITE);
  }
  // Simple smile.
  gfx->drawCircle(120, 130, 45, RGB565_WHITE);
  gfx->fillRect(60, 75, 120, 55, RGB565_BLACK);

  open = !open;
  delay(open ? 2200 : 180);
}
