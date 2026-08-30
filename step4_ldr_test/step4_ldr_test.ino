// Step 4 — LDR bring-up (digital-output module).
//
// This module is the 3-pin variant: VCC, GND, DO. No analog out — an LM393 comparator
// comparing the LDR against a trimpot, so the threshold is set in hardware by turning
// the little screw. That's sufficient here: the 7-pin display has no backlight control,
// so there is nothing a smooth light LEVEL could have driven anyway.
//
// D14, not D34: GPIO34-39 are input-only with NO internal pull-up, and if this module's
// DO is open-drain without its own pull-up it would float and read noise.
//
// Prints continuously and never halts (README gotcha 8).

#define LDR_PIN 14
#define STABLE_MS 800       // light must hold its new state this long to count

int rawNow = 0, stableState = -1;
int lastRaw = -1;
unsigned long changedAt = 0, lastReport = 0;
unsigned long flips = 0;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nStep 4: LDR bring-up (digital DO on GPIO14)");
  pinMode(LDR_PIN, INPUT_PULLUP);
  Serial.println("Cover the sensor, then uncover it. The state should flip.");
  Serial.println("If it never flips, turn the trimpot slowly until it does.\n");
  changedAt = millis();
}

void loop() {
  rawNow = digitalRead(LDR_PIN);
  unsigned long now = millis();

  // Debounce in TIME, not by voltage: a hand passing over, or a flickering bulb, would
  // otherwise thrash the pet between awake and asleep. It must mean it.
  if (rawNow != lastRaw) { lastRaw = rawNow; changedAt = now; }
  if (rawNow != stableState && now - changedAt >= STABLE_MS) {
    stableState = rawNow;
    flips++;
    Serial.printf(">>> settled: DO=%d  (%s)\n", stableState,
                  stableState ? "HIGH" : "LOW");
  }

  if (now - lastReport >= 500) {
    lastReport = now;
    Serial.printf("DO now=%d  stable=%d  flips=%lu\n", rawNow, stableState, flips);
  }
  delay(20);
}
