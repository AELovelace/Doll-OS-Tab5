//   SysInfo.ino
//   heap instrumentation ("free" command) and battery voltage ("battery" command).
//   Ported from DOLL-OS's free.ino; battery reporting is new here -- DOLL-OS read
//   M5Cardputer.Power directly (statusManagement() in terminal.ino, plus motoko's
//   "/battery" subcommand), which doesn't exist on this board. This board does
//   have real battery-monitoring hardware though (a charge IC + a divided ADC pin,
//   confirmed from Freenove's own Battery_Voltage example for this board family),
//   so unlike the M5-specific stubs this is a real reading, not a placeholder.
#include <esp_heap_caps.h>
#include <esp_core_dump.h>
#include <esp_system.h>
#include <time.h>

//   Network time (NTP)
//   Shared by Dapper (HTTPS certificate date validation needs a plausible clock before
//   trusting a TLS session) and the .dapp TIME opcode (AppRunner.ino) -- both just need
//   "is the clock synced" and "block briefly trying to sync it," differing only in what
//   they call to stay responsive while waiting, so that's a callback parameter rather
//   than two copies of the same poll loop.

//1700000000 ~= 2023-11-14; anything before that means the clock is still at its
//post-boot default (Jan 1 1970) and has never been NTP-synced.
bool ntpClockReady() {
    return time(nullptr) >= 1700000000;
}

//blocks up to timeoutMs trying to NTP-sync the clock, calling yieldFn periodically so
//the caller's own UI/watchdog stays serviced during the wait. Returns false (with
//`error` set) if the clock still isn't plausible once the timeout expires.
bool ntpEnsureClock(String& error, unsigned long timeoutMs, void (*yieldFn)()) {
    if (ntpClockReady()) return true;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    unsigned long started = millis();
    while (!ntpClockReady() && millis() - started < timeoutMs) {
        yieldFn();
    }
    if (!ntpClockReady()) {
        error = "clock synchronization failed; NTP time is not available";
        return false;
    }
    return true;
}

//   PSRAM allocation helpers
//   This board's ESP32-S3 module carries external PSRAM. Internal SRAM is the scarce
//   pool -- WiFi (AP+STA here) and mbedTLS (ssh) both want room there -- so large,
//   long-lived buffers are pushed out to PSRAM to leave internal RAM free. TFT_eSPI
//   already does this for the ~150KB frame sprite on its own (callocSprite prefers
//   PSRAM, see Extensions/Sprite.cpp); these helpers do the same for our own buffers.
//   All of it is contingent on PSRAM being enabled in the Arduino board menu -- with it
//   off, psramFound() is false, every allocation below falls back to internal RAM, and
//   reportPsramStatus() says so loudly at boot.

//tries PSRAM first, falls back to internal RAM, and logs which pool actually served the
//request. Zero-initialized like calloc. `tag` names the buffer in the boot log.
void* psramOrInternalCalloc(size_t count, size_t size, const char* tag) {
    void* p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM);
    if (p != nullptr) {
        Serial.printf("[psram] %s: %u bytes -> PSRAM\n", tag, (unsigned)(count * size));
        return p;
    }
    p = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    Serial.printf("[psram] %s: %u bytes -> INTERNAL RAM (PSRAM unavailable)\n",
                  tag, (unsigned)(count * size));
    return p;
}

//   Routes the *general* heap to PSRAM. After this call, ordinary malloc/new/String
//   allocations of at least PSRAM_MALLOC_EXTMEM_LIMIT bytes are served from PSRAM
//   instead of internal SRAM -- so everything that doesn't specifically demand internal
//   or DMA-capable memory drifts out of the scarce pool on its own, no per-buffer edits
//   needed. WiFi/SPI/DMA paths ask for their memory with explicit capability flags, so
//   they still get internal RAM and are unaffected. Allocations below the limit stay
//   internal so tiny, frequent ones don't pay PSRAM's slower access. Lower the limit to
//   push more out (at some speed cost); raise it to keep more in fast internal RAM.
static const size_t PSRAM_MALLOC_EXTMEM_LIMIT = 512;

void enablePsramHeap() {
    if (!psramFound()) {
        Serial.println("[psram] general heap stays on internal RAM (no PSRAM present)");
        return;
    }
    heap_caps_malloc_extmem_enable(PSRAM_MALLOC_EXTMEM_LIMIT);
    Serial.printf("[psram] general heap: allocations >= %u bytes now land in PSRAM\n",
                  (unsigned)PSRAM_MALLOC_EXTMEM_LIMIT);
}

