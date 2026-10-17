#include <FastLED.h>
#include <EEPROM.h>

#define NUM_LEDS   30
#define CLOCK_PIN  2   // CI / CIN
#define DATA_PIN   3   // DI / DIN

#define FX_SOLID   0
#define FX_SPIN    1
#define FX_STROBE  2
#define FX_PULSE   3
#define FX_CHASE   4
#define FX_RAINBOW 5
#define FX_SPLIT   6
#define FX_ALT     7
#define FX_GRAD    8
#define FX_SWAP    9
#define FX_COUNT   10

#define MAX_STEPS  12
#define EE_MAGIC   0xC3
#define EE_VERSION 2

struct Step {
  uint8_t effect;
  uint8_t r, g, b;
  uint8_t r2, g2, b2;
  uint8_t bri;
  uint8_t speed;
  uint16_t dur;
};

CRGB leds[NUM_LEDS];

// FastLED 3.10 sul Nano manda l'APA102 solo sui pin SPI 11 e 13.
volatile uint8_t *dataOut;
volatile uint8_t *clockOut;
uint8_t dataMask;
uint8_t clockMask;

Step seq[MAX_STEPS];
uint8_t seqCount = 0;
int8_t playIndex = -1;

uint8_t fx = FX_SOLID;
uint8_t cr = 0, cg = 150, cb = 255;
uint8_t cr2 = 255, cg2 = 100, cb2 = 0;
uint8_t bri = 255;
uint8_t spd = 160;
uint8_t renderBri = 255;
uint8_t pos = 0;
uint8_t introLaps = 0;
uint16_t pulse = 0;
bool strobeOn = true;
bool intro = false;
bool dirty = true;
unsigned long lastAnim = 0;
unsigned long stepStart = 0;

void apa102Send(uint8_t value) {
  for (uint8_t mask = 0x80; mask; mask >>= 1) {
    if (value & mask) *dataOut |= dataMask;
    else              *dataOut &= ~dataMask;
    *clockOut |= clockMask;
    *clockOut &= ~clockMask;
  }
}

uint8_t scale(uint8_t value) {
  return (uint16_t)value * renderBri / 255;
}

void showLeds() {
  apa102Send(0x00);
  apa102Send(0x00);
  apa102Send(0x00);
  apa102Send(0x00);

  for (int i = 0; i < NUM_LEDS; i++) {
    apa102Send(0xFF);
    apa102Send(scale(leds[i].b));
    apa102Send(scale(leds[i].g));
    apa102Send(scale(leds[i].r));
  }

  apa102Send(0xFF);
  apa102Send(0xFF);
  apa102Send(0xFF);
  apa102Send(0xFF);
}

uint16_t frameMs() {
  return 16 + (uint16_t)(255 - spd) * 100 / 255;
}

void applyLook(uint8_t effect, uint8_t r, uint8_t g, uint8_t b,
               uint8_t r2, uint8_t g2, uint8_t b2,
               uint8_t brightness, uint8_t speed) {
  fx = effect < FX_COUNT ? effect : FX_SOLID;
  cr = r;
  cg = g;
  cb = b;
  cr2 = r2;
  cg2 = g2;
  cb2 = b2;
  bri = brightness;
  spd = speed;
  renderBri = bri;
  pos = 0;
  pulse = 0;
  strobeOn = false;
  lastAnim = 0;
  dirty = true;
  bool flat = fx == FX_SOLID || fx == FX_STROBE || fx == FX_PULSE || fx == FX_SWAP;
  fill_solid(leds, NUM_LEDS, flat ? CRGB(cr, cg, cb) : CRGB::Black);
  showLeds();
}

void applyStep(const Step &step) {
  applyLook(step.effect, step.r, step.g, step.b, step.r2, step.g2, step.b2, step.bri, step.speed);
}

int stepAddr(uint8_t index, uint8_t version) {
  return 4 + index * (version == 1 ? 8 : 11);
}

void writeStep(uint8_t index, const Step &step) {
  int addr = stepAddr(index, EE_VERSION);
  EEPROM.update(addr + 0, step.effect);
  EEPROM.update(addr + 1, step.r);
  EEPROM.update(addr + 2, step.g);
  EEPROM.update(addr + 3, step.b);
  EEPROM.update(addr + 4, step.r2);
  EEPROM.update(addr + 5, step.g2);
  EEPROM.update(addr + 6, step.b2);
  EEPROM.update(addr + 7, step.bri);
  EEPROM.update(addr + 8, step.speed);
  EEPROM.update(addr + 9, step.dur & 0xFF);
  EEPROM.update(addr + 10, step.dur >> 8);
}

