// Wire continuity test — no multimeter required.
//
// Drives the display's RST line directly as a plain GPIO, with no SPI and no library.
// Holding a GC9A01 in reset blanks the panel, so if this wire has continuity end to end
// the screen VISIBLY CHANGES every 1.5 seconds. If the picture sits frozen, the fault is
// this wire, the ESP32 pin, or the module's header contact — and none of the SPI signals
// can be reaching it either.

#define TFT_RST 25

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nRST wire test on GPIO25");
  Serial.println("Watch the panel: it should blank and restore every 1.5s.");
  pinMode(TFT_RST, OUTPUT);
}

void loop() {
  digitalWrite(TFT_RST, LOW);
  Serial.println("RST LOW  -> panel should go blank");
  delay(1500);
  digitalWrite(TFT_RST, HIGH);
  Serial.println("RST HIGH -> panel holds its last image");
  delay(1500);
}
