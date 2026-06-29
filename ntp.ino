void setupNTP() {
  // IST offset: 5h30m -> seconds offset
  configTime(5 * 3600 + 30 * 60, 0, NTP_POOL);
  Serial.println("Waiting for NTP time sync...");
  int retries = 0;
  while (time(nullptr) < 100000 && retries < 15) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  Serial.println();
  time_t now = time(nullptr);
  if (now < 100000) Serial.println("NTP sync failed or slow; schedules will use device time.");
  else Serial.println("NTP synced.");
}

int minuteOfDayNow() {
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  return t->tm_hour * 60 + t->tm_min;
}