Step readStep(uint8_t index, uint8_t version) {
  Step step;
  int addr = stepAddr(index, version);
  step.effect = EEPROM.read(addr + 0);
  step.r = EEPROM.read(addr + 1);
  step.g = EEPROM.read(addr + 2);
  step.b = EEPROM.read(addr + 3);
  if (version == 1) {
    step.r2 = 255;
    step.g2 = 100;
    step.b2 = 0;
    step.bri = EEPROM.read(addr + 4);
    step.speed = EEPROM.read(addr + 5);
    step.dur = EEPROM.read(addr + 6) | ((uint16_t)EEPROM.read(addr + 7) << 8);
  } else {
    step.r2 = EEPROM.read(addr + 4);
    step.g2 = EEPROM.read(addr + 5);
    step.b2 = EEPROM.read(addr + 6);
    step.bri = EEPROM.read(addr + 7);
    step.speed = EEPROM.read(addr + 8);
    step.dur = EEPROM.read(addr + 9) | ((uint16_t)EEPROM.read(addr + 10) << 8);
  }
  return step;
}

bool loadSequence() {
  if (EEPROM.read(0) != EE_MAGIC) return false;
  uint8_t version = EEPROM.read(1);
  if (version != 1 && version != EE_VERSION) return false;
  uint8_t count = EEPROM.read(2);
  if (count == 0 || count > MAX_STEPS) return false;
  seqCount = count;
  for (uint8_t i = 0; i < seqCount; i++) seq[i] = readStep(i, version);
  return true;
}

void saveSequence() {
  EEPROM.update(0, EE_MAGIC);
  EEPROM.update(1, EE_VERSION);
  EEPROM.update(2, seqCount);
  for (uint8_t i = 0; i < seqCount; i++) writeStep(i, seq[i]);
}

void clearSequence() {
  seqCount = 0;
  playIndex = -1;
  EEPROM.update(0, 0);
}

void playSequence() {
  if (seqCount == 0) return;
  intro = false;
  playIndex = 0;
  stepStart = millis();
  applyStep(seq[0]);
}

void dumpSequence() {
  Serial.print("B ");
  Serial.println(seqCount);
  for (uint8_t i = 0; i < seqCount; i++) {
    Step step = seq[i];
    Serial.print("T ");
    Serial.print(step.effect);
    Serial.print(" ");
    Serial.print(step.r);
    Serial.print(" ");
    Serial.print(step.g);
    Serial.print(" ");
    Serial.print(step.b);
    Serial.print(" ");
    Serial.print(step.r2);
    Serial.print(" ");
    Serial.print(step.g2);
    Serial.print(" ");
    Serial.print(step.b2);
    Serial.print(" ");
    Serial.print(step.bri);
    Serial.print(" ");
    Serial.print(step.speed);
    Serial.print(" ");
    Serial.println(step.dur);
  }
  Serial.println("END");
}

bool parseLook(char *line, Step &step) {
  int effect, r, g, b, r2, g2, b2, brightness, speed, dur;
  int got = sscanf(line, "%d %d %d %d %d %d %d %d %d %d",
                   &effect, &r, &g, &b, &r2, &g2, &b2, &brightness, &speed, &dur);
  if (got < 9) return false;
  step.effect = constrain(effect, 0, FX_COUNT - 1);
  step.r = constrain(r, 0, 255);
  step.g = constrain(g, 0, 255);
  step.b = constrain(b, 0, 255);
  step.r2 = constrain(r2, 0, 255);
  step.g2 = constrain(g2, 0, 255);
  step.b2 = constrain(b2, 0, 255);
  step.bri = constrain(brightness, 0, 255);
  step.speed = constrain(speed, 0, 255);
  step.dur = got >= 10 ? constrain(dur, 0, 65535) : 0;
  return true;
}

void handleLine(char *line) {
  if (line[0] == 'L' && line[1] == ' ') {
    Step step;
    if (!parseLook(line + 2, step)) return;
    intro = false;
    playIndex = -1;
    applyStep(step);
    Serial.println("OK");
  } else if (strcmp(line, "N") == 0) {
    seqCount = 0;
    Serial.println("OK");
  } else if (line[0] == 'T' && line[1] == ' ') {
    if (seqCount < MAX_STEPS) {
      Step step;
      if (parseLook(line + 2, step)) seq[seqCount++] = step;
    }
    Serial.println("OK");
  } else if (strcmp(line, "S") == 0) {
    saveSequence();
    playSequence();
    Serial.println("OK");
  } else if (strcmp(line, "P") == 0) {
    playSequence();
    Serial.println("OK");
  } else if (strcmp(line, "C") == 0) {
    clearSequence();
    intro = false;
    applyLook(FX_SOLID, 0, 150, 255, 255, 100, 0, 255, 0);
    Serial.println("OK");
  } else if (strcmp(line, "D") == 0) {
    dumpSequence();
  }
}

void pollSerial() {
  static char buf[72];
  static uint8_t n = 0;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (n > 0) {
        buf[n] = 0;
        handleLine(buf);
        n = 0;
      }
    } else if (n < sizeof(buf) - 1) {
      buf[n++] = c;
    }
  }
}

void finishIntro() {
  intro = false;
  applyLook(FX_SOLID, 0, 150, 255, 255, 100, 0, 255, 0);
}

void renderSolid() {
  if (!dirty) return;
  fill_solid(leds, NUM_LEDS, CRGB(cr, cg, cb));
  renderBri = bri;
  showLeds();
  dirty = false;
}

