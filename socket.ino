void logMsg(const String &msg) {
    // Get current time
    time_t nowEpoch = time(nullptr);
    struct tm *t = localtime(&nowEpoch);

    // Format timestamp as HH:MM:SS
    char timeBuf[20];
    sprintf(timeBuf, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);

    // Create log message with timestamp
    String logWithTime = String(timeBuf) + " - " + msg;

    // Print to serial
    Serial.println(logWithTime);

    // Send via WebSocket to all clients
    ws.textAll(logWithTime);
}