//boot-time visibility into where memory is going. Prints PSRAM presence/size and both
//heap pools so a glance at the serial log confirms the big buffers left internal SRAM.
void reportPsramStatus() {
    if (psramFound()) {
        Serial.printf("[psram] enabled: total=%u free=%u\n",
                      (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    } else {
        Serial.println("[psram] NOT available -- enable PSRAM in the Arduino board menu, "
                       "or the ~150KB frame sprite + history stay in internal SRAM");
    }
    Serial.printf("[psram] heap: internal free=%u, spiram free=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
//   Crash postmortem
//   ESP-IDF writes an ELF core dump to the `coredump` partition (partitions.csv) on every
//   panic -- this core builds with CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH + _DATA_FORMAT_ELF,
//   so it happens with no opt-in from us. Reading it back on-device matters because the
//   panic *text* is easy to lose on this board: the IDF console is UART0
//   (CONFIG_ESP_CONSOLE_UART_NUM=0) with USB-Serial-JTAG only as the secondary, and
//   CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=0 resets immediately, so over a USB-only
//   cable the backtrace is usually gone before a monitor can catch it. The dump survives
//   the reset, so we read the summary out of flash instead of watching for the live print.
//
//   The dump is NOT erased after reporting: it persists until "crash clear", so a crash
//   can still be read minutes later over telnet rather than only in the boot window.
//   "crash clear" prints the summary to serial on its way out, so erasing never silently
//   destroys the only copy.
//   Addresses are raw PCs -- resolve them against the build's .elf with:
//     xtensa-esp32s3-elf-addr2line -pfiaC -e .arduino-build/<profile>/DS.ino.elf <pc> ...

static const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external reset pin";
        case ESP_RST_SW:        return "software (esp_restart)";
        case ESP_RST_PANIC:     return "PANIC / unhandled exception";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "TASK WATCHDOG";
        case ESP_RST_WDT:       return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (supply dipped)";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "unknown";
    }
}

//Xtensa EXCCAUSE values, limited to the ones that actually show up in practice.
//LoadProhibited/StoreProhibited are the null- or garbage-pointer dereferences;
//LoadStoreError is the classic "PSRAM/DMA address used where it isn't valid".
static const char* xtensaExcCauseName(uint32_t cause) {
    switch (cause) {
        case 0:  return "IllegalInstruction";
        case 2:  return "InstructionFetchError";
        case 3:  return "LoadStoreError";
        case 5:  return "Alloca";
        case 6:  return "IntegerDivideByZero";
        case 9:  return "LoadStoreAlignment";
        case 13: return "LoadStorePIFDataError";
        case 15: return "LoadStorePIFAddrError";
        case 20: return "InstrFetchProhibited";
        case 28: return "LoadProhibited";
        case 29: return "StoreProhibited";
        default: return "other";
    }
}

//EXCVADDR only holds a meaningful address for the fetch/load/store causes. For anything
//else (IllegalInstruction, divide-by-zero, an abort()) the register is whatever was left
//in it, and printing it invites chasing a "null deref" that never happened.
static bool xtensaExcVaddrValid(uint32_t cause) {
    switch (cause) {
        case 2: case 3: case 9: case 13: case 15: case 20: case 28: case 29:
            return true;
        default:
            return false;
    }
}

//emit is outLine (shell) or crashEmitSerial (boot) -- same report either way, so a crash
//can be read over telnet long after the boot log has scrolled past
static void crashEmitSerial(const String& text, int color) {
    (void)color;
    Serial.println(text);
}

void reportLastCrash(void (*emit)(const String&, int)) {
    esp_reset_reason_t reason = esp_reset_reason();
    emit("LAST RESET: " + String(resetReasonName(reason)), C_CYAN);

    //a clean boot still checks the partition: a dump from an *earlier* crash is
    //retained deliberately, and silently skipping it would hide it forever
    if (esp_core_dump_image_check() != ESP_OK) {
        emit("  no core dump stored", C_CYAN);
        return;
    }

    esp_core_dump_summary_t summary;
    if (esp_core_dump_get_summary(&summary) != ESP_OK) {
        emit("  core dump present but unreadable", C_RED);
        return;
    }

    emit("CORE DUMP:", C_RED);
    emit("  task: " + String(summary.exc_task), C_RED);
    emit("  PC:   0x" + String(summary.exc_pc, HEX), C_RED);
    emit("  cause: " + String(summary.ex_info.exc_cause) + " ("
         + String(xtensaExcCauseName(summary.ex_info.exc_cause)) + ")", C_RED);
    if (xtensaExcVaddrValid(summary.ex_info.exc_cause)) {
        emit("  faulting addr: 0x" + String(summary.ex_info.exc_vaddr, HEX), C_RED);
    } else {
        emit("  faulting addr: n/a for this cause", C_RED);
    }

    String backtrace = "  backtrace:";
    for (uint32_t i = 0; i < summary.exc_bt_info.depth && i < 16; i++) {
        backtrace += " 0x" + String(summary.exc_bt_info.bt[i], HEX);
    }
    if (summary.exc_bt_info.corrupted) {
        backtrace += " (CORRUPTED)";
    }
    emit(backtrace, C_RED);
    emit("  resolve with addr2line -e <build>/DS.ino.elf", C_CYAN);
}

//no-arg wrapper so DS.ino can call this the way it calls reportPsramStatus() --
//Arduino auto-generates prototypes, and a plain void() is the reliable shape
void reportLastCrashSerial() {
    reportLastCrash(crashEmitSerial);
}

void handleCrashCommand(const String parts[], int partCount) {
    if (partCount > 1 && parts[1] == "clear") {
        //dump to serial before erasing -- this is the last moment the data exists, and a
        //copy in the serial scrollback beats losing a crash to a mistyped command. Printed
        //even when the caller is telnet: the serial log is the record that outlives the
        //session, which is the whole reason the dump was worth keeping.
        Serial.println("[crash] erasing stored core dump -- final copy follows");
        reportLastCrash(crashEmitSerial);
        Serial.flush();
        if (esp_core_dump_image_erase() == ESP_OK) {
            outLine("crash: core dump erased (copy printed to serial)", C_PINK);
        } else {
            outLine("crash: nothing to erase", C_RED);
        }
        return;
    }
    if (partCount > 1) {
        outLine("usage: crash [clear]");
        return;
    }
    reportLastCrash(outLine);
}

static bool batteryAdcInitialized = false;

static void ensureBatteryAdc() {
    if (batteryAdcInitialized) {
        return;
    }
    pinMode(BATTERY_ADC_PIN, INPUT);
    batteryAdcInitialized = true;
}

float readBatteryVoltage() {
    ensureBatteryAdc();
    return analogReadMilliVolts(BATTERY_ADC_PIN) * BATTERY_ADC_DIVIDER / 1000.0f;
}

//rough linear estimate between the configured empty/full voltage points --
//there's no fuel-gauge chip on this board, just a divided ADC pin, so this is an
//approximation the same way DOLL-OS's own M5Cardputer battery percent was
int readBatteryPercent() {
    float v = readBatteryVoltage();
    float pct = (v - BATTERY_VOLTAGE_EMPTY) / (BATTERY_VOLTAGE_FULL - BATTERY_VOLTAGE_EMPTY) * 100.0f;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (int)(pct + 0.5f);
}

void handleBatteryCommand(const String parts[], int partCount) {
    float v = readBatteryVoltage();
    int pct = readBatteryPercent();
    outLine("Battery: " + String(pct) + "% (" + String(v, 2) + "V)", C_CYAN);
}

static void showStringState(const char* label, const String& value) {
    outLine(String(label) + " len=" + String(value.length()), C_CYAN);
}

static void showHeapCapsSummary(const char* label, uint32_t caps) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);

    size_t freeHeap = heap_caps_get_free_size(caps);
    size_t largest = heap_caps_get_largest_free_block(caps);
    if (freeHeap == 0 && largest == 0 && info.total_allocated_bytes == 0) {
        return;
    }

    int fragPct = freeHeap == 0 ? 0 : 100 - (int)((largest * 100) / freeHeap);
    outLine(String(label)
        + " free=" + String(freeHeap)
        + " largest=" + String(largest)
        + " frag=" + String(fragPct) + "%"
        + " alloc=" + String(info.total_allocated_bytes), C_CYAN);
}

