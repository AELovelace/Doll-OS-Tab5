//   Power.ino
//   Paired low-power mode initiated by DS-Slave's rotary Settings menu. The main
//   board keeps RAM and execution state in light sleep; the slave performs a full
//   deep-sleep reset and wakes this board by driving the keyboard UART RX line low.

static bool systemLightSleepActive = false;

void enterSystemLightSleep() {
    if (systemLightSleepActive) {
        return;                                    // Ignore duplicate protocol bytes during transition.
    }
    systemLightSleepActive = true;

    const gpio_num_t wakePin = gpio_num_t(KEYBOARD_SERIAL_RX_PIN);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);  // Give this manual sleep one wake owner.
    const esp_err_t pinResult = gpio_wakeup_enable(wakePin, GPIO_INTR_LOW_LEVEL);
    const esp_err_t sourceResult = pinResult == ESP_OK
        ? esp_sleep_enable_gpio_wakeup()
        : pinResult;
    if (sourceResult != ESP_OK) {
        Serial.printf("[sleep] GPIO%d wake setup failed: %d\n",
                      KEYBOARD_SERIAL_RX_PIN, int(sourceResult));
        gpio_wakeup_disable(wakePin);
        systemLightSleepActive = false;
        return;
    }

    Serial.println("[sleep] slave requested paired low-power mode");
    Serial.flush();
    telnetClient.stop();                           // Close the socket before intentionally stopping Wi-Fi.
    ledSetTelnetConnected(false);
    WiFi.disconnect(false, false);                 // Preserve saved credentials while releasing the radio.
    WiFi.mode(WIFI_OFF);                           // Manual light sleep cannot retain the Wi-Fi association.
    ledSetWifiConnected(false);

    const int ampWasEnabled = digitalRead(AUDIO_AMP_ENABLE_PIN);
    pinMode(AUDIO_AMP_ENABLE_PIN, OUTPUT);
    digitalWrite(AUDIO_AMP_ENABLE_PIN, LOW);       // Mute the speaker amplifier throughout light sleep.
    ledPrepareForSleep();                          // Darken the rear RGB status indicator.
    displaySetSleeping(true);                      // Sleep the TFT controller and switch off its backlight.

    const esp_err_t sleepResult = esp_light_sleep_start();  // Returns here with all RAM and tasks preserved.

    gpio_wakeup_disable(wakePin);                  // Stop ordinary keyboard traffic becoming a wake source.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    displaySetSleeping(false);                     // Restore the preserved frame before network reconnect work.
    digitalWrite(AUDIO_AMP_ENABLE_PIN, ampWasEnabled ? HIGH : LOW);

    WiFi.mode(WIFI_STA);                           // Restart STA without blocking the newly restored screen.
    WiFi.setAutoReconnect(false);
    WiFi.reconnect();                              // Existing maintenance logic handles a slow association.
    telnetServer.begin();                          // Rebind the listener after the network interface restart.
    telnetServer.setNoDelay(true);
    ledService();                                  // Restore the logical rear-LED indication immediately.

    Serial.printf("[sleep] main unit awake (cause=%d, result=%d)\n",
                  int(esp_sleep_get_wakeup_cause()), int(sleepResult));
    systemLightSleepActive = false;
}
