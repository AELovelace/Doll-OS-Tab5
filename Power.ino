//   Power.ino
//   Tab5 low-power compatibility path. The main
//   board keeps RAM and execution state in light sleep; the slave performs a full
//   deep-sleep reset and wakes this board by driving the keyboard UART RX line low.

static bool systemLightSleepActive = false;

//   Internal-I2C arbitration (contract and rationale in global.h). Lives here
//   rather than in Radio.ino because the bus is board hardware, not an audio
//   detail: the codec is only one of its four owners.
//
//   Created once from setup() instead of lazily on first use. Every bus user runs
//   on either loop() or radioTask, and radioTask is not created until the shell
//   runs "radio", so setup() is comfortably ahead of the first contended access --
//   which avoids both a lazy-init race and a FreeRTOS call during static
//   construction. A NULL handle (allocation failed) degrades to the old
//   unsynchronized behaviour rather than disabling audio outright.
static SemaphoreHandle_t boardI2cMutex = NULL;

void boardI2cBegin() {
    if (boardI2cMutex == NULL) {
        boardI2cMutex = xSemaphoreCreateRecursiveMutex();
        if (boardI2cMutex == NULL) {
            Serial.println("[i2c] could not create internal-bus mutex; access stays unsynchronized");
        }
    }
}

bool boardI2cLock(uint32_t timeoutMs) {
    if (boardI2cMutex == NULL) {
        return true;
    }
    return xSemaphoreTakeRecursive(boardI2cMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void boardI2cUnlock() {
    if (boardI2cMutex != NULL) {
        xSemaphoreGiveRecursive(boardI2cMutex);
    }
}

void enterSystemLightSleep() {
    if (systemLightSleepActive) {
        return;                                    // Ignore duplicate protocol bytes during transition.
    }
    systemLightSleepActive = true;

    const gpio_num_t wakePin = gpio_num_t(TAB5_KEYBOARD_INTERRUPT_PIN);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);  // Give this manual sleep one wake owner.
    const esp_err_t pinResult = gpio_wakeup_enable(wakePin, GPIO_INTR_LOW_LEVEL);
    const esp_err_t sourceResult = pinResult == ESP_OK
        ? esp_sleep_enable_gpio_wakeup()
        : pinResult;
    if (sourceResult != ESP_OK) {
        Serial.printf("[sleep] GPIO%d wake setup failed: %d\n",
                      TAB5_KEYBOARD_INTERRUPT_PIN, int(sourceResult));
        gpio_wakeup_disable(wakePin);
        systemLightSleepActive = false;
        return;
    }

    Serial.println("[sleep] slave requested paired low-power mode");
    Serial.flush();
    const bool resumeWifi = wifiStationIsReady(); // Remember whether this boot ever initialized the network stack.
    stopTelnetServer();                            // Close the listener before intentionally stopping Wi-Fi.
    if (resumeWifi) {
        WiFi.disconnect(false, false);             // Preserve saved credentials while releasing the radio.
        WiFi.mode(WIFI_OFF);                       // Manual light sleep cannot retain the Wi-Fi association.
    }
    ledSetWifiConnected(false);

    ledPrepareForSleep();                          // Darken the rear RGB status indicator.
    displaySetSleeping(true);                      // Sleep the TFT controller and switch off its backlight.

    const esp_err_t sleepResult = esp_light_sleep_start();  // Returns here with all RAM and tasks preserved.

    gpio_wakeup_disable(wakePin);                  // Stop ordinary keyboard traffic becoming a wake source.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    displaySetSleeping(false);                     // Restore the preserved frame before network reconnect work.
    if (resumeWifi) {
        WiFi.mode(WIFI_STA);                       // Restart STA without blocking the newly restored screen.
        WiFi.setAutoReconnect(false);
        WiFi.reconnect();                          // Existing maintenance logic handles a slow association.
        startTelnetServer();                       // Rebind only after lwIP is live again.
    }
    ledService();                                  // Restore the logical rear-LED indication immediately.

    Serial.printf("[sleep] main unit awake (cause=%d, result=%d)\n",
                  int(esp_sleep_get_wakeup_cause()), int(sleepResult));
    systemLightSleepActive = false;
}