void renderRun(uint8_t trail, uint8_t width) {
  if (lastAnim != 0 && millis() - lastAnim < frameMs()) return;
  lastAnim = millis();
  fadeToBlackBy(leds, NUM_LEDS, trail);
  for (uint8_t k = 0; k < width; k++) {
    leds[(pos + k) % NUM_LEDS] = CRGB(cr, cg, cb);
  }
  renderBri = bri;
  showLeds();
  pos++;
  if (pos >= NUM_LEDS) {
    pos = 0;
    if (intro) {
      introLaps++;
      if (introLaps >= 3) finishIntro();
    }
  }
}

void renderStrobe() {
  uint16_t period = 30 + (uint16_t)(255 - spd) * 3;
  if (lastAnim != 0 && millis() - lastAnim < period) return;
  lastAnim = millis();
  strobeOn = !strobeOn;
  fill_solid(leds, NUM_LEDS, CRGB(cr, cg, cb));
  renderBri = strobeOn ? bri : 0;
  showLeds();
}

void renderPulse() {
  if (lastAnim != 0 && millis() - lastAnim < 20) return;
  lastAnim = millis();
  pulse += 2 + spd / 6;
  if (pulse >= 512) pulse -= 512;
  uint8_t wave = pulse < 256 ? pulse : 511 - pulse;
  fill_solid(leds, NUM_LEDS, CRGB(cr, cg, cb));
  renderBri = (uint16_t)bri * (40 + (uint16_t)wave * 215 / 255) / 255;
  showLeds();
}

void renderRainbow() {
  if (lastAnim != 0 && millis() - lastAnim < frameMs()) return;
  lastAnim = millis();
  CHSV hsv = rgb2hsv_approximate(CRGB(cr, cg, cb));
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CHSV(hsv.h + pos + i * 8, 255, 255);
  }
  pos += 1 + spd / 24;
  renderBri = bri;
  showLeds();
}

void renderDuo() {
  if (lastAnim != 0 && millis() - lastAnim < frameMs()) return;
  lastAnim = millis();
  pos++;
  CRGB c1(cr, cg, cb);
  CRGB c2(cr2, cg2, cb2);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t at = (i + pos) % NUM_LEDS;
    if (fx == FX_SPLIT) {
      leds[i] = at < NUM_LEDS / 2 ? c1 : c2;
    } else if (fx == FX_ALT) {
      leds[i] = (at & 1) ? c2 : c1;
    } else {
      uint8_t along = (uint16_t)at * 255 / (NUM_LEDS - 1);
      uint8_t amount = along < 128 ? along * 2 : (255 - along) * 2;
      leds[i] = blend(c1, c2, amount);
    }
  }
  renderBri = bri;
  showLeds();
}

void renderSwap() {
  uint16_t period = 30 + (uint16_t)(255 - spd) * 3;
  if (lastAnim != 0 && millis() - lastAnim < period) return;
  lastAnim = millis();
  strobeOn = !strobeOn;
  fill_solid(leds, NUM_LEDS, strobeOn ? CRGB(cr, cg, cb) : CRGB(cr2, cg2, cb2));
  renderBri = bri;
  showLeds();
}

void render() {
  switch (fx) {
    case FX_SPIN:    renderRun(48, 1); break;
    case FX_CHASE:   renderRun(70, 4); break;
    case FX_STROBE:  renderStrobe(); break;
    case FX_PULSE:   renderPulse(); break;
    case FX_RAINBOW: renderRainbow(); break;
    case FX_SPLIT:
    case FX_ALT:
    case FX_GRAD:    renderDuo(); break;
    case FX_SWAP:    renderSwap(); break;
    default:         renderSolid(); break;
  }
}

void advanceSequence() {
  if (playIndex < 0 || seqCount == 0) return;
  uint16_t dur = seq[playIndex].dur;
  if (dur == 0) return;
  if (millis() - stepStart < (unsigned long)dur * 1000UL) return;
  playIndex++;
  if (playIndex >= seqCount) playIndex = 0;
  stepStart = millis();
  applyStep(seq[playIndex]);
}

void setup() {
  Serial.begin(115200);

  pinMode(DATA_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  digitalWrite(DATA_PIN, LOW);
  digitalWrite(CLOCK_PIN, LOW);
  dataOut = portOutputRegister(digitalPinToPort(DATA_PIN));
  clockOut = portOutputRegister(digitalPinToPort(CLOCK_PIN));
  dataMask = digitalPinToBitMask(DATA_PIN);
  clockMask = digitalPinToBitMask(CLOCK_PIN);

  fill_solid(leds, NUM_LEDS, CRGB::Black);
  showLeds();

  if (loadSequence()) {
    playSequence();
  } else {
    intro = true;
    applyLook(FX_SPIN, 255, 0, 0, 255, 100, 0, 255, 170);
  }
}

void loop() {
  pollSerial();
  if (!intro) advanceSequence();
  render();
}
