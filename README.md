# Myutu (DeskPet) 🐣

An ESP32-based desktop pet — a Tamagotchi-style desk toy that shows emotions on a
round color display and reacts to touch, motion, and ambient light. **No AI, no WiFi,
no server, no subscriptions.** All logic runs locally on the ESP32. Battery-powered,
cordless, giftable.

Think: a tiny living creature on your desk that sleeps when the room goes dark, wakes
when you tap it, feels joy when you pat it, and gets dizzy when you shake it.

---

## Concept

A self-contained hardware pet with a personality driven entirely by on-device logic:

- 🌙 **Sleeping** — idle, or when the room is dark
- 👋 **Tap → wakes up** — blinks, looks around
- 🤗 **Pat (sustained touch) → joy** — happy face, hearts
- 😵 **Shake → dizzy/annoyed**
- 😴 **Left alone → drifts back to sleep** (idle decay)
- 🎉 **Double tap → little celebration animation**

Positioned as a potentially sellable product competing with the Divoom Ditoo and
Bitzee digital pet. Target retail ₹3,999–4,999 with a controlled per-unit BOM (~₹1,900
in electronics).

---

## Hardware (all ordered & received ✅)

| # | Component | Notes |
|---|-----------|-------|
| 1 | **ESP32 DevKit** (SquadPixel, DOIT-style, micro-USB) | Main MCU. CH340 USB-serial chip. |
| 2 | **1.28" round TFT display** (GC9A01, 240×240, IPS) | Color, SPI. The pet's face. |
| 3 | **MPU6050** accelerometer + gyroscope | Tap / shake / tilt detection (I2C). |
| 4 | ~~**TTP223** capacitive touch~~ | **DROPPED** — accelerometer covers interaction. |
| 5 | ~~**LM393 LDR** light sensor~~ | **DROPPED** — see Step 4. |
| 6 | **3.7V 1000mAh LiPo** battery (JST) | Power cell. |
| 7 | **TP4056 USB-C** charging module (with DW01 protection) | LiPo charging + protection. |
| 8 | **Breadboard + jumper wires** | Prototyping (not in final build). |

### Soldering kit (one-time tools)
63/37 solder wire · flux paste · adjustable-temp soldering iron kit · silicone heat mat.

### ⚠️ Known hardware gap — not yet ordered
The TP4056 outputs ~3.7–4.2V from the LiPo. This can't safely feed the ESP32's 3V3 pin,
and VIN/5V browns out as the battery drains. **Add a small MT3608 boost converter (~₹40)**
to step 3.7V → 5V into VIN before testing battery-powered operation. Not needed for the
USB-powered dev phase.

---

## Pin Map

Standard ESP32 WROOM GPIO labels. **Strapping pins (GPIO0, 2, 12, 15) deliberately avoided**
for sensors so they don't interfere with boot.

