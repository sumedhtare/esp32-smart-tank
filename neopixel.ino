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

NeoFade neoFade;

void applyNeoPixelColor(uint32_t color, uint8_t brightness, unsigned long fadeDuration = 10000) {
  lastNeoColor = color;           // store last color
  neoFade.startColor = neoPixel.getPixelColor(0);
  neoFade.targetColor = color;
  neoFade.startBrightness = neoPixel.getBrightness();
  neoFade.targetBrightness = brightness;
  neoFade.startTime = millis();
  neoFade.duration = fadeDuration;
  neoFade.active = true;
}

void updateNeoPixelFade() {
  if (!neoFade.active) return;

  unsigned long now = millis();
  float t = (float)(now - neoFade.startTime) / neoFade.duration;
  if (t >= 1.0) t = 1.0; // clamp
  uint8_t sr = (neoFade.startColor >> 16) & 0xFF;
  uint8_t sg = (neoFade.startColor >> 8) & 0xFF;
  uint8_t sb = neoFade.startColor & 0xFF;

  uint8_t tr = (neoFade.targetColor >> 16) & 0xFF;
  uint8_t tg = (neoFade.targetColor >> 8) & 0xFF;
  uint8_t tb = neoFade.targetColor & 0xFF;

  uint8_t r = sr + t * (tr - sr);
  uint8_t g = sg + t * (tg - sg);
  uint8_t b = sb + t * (tb - sb);
  uint8_t bri = neoFade.startBrightness + t * (neoFade.targetBrightness - neoFade.startBrightness);

  neoPixel.setBrightness(bri);
  neoPixel.fill(neoPixel.Color(r, g, b));
  neoPixel.show();

  if (t >= 1.0) neoFade.active = false; // fade complete
}
