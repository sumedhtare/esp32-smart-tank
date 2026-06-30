//void applyNeoPixelColor(uint32_t colorHex, uint8_t brightness) {
//  neoPixel.setBrightness(brightness);
//  neoPixel.fill(neoPixel.Color(
//    (colorHex >> 16) & 0xFF,
//    (colorHex >> 8) & 0xFF,
//    colorHex & 0xFF
//  ));
//  neoPixel.show();
//}

struct NeoFade {
  bool active = false;
  uint32_t startColor = 0;
  uint32_t targetColor = 0;
  uint8_t startBrightness = 0;
  uint8_t targetBrightness = 0;
  unsigned long startTime = 0;
  unsigned long duration = 1000; // fade duration in ms
};

NeoFade neoFade[NEO_STRIP_COUNT];

void applyNeoPixelColor(uint8_t idx, uint32_t color, uint8_t brightness, unsigned long fadeDuration = 10000) {
  if (idx >= NEO_STRIP_COUNT) return;
  Adafruit_NeoPixel &strip = *neoStrips[idx];
  lastNeoColor[idx] = color;          // store last color for this strip
  neoFade[idx].startColor = strip.getPixelColor(0);
  neoFade[idx].targetColor = color;
  neoFade[idx].startBrightness = strip.getBrightness();
  neoFade[idx].targetBrightness = brightness;
  neoFade[idx].startTime = millis();
  neoFade[idx].duration = fadeDuration;
  neoFade[idx].active = true;
}

void updateNeoPixelFade() {
  for (uint8_t idx = 0; idx < NEO_STRIP_COUNT; idx++) {
    if (!neoFade[idx].active) continue;
    Adafruit_NeoPixel &strip = *neoStrips[idx];

    unsigned long now = millis();
    float t = (float)(now - neoFade[idx].startTime) / neoFade[idx].duration;
    if (t >= 1.0) t = 1.0; // clamp
    uint8_t sr = (neoFade[idx].startColor >> 16) & 0xFF;
    uint8_t sg = (neoFade[idx].startColor >> 8) & 0xFF;
    uint8_t sb = neoFade[idx].startColor & 0xFF;

    uint8_t tr = (neoFade[idx].targetColor >> 16) & 0xFF;
    uint8_t tg = (neoFade[idx].targetColor >> 8) & 0xFF;
    uint8_t tb = neoFade[idx].targetColor & 0xFF;

    uint8_t r = sr + t * (tr - sr);
    uint8_t g = sg + t * (tg - sg);
    uint8_t b = sb + t * (tb - sb);
    uint8_t bri = neoFade[idx].startBrightness + t * (neoFade[idx].targetBrightness - neoFade[idx].startBrightness);

    strip.setBrightness(bri);
    strip.fill(strip.Color(r, g, b));
    strip.show();

    if (t >= 1.0) neoFade[idx].active = false; // fade complete
  }
}