### GC9A01 Display (Hardware SPI)
| Display pin | ESP32 GPIO |
|-------------|------------|
| VCC | 3V3 (⚠️ NOT 5V — it's a 3.3V part) |
| GND | GND |
| SCL (clock) | GPIO18 |
| SDA (MOSI) | GPIO23 |
| DC | GPIO4 |
| CS | GPIO5 |
| RST | GPIO25 |

⚠️ **Two deviations from the original plan, confirmed against the actual parts:**
- The display module is the **7-pin variant — there is no BLK pin.** The backlight is
  hardwired on. Consequence: brightness can't be controlled in hardware, so the
  "dim when sleepy" behaviour must be done by **drawing dimmer pixels** instead.
- **GPIO16/17 are not broken out on this ESP32 board**, so DC and RST moved to
  **GPIO4** (freed up by the missing BLK) and **GPIO25**. Both are safe outputs and
  neither is a strapping pin.

### MPU6050 (I2C)
| Module pin | ESP32 GPIO |
|------------|------------|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |
| INT (optional) | GPIO27 (interrupt-based tap) |

### TTP223 Touch
| Module pin | ESP32 GPIO |
|------------|------------|
| VCC | 3V3 |
| GND | GND |
| SIG | GPIO13 |

Leave the TTP223 in default **momentary** mode (output HIGH only while touched).

### LDR (LM393)
| Module pin | ESP32 GPIO |
|------------|------------|
| VCC | 3V3 |
| GND | GND |
| DO (digital out) | GPIO14 |

⚠️ **The module turned out to be the 3-pin digital-only variant — there is no AO.**
An LM393 comparator with a trimpot; the threshold is set in hardware by turning the
screw. That's sufficient here: with the 7-pin display the backlight is hardwired on, so
a smooth light *level* had nothing to drive anyway.
**Use GPIO14, not GPIO34** — GPIO34-39 are input-only with **no internal pull-up**, and
an open-drain DO would float. **Measured polarity: DO reads HIGH when dark.**

---

## Development Environment

- **Host:** macOS
- **IDE:** Arduino IDE, or `arduino-cli` (installed via Homebrew) — see below
- **Board package:** `esp32` by Espressif — **v3.3.10**, compiles and uploads fine
- **FQBN:** `esp32:esp32:esp32` (ESP32 Dev Module)
- **Port:** `/dev/cu.usbserial-0001` (the `usbserial` prefix ⇒ CH340 chip)
- **Upload speed:** 115200 if the default is flaky

### Command-line workflow (faster than the IDE for iterating)
```sh
arduino-cli compile --fqbn esp32:esp32:esp32 <sketch_dir>
arduino-cli upload -p /dev/cu.usbserial-0001 --fqbn esp32:esp32:esp32 <sketch_dir>
arduino-cli monitor -p /dev/cu.usbserial-0001 --config baudrate=115200
```
Upload auto-resets the board over RTS — **no BOOT button needed** with this toolchain.
The board can also be reset on demand by pulsing RTS, which replays `setup()`.

### Required libraries
- **GFX Library for Arduino** by *moononournation* — GC9A01 support
- **Adafruit MPU6050** (+ Adafruit Unified Sensor, Adafruit BusIO) — for the accelerometer

---

## Gotchas already solved (so you don't re-hit them)

1. **Charge-only vs data cable.** A charge-only USB cable shows no serial port. Confirmed
   working data cable present — board enumerates as `/dev/cu.usbserial-0001`. (Quick test:
   plug phone in with the cable; if it offers file transfer, it carries data.)

2. **`LED_BUILTIN` not declared.** The stock Blink example fails to compile on this ESP32
   board because `LED_BUILTIN` isn't defined. Fix: hard-code the onboard LED to **GPIO2**.

3. **`SyntaxError: future feature annotations` on upload.** ESP32 core 3.x ran its flasher
   as a Python script needing Python ≥3.7 and the Mac's default Python was too old.
   **No longer an issue** — the machine now has Python 3.11.4 and core 3.3.10 flashes
   cleanly. (The old workaround was downgrading the core to 2.0.17; that's obsolete.)

4. **Board won't enter flash mode.** Hold the **BOOT** button while the console shows
   `Connecting......`, release once writing starts. Only needed from the Arduino IDE —
   `arduino-cli` auto-resets over RTS and never needs it.

5. **`'BLACK' was not declared in this scope`.** GFX Library 1.6.x renamed the colour
   constants. Use **`RGB565_BLACK`, `RGB565_WHITE`, `RGB565_RED`…**, not the bare names
   used in most online examples.

6. **GPIO16/17 are not broken out on this board.** The original pin map assumed they were.
   DC and RST moved to **GPIO4** and **GPIO25**. Check a pin physically exists before
   committing a pin map to code.

7. **Display is the 7-pin variant — no BLK pin.** Backlight is hardwired on. It lights the
   instant USB power is applied, which makes it a free power-wiring check: lit ⇒ VCC and
   GND are good and any remaining fault is SPI; dark ⇒ it's a power problem, don't bother
   debugging SPI yet.

8. **Serial Monitor shows nothing.** If a sketch only prints in `setup()`, attaching the
   monitor afterwards captures nothing, because `loop()` is silent and the monitor doesn't
   reset the board. Make bring-up sketches print continuously in `loop()`.

9. **Do not use `Arduino_Canvas` on this build — draw directly to the panel.**
    Tried three ways: 16-bit offset, 8-bit indexed offset, and 8-bit indexed full-size.
    **All three eventually corrupted the panel's addressing**, giving a shifted image that
    wraps around the bottom. Sometimes only after minutes of correct operation, which is
    what made it so slow to pin down. Direct drawing has never failed here.
    **Flicker is handled instead by skipping frames that would look identical** — a still
    face performs zero writes and so cannot flicker. Revisit double-buffering only on the
    soldered board, where signal integrity is a different problem.
    Background on the individual failures: A full 240×240 16-bit framebuffer is
   **115 KB in one contiguous block**, and a plain ESP32-WROOM with no PSRAM can't
   reliably allocate it — `begin()` returns false, the sketch halts, and the panel is
   left uninitialised showing garbage lines. **Draw directly to the panel and clear only
   the region that changes.** (`Arduino_Canvas_Indexed` at 8 bits ≈ 58 KB is the
   fallback if double-buffering ever becomes necessary.)

10. **Reading serial from the command line.** `arduino-cli monitor` asserts RTS, which
    holds EN low and **keeps the board in reset — it reads zero bytes** — and it leaves
    the port at 9600 on exit, so the next reader gets garbage. Read the port directly
    instead: open it, then `tcsetattr` the *open* descriptor to 115200 (setting `stty`
    before opening does not survive the open). Never leave a monitor running in the
    background while another reader is active; they corrupt each other.

11. **Vertical bands / striped garbage on the display = a bad jumper wire.** Cost most of
    an afternoon chasing SPI clock speed and framebuffer code; **it was a dead dupont wire
    all along.** The tell: **the image is frozen.** A sketch that floods the screen with
    colour and changes nothing means the panel isn't receiving valid commands and is still
    showing its last good frame — so the fault is a signal wire, not rendering code.
    Power is fine in this state (the panel is lit), so suspect `DC`, `CS`, `SCK`, `SDA`.
    **Swap the wires before touching the code.**

12. **Continuity testing without a multimeter.** Drive `RST` as a plain GPIO — no SPI, no
    library — toggling it every 1.5 s (`wire_test/`). Holding a GC9A01 in reset blanks the
    panel, so **a visible blink proves that wire, that ESP32 pin and that header contact
    all work end to end.** This is what isolated the fault above to physical rather than
    software. Any pin with a visible effect can be tested this way.

13. **Two display labels collide with the MPU's.** Both boards have pins marked `SDA` and
    `SCL` and they go to completely different GPIOs — display SDA→23, SCL→18; MPU
    SDA→21, SCL→22. Easy to cross while adding the second module.

14. **⚠️ Do NOT put I2C on GPIO21/22 alongside this display.** Cost several hours.
    Symptom: the display renders **shifted and wrapping** (a centred circle appears as two
    arcs meeting in the middle) — but **only while I2C traffic is running**. Display-only
    sketches are perfect, so it reads like a software bug and isn't one.
    **Cause: crosstalk.** On the DOIT 30-pin header the order is
    `D23, D22, TX0, RX0, D21, D19, D18, D5` — so the display's `SDA` (D23) is *physically
    adjacent* to I2C `SCL` (D22), and D21 sits two pins from `SCK` (D18). Parallel dupont
    wires on neighbouring pins couple I2C's fast edges into the SPI lines; the panel reads
    the noise as commands, and one of them sets its vertical scroll address.
    **Fix: move I2C to GPIO32/33** on the opposite header. Confirmed working.
    **Method that found it:** bisection — a display-only sketch (clean), then the same
    sketch plus I2C polling and nothing else (corrupted). Add one variable at a time.

15. **Draw reference geometry when debugging alignment.** A circle at `(120,120,r=119)`
    plus a centre crosshair turns "looks shifted" into a measurement: on a correct panel
    the circle hugs the bezel exactly and the crosshair sits dead centre.

16. **Don't toggle `RST` manually before `gfx->begin()`.** It leaves the GC9A01's init
    incomplete and its scroll address unset — producing the same shifted image, so it
    mimics the bug above. A reset-toggle liveness test is useful, but only in a sketch
    that isn't also initialising the display.

### ✅ MILESTONE — the pet works.
Steps 0, 1, 2 and the Step 5 renderer are all verified on hardware. `myutu_pet/` runs ten
expressions driven by real gestures, at ~40 fps, drawing directly to the panel with
frame-skipping for flicker control and a per-frame scroll-register reset for stability.

---

## Build & Bring-up Sequence

Staged approach: **validate each component individually on USB power before integrating.**
Do NOT connect the battery until display + sensors + animations all work on USB.

- [x] **Step 0 — Toolchain.** Blink uploads and runs. ✅
- [x] **Step 1 — Display.** ✅ Colour floods, orientation and blinking eyes all render.
      **Confirmed settings: `rotation = 0`, `IPS = true`.** Sketch: `step1_display_test/`.
- [ ] **Step 2 — MPU6050.** Wire I2C, confirm accelerometer values change on tilt.
- [–] **Step 3 — TTP223. DROPPED.** The accelerometer covers interaction on its own;
      `happy` is a double tap. One less module, three fewer wires. `digitalRead(13)` goes HIGH on touch.
- [–] **Step 4 — LDR. DROPPED.** Worked and was verified (see `d2cab64`), but wiring it
      destabilised the breadboard. `sleeping` is now an idle timeout only.
      Restore with: `git checkout d2cab64 -- myutu_pet/myutu_pet.ino` `analogRead(34)` swings when covered; note bright/dark thresholds.
- [~] **Step 5 — Emotion state machine.** Renderer and state machine DONE; sensors partial. sleep → wake → joy → dizzy → idle-decay, drawing
      real faces on the round display. **Faces are already designed and verified on glass**
      — see `faces_showcase/` and the Expression Design section below. What's left is
      wiring sensor events to them, not drawing them.
- [ ] **Step 6 — Power.** Add MT3608 boost → VIN, test on LiPo, tune power/sleep.
- [ ] **Step 7 — Enclosure.** 3D-printed shell (see reference below).

### Display init snippet (Step 1 starting point)
```cpp
#include <Arduino_GFX_Library.h>

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    4 /* DC */, 5 /* CS */, 18 /* SCK */, 23 /* MOSI */, GFX_NOT_DEFINED /* MISO */);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, 25 /* RST */, 0 /* rotation */, true /* IPS */);
```
If the image is mirrored / off-color / garbled, adjust the **rotation** (0–3) and the
**IPS** true/false flag — those are the usual culprits.

---

## Expression Design

**Ten faces, drawn with eyes alone.** Live in `myutu_pet/` — the pet itself.

| Face | Triggered by | Drawn as |
|------|--------------|----------|
| neutral | idle, awake | rounded rects |
| surprised | one tap | large ellipses, raised; brows high and flat |
| happy | two quick taps | downward-opening arcs, brows lifted |
| love | three quick taps | joy-squint arcs + small hearts drifting up |
| dizzy | shake | X eyes orbiting counter-phase, **no brows** |
| angry | shake again within 4 s | wedge sliced off each eye's inner top |
| curious | tilt and hold ~0.6 s | asymmetric, tipped, looking into the lean |
| scared | free fall (accel < 4 m/s²) | small, high, trembling |
| sleepy | idle 20 s | half-lidded and low, brows drooping outward |
| sleeping | **room goes dark**, or idle 45 s | small flat lines + Z's drifting up, top right |

Rules the design follows — worth keeping when adding to it:

- **Every face needs a trigger.** An early pass had twelve including wink and suspicious;
  nothing in the hardware could cause them. Each of the ten above is genuinely detectable.
- **Emotion is carried by EYE SHAPE, never by objects.** Heart-shaped eyes were tried and
  removed — eyes that become things break the rule the face runs on. Accent marks (Z's,
  hearts) sit in a separate layer above.
- **Brows are a signal, not decoration.** `dizzy` and `angry` deliberately have none: an X
  already says "not okay", and angry's wedge *is* the expression. A brow on top of either
  reads as a second, contradictory signal.
- **One colour.** Per-emotion mood colour is implemented (`MOOD_COLOUR 1`) but off — a
  single gold reads as one creature with moods rather than a status light.
- **Saccades, not drift.** Real eyes dart and hold. A slow pan reads as a camera.
- **Squash and stretch + overshoot.** Eyes widen as they close and spring past their
  target when opening. Easing is most of what separates a creature from a screen.
- **Blink-through transitions.** Expressions change at the closed point of a blink (340 ms),
  never by snapping between two shapes.

Rules the design follows — worth keeping when adding to it:

- **Every face needs a trigger.** An earlier pass had twelve including angry, sad, love
  and wink; nothing in the hardware could ever cause them. Six faces seen constantly
  read as more coherent than twelve seen rarely.
- **Brows are a signal, not decoration.** Only four faces have them; `neutral` and
  `sleeping` are deliberately bare, so a brow appearing *means* something. One thick bar
  each — the angle carries the emotion, so `brow()` takes inner/outer drops and mirrors
  itself per eye.
- **Colour is warm amber**, `RGB565(255, 208, 70)`. Nearly white's contrast on black,
  but reads as lit-from-inside rather than as an appliance.
- **Blinks scale eye height only**, so each face keeps its character through the blink
  instead of all six blinking identically. Plus a 2 px breathing bob throughout.
- **Rendering:** a 240×140 canvas at y=40 (67 KB) double-buffers just the eye band —
  a full-screen canvas is 115 KB and won't allocate (gotcha 9). Labels sit outside the
  band and are drawn only on change.

---

## Reference

**Deskbuddy** (open-source desktop pet) — useful for enclosure design and state-machine
structure. Note: it uses a monochrome OLED + WiFi, so its GFX/drawing code is **not**
directly portable to our round color GC9A01 — reuse the logic, rewrite the rendering.

https://living.ai/emo/ expressions

---

## Scope Decision — accelerometer only

The TTP223 and the LDR are both dropped. The MPU6050 alone distinguishes tap, double tap,
triple tap, tilt, shake, repeat-shake and free fall — enough for all ten expressions.

What this buys: **9 wires instead of 15**, two fewer modules to fit in the enclosure, a
lower BOM, and two fewer things that can work loose. Wiring faults, not code, consumed
most of the bring-up effort, so fewer connections is a real reliability gain.

What it costs: no pat (a double tap stands in), and no "sleeps when the room goes dark" —
which was the headline behaviour in the original concept. `sleeping` is an idle timeout.

**Consequence for Step 6:** the MPU is now the ONLY input, so it is also the only thing
that can wake the ESP32 from deep sleep. Wiring its `INT` pin to GPIO27 stops being
optional — it's the difference between hours and days of battery life.

---

## Design Principles

- **Fully offline by design** — no AI, WiFi, or server dependency. Zero recurring cost.
- **Staged bring-up** — prove each module alone before combining.
- **USB power for all dev** — battery is the last thing added, never the first.
- **Avoid strapping pins** for sensors (GPIO0, 2, 12, 15).
- **Verify each pin exists on the actual board** before committing to a pin map —
  GPIO16/17 turned out not to be broken out.
- **3.3V discipline** — the GC9A01 and sensors are 3.3V parts.

---

## Next Step

**Step 2 — MPU6050.** Wire the accelerometer on I2C (SDA→GPIO21, SCL→GPIO22) and confirm
the values move when the board is tilted and shaken. Note the rough magnitudes for a tap
and for a shake — those thresholds feed the emotion state machine in Step 5.
