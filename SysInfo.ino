//   SysInfo.ino
//   heap instrumentation ("free" command) and battery voltage ("battery" command).
//   Ported from DOLL-OS's free.ino; battery reporting is new here -- DOLL-OS read
//   M5Cardputer.Power directly (statusManagement() in terminal.ino, plus motoko's
//   "/battery" subcommand), which doesn't exist on this board. This board does
//   have real battery-monitoring hardware though (a charge IC + a divided ADC pin,
//   confirmed from Freenove's own Battery_Voltage example for this board family),
//   so unlike the M5-specific stubs this is a real reading, not a placeholder.
#include <esp_heap_caps.h>
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

float readBatteryVoltage() {
    return M5.Power.getBatteryVoltage() / 1000.0f;
}

//rough linear estimate between the configured empty/full voltage points --
//there's no fuel-gauge chip on this board, just a divided ADC pin, so this is an
//approximation the same way DOLL-OS's own M5Cardputer battery percent was
int readBatteryPercent() {
    const int level = M5.Power.getBatteryLevel();
    return level < 0 ? 0 : min(level, 100);
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

void recordHeapCheckpoint(const char* tag) {
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
    //MALLOC_CAP_INTERNAL, not 8BIT: with PSRAM in the heap, 8BIT includes it, and ~8MB of
    //PSRAM headroom swamps the number that actually matters -- the scarce internal pool
    //WiFi/TLS/DMA must allocate from
    checkpoint.freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    checkpoint.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    checkpoint.minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
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