//   Every task stack is carved out of internal RAM, and the big ones here (ssh 40KB,
//   radio 12KB) are sized by guesswork against mbedtls/decoder worst cases. "unused" is
//   the high-water mark: stack bytes never touched since the task started, i.e. exactly
//   what could be handed back to the internal pool by lowering that task's stack size.
//   Read it after a real ssh session or a few minutes of playback, not at idle.
static void showTaskStacks() {
    UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    if (taskCount == 0) return;
    //snapshot buffer in PSRAM so measuring the internal pool doesn't disturb it
    TaskStatus_t* tasks = (TaskStatus_t*)heap_caps_calloc(
        taskCount, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    if (tasks == nullptr) return;

    taskCount = uxTaskGetSystemState(tasks, taskCount, NULL);
    outLine("TASK STACKS (unused bytes):", C_CYAN);
    for (UBaseType_t i = 0; i < taskCount; i++) {
        outLine(String(tasks[i].pcTaskName)
            + " unused=" + String((unsigned)(tasks[i].usStackHighWaterMark * sizeof(StackType_t))),
            C_CYAN);
    }
    heap_caps_free(tasks);
}

//the radio and ssh tasks record checkpoints too, so the ring needs a lock
static portMUX_TYPE heapCheckpointMux = portMUX_INITIALIZER_UNLOCKED;

void recordHeapCheckpoint(const char* tag) {
    //sampled before the critical section: the heap_caps_* calls take the heap's own
    //lock, which must not be nested inside this spinlock
    //MALLOC_CAP_INTERNAL, not 8BIT: with PSRAM in the heap, 8BIT includes it, and ~8MB of
    //PSRAM headroom swamps the number that actually matters -- the scarce internal pool
    //WiFi/TLS/DMA must allocate from
    uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    uint32_t minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    portENTER_CRITICAL(&heapCheckpointMux);
    int slot;
    if (heapCheckpointCount < HEAP_CHECKPOINT_MAX) {
        slot = (heapCheckpointHead + heapCheckpointCount) % HEAP_CHECKPOINT_MAX;
        heapCheckpointCount++;
    } else {
        slot = heapCheckpointHead;
        heapCheckpointHead = (heapCheckpointHead + 1) % HEAP_CHECKPOINT_MAX;
    }

    HeapCheckpoint& checkpoint = heapCheckpoints[slot];
    checkpoint.tag = tag;
    checkpoint.freeHeap = freeHeap;
    checkpoint.largestBlock = largestBlock;
    checkpoint.minFreeHeap = minFreeHeap;
    portEXIT_CRITICAL(&heapCheckpointMux);
}

void reserveHotStrings() {
    currentCommand.reserve(COMMAND_MAX_LEN);
    commandHistoryDraft.reserve(128);
    cwd.reserve(64);
    sshInputBuffer.reserve(128);
    motokoChannel.reserve(64);
    motokoInputBuffer.reserve(256);

    for (int i = 0; i < COMMAND_HISTORY_MAX; i++) {
        commandHistory[i].reserve(64);
    }
}

void handleFreeCommand(const String parts[], int partCount) {
    if (partCount == 1) {
        showFree();
    } else if (parts[1] == "details") {
        showFreeDetailed();
    } else {
        showFreeHelp();
    }
}

void showFree() {
    outLine("TOTAL FREE: " + String(ESP.getFreeHeap()), C_RED);
    outLine("MAXALLOCHEAP: " + String(ESP.getMaxAllocHeap()), C_RED);
    outLine("MINALLOCHEAP: " + String(ESP.getMinFreeHeap()), C_RED);
}

void showFreeDetailed() {
    //headline numbers are INTERNAL only (same reasoning as recordHeapCheckpoint) --
    //the per-pool breakdown below still shows PSRAM via the caps summaries
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);

    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t minFree = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    int fragPct = freeHeap == 0 ? 0 : 100 - (int)((largest * 100) / freeHeap);

    outLine("FREE: " + String(freeHeap));
    outLine("LARGEST: " + String(largest));
    outLine("MIN FREE: " + String(minFree));
    outLine("FRAG: " + String(fragPct) + "%");
    outLine("ALLOC BYTES: " + String(info.total_allocated_bytes));
    outLine("FREE BLOCKS: " + String(info.free_blocks));
    outLine("ALLOC BLOCKS: " + String(info.allocated_blocks));

    outLine("HEAP CAPS:", C_CYAN);
    showHeapCapsSummary("INTERNAL", MALLOC_CAP_INTERNAL);
    showHeapCapsSummary("DMA", MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    showHeapCapsSummary("8BIT", MALLOC_CAP_8BIT);
    showHeapCapsSummary("32BIT", MALLOC_CAP_32BIT);

#if defined(BOARD_HAS_PSRAM)
    showHeapCapsSummary("SPIRAM", MALLOC_CAP_SPIRAM);
#endif

    showTaskStacks();

    outLine("STRING STATE:", C_CYAN);
    showStringState("currentCommand", currentCommand);
    showStringState("commandDraft", commandHistoryDraft);
    showStringState("cwd", cwd);
    showStringState("sshInput", sshInputBuffer);
    showStringState("motokoChannel", motokoChannel);
    showStringState("motokoInput", motokoInputBuffer);
    outLine("commandHistory used=" + String(commandHistoryCount)
        + "/" + String(COMMAND_HISTORY_MAX), C_CYAN);

    if (heapCheckpointCount > 0) {
        outLine("HEAP CHECKPOINTS:", C_CYAN);
        for (int i = 0; i < heapCheckpointCount; i++) {
            const HeapCheckpoint& checkpoint = heapCheckpoints[(heapCheckpointHead + i) % HEAP_CHECKPOINT_MAX];
            outLine(
                String(checkpoint.tag)
                + " free=" + String(checkpoint.freeHeap)
                + " largest=" + String(checkpoint.largestBlock)
                + " min=" + String(checkpoint.minFreeHeap),
                C_CYAN
            );
        }
    }
}

void showFreeHelp() {
    outLine("free: Shows free RAM", C_CYAN);
    outLine("free details: show details", C_CYAN);
}
