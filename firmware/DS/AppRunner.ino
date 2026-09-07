//   AppRunner.ino
//   tiny executable script runtime for DOLL-OS. Apps are plain text .dapp files stored
//   in /apps on LittleFS or /sd/apps on the FTP-served SD card, then launched from
//   the shell with "run". The format is intentionally small: a few display/shell
//   commands, numeric variables, labels, and jumps.
#include <LittleFS.h>
#include <FS.h>
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <new>
#include <time.h>
//EXPR hands whole arithmetic expressions to the same evaluator the "calc" shell command
//uses (Calc.ino). The script language deliberately has no expression grammar of its own --
//there is no reason to grow a second, worse one when this is already linked in.
#include "tinyexpr.h"

//   Where a running app's memory lives
//   The whole script is read into RAM before the first line executes, so these caps are
//   really a memory budget. They used to be far smaller (160 lines / 32 labels / 16 vars)
//   because the arrays were plain stack locals in handleRunCommand and appExecute -- a
//   String is only a 12-byte header, but 160 of them plus labels already spent ~2.5KB of
//   the loopTask's 8KB stack, and the interpreter still has to run string expansion and
//   display work underneath. Raising the constants alone would have overflowed the stack
//   rather than printing a limit error.
//   They now come from PSRAM instead (DappProgram::alloc below), which this board has 8MB
//   of, so the caps are set by what a text script plausibly needs rather than by the
//   stack. 4000 lines matches EDIT_MAX_LINES in Edit.ino -- the on-device editor could
//   already write files longer than the runner would accept.
const int DAPP_MAX_LINES = 4000;
//DappLabel is a String (16 bytes, SSO covers every label name in this codebase) plus an
//int lineIndex -- 20 bytes/entry, so this table costs labelCount*20 bytes of PSRAM
//regardless of how many labels a given script actually declares (fixed calloc in
//DappProgram::alloc). 512 entries is 10KB of the board's 8MB PSRAM -- tracker-music.dapp,
//the largest app so far, was already at 254/256 after some careful array-lookup rewrites
//to avoid hitting this cap; doubling it buys real headroom for basically free.
const int DAPP_MAX_LABELS = 512;
//DappVar/DappStringVar are 24/36 bytes each (String header(s) + value + used flag) --
//doubled from 32/16 since both tables together only cost ~5KB either way.
const int DAPP_MAX_VARS = 128;
const int DAPP_MAX_STRING_VARS = 64;
//per-string-variable cap, enforced on every SETSTR/APPEND rather than a fixed table size
//like the others here -- a script only pays for what it actually puts in a string, so this
//is a safety ceiling on one runaway variable, not a standing allocation. Left as-is: no
//app has come close to it, and it's the one knob here where "bigger" has a real per-script
//cost instead of being free headroom.
const int DAPP_MAX_STRING_LEN = 4096;

//   DIM'd numeric arrays. DappArray itself (28 bytes: name, a pointer into the pool below,
//   size, used flag) is just bookkeeping -- the actual cell data for every DIM'd array in a
//   script is carved out of the one shared pool below by a bump pointer, so DAPP_MAX_ARRAYS
//   only caps how many *separate* arrays a script can DIM, not how big any one of them can
//   be (that's DAPP_ARRAY_POOL_CELLS, shared across all of them). 32 arrays is still under
//   1KB of table; the pool is the real cost -- 16384 longs is 64KB of the board's 8MB PSRAM,
//   double the old 32KB, and a 10x20 Tetris well plus its piece tables spends under 500 of
//   them, so this is pure headroom for scripts wanting bigger or more numerous arrays.
const int DAPP_MAX_ARRAYS = 32;
const int DAPP_ARRAY_POOL_CELLS = 16384;

//GOSUB nesting. Deep enough for the routine-calls-routine shape real scripts have, shallow
//enough that a runaway recursion reports a clean error instead of eating PSRAM.
const int DAPP_MAX_CALL_DEPTH = 64;

//CANVAS bounds. The panel can't usefully render finer than this, and the cap keeps the
//cell buffer (2 bytes/cell) under 15KB at its largest.
const int DAPP_CANVAS_MAX_COLS = 120;
const int DAPP_CANVAS_MAX_ROWS = 60;

//runaway-loop backstop. Generous now that it isn't standing in for a memory limit, but a
//tight GOTO loop at this cap runs for minutes, and the interpreter only services the
//display/radio inside WAIT and INPUT -- so appExecute yields every DAPP_STEPS_PER_YIELD
//steps to keep the scheduler and watchdog fed through a long compute loop.
//
//The counter resets whenever the script blocks on purpose (a WAIT with a real duration, or
//an INPUT), because "runaway" means *not yielding*: a game loop that paces itself with
//WAIT 30 would otherwise hit the cap after a few minutes of legitimate play, while a tight
//GOTO loop -- the thing this actually guards against -- never resets and still trips.
const int DAPP_MAX_STEPS = 1000000;
const int DAPP_STEPS_PER_YIELD = 256;

static void appPollAbortChord();

//How often the bulk-work yield below is allowed to actually sleep. Paced by the clock
//rather than by a character or cell count, which is what the callers can cheaply count but
//has no relationship to how long the work took.
const unsigned long DAPP_YIELD_INTERVAL_MS = 20;
static unsigned long dappLastYieldMs = 0;

static void appRuntimeYield(bool serviceUi) {
    esp_task_wdt_reset();
    if (!serviceUi) {
        //Bulk-work callers -- text expansion, canvas fill, telnet frame building -- reach
        //here every 128 characters or 256 cells. The watchdog reset above is the entire
        //point of those calls. The unconditional delay(1) that used to follow cost a
        //millisecond per 128 characters, so an 800-cell FLIP spent ~3ms asleep before
        //drawing anything, and a long PRINT stalled proportionally to its length -- all of
        //it latency with no scheduling benefit, since it is esp_task_wdt_reset that feeds
        //the watchdog. A real yield still happens, just on a clock.
        unsigned long now = millis();
        if (now - dappLastYieldMs >= DAPP_YIELD_INTERVAL_MS) {
            dappLastYieldMs = now;
            delay(1);
        }
        return;
    }

    appPollAbortChord();
    ftpService();
    radioService();
    maintainInternetConnection();
    ledService();
    //Canvas apps build a frame through CLS/PUT and make it visible with FLIP.
    //Pushing here paints that half-built frame and reads as a screen flash.
    if (!dappCanvasActive) {
        drawDisplayFrame();
    }
    dappLastYieldMs = millis();
    delay(1);
}

static void appRuntimeYield() {
    appRuntimeYield(true);
}

//placement-new over one PSRAM block per array: heap_caps_calloc gives raw bytes, and
//these elements hold Strings that need their constructors run. Returns false with
//everything released if any block can't be had.
bool DappProgram::alloc() {
    void* lineMem = psramOrInternalCalloc(DAPP_MAX_LINES, sizeof(DappLine), "dappLines");
    void* labelMem = psramOrInternalCalloc(DAPP_MAX_LABELS, sizeof(DappLabel), "dappLabels");
    void* varMem = psramOrInternalCalloc(DAPP_MAX_VARS, sizeof(DappVar), "dappVars");
    void* stringVarMem = psramOrInternalCalloc(DAPP_MAX_STRING_VARS, sizeof(DappStringVar), "dappStringVars");
    void* arrayMem = psramOrInternalCalloc(DAPP_MAX_ARRAYS, sizeof(DappArray), "dappArrays");
    //the pool and the call stack are plain longs/ints -- calloc's zeroing is their whole
    //initialization, so unlike the four above they need no placement-new pass
    void* poolMem = psramOrInternalCalloc(DAPP_ARRAY_POOL_CELLS, sizeof(long), "dappArrayPool");
    void* callMem = psramOrInternalCalloc(DAPP_MAX_CALL_DEPTH, sizeof(int), "dappCallStack");

    if (!lineMem || !labelMem || !varMem || !stringVarMem || !arrayMem || !poolMem || !callMem) {
        heap_caps_free(lineMem);
        heap_caps_free(labelMem);
        heap_caps_free(varMem);
        heap_caps_free(stringVarMem);
        heap_caps_free(arrayMem);
        heap_caps_free(poolMem);
        heap_caps_free(callMem);
        return false;
    }

    lines = (DappLine*)lineMem;
    labels = (DappLabel*)labelMem;
    vars = (DappVar*)varMem;
    stringVars = (DappStringVar*)stringVarMem;
    arrays = (DappArray*)arrayMem;
    arrayPool = (long*)poolMem;
    callStack = (int*)callMem;

    for (int i = 0; i < DAPP_MAX_LINES; i++) {
        new (&lines[i]) DappLine();
        if ((i & 0x7f) == 0x7f) appRuntimeYield(false);
    }
    for (int i = 0; i < DAPP_MAX_LABELS; i++) new (&labels[i]) DappLabel();
    for (int i = 0; i < DAPP_MAX_VARS; i++) new (&vars[i]) DappVar();
    for (int i = 0; i < DAPP_MAX_STRING_VARS; i++) new (&stringVars[i]) DappStringVar();
    for (int i = 0; i < DAPP_MAX_ARRAYS; i++) new (&arrays[i]) DappArray();

    //the () above is value-initialization, so the plain members (`used`, `lineIndex`,
    //`value`) are zeroed and the Strings are properly constructed -- appEnsureVar can
    //trust `used == false` on a fresh program.
    return true;
}

DappProgram::~DappProgram() {
    if (lines) {
        for (int i = 0; i < DAPP_MAX_LINES; i++) {
            lines[i].~DappLine();
            if ((i & 0xff) == 0xff) appRuntimeYield(false);
        }
        heap_caps_free(lines);
    }
    if (labels) {
        for (int i = 0; i < DAPP_MAX_LABELS; i++) labels[i].~DappLabel();
        heap_caps_free(labels);
    }
    if (vars) {
        for (int i = 0; i < DAPP_MAX_VARS; i++) vars[i].~DappVar();
        heap_caps_free(vars);
    }
    if (stringVars) {
        for (int i = 0; i < DAPP_MAX_STRING_VARS; i++) stringVars[i].~DappStringVar();
        heap_caps_free(stringVars);
    }
    if (arrays) {
        for (int i = 0; i < DAPP_MAX_ARRAYS; i++) arrays[i].~DappArray();
        heap_caps_free(arrays);
    }
    //no per-array free: every DappArray::values points into arrayPool, which goes as one block
    if (arrayPool) {
        heap_caps_free(arrayPool);
    }
    if (callStack) {
        heap_caps_free(callStack);
    }
}

static bool endsWithIgnoreCase(String value, const String& suffix) {
    value.toLowerCase();
    String s = suffix;
    s.toLowerCase();
    return value.endsWith(s);
}

static String trimCopy(String value) {
    value.trim();
    return value;
}

static bool isAppCommentOrBlank(const String& rawLine) {
    String line = trimCopy(rawLine);
    return line.length() == 0 || line.startsWith("#") || line.startsWith("//");
}

static void appApplyMetadataDirective(const String& rawLine, DappProgram& program) {
    String line = trimCopy(rawLine);
    if (line.startsWith("#")) {
        line = trimCopy(line.substring(1));
    } else if (line.startsWith("//")) {
        line = trimCopy(line.substring(2));
    } else {
        return;
    }

    if (!line.startsWith("@")) return;
    int space = line.indexOf(' ');
    String field = space >= 0 ? line.substring(1, space) : line.substring(1);
    String value = space >= 0 ? trimCopy(line.substring(space + 1)) : "";
    field.toLowerCase();
    value.toLowerCase();

    if (field == "echo") {
        if (value == "off") program.echoInput = false;
        else if (value == "on") program.echoInput = true;
    }
}

static int appColorByName(String name) {
    name.trim();
    name.toLowerCase();
    if (name == "black") return C_BLACK;
    if (name == "red") return C_RED;
    if (name == "green") return C_GREEN;
    if (name == "yellow") return C_YELLOW;
    if (name == "blue") return C_BLUE;
    if (name == "magenta") return C_MAGENTA;
    if (name == "cyan") return C_CYAN;
    if (name == "pink") return C_PINK;
    return C_WHITE;
}

static bool appOpenResolvedFile(const String& resolved, File& outFile) {
    RoutedPath r = routePath(resolved);
    if (r.isSd && !sdCardMounted) {
        return false;
    }

    ledPulseStorageRead(r.isSd);
    File f = r.fs->open(r.realPath, "r");
    if (!f || f.isDirectory()) {
        if (f) {
            f.close();
        }
        return false;
    }

    outFile = f;
    return true;
}

static bool appOpenCandidate(const String& candidate, File& outFile, String& resolvedOut) {
    String resolved = resolvePath(cwd, candidate);
    if (appOpenResolvedFile(resolved, outFile)) {
        resolvedOut = resolved;
        return true;
    }

    if (!endsWithIgnoreCase(resolved, ".dapp") && appOpenResolvedFile(resolved + ".dapp", outFile)) {
        resolvedOut = resolved + ".dapp";
        return true;
    }

    return false;
}

static bool appOpenByName(const String& target, File& outFile, String& resolvedOut) {
    if (target.indexOf('/') >= 0) {
        return appOpenCandidate(target, outFile, resolvedOut);
    }

    String name = target;
    if (!endsWithIgnoreCase(name, ".dapp")) {
        name += ".dapp";
    }

    const String candidates[] = {
        "/sd/apps/" + name,
        "/apps/" + name,
        "/system/apps/" + name,
        target,
    };

    for (int i = 0; i < 4; i++) {
        if (appOpenCandidate(candidates[i], outFile, resolvedOut)) {
            return true;
        }
    }
    return false;
}

static void ensureAppDirectories() {
    ledPulseStorageRead(false);
    File flashApps = LittleFS.open("/apps");
    if (!flashApps || !flashApps.isDirectory()) {
        if (flashApps) {
            flashApps.close();
        }
        ledPulseStorageWrite(false);
        LittleFS.mkdir("/apps");
    } else {
        flashApps.close();
    }

    if (sdCardMounted) {
        ledPulseStorageRead(true);
        File sdApps = SD_MMC.open("/apps");
        if (!sdApps || !sdApps.isDirectory()) {
            if (sdApps) {
                sdApps.close();
            }
            ledPulseStorageWrite(true);
            SD_MMC.mkdir("/apps");
        } else {
            sdApps.close();
        }
    }
}

static void listAppsInDir(fs::FS& fs, const String& realPath, const String& label) {
    bool isSd = label.startsWith("/sd/");
    ledPulseStorageRead(isSd);
    File dir = fs.open(realPath);
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            dir.close();
        }
        outLine(label + ": (missing)");
        return;
    }

    outLine(label + ":", C_CYAN);
    File entry = dir.openNextFile();
    int count = 0;
    while (entry) {
        ledPulseStorageRead(isSd);
        String name = entry.name();
        if (!entry.isDirectory() && endsWithIgnoreCase(name, ".dapp")) {
            outLine("  " + name + "  " + String(entry.size()) + "b");
            count++;
        }
        entry.close();
        entry = dir.openNextFile();
    }
    if (count == 0) {
        outLine("  (none)");
    }
    dir.close();
}

void handleAppsCommand(const String parts[], int partCount) {
    ensureAppDirectories();
    outLine("Apps: SD overrides, Dapper/user apps, then firmware fallbacks.", C_CYAN);
    outLine("/rom/apps:", C_CYAN);
    outLine("  wifi-manager  [native]");
    if (sdCardMounted) {
        listAppsInDir(SD_MMC, "/apps", "/sd/apps");
    } else {
        outLine("/sd/apps: SD not mounted", C_YELLOW);
    }
    listAppsInDir(LittleFS, "/apps", "/apps");
    listAppsInDir(LittleFS, "/system/apps", "/system/apps");
}

static int appFindLabel(const DappLabel labels[], int labelCount, String name) {
    name.trim();
    if (name.startsWith(":")) {
        name.remove(0, 1);
    }
    for (int i = 0; i < labelCount; i++) {
        if (labels[i].name == name) {
            return labels[i].lineIndex;
        }
    }
    return -1;
}

//   Slot tables are filled strictly front-to-back by the appEnsure* routines below and
//   nothing ever releases a slot, so the first unused entry is the end of the table. The
//   scans stop there rather than walking all DAPP_MAX_* entries: an app with four variables
//   was paying for sixty-four comparisons on every operand of every instruction.
static int appFindVar(DappVar vars[], const String& name) {
    for (int i = 0; i < DAPP_MAX_VARS; i++) {
        if (!vars[i].used) {
            break;
        }
        if (vars[i].name == name) {
            return i;
        }
    }
    return -1;
}

static int appEnsureVar(DappVar vars[], const String& name) {
    int existing = appFindVar(vars, name);
    if (existing >= 0) {
        return existing;
    }
    for (int i = 0; i < DAPP_MAX_VARS; i++) {
        if (!vars[i].used) {
            vars[i].used = true;
            vars[i].name = name;
            vars[i].value = 0;
            return i;
        }
    }
    return -1;
}

static int appFindStringVar(DappStringVar vars[], const String& name) {
    for (int i = 0; i < DAPP_MAX_STRING_VARS; i++) {
        if (!vars[i].used) {
            break;
        }
        if (vars[i].name == name) {
            return i;
        }
    }
    return -1;
}

static int appEnsureStringVar(DappStringVar vars[], const String& name) {
    int existing = appFindStringVar(vars, name);
    if (existing >= 0) {
        return existing;
    }
    for (int i = 0; i < DAPP_MAX_STRING_VARS; i++) {
        if (!vars[i].used) {
            vars[i].used = true;
            vars[i].name = name;
            vars[i].value = "";
            vars[i].value.reserve(80);
            return i;
        }
    }
    return -1;
}

//   The substring is only needed when the value actually exceeds the cap, which almost
//   never happens -- but building it unconditionally meant every string assignment, however
//   short, paid for an allocate-and-copy it then threw away.
static void appSetStringValue(DappStringVar vars[], int slot, const String& value) {
    if ((int)value.length() <= DAPP_MAX_STRING_LEN) {
        vars[slot].value = value;
        return;
    }
    vars[slot].value = value.substring(0, DAPP_MAX_STRING_LEN);
}

//   APPEND's path. Building `existing + addition` and handing that to appSetStringValue
//   copied the whole accumulated string twice per append, so a script growing a line in a
//   loop was quadratic with a factor of two on top. concat grows the existing buffer in
//   place instead, and only the overflow case -- where the result has to be cut back to the
//   cap anyway -- falls back to a copy.
static void appAppendStringValue(DappStringVar vars[], int slot, const String& addition) {
    if ((int)(vars[slot].value.length() + addition.length()) <= DAPP_MAX_STRING_LEN) {
        vars[slot].value.concat(addition);
        return;
    }
    String combined = vars[slot].value + addition;
    vars[slot].value = combined.substring(0, DAPP_MAX_STRING_LEN);
}

static bool appIsInteger(const String& token) {
    if (token.length() == 0) {
        return false;
    }
    int start = (token[0] == '-' || token[0] == '+') ? 1 : 0;
    if (start >= token.length()) {
        return false;
    }
    for (int i = start; i < token.length(); i++) {
        if (!isDigit(token[i])) {
            return false;
        }
    }
    return true;
}

static bool appIsNameChar(char ch) {
    return (ch >= 'a' && ch <= 'z')
        || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9')
        || ch == '_';
}

//   Arrays (DIM)
//
//   Reads and writes go through appArrayCell, which is the only place a bad index can be
//   caught -- and it has no way to return an error, since it is called from deep inside
//   value expansion. It sets program.fault instead; appExecute checks that after every
//   instruction and stops the app. A silently-zero out-of-range read is exactly the bug
//   that makes a 200-cell game board impossible to debug, so it is worth the plumbing.

static int appFindArray(DappProgram& program, const String& name) {
    for (int i = 0; i < DAPP_MAX_ARRAYS; i++) {
        if (!program.arrays[i].used) {
            break;
        }
        if (program.arrays[i].name == name) {
            return i;
        }
    }
    return -1;
}

static long* appArrayCell(DappProgram& program, const String& name, long index) {
    int slot = appFindArray(program, name);
    if (slot < 0) {
        program.fault = "no array named " + name + " (DIM it first)";
        return nullptr;
    }
    if (index < 0 || index >= program.arrays[slot].size) {
        program.fault = "index " + String(index) + " outside " + name +
                        "[0.." + String(program.arrays[slot].size - 1) + "]";
        return nullptr;
    }
    return &program.arrays[slot].values[index];
}

//carves `size` cells off the shared pool. Re-DIMming a name at its existing size just
//zeroes it -- that is what a script restarting a game wants, and it costs no new cells.
static bool appDimArray(DappProgram& program, const String& name, long size) {
    if (size <= 0) {
        outLine("run: DIM size must be greater than 0", C_RED);
        return false;
    }

    int existing = appFindArray(program, name);
    if (existing >= 0) {
        if (program.arrays[existing].size != (int)size) {
            outLine("run: " + name + " is already DIM'd at " + String(program.arrays[existing].size), C_RED);
            return false;
        }
        for (int i = 0; i < program.arrays[existing].size; i++) {
            program.arrays[existing].values[i] = 0;
            if ((i & 0xff) == 0xff) appRuntimeYield();
        }
        return true;
    }

    if (program.arrayPoolUsed + size > DAPP_ARRAY_POOL_CELLS) {
        outLine("run: out of array space (max " + String(DAPP_ARRAY_POOL_CELLS) + " cells total)", C_RED);
        return false;
    }

    for (int i = 0; i < DAPP_MAX_ARRAYS; i++) {
        if (program.arrays[i].used) {
            continue;
        }
        program.arrays[i].used = true;
        program.arrays[i].name = name;
        program.arrays[i].values = program.arrayPool + program.arrayPoolUsed;
        program.arrays[i].size = (int)size;
        for (int c = 0; c < (int)size; c++) {
            program.arrays[i].values[c] = 0;
            if ((c & 0xff) == 0xff) appRuntimeYield();
        }
        program.arrayPoolUsed += (int)size;
        return true;
    }

    outLine("run: too many arrays (max " + String(DAPP_MAX_ARRAYS) + ")", C_RED);
    return false;
}

static bool appLifeStep(DappProgram& program, String currentName, String nextName, int cols, int rows) {
    currentName.trim();
    nextName.trim();
    if (currentName.startsWith("$")) currentName.remove(0, 1);
    if (nextName.startsWith("$")) nextName.remove(0, 1);

    if (cols < 1 || rows < 1 || cols > DAPP_CANVAS_MAX_COLS || rows > DAPP_CANVAS_MAX_ROWS) {
        outLine("run: LIFE size must be 1.." + String(DAPP_CANVAS_MAX_COLS) + " by 1.." +
                String(DAPP_CANVAS_MAX_ROWS), C_RED);
        return false;
    }

    int currentSlot = appFindArray(program, currentName);
    int nextSlot = appFindArray(program, nextName);
    if (currentSlot < 0 || nextSlot < 0) {
        outLine("run: LIFE needs two DIM'd arrays", C_RED);
        return false;
    }

    int cells = cols * rows;
    DappArray& current = program.arrays[currentSlot];
    DappArray& next = program.arrays[nextSlot];
    if (current.size < cells || next.size < cells) {
        outLine("run: LIFE arrays must each have at least " + String(cells) + " cells", C_RED);
        return false;
    }

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            int neighbours = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int ny = y + dy;
                if (ny < 0 || ny >= rows) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx;
                    if (nx < 0 || nx >= cols) continue;
                    neighbours += current.values[ny * cols + nx] != 0 ? 1 : 0;
                }
            }

            bool alive = current.values[y * cols + x] != 0;
            next.values[y * cols + x] = (neighbours == 3 || (alive && neighbours == 2)) ? 1 : 0;
        }
        appRuntimeYield();
    }

    for (int i = 0; i < cells; i++) {
        current.values[i] = next.values[i];
        if ((i & 0xff) == 0xff) appRuntimeYield();
    }
    return true;
}

//splits "board[$i+1]" into "board" and "$i+1". Takes the first '[' and the last ']' so a
//nested subscript (board[$row[$i]]) stays intact for the recursive evaluation above.
static bool appParseSubscript(const String& token, String& nameOut, String& indexOut) {
    int open = token.indexOf('[');
    if (open <= 0 || !token.endsWith("]")) {
        return false;
    }
    nameOut = trimCopy(token.substring(0, open));
    indexOut = trimCopy(token.substring(open + 1, token.length() - 1));
    return nameOut.length() > 0 && indexOut.length() > 0;
}

//index just past the ']' that closes the '[' at openIdx, or -1 if it never closes. Used by
//the text expanders, which scan a name and then have to know where its subscript ends.
static int appScanSubscript(const String& text, int openIdx) {
    int depth = 0;
    for (int i = openIdx; i < (int)text.length(); i++) {
        if (((i - openIdx) & 0x7f) == 0x7f) appRuntimeYield(false);
        if (text[i] == '[') {
            depth++;
        } else if (text[i] == ']') {
            depth--;
            if (depth == 0) {
                return i + 1;
            }
        }
    }
    return -1;
}

//   Key codes returned by KEY. Deliberately small and printable-ASCII-compatible: anything
//   32..126 is the character itself, so a script can compare against "a" via its code or
//   just use these names for the rest.
const long DAPP_KEY_NONE  = 0;
const long DAPP_KEY_UP    = 1;
const long DAPP_KEY_DOWN  = 2;
const long DAPP_KEY_LEFT  = 3;
const long DAPP_KEY_RIGHT = 4;
const long DAPP_KEY_ENTER = 5;
const long DAPP_KEY_ESC   = 6;
const long DAPP_KEY_BACK  = 8;
const long DAPP_KEY_TAB   = 9;
const long DAPP_KEY_SPACE = 32;

//file-op status, declared up here because appBuiltinValue below reads them; the handle
//itself and the routines live in the Files section further down
static long dappFok = 0;    //result of the last FOPEN/FDELETE, read as $fok
static long dappFeof = 0;   //set by FREAD at end of file, read as $feof
static long dappHttpCode = 0;
static long dappHttpLength = 0;
static long dappHttpTruncated = 0;
static long dappHttpOk = 0;
static long dappJsonOk = 0;
//TIME's output -- an NTP sync (via ntpEnsureClock, SysInfo.ino) broken down with
//gmtime_r into the fields scripts actually want, since tinyexpr has no calendar math
//and making every app reimplement leap years to turn an epoch into a date is a bad time
static long dappTimeOk = 0;
static long dappTimeEpoch = 0;
static long dappTimeYear = 0;
static long dappTimeMonth = 0;
static long dappTimeDay = 0;
static long dappTimeHour = 0;
static long dappTimeMinute = 0;
static long dappTimeSecond = 0;
static long dappTimeWeekday = 0;   //0=Sunday..6=Saturday, matching struct tm

//   Buffer and renderer state. The blocks themselves live further down with their
//   routines, but these are read as $buflen/$htmllines/... by the two lookups below, so
//   the declarations have to sort above them.
static uint8_t* dappBuf = nullptr;
static size_t dappBufCap = 0;
static size_t dappBufLen = 0;
static long dappBufOk = 0;
static long dappHtmlOk = 0;
static long dappHtmlLines = 0;
static long dappHtmlLinks = 0;
static long dappHtmlBytes = 0;

static const int DAPP_HTTP_MAX_HEADERS = 8;
static String dappHttpHeaderNames[DAPP_HTTP_MAX_HEADERS];
static String dappHttpHeaderValues[DAPP_HTTP_MAX_HEADERS];
static int dappHttpHeaderCount = 0;

static long appBuiltinValue(String name) {
    name.trim();
    name.toLowerCase();
    if (name == "battery") return readBatteryPercent();
    if (name == "heap") return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (name == "millis") return millis();
    if (name == "seconds") return millis() / 1000;
    if (name == "wifi") return wifiIsConnected() == 1 ? 1 : 0;
    if (name == "fok") return dappFok;
    if (name == "feof") return dappFeof;
    if (name == "ledok") return rearLedAvailable() ? 1 : 0;
    if (name == "httpcode") return dappHttpCode;
    if (name == "httplen") return dappHttpLength;
    if (name == "httptruncated") return dappHttpTruncated;
    if (name == "httpok") return dappHttpOk;
    if (name == "jsonok") return dappJsonOk;
    if (name == "timeok") return dappTimeOk;
    if (name == "timeepoch") return dappTimeEpoch;
    if (name == "timeyear") return dappTimeYear;
    if (name == "timemonth") return dappTimeMonth;
    if (name == "timeday") return dappTimeDay;
    if (name == "timehour") return dappTimeHour;
    if (name == "timeminute") return dappTimeMinute;
    if (name == "timesecond") return dappTimeSecond;
    if (name == "timeweekday") return dappTimeWeekday;
    if (name == "audiook") return dappSynthLastOk() ? 1 : 0;
    if (name == "buflen") return (long)dappBufLen;
    if (name == "bufcap") return (long)dappBufCap;
    if (name == "bufok") return dappBufOk;
    if (name == "htmlok") return dappHtmlOk;
    if (name == "htmllines") return dappHtmlLines;
    if (name == "htmllinks") return dappHtmlLinks;
    if (name == "htmlbytes") return dappHtmlBytes;

    //KEY's vocabulary, so scripts compare against a name instead of a magic number.
    //Numeric only -- these are not defined for string expansion (PRINT "$kup" is empty),
    //because they exist to be compared, not printed.
    if (name == "kup") return DAPP_KEY_UP;
    if (name == "kdown") return DAPP_KEY_DOWN;
    if (name == "kleft") return DAPP_KEY_LEFT;
    if (name == "kright") return DAPP_KEY_RIGHT;
    if (name == "kenter") return DAPP_KEY_ENTER;
    if (name == "kesc") return DAPP_KEY_ESC;
    if (name == "kback") return DAPP_KEY_BACK;
    if (name == "ktab") return DAPP_KEY_TAB;
    if (name == "kspace") return DAPP_KEY_SPACE;
    return 0;
}

//   Operand normalization -- trim, optionally drop matching quotes, drop a leading '$'.
//
//   The three lookups below run on every operand of every instruction, and each of them
//   used to do this by copying: taken by value (one copy), handed to a quote stripper
//   (another), then mutated. Reporting the result as bounds into the caller's String
//   instead means the common token -- already trimmed and unquoted by splitCommand -- costs
//   no allocation at all, and a '$'-prefixed one costs a single substring rather than three
//   whole-string copies.
//
//   stripQuotes is off for the string-valued callers, and that asymmetry is load-bearing
//   rather than an oversight: splitCommand removes an argument's quotes as it splits, so a
//   literal like `APPEND pad " "` arrives here as a bare space with no quotes left to
//   protect it. Stripping again would take a legitimate one-character value apart. (The
//   browser runtime keeps quotes on until after this step, which is why padding built out
//   of " " worked there and used to vanish on hardware.)
static void appNormalizeToken(const String& token, int& begin, int& end, bool stripQuotes) {
    begin = 0;
    end = (int)token.length();
    while (begin < end && isspace((unsigned char)token[begin])) begin++;
    while (end > begin && isspace((unsigned char)token[end - 1])) end--;
    if (stripQuotes && end - begin >= 2) {
        char first = token[begin];
        char last = token[end - 1];
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            begin++;
            end--;
        }
    }
    if (begin < end && token[begin] == '$') {
        begin++;
    }
}

static long appValueOfNormalized(const String& token, DappProgram& program) {
    String name;
    String indexToken;
    if (appParseSubscript(token, name, indexToken)) {
        long* cell = appArrayCell(program, name, appValueOf(indexToken, program));
        return cell ? *cell : 0;
    }

    if (appIsInteger(token)) {
        return token.toInt();
    }
    int slot = appFindVar(program.vars, token);
    if (slot >= 0) {
        return program.vars[slot].value;
    }
    return appBuiltinValue(token);
}

static long appValueOf(const String& token, DappProgram& program) {
    int begin, end;
    appNormalizeToken(token, begin, end, true);
    if (begin == 0 && end == (int)token.length()) {
        return appValueOfNormalized(token, program);   //nothing to strip -- no copy at all
    }
    String normalized = token.substring(begin, end);
    return appValueOfNormalized(normalized, program);
}

static String appStringValueOfNormalized(const String& token, DappProgram& program) {
    String name;
    String indexToken;
    if (appParseSubscript(token, name, indexToken)) {
        long* cell = appArrayCell(program, name, appValueOf(indexToken, program));
        return cell ? String(*cell) : "";
    }

    int stringSlot = appFindStringVar(program.stringVars, token);
    if (stringSlot >= 0) {
        return program.stringVars[stringSlot].value;
    }
    int slot = appFindVar(program.vars, token);
    if (slot >= 0) {
        return String(program.vars[slot].value);
    }

    String lowered = token;
    lowered.toLowerCase();
    if (lowered == "cwd") return cwd;
    if (lowered == "ip") return WiFi.localIP().toString();
    if (lowered == "battery") return String(readBatteryPercent());
    if (lowered == "heap") return String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (lowered == "millis") return String(millis());
    if (lowered == "seconds") return String(millis() / 1000);
    if (lowered == "wifi") return wifiIsConnected() == 1 ? "1" : "0";
    if (lowered == "fok") return String(dappFok);
    if (lowered == "feof") return String(dappFeof);
    if (lowered == "ledok") return rearLedAvailable() ? "1" : "0";
    if (lowered == "httpcode") return String(dappHttpCode);
    if (lowered == "httplen") return String(dappHttpLength);
    if (lowered == "httptruncated") return String(dappHttpTruncated);
    if (lowered == "httpok") return String(dappHttpOk);
    if (lowered == "jsonok") return String(dappJsonOk);
    if (lowered == "timeok") return String(dappTimeOk);
    if (lowered == "timeepoch") return String(dappTimeEpoch);
    if (lowered == "timeyear") return String(dappTimeYear);
    if (lowered == "timemonth") return String(dappTimeMonth);
    if (lowered == "timeday") return String(dappTimeDay);
    if (lowered == "timehour") return String(dappTimeHour);
    if (lowered == "timeminute") return String(dappTimeMinute);
    if (lowered == "timesecond") return String(dappTimeSecond);
    if (lowered == "timeweekday") return String(dappTimeWeekday);
    if (lowered == "audiook") return dappSynthLastOk() ? "1" : "0";
    if (lowered == "buflen") return String((long)dappBufLen);
    if (lowered == "bufcap") return String((long)dappBufCap);
    if (lowered == "bufok") return String(dappBufOk);
    if (lowered == "htmlok") return String(dappHtmlOk);
    if (lowered == "htmllines") return String(dappHtmlLines);
    if (lowered == "htmllinks") return String(dappHtmlLinks);
    if (lowered == "htmlbytes") return String(dappHtmlBytes);
    return "";
}

static String appStringValueOf(const String& token, DappProgram& program) {
    int begin, end;
    //quotes are deliberately left alone here -- see the note on stripMatchingQuotes
    appNormalizeToken(token, begin, end, false);
    if (begin == 0 && end == (int)token.length()) {
        return appStringValueOfNormalized(token, program);
    }
    String normalized = token.substring(begin, end);
    return appStringValueOfNormalized(normalized, program);
}

//shared scanner for the two expanders below: at text[i] == '$', works out how far the
//reference runs (name, plus a [subscript] if one follows) and hands back the reference
//text without the '$'. Returns the index to continue scanning from, or -1 if the '$' is
//not the start of a reference and should be emitted literally.
static int appScanReference(const String& text, int dollarIdx, String& refOut) {
    int start = dollarIdx + 1;
    int end = start;
    while (end < (int)text.length() && appIsNameChar(text[end])) {
        end++;
    }
    if (end == start) {
        return -1;
    }
    if (end < (int)text.length() && text[end] == '[') {
        int close = appScanSubscript(text, end);
        if (close > 0) {
            end = close;
        }
    }
    refOut = text.substring(start, end);
    return end;
}

//   Both expanders used to build their result a character at a time into an unreserved
//   String, so a long PRINT re-grew the output buffer over and over. They now copy the
//   literal stretches between references in whole runs, and the common case -- text with no
//   '$' in it at all -- returns without entering the loop. Quote stripping is done with
//   index bounds rather than by building a trimmed copy first.
static String appExpandText(const String& text, DappProgram& program) {
    int begin = 0;
    int end = (int)text.length();
    if (end - begin >= 2) {
        char first = text[begin];
        char last = text[end - 1];
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            begin++;
            end--;
        }
    }

    int firstDollar = text.indexOf('$', begin);
    if (firstDollar < 0 || firstDollar >= end) {
        return text.substring(begin, end);
    }

    const char* base = text.c_str();
    String out;
    out.reserve(end - begin + 16);
    int runStart = begin;
    for (int i = firstDollar; i < end; i++) {
        if ((i & 0x7f) == 0x7f) appRuntimeYield(false);
        if (text[i] != '$') {
            continue;
        }

        String ref;
        int next = appScanReference(text, i, ref);
        if (next < 0 || next > end) {
            continue;   //not a reference -- the '$' stays part of the literal run
        }
        out.concat(base + runStart, i - runStart);
        out.concat(appStringValueOf(ref, program));
        i = next - 1;
        runStart = next;
    }
    out.concat(base + runStart, end - runStart);
    return out;
}

//EXPR's expander. Same scan as appExpandText, but every reference becomes its *numeric*
//value -- a string variable landing in the middle of an expression would only ever be a
//syntax error, so this guarantees tinyexpr sees digits wherever the script wrote a '$'.
static String appExpandNumericText(const String& text, DappProgram& program) {
    const int end = (int)text.length();
    int firstDollar = text.indexOf('$');
    if (firstDollar < 0) {
        return text;
    }

    const char* base = text.c_str();
    String out;
    out.reserve(end + 16);
    int runStart = 0;
    for (int i = firstDollar; i < end; i++) {
        if ((i & 0x7f) == 0x7f) appRuntimeYield(false);
        if (text[i] != '$') {
            continue;
        }

        String ref;
        int next = appScanReference(text, i, ref);
        if (next < 0) {
            continue;   //not a reference -- the '$' stays part of the literal run
        }
        out.concat(base + runStart, i - runStart);
        out.concat(appValueOf(ref, program));
        i = next - 1;
        runStart = next;
    }
    out.concat(base + runStart, end - runStart);
    return out;
}

static String appStringOperand(String token, DappProgram& program) {
    token.trim();
    bool explicitVariable = token.startsWith("$");
    bool quoted = token.length() >= 2 &&
        ((token[0] == '"' && token[token.length() - 1] == '"') ||
         (token[0] == '\'' && token[token.length() - 1] == '\''));

    if (explicitVariable ||
        (!quoted && appFindStringVar(program.stringVars, token) >= 0) ||
        (!quoted && appFindVar(program.vars, token) >= 0)) {
        return appStringValueOf(token, program);
    }

    return appExpandText(token, program);
}

//resolves a write target -- "score" (a numeric variable, created on first use) or
//"board[$i]" (a cell of an existing array). nullptr means the fault is already recorded.
static long* appNumericTargetNormalized(DappProgram& program, const String& token) {
    String name;
    String indexToken;
    if (appParseSubscript(token, name, indexToken)) {
        return appArrayCell(program, name, appValueOf(indexToken, program));
    }

    int slot = appEnsureVar(program.vars, token);
    if (slot < 0) {
        program.fault = "too many variables";
        return nullptr;
    }
    return &program.vars[slot].value;
}

static long* appNumericTarget(DappProgram& program, const String& token) {
    int begin, end;
    appNormalizeToken(token, begin, end, false);
    if (begin == 0 && end == (int)token.length()) {
        return appNumericTargetNormalized(program, token);
    }
    String normalized = token.substring(begin, end);
    return appNumericTargetNormalized(program, normalized);
}

//   CANVAS -- a character grid addressed by cell, drawn over the terminal area of the panel
//   and over the telnet client's screen. Scripts that draw a frame at a time need this:
//   PRINT appends to a scrolling history, so a 20-row playfield redrawn with CLEAR+PRINT
//   both flickers and walks the telnet client's scrollback. The buffer itself lives in
//   global.h because Display.ino renders it -- see the lifetime note there.

static void appCanvasClear() {
    if (!dappCanvasCells) {
        return;
    }
    for (int i = 0; i < dappCanvasCols * dappCanvasRows; i++) {
        dappCanvasCells[i].ch = ' ';
        dappCanvasCells[i].color = C_WHITE;
        if ((i & 0xff) == 0xff) appRuntimeYield(false);
    }
}

//safe to call when no canvas is up. Clears the flag *before* releasing the buffer so the
//renderer in Display.ino can never follow a dangling pointer.
static void appCanvasEnd() {
    bool wasActive = dappCanvasActive;
    dappCanvasActive = false;
    dappCanvasCols = 0;
    dappCanvasRows = 0;
    if (dappCanvasCells) {
        heap_caps_free(dappCanvasCells);
        dappCanvasCells = nullptr;
    }

    if (wasActive) {
        if (telnetClient && telnetClient.connected()) {
            telnetClient.print("\x1b[?25h\x1b[0m\x1b[2J\x1b[H");   //cursor back, screen back
        }
        markDisplayDirty();
    }
}

static bool appCanvasBegin(int cols, int rows) {
    if (cols < 1 || rows < 1 || cols > DAPP_CANVAS_MAX_COLS || rows > DAPP_CANVAS_MAX_ROWS) {
        outLine("run: CANVAS size must be 1.." + String(DAPP_CANVAS_MAX_COLS) + " by 1.." +
                String(DAPP_CANVAS_MAX_ROWS), C_RED);
        return false;
    }

    if (dappCanvasCells && dappCanvasCols == cols && dappCanvasRows == rows) {
        //same geometry as the canvas already up -- reuse it rather than churning PSRAM
        appCanvasClear();
        return true;
    }

    appCanvasEnd();
    dappCanvasCells = (DappCanvasCell*)psramOrInternalCalloc(cols * rows, sizeof(DappCanvasCell), "dappCanvas");
    if (!dappCanvasCells) {
        outLine("run: not enough memory for that CANVAS", C_RED);
        return false;
    }

    dappCanvasCols = cols;
    dappCanvasRows = rows;
    dappCanvasActive = true;
    appCanvasClear();

    if (telnetClient && telnetClient.connected()) {
        telnetClient.print("\x1b[2J\x1b[?25l");   //clear, and park the cursor out of the way
    }
    markDisplayDirty();
    return true;
}

//writes `text` rightward from (col, row), clipped at the edges. Off-grid rows are dropped
//rather than reported: a script drawing a piece that overhangs the well is doing something
//normal, and making it check bounds first would double the size of every draw routine.
static void appCanvasPut(int col, int row, const String& text, int color) {
    if (!dappCanvasCells || row < 0 || row >= dappCanvasRows) {
        return;
    }
    for (int i = 0; i < (int)text.length(); i++) {
        int x = col + i;
        if (x < 0) {
            continue;
        }
        if (x >= dappCanvasCols) {
            break;
        }
        DappCanvasCell& cell = dappCanvasCells[row * dappCanvasCols + x];
        cell.ch = text[i];
        cell.color = (uint8_t)color;
        if ((i & 0x7f) == 0x7f) appRuntimeYield(false);
    }
}

//pushes the grid to both surfaces. The telnet side homes the cursor and overwrites in
//place instead of clearing first -- clearing every frame is what makes a terminal game
//flicker -- and emits one SGR code per color run rather than per cell.
static void appCanvasFlip() {
    if (!dappCanvasCells) {
        return;
    }

    markDisplayDirty();
    drawDisplayFrame();

    if (!telnetClient || !telnetClient.connected()) {
        return;
    }

    String frame = "\x1b[H";
    frame.reserve((size_t)(dappCanvasCols + 8) * dappCanvasRows + 16);
    for (int row = 0; row < dappCanvasRows; row++) {
        int runColor = -1;
        for (int col = 0; col < dappCanvasCols; col++) {
            if (((row * dappCanvasCols + col) & 0xff) == 0xff) appRuntimeYield(false);
            const DappCanvasCell& cell = dappCanvasCells[row * dappCanvasCols + col];
            if ((int)cell.color != runColor) {
                runColor = cell.color;
                frame += "\x1b[";
                frame += String(runColor);
                frame += "m";
            }
            frame += cell.ch;
        }
        frame += "\x1b[0m";
        if (row < dappCanvasRows - 1) {
            frame += "\r\n";   //no newline after the last row, or the terminal scrolls
        }
    }
    telnetClient.print(frame);
}

//   Files -- FOPEN/FREAD/FWRITE/FCLOSE and friends. One open handle at a time, like the
//   one canvas and the one telnet client: a script that genuinely needs two files open at
//   once has outgrown this language. Error philosophy splits by kind: a file that isn't
//   there is a fact a script can plan around, so FOPEN/FDELETE report through $fok and
//   never stop the app -- but FREAD/FWRITE against a handle in the wrong state is a
//   programming bug and stops it like any other.
static File dappFile;
static bool dappFileOpen = false;
static bool dappFileReadable = false;
static bool dappFileWritable = false;
static bool dappFileIsSd = false;

static void appFileClose() {
    if (dappFileOpen) {
        dappFile.close();
        dappFileOpen = false;
    }
    dappFileReadable = false;
    dappFileWritable = false;
    dappFileIsSd = false;
}

//routes the unified /sd-or-flash namespace the same way `run` and the shell do, so a
//script's paths mean what its author's shell commands mean. Missing SD reads as "can't".
static bool appFileRoute(const String& path, RoutedPath& out) {
    out = routePath(resolvePath(cwd, path));
    return !(out.isSd && !sdCardMounted);
}

static void appFileOpen(const String& path, const char* fsMode, bool readable, bool writable) {
    appFileClose();
    dappFok = 0;
    dappFeof = 0;

    RoutedPath r;
    if (!appFileRoute(path, r)) {
        return;
    }

    if (writable) {
        ledPulseStorageWrite(r.isSd);
    } else {
        ledPulseStorageRead(r.isSd);
    }
    File f = r.fs->open(r.realPath, fsMode);
    if (!f || f.isDirectory()) {
        if (f) {
            f.close();
        }
        return;
    }

    dappFile = f;
    dappFileOpen = true;
    dappFileReadable = readable;
    dappFileWritable = writable;
    dappFileIsSd = r.isSd;
    dappFok = 1;
}

//Writes one immediate child per line as D|name|0 or F|name|bytes. A snapshot file
//keeps directory iteration separate from the script's single open file handle.
static bool appFileListToFile(const String& requestedPath, const String& requestedOutput) {
    appFileClose();
    dappFok = 0;
    RoutedPath source;
    RoutedPath destination;
    if (!appFileRoute(requestedPath, source) || !appFileRoute(requestedOutput, destination)) return false;
    File dir = source.fs->open(source.realPath);
    bool synthesizeSdRoot = false;
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            dir.close();
        }
        if (source.isSd && source.realPath == "/" && sdCardMounted) {
            synthesizeSdRoot = true;
        } else {
            return false;
        }
    }
    File output = destination.fs->open(destination.realPath, "w");
    if (!output || output.isDirectory()) {
        if (output) output.close();
        if (dir) dir.close();
        return false;
    }
    if (synthesizeSdRoot) {
        File apps = source.fs->open("/apps");
        if (apps && apps.isDirectory()) {
            output.println("D|apps|0");
        }
        if (apps) apps.close();
        output.close();
        dappFok = 1;
        return true;
    }
    File entry = dir.openNextFile();
    while (entry) {
        String name = entry.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        output.print(entry.isDirectory() ? "D|" : "F|");
        output.print(name);
        output.print('|');
        output.println(entry.isDirectory() ? 0 : (long)entry.size());
        entry.close();
        entry = dir.openNextFile();
        appRuntimeYield();
    }
    if (!source.isSd && resolvePath(cwd, requestedPath) == "/" && sdCardMounted) output.println("D|sd|0");
    output.close();
    dir.close();
    dappFok = 1;
    return true;
}

//one line, newline consumed, CR stripped -- the same shape appLoad reads scripts with,
//but capped per-character so a newline-free multi-megabyte file can't balloon a String.
static String appFileReadLine() {
    String line = "";
    if (!dappFile.available()) {
        dappFeof = 1;
        return line;
    }
    dappFeof = 0;
    ledPulseStorageRead(dappFileIsSd);
    while (dappFile.available()) {
        char ch = (char)dappFile.read();
        if (ch == '\n') {
            break;
        }
        if (line.length() < DAPP_MAX_STRING_LEN) {
            line += ch;
            if ((line.length() & 0x7f) == 0x7f) appRuntimeYield();
        }
    }
    if (line.endsWith("\r")) {
        line.remove(line.length() - 1);
    }
    return line;
}

//Raw byte I/O is deliberately numeric: Arduino String text can contain an embedded
//NUL, but every print/expansion surface eventually treats it as a C string. Returning
//0..255 plus a separate $feof is unambiguous for all 256 byte values.
static long appFileReadByte() {
    if (!dappFile.available()) {
        dappFeof = 1;
        return 0;
    }
    dappFeof = 0;
    ledPulseStorageRead(dappFileIsSd);
    return (long)(uint8_t)dappFile.read();
}

//A bounded sink lets HTTPClient do its own Content-Length/chunked decoding without
//ever allowing a response to grow a script String past the runtime's 4096-byte cap.
class DappHttpBodySink : public Stream {
public:
    explicit DappHttpBodySink(size_t maximum) : maximumBytes(maximum) {
        body.reserve(maximum);
    }

    size_t write(uint8_t value) override {
        return write(&value, 1);
    }

    size_t write(const uint8_t* buffer, size_t size) override {
        size_t room = body.length() < maximumBytes ? maximumBytes - body.length() : 0;
        size_t accepted = size < room ? size : room;
        for (size_t i = 0; i < accepted; i++) {
            body += (char)buffer[i];
            if ((i & 0x7f) == 0x7f) appRuntimeYield(false);
        }
        if (accepted < size) truncated = true;
        return accepted;
    }

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

    String body;
    bool truncated = false;

private:
    size_t maximumBytes;
};

static void appHttpClearHeaders() {
    for (int i = 0; i < dappHttpHeaderCount; i++) {
        dappHttpHeaderNames[i] = "";
        dappHttpHeaderValues[i] = "";
    }
    dappHttpHeaderCount = 0;
}

static bool appHttpSetHeader(const String& requestedName, const String& value) {
    String name = requestedName;
    name.trim();
    if (name.length() == 0 || name.indexOf('\r') >= 0 || name.indexOf('\n') >= 0 ||
        value.indexOf('\r') >= 0 || value.indexOf('\n') >= 0) return false;
    for (int i = 0; i < dappHttpHeaderCount; i++) {
        if (dappHttpHeaderNames[i].equalsIgnoreCase(name)) {
            dappHttpHeaderValues[i] = value;
            return true;
        }
    }
    if (dappHttpHeaderCount >= DAPP_HTTP_MAX_HEADERS) return false;
    dappHttpHeaderNames[dappHttpHeaderCount] = name;
    dappHttpHeaderValues[dappHttpHeaderCount] = value;
    dappHttpHeaderCount++;
    return true;
}

static bool appJsonEscape(const String& input, String& output) {
    static const char hex[] = "0123456789ABCDEF";
    output = "";
    output.reserve(input.length());
    for (size_t i = 0; i < input.length(); i++) {
        if ((i & 0x7f) == 0x7f) appRuntimeYield(false);
        uint8_t ch = (uint8_t)input[i];
        String addition;
        if (ch == '"') addition = "\\\"";
        else if (ch == '\\') addition = "\\\\";
        else if (ch == '\b') addition = "\\b";
        else if (ch == '\f') addition = "\\f";
        else if (ch == '\n') addition = "\\n";
        else if (ch == '\r') addition = "\\r";
        else if (ch == '\t') addition = "\\t";
        else if (ch < 0x20) {
            addition = "\\u00";
            addition += hex[ch >> 4];
            addition += hex[ch & 0x0f];
        } else {
            addition += (char)ch;
        }
        if (output.length() + addition.length() > DAPP_MAX_STRING_LEN) {
            output = "";
            return false;
        }
        output += addition;
    }
    return true;
}

static bool appJsonGetPath(const String& json, const String& requestedPath, String& output) {
    JsonDocument document;
    if (deserializeJson(document, json)) return false;

    String path = requestedPath;
    path.trim();
    JsonVariantConst current = document.as<JsonVariantConst>();
    int position = 0;
    while (position < (int)path.length()) {
        if (path[position] == '.') {
            position++;
            continue;
        }
        if (path[position] == '[') {
            int close = path.indexOf(']', position + 1);
            if (close < 0 || !current.is<JsonArrayConst>()) return false;
            String indexText = path.substring(position + 1, close);
            if (indexText.length() == 0) return false;
            for (size_t i = 0; i < indexText.length(); i++) {
                if ((i & 0x7f) == 0x7f) appRuntimeYield(false);
                if (!isDigit(indexText[i])) return false;
            }
            current = current[(size_t)indexText.toInt()];
            if (current.isNull()) return false;
            position = close + 1;
            continue;
        }

        int end = position;
        while (end < (int)path.length() && path[end] != '.' && path[end] != '[') {
            end++;
            if (((end - position) & 0x7f) == 0x7f) appRuntimeYield(false);
        }
        if (end == position || !current.is<JsonObjectConst>()) return false;
        String key = path.substring(position, end);
        current = current[key.c_str()];
        if (current.isNull()) return false;
        position = end;
    }

    output = "";
    if (current.is<const char*>()) {
        output = current.as<const char*>();
    } else {
        serializeJson(current, output);
    }
    if (output.length() > DAPP_MAX_STRING_LEN) {
        output = "";
        return false;
    }
    return true;
}

//   URL pieces
//
//   Every networked .dapp rewrote these by hand -- browse.dapp had ~90 lines of parseurl
//   and absurl -- so they are primitives now. The renderer below needs them internally to
//   turn an href into something followable, and the redirect hop above needs them to know
//   which origin a socket belongs to, which is reason enough on its own.

//splits scheme://host[:port]/path -- any piece the URL does not have comes back empty
static void appUrlSplit(const String& url, String& scheme, String& origin, String& path) {
    scheme = "";
    origin = "";
    path = "";
    int schemeEnd = url.indexOf("://");
    if (schemeEnd < 0) return;
    scheme = url.substring(0, schemeEnd);
    int hostStart = schemeEnd + 3;
    int hostEnd = url.indexOf('/', hostStart);
    if (hostEnd < 0) {
        origin = url;
        path = "/";
        return;
    }
    origin = url.substring(0, hostEnd);
    path = url.substring(hostEnd);
}

//collapses "." and ".." segments so a chain of relative links can't walk off the root.
//browse.dapp never did this and pasted "../" straight into the request line, which every
//server then answered differently.
static String appUrlNormalizePath(const String& path) {
    String segments[32];
    int count = 0;
    int i = 0;
    const int n = path.length();
    while (i < n) {
        int slash = path.indexOf('/', i);
        if (slash < 0) slash = n;
        String segment = path.substring(i, slash);
        i = slash + 1;
        if (segment == "." || segment.length() == 0) {
            continue;
        }
        if (segment == "..") {
            if (count > 0) count--;
            continue;
        }
        if (count < 32) segments[count++] = segment;
    }
    String out = "";
    for (int s = 0; s < count; s++) {
        out += "/";
        out += segments[s];
    }
    //a path ending in / names a directory and has to keep saying so
    if (out.length() == 0 || (n > 0 && path[n - 1] == '/')) out += "/";
    return out;
}

//   in: an absolute base URL and a raw href. Returns "" for anything that is not a
//   followable http(s) document -- fragments, mailto:, javascript:, empty -- so callers
//   test the result rather than carrying a separate ok flag.
static String appHtmlResolveUrl(const String& base, const String& href) {
    String target = href;
    target.trim();
    if (target.length() == 0) return "";

    String lowered = target;
    lowered.toLowerCase();
    if (lowered.startsWith("http://") || lowered.startsWith("https://")) return target;
    if (target[0] == '#') return "";
    //any other scheme is something this runtime cannot fetch
    int colon = target.indexOf(':');
    int slash = target.indexOf('/');
    if (colon > 0 && (slash < 0 || colon < slash) && !target.startsWith("//")) return "";

    String scheme, origin, path;
    appUrlSplit(base, scheme, origin, path);
    if (origin.length() == 0) return "";

    if (target.startsWith("//")) {
        return scheme + ":" + target;
    }
    //a query or fragment alone hangs off the base path, not the base directory
    if (target[0] == '?') {
        int cut = path.indexOf('?');
        if (cut >= 0) path = path.substring(0, cut);
        return origin + path + target;
    }
    //the query is carried across untouched: it is not path structure, and a "/" inside one
    //must not read as a segment boundary to the normalizer
    String query = "";
    int q = target.indexOf('?');
    if (q >= 0) {
        query = target.substring(q);
        target = target.substring(0, q);
    }

    if (target.startsWith("/")) {
        return origin + appUrlNormalizePath(target) + query;
    }

    //relative: resolve against the directory the base document lives in
    int cut = path.indexOf('?');
    if (cut >= 0) path = path.substring(0, cut);
    int lastSlash = path.lastIndexOf('/');
    String dir = lastSlash >= 0 ? path.substring(0, lastSlash + 1) : "/";
    return origin + appUrlNormalizePath(dir + target) + query;
}

//   One long-lived HTTP session instead of one per request
//
//   Every request used to build its own HTTPClient and TLS client on the stack, so a page
//   read in sixteen ranged windows paid sixteen TLS handshakes -- on this board that was a
//   large share of the wall clock, and none of it bought anything: the windows all came
//   from the same host. The client objects now outlive the request and HTTPClient is told
//   to keep the socket (setReuse), so a second request to the same origin skips connect
//   and handshake entirely.
//
//   useHTTP10(true) had to go with it. HTTP/1.0 implies Connection: close, which is the
//   exact thing being avoided, and the reason it was there -- avoiding chunked framing --
//   was never real: writeToStream() decodes chunked transfers itself.
//
//   Redirects are no longer HTTPClient's job either. It follows them internally without
//   telling the caller where it ended up, which would leave a pooled socket pointing at a
//   host this code still believed was `origin` -- a later same-origin request would reuse
//   it and read the wrong server's response. Hopping here instead keeps the origin the
//   socket belongs to and the origin recorded for reuse the same string by construction.
static HTTPClient dappHttp;
static WiFiClient dappHttpPlain;
static WiFiClientSecure dappHttpSecure;
static String dappHttpOrigin;          //origin the pooled socket belongs to, "" when none
static bool dappHttpSessionOpen = false;
static const int DAPP_HTTP_MAX_REDIRECTS = 5;

//scheme://host[:port] -- the unit a keep-alive socket is valid for
static String appHttpOriginOf(const String& url) {
    int schemeEnd = url.indexOf("://");
    if (schemeEnd < 0) return "";
    int hostEnd = url.indexOf('/', schemeEnd + 3);
    return hostEnd < 0 ? url : url.substring(0, hostEnd);
}

static void appHttpSessionEnd() {
    if (dappHttpSessionOpen) {
        dappHttp.setReuse(false);   //without this end() is a no-op by design
        dappHttp.end();
        dappHttpSessionOpen = false;
    }
    dappHttpPlain.stop();
    dappHttpSecure.stop();
    dappHttpOrigin = "";
}

//   Runs one method/url against `sink`, following redirects by hand. Sets $httpcode and
//   $httpok; the caller owns $httplen because only it knows what the sink counted.
//   Returns the number of body bytes handed to the sink, or -1 if the request never got
//   far enough to have a body.
static long appHttpPerform(const String& method, const String& requestedUrl,
                           const String& requestBody, Stream* sink) {
    dappHttpCode = 0;
    dappHttpOk = 0;

    String url = requestedUrl;
    url.trim();

    for (int hop = 0; hop <= DAPP_HTTP_MAX_REDIRECTS; hop++) {
        bool secure = url.startsWith("https://");
        if ((!secure && !url.startsWith("http://")) || wifiIsConnected() != 1) {
            dappHttpCode = -1;
            return -1;
        }

        //a pooled socket is only good for the origin it was opened to
        String origin = appHttpOriginOf(url);
        if (dappHttpSessionOpen && origin != dappHttpOrigin) {
            appHttpSessionEnd();
        }
        //whether this attempt is about to ride a socket the peer may have closed
        //while we were parked on an INPUT prompt -- see the retry below
        bool reusedSocket = dappHttpSessionOpen;

        dappHttp.setConnectTimeout(5000);
        dappHttp.setTimeout(10000);
        dappHttp.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        dappHttp.setReuse(true);

        bool started;
        if (secure) {
            //General .dapp URLs cannot be pinned to Dapper's one repository CA. This
            //matches ASUKA's generic fetch behavior: encrypted, but not authenticated.
            dappHttpSecure.setInsecure();
            started = dappHttp.begin(dappHttpSecure, url);
        } else {
            started = dappHttp.begin(dappHttpPlain, url);
        }
        if (!started) {
            appHttpSessionEnd();
            dappHttpCode = -2;
            return -1;
        }

        dappHttp.addHeader("Accept-Encoding", "identity");
        dappHttp.addHeader("User-Agent", "DOLL-OS-dapp/1.5");
        for (int i = 0; i < dappHttpHeaderCount; i++) {
            dappHttp.addHeader(dappHttpHeaderNames[i], dappHttpHeaderValues[i]);
        }
        //Location has to survive into the next hop, and HTTPClient only retains headers
        //that were asked for before the request went out
        const char* collect[] = { "Location" };
        dappHttp.collectHeaders(collect, 1);

        ledPulseNetwork();
        appRuntimeYield();
        int status = method == "POST"
            ? dappHttp.POST((uint8_t*)requestBody.c_str(), requestBody.length())
            : dappHttp.GET();
        appRuntimeYield();
        dappHttpCode = status;

        if (status <= 0) {
            appHttpSessionEnd();
            //   A pooled socket that returns no response at all is almost always one the
            //   peer already hung up on. Keep-alive idle timeouts are short -- node's
            //   default is five seconds -- and a .dapp parked on an INPUT prompt blows
            //   past that on every turn, so the first request after any human-length
            //   pause lands on a dead connection. The server never saw it, which is what
            //   makes replaying it safe even for POST. Without this a polling app just
            //   quietly does nothing whenever the user took a moment to type.
            if (reusedSocket) {
                hop--;   //a fresh-connection retry is not a redirect, so keep the budget
                continue;
            }
            return -1;
        }

        bool redirect = (status == 301 || status == 302 || status == 303 ||
                         status == 307 || status == 308);
        if (redirect && hop < DAPP_HTTP_MAX_REDIRECTS) {
            String location = dappHttp.header("Location");
            location.trim();
            if (location.length() > 0) {
                //Location is resolved against the URL that produced it, so a relative
                //redirect target lands where the server meant it to
                String next = appHtmlResolveUrl(url, location);
                //the redirect's own body was never read off the socket. end() only drains
                //what has already buffered, so anything still in flight would be handed to
                //the next request as if it were that response -- drop the connection
                //instead and pay one handshake for a case that is rare anyway.
                appHttpSessionEnd();
                if (next.length() == 0) {
                    return -1;
                }
                url = next;
                continue;
            }
        }

        long copied = dappHttp.writeToStream(sink);
        appRuntimeYield();
        dappHttp.end();
        //end() honours setReuse, so a keep-alive socket is still up here
        dappHttpSessionOpen = dappHttp.connected();
        dappHttpOrigin = dappHttpSessionOpen ? origin : "";
        dappHttpOk = (status >= 200 && status < 300) ? 1 : 0;
        return copied;
    }

    return -1;
}

static void appHttpRequest(const String& method, const String& requestedUrl,
                           const String& requestBody, size_t maximumBytes,
                           String& response) {
    dappHttpLength = 0;
    dappHttpTruncated = 0;
    response = "";

    DappHttpBodySink sink(maximumBytes);
    long copied = appHttpPerform(method, requestedUrl, requestBody, &sink);
    response = sink.body;
    dappHttpLength = response.length();
    dappHttpTruncated = sink.truncated ? 1 : 0;
    //a truncated read is still a read: the sink stopped the copy on purpose
    if (copied < 0 && !sink.truncated) {
        dappHttpOk = 0;
    }
    //stopping short leaves the rest of the body queued on the socket -- pooling it would
    //hand those bytes to the next request as if they were its response
    if (sink.truncated) appHttpSessionEnd();
}

//   The byte buffer
//
//   String variables cap at DAPP_MAX_STRING_LEN and every string op is O(n) over a PSRAM
//   String, which made "hold a document" the one thing scripts could not do. This is a
//   single flat block instead: one allocation, byte-addressed, never copied into a String
//   except through an explicit slice. Raising DAPP_MAX_STRING_LEN would have been the
//   other way to get here and a much worse one -- 32 string vars share that cap, and
//   appExpandText walks a string on every step that mentions one.
static const size_t DAPP_BUF_DEFAULT_BYTES = 65536;
static const size_t DAPP_BUF_MAX_BYTES = 262144;

static void appBufFree() {
    if (dappBuf) {
        heap_caps_free(dappBuf);
        dappBuf = nullptr;
    }
    dappBufCap = 0;
    dappBufLen = 0;
}

static bool appBufAlloc(size_t bytes) {
    appBufFree();
    if (bytes == 0 || bytes > DAPP_BUF_MAX_BYTES) return false;
    dappBuf = (uint8_t*)psramOrInternalCalloc(bytes, 1, "dappBuf");
    if (!dappBuf) return false;
    dappBufCap = bytes;
    dappBufLen = 0;
    return true;
}

//   Fills the buffer straight from the socket. memcpy per TCP segment, not a byte at a
//   time: DappHttpBodySink yields every 128 bytes, which on a 64KB read would be 512
//   delay(1) calls -- half a second of doing nothing.
class DappBufSink : public Stream {
public:
    size_t write(uint8_t value) override { return write(&value, 1); }

    size_t write(const uint8_t* buffer, size_t size) override {
        size_t room = dappBufLen < dappBufCap ? dappBufCap - dappBufLen : 0;
        size_t accepted = size < room ? size : room;
        if (accepted > 0) {
            memcpy(dappBuf + dappBufLen, buffer, accepted);
            dappBufLen += accepted;
        }
        appRuntimeYield(false);
        if (accepted < size) {
            //short write stops writeToStream, which leaves unread bytes on the socket --
            //the caller has to drop the session rather than pool a desynchronised one
            truncated = true;
            return accepted;
        }
        return size;
    }

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

    bool truncated = false;
};

//   HTML to text
//
//   A straight port of the state machine browse.dapp carried in numeric variables, which
//   is the only reason this is trustworthy: the shape has already been debugged against
//   real pages. What changes is the cost -- the script spent roughly 25 interpreter steps
//   per byte, each one re-parsing its own source line, and this spends a switch.
//
//   Feeding is incremental and state lives across calls, so a document can arrive in as
//   many pieces as the transport happens to deliver, and a tag split down the middle by a
//   segment boundary resumes correctly.
//
//   Deliberately not handled: JS, CSS, images, forms. Two things browse.dapp got wrong are
//   fixed here -- a bare "<" inside a script body no longer reads as a tag, and non-ASCII
//   text is transliterated to ASCII rather than dropped byte by byte (the panel renders
//   nothing above 126 anyway, see ansiFilterByte in Display.ino).

//parser states
static const int DHTML_TEXT = 0;
static const int DHTML_TAG = 1;
static const int DHTML_COMMENT = 2;
static const int DHTML_SKIP_LT = 3;   //saw '<' inside <script>/<style>

//block-level tags: a line break before and after whatever they contain
static const char* const DHTML_BLOCK_TAGS[] = {
    "p", "div", "br", "hr", "li", "tr", "ul", "ol", "dl", "dt", "dd",
    "h1", "h2", "h3", "h4", "h5", "h6", "pre", "table", "title", "form",
    "nav", "main", "aside", "header", "footer", "figure", "option",
    "article", "section", "blockquote", "body", "head"
};
static const int DHTML_BLOCK_TAG_COUNT =
    sizeof(DHTML_BLOCK_TAGS) / sizeof(DHTML_BLOCK_TAGS[0]);

//   Maps a Unicode codepoint onto something the 6x8 panel font can actually draw.
//   Dropping is preferred over '?' for the long tail: a page of unrenderable symbols
//   should read as gaps, not as noise.
static void appHtmlAppendCp(String& out, long cp) {
    if (cp < 32) return;
    if (cp < 127) {
        out += (char)cp;
        return;
    }
    //Latin-1 letters lose their accent rather than the whole character
    static const char* const latin1 =
        "AAAAAAECEEEEIIII"    //C0..CF
        "DNOOOOOxOUUUUYPs"    //D0..DF
        "aaaaaaeceeeeiiii"    //E0..EF
        "dnooooo/ouuuuypy";   //F0..FF
    //the ligatures and the sharp s are two letters, so they cannot come from the table
    if (cp == 0x00DF) { out += "ss"; return; }
    if (cp == 0x00C6) { out += "AE"; return; }
    if (cp == 0x00E6) { out += "ae"; return; }
    if (cp >= 0xC0 && cp <= 0xFF) {
        out += latin1[cp - 0xC0];
        return;
    }
    switch (cp) {
        case 0x00A0: out += ' '; return;
        case 0x00A3: out += "GBP"; return;
        case 0x00A9: out += "(c)"; return;
        case 0x00AB: case 0x00BB: out += '"'; return;
        case 0x00AE: out += "(r)"; return;
        case 0x00B7: case 0x2022: out += '*'; return;
        case 0x00D7: out += 'x'; return;
        case 0x2018: case 0x2019: case 0x201B: out += '\''; return;
        case 0x201C: case 0x201D: case 0x201E: out += '"'; return;
        case 0x2013: case 0x2014: case 0x2212: out += '-'; return;
        case 0x2026: out += "..."; return;
        case 0x20AC: out += "EUR"; return;
        case 0x2122: out += "(tm)"; return;
        default: return;
    }
}

//named entities worth the table space; everything else falls through to numeric forms
static long appHtmlNamedEntity(const String& name) {
    if (name == "amp") return '&';
    if (name == "lt") return '<';
    if (name == "gt") return '>';
    if (name == "quot") return '"';
    if (name == "apos") return '\'';
    if (name == "nbsp") return 0x00A0;
    if (name == "mdash") return 0x2014;
    if (name == "ndash") return 0x2013;
    if (name == "hellip") return 0x2026;
    if (name == "lsquo") return 0x2018;
    if (name == "rsquo") return 0x2019;
    if (name == "ldquo") return 0x201C;
    if (name == "rdquo") return 0x201D;
    if (name == "bull") return 0x2022;
    if (name == "middot") return 0x00B7;
    if (name == "copy") return 0x00A9;
    if (name == "reg") return 0x00AE;
    if (name == "trade") return 0x2122;
    if (name == "laquo") return 0x00AB;
    if (name == "raquo") return 0x00BB;
    if (name == "times") return 0x00D7;
    if (name == "deg") return 0x00B0;
    if (name == "pound") return 0x00A3;
    if (name == "euro") return 0x20AC;
    if (name == "szlig") return 0x00DF;
    if (name == "aelig") return 0x00E6;
    if (name == "AElig") return 0x00C6;
    return -1;
}

//   "eacute", "uuml", "ntilde" and the rest of the Latin-1 accent names all transliterate
//   to their base letter, so they are matched by shape rather than listed one by one --
//   sixty table entries that would all resolve to the character already sitting in front
//   of the suffix. Case is taken from the source name so &Eacute; still yields "E".
static long appHtmlAccentEntity(const String& name) {
    static const char* const suffixes[] = {
        "acute", "grave", "circ", "tilde", "uml", "ring", "cedil", "slash"
    };
    if (name.length() < 4) return -1;
    char base = name[0];
    if (!strchr("aeiouncyAEIOUNCY", base)) return -1;
    String rest = name.substring(1);
    rest.toLowerCase();
    for (int i = 0; i < 8; i++) {
        if (rest == suffixes[i]) return (long)base;
    }
    return -1;
}

struct DappHtmlRender {
    bool active = false;

    int wrapcol = 76;
    int maxlinks = 200;
    String base;

    //output goes to a pair of files, or to a string for the scrape-a-small-page case
    bool toString = false;
    String out;
    File textFile;
    File linkFile;
    bool haveText = false;
    bool haveLink = false;
    //LittleFS makes a syscall of every write, so lines are batched into one
    String textPending;
    String linkPending;

    //lines/links/bytes are the script-visible $htmllines/$htmllinks/$htmlbytes rather
    //than members, so there is one copy of each rather than a mirror to keep in step
    int state = DHTML_TEXT;
    bool skip = false;
    String skipTag;
    bool inEntity = false;
    String entity;
    String tagbuf;
    int tagn = 0;
    int dash1 = 0;
    int dash2 = 0;
    String line;
    String word;
    int utf8Need = 0;
    long utf8Cp = 0;

    void reset() {
        state = DHTML_TEXT;
        skip = false;
        skipTag = "";
        inEntity = false;
        entity = "";
        tagbuf = "";
        tagn = 0;
        dash1 = 0;
        dash2 = 0;
        line = "";
        word = "";
        utf8Need = 0;
        utf8Cp = 0;
        dappHtmlLines = 0;
        dappHtmlLinks = 0;
        dappHtmlBytes = 0;
        out = "";
        textPending = "";
        linkPending = "";
    }

    void writeText(const String& text) {
        if (toString) {
            if (out.length() + text.length() + 1 <= DAPP_MAX_STRING_LEN) {
                if (out.length() > 0) out += "\n";
                out += text;
            }
            return;
        }
        if (!haveText) return;
        textPending += text;
        textPending += "\n";
        if (textPending.length() >= 1024) {
            textFile.write((const uint8_t*)textPending.c_str(), textPending.length());
            textPending = "";
            appRuntimeYield(false);
        }
    }

    void writeLink(long number, const String& url) {
        if (!haveLink) return;
        linkPending += String(number);
        linkPending += " ";
        linkPending += url;
        linkPending += "\n";
        if (linkPending.length() >= 512) {
            linkFile.write((const uint8_t*)linkPending.c_str(), linkPending.length());
            linkPending = "";
        }
    }

    void flushLine() {
        if (line.length() == 0) return;
        writeText(line);
        dappHtmlLines++;
        line = "";
    }

    void flushWord() {
        if (word.length() == 0) return;
        if (line.length() == 0) {
            line = word;
        } else if ((int)(line.length() + 1 + word.length()) > wrapcol) {
            flushLine();
            line = word;
        } else {
            line += " ";
            line += word;
        }
        word = "";
    }

    void emitCp(long cp) {
        if (skip) return;
        if (cp == 0x00A0) {      //a non-breaking space still separates words here
            flushWord();
            return;
        }
        appHtmlAppendCp(word, cp);
        //a pathologically long "word" would defeat wrapping entirely
        if ((int)word.length() >= wrapcol) flushWord();
    }

    void resolveEntity() {
        if (skip) {
            entity = "";
            return;
        }
        String name = entity;
        entity = "";
        String lowered = name;
        lowered.toLowerCase();

        long cp = -1;
        if (lowered.startsWith("#x")) {
            cp = strtol(lowered.c_str() + 2, nullptr, 16);
        } else if (lowered.startsWith("#")) {
            cp = strtol(lowered.c_str() + 1, nullptr, 10);
        } else {
            cp = appHtmlNamedEntity(lowered);
            if (cp < 0) cp = appHtmlAccentEntity(name);
        }
        if (cp > 0) {
            emitCp(cp);
            return;
        }
        //not an entity after all -- put the source text back verbatim, semicolon included,
        //since that character was consumed getting here
        emitCp('&');
        for (size_t i = 0; i < name.length(); i++) emitCp((uint8_t)name[i]);
        emitCp(';');
    }

    //in: tagbuf, everything between < and >
    void endTag() {
        if (tagbuf.length() == 0) return;
        char first = tagbuf[0];
        if (first == '!' || first == '?') return;

        bool closing = first == '/';
        int i = closing ? 1 : 0;
        String name = "";
        while (i < (int)tagbuf.length() && name.length() < 12) {
            char ch = tagbuf[i];
            if (!isAlphaNumeric(ch)) break;
            name += (char)tolower(ch);
            i++;
        }
        if (name.length() == 0) return;

        if (name == "script" || name == "style" || name == "template") {
            if (closing) {
                if (skipTag == name) {
                    skip = false;
                    skipTag = "";
                }
            } else {
                skip = true;
                skipTag = name;
            }
            return;
        }
        if (skip) return;

        if (name == "a") {
            anchorTag(closing);
            return;
        }
        for (int b = 0; b < DHTML_BLOCK_TAG_COUNT; b++) {
            if (name == DHTML_BLOCK_TAGS[b]) {
                flushWord();
                flushLine();
                return;
            }
        }
        if (name == "td" || name == "th") {
            flushWord();
        }
    }

    //   An opening <a href> becomes a numbered marker in the text and one line in the link
    //   file; the number is what the reader types to follow it. The closing </a> flushes
    //   nothing on purpose, so "link</a>." keeps its punctuation attached instead of
    //   stranding it as a word of its own.
    void anchorTag(bool closing) {
        if (closing) return;
        if (!haveLink || dappHtmlLinks >= maxlinks) return;

        String href;
        if (!tagAttribute("href", href)) return;
        String absolute = appHtmlResolveUrl(base, href);
        if (absolute.length() == 0) return;

        flushWord();
        dappHtmlLinks++;
        word = "[";
        word += String(dappHtmlLinks);
        word += "]";
        flushWord();
        writeLink(dappHtmlLinks, absolute);
    }

    //in: tagbuf -- pulls one attribute value, quoted or bare
    bool tagAttribute(const char* wanted, String& value) {
        value = "";
        const int n = tagbuf.length();
        const int wantedLen = strlen(wanted);
        int i = 0;
        while (i < n) {
            //an attribute name only starts after whitespace, so href inside another
            //value (?href=...) is not mistaken for the attribute itself
            if (i > 0 && !isSpace(tagbuf[i - 1])) { i++; continue; }
            bool match = true;
            for (int k = 0; k < wantedLen; k++) {
                if (i + k >= n || tolower(tagbuf[i + k]) != wanted[k]) { match = false; break; }
            }
            if (!match) { i++; continue; }

            int j = i + wantedLen;
            while (j < n && isSpace(tagbuf[j])) j++;
            if (j >= n || tagbuf[j] != '=') { i++; continue; }
            j++;
            while (j < n && isSpace(tagbuf[j])) j++;
            if (j >= n) return false;

            char quote = 0;
            if (tagbuf[j] == '"' || tagbuf[j] == '\'') { quote = tagbuf[j]; j++; }
            while (j < n && value.length() < 300) {
                char ch = tagbuf[j];
                if (quote ? ch == quote : (isSpace(ch) || ch == '>')) break;
                value += ch;
                j++;
            }
            //href values arrive HTML-escaped and have to be unescaped before use
            value.replace("&amp;", "&");
            return value.length() > 0;
        }
        return false;
    }

    void feedByte(uint8_t byte) {
        dappHtmlBytes++;

        if (state == DHTML_COMMENT) {
            if (byte == '>' && dash1 == '-' && dash2 == '-') {
                state = DHTML_TEXT;
            } else {
                dash2 = dash1;
                dash1 = byte;
            }
            return;
        }

        if (state == DHTML_SKIP_LT) {
            //inside <script>, only "</" can begin a tag -- "if (a<b)" must stay text
            if (byte == '/') {
                state = DHTML_TAG;
                tagbuf = "/";
                tagn = 1;
            } else {
                state = DHTML_TEXT;
            }
            return;
        }

        if (state == DHTML_TAG) {
            if (byte == '>') {
                state = DHTML_TEXT;
                endTag();
                tagbuf = "";
                tagn = 0;
                return;
            }
            if (tagn < 400) {
                tagbuf += (char)byte;
                tagn++;
                if (tagn == 3 && tagbuf == "!--") {
                    state = DHTML_COMMENT;
                    dash1 = 0;
                    dash2 = 0;
                }
            }
            return;
        }

        //DHTML_TEXT from here
        if (inEntity) {
            if (byte == ';') {
                inEntity = false;
                resolveEntity();
            } else if (byte <= 32 || entity.length() >= 12) {
                //not an entity: replay the text literally, then reprocess this byte
                inEntity = false;
                String replay = entity;
                entity = "";
                emitCp('&');
                for (size_t i = 0; i < replay.length(); i++) emitCp((uint8_t)replay[i]);
                feedByte(byte);
            } else {
                entity += (char)byte;
            }
            return;
        }

        if (byte == '<') {
            state = skip ? DHTML_SKIP_LT : DHTML_TAG;
            if (!skip) {
                tagbuf = "";
                tagn = 0;
            }
            return;
        }
        if (byte == '&') {
            inEntity = true;
            entity = "";
            return;
        }

        //UTF-8: gather a sequence, then transliterate the codepoint it names
        if (utf8Need > 0) {
            if ((byte & 0xC0) == 0x80) {
                utf8Cp = (utf8Cp << 6) | (byte & 0x3F);
                if (--utf8Need == 0) emitCp(utf8Cp);
                return;
            }
            utf8Need = 0;   //malformed; fall through and treat this byte on its own
        }
        if (byte >= 0x80) {
            if ((byte & 0xE0) == 0xC0)      { utf8Need = 1; utf8Cp = byte & 0x1F; }
            else if ((byte & 0xF0) == 0xE0) { utf8Need = 2; utf8Cp = byte & 0x0F; }
            else if ((byte & 0xF8) == 0xF0) { utf8Need = 3; utf8Cp = byte & 0x07; }
            return;
        }

        if (byte <= 32) {
            flushWord();
            return;
        }
        emitCp(byte);
    }

    void feed(const uint8_t* data, size_t n) {
        for (size_t i = 0; i < n; i++) {
            feedByte(data[i]);
            if ((i & 0x3ff) == 0x3ff) appRuntimeYield(false);
        }
    }
};

static DappHtmlRender dappHtml;

//pipes an HTTP body straight into the renderer -- no buffer in between, so a page is not
//limited by anything this runtime allocates
class DappHtmlSink : public Stream {
public:
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t* buffer, size_t size) override {
        dappHtml.feed(buffer, size);
        return size;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};

//   Opens the renderer's own output files. Shared by HTMLOPEN and HTMLTEXT so the two
//   cannot drift; linkPath "-" means "render text, do not collect links". Returns false
//   with nothing left open if either file will not take a write.
static bool appHtmlBegin(const String& textPath, const String& linkPath, const String& base,
                         int wrapcol, int maxlinks) {
    dappHtml.reset();
    dappHtml.toString = false;
    dappHtml.haveText = false;
    dappHtml.haveLink = false;
    dappHtml.base = base;
    dappHtml.wrapcol = (wrapcol < 16 || wrapcol > 240) ? 76 : wrapcol;
    dappHtml.maxlinks = maxlinks < 0 ? 0 : maxlinks;

    RoutedPath textRoute;
    if (!appFileRoute(textPath, textRoute)) return false;
    ledPulseStorageWrite(textRoute.isSd);
    dappHtml.textFile = textRoute.fs->open(textRoute.realPath, "w");
    if (!dappHtml.textFile || dappHtml.textFile.isDirectory()) {
        if (dappHtml.textFile) dappHtml.textFile.close();
        return false;
    }
    dappHtml.haveText = true;

    if (linkPath != "-") {
        RoutedPath linkRoute;
        if (appFileRoute(linkPath, linkRoute)) {
            dappHtml.linkFile = linkRoute.fs->open(linkRoute.realPath, "w");
            if (dappHtml.linkFile && !dappHtml.linkFile.isDirectory()) {
                dappHtml.haveLink = true;
            } else if (dappHtml.linkFile) {
                dappHtml.linkFile.close();
            }
        }
        if (!dappHtml.haveLink) {
            dappHtml.textFile.close();
            dappHtml.haveText = false;
            return false;
        }
    }
    dappHtml.active = true;
    return true;
}

static void appHtmlClose() {
    if (!dappHtml.active) return;
    dappHtml.flushWord();
    dappHtml.flushLine();
    if (dappHtml.haveText) {
        if (dappHtml.textPending.length() > 0) {
            dappHtml.textFile.write((const uint8_t*)dappHtml.textPending.c_str(),
                                    dappHtml.textPending.length());
            dappHtml.textPending = "";
        }
        dappHtml.textFile.close();
        dappHtml.haveText = false;
    }
    if (dappHtml.haveLink) {
        if (dappHtml.linkPending.length() > 0) {
            dappHtml.linkFile.write((const uint8_t*)dappHtml.linkPending.c_str(),
                                    dappHtml.linkPending.length());
            dappHtml.linkPending = "";
        }
        dappHtml.linkFile.close();
        dappHtml.haveLink = false;
    }
    dappHtml.active = false;
}

//   KEY -- non-blocking key polling
//
//   INPUT blocks until Enter, which is fine for a prompt and useless for a game: gravity
//   has to keep ticking while nothing is pressed. This decodes at most one key per call
//   from either input source, leaving the rest queued. Each source keeps its own escape
//   state for the same reason LineEditState does (global.h): a half-arrived arrow key on
//   one source must not corrupt the other's parse.

//DappKeyPhase/DappKeyState are defined in global.h -- a hoisted prototype for
//appPollKeySource() below mentions the type, and lands above this file
static DappKeyState dappTelnetKeys;
static DappKeyState dappKeyboardKeys;

//a lone Escape and the start of an arrow key are the same byte, so an unresolved ESC is
//held briefly and only reported as Escape once nothing followed it. Same 40ms rule the
//Game Boy runner uses for its telnet menu key (Gameboy.ino).
const unsigned long DAPP_ESC_SETTLE_MS = 40;

//   Aborting a stuck app -- Ctrl+X, the editor's own exit chord, stops a running .dapp
//   from either input source. Necessary because the step guard deliberately never trips
//   a loop that WAITs, which makes "WAIT in a loop forever" both a legitimate program
//   shape and, when unintended, one that nothing else could stop.
//
//   The check uses peek() so nothing but the abort byte itself is ever consumed -- WAIT
//   and INPUT must not steal bytes a KEY loop or the line editor is about to read. The
//   cost of that safety: the chord only registers as the *next unread* byte. That covers
//   the case that matters (an app looping without reading its input has no other reader,
//   so ^X is at the head), and KEY apps don't rely on it at all -- appPollKeySource traps
//   the byte the moment it is read, wherever it sits in the queue.
static bool dappAbort = false;
const uint8_t DAPP_ABORT_BYTE = 0x18;   //^X

static void appPollAbortChord() {
    if (dappAbort) {
        return;
    }
    if (telnetClient && telnetClient.connected() && telnetClient.peek() == DAPP_ABORT_BYTE) {
        telnetReadFilteredByte();
        dappAbort = true;
        return;
    }
    if (keyboardPeekRawByte() == DAPP_ABORT_BYTE) {
        keyboardReadRawByte();
        dappAbort = true;
    }
}

static long appDecodeKeyByte(uint8_t b, DappKeyState& st) {
    if (st.phase == DKEY_ESC) {
        if (b == '[') {
            st.phase = DKEY_CSI;
            st.params = "";
            return DAPP_KEY_NONE;
        }
        st.phase = DKEY_NORMAL;
        return DAPP_KEY_ESC;
    }

    if (st.phase == DKEY_CSI) {
        if (b < 0x40 || b > 0x7E) {
            st.params += (char)b;
            return DAPP_KEY_NONE;
        }
        const String params = st.params;
        st.phase = DKEY_NORMAL;

        //Ctrl+Up/Down nudges radio volume everywhere else (shell prompt, raw ssh/telnet,
        //the text editor) -- this decoder collapsed the params and read every CSI...A/B
        //as a plain arrow, so the same chord silently steered whatever app was running
        //instead of touching the volume at all while one was open
        if (params == "1;5" && (b == 'A' || b == 'B')) {
            radioAdjustVolume(b == 'A' ? 1 : -1);
            return DAPP_KEY_NONE;
        }
        switch ((char)b) {
            case 'A': return DAPP_KEY_UP;
            case 'B': return DAPP_KEY_DOWN;
            case 'C': return DAPP_KEY_RIGHT;
            case 'D': return DAPP_KEY_LEFT;
            default:  return DAPP_KEY_NONE;   //function keys and the rest: not in the vocabulary
        }
    }

    if (b == 0x1B) {
        st.phase = DKEY_ESC;
        st.escAtMs = millis();
        return DAPP_KEY_NONE;
    }
    if (b == '\r' || b == '\n') return DAPP_KEY_ENTER;
    if (b == 0x08 || b == 0x7F) return DAPP_KEY_BACK;
    if (b == '\t') return DAPP_KEY_TAB;
    //Ctrl+C and Ctrl+T are how every other modal screen in DOLL-OS is escaped, so a script that
    //quits on $kesc quits on the chords its user already knows too
    if (b == 0x03 || b == 0x14) return DAPP_KEY_ESC;
    if (b >= 32 && b < 127) return (long)b;
    return DAPP_KEY_NONE;
}

static long appPollKeySource(int (*readByte)(), DappKeyState& st) {
    if (st.phase == DKEY_ESC && millis() - st.escAtMs > DAPP_ESC_SETTLE_MS) {
        st.phase = DKEY_NORMAL;
        return DAPP_KEY_ESC;
    }

    int b;
    int scanned = 0;
    while ((b = readByte()) >= 0) {
        if ((++scanned & 0x3f) == 0) appRuntimeYield(false);
        //^X aborts the app rather than reaching the script -- KEY never returns it
        if (st.phase == DKEY_NORMAL && (uint8_t)b == DAPP_ABORT_BYTE) {
            dappAbort = true;
            return DAPP_KEY_NONE;
        }
        long key = appDecodeKeyByte((uint8_t)b, st);
        if (key != DAPP_KEY_NONE) {
            return key;
        }
    }
    return DAPP_KEY_NONE;
}

static long appPollKey() {
    long key = appPollKeySource(telnetReadFilteredByte, dappTelnetKeys);
    if (key != DAPP_KEY_NONE) {
        return key;
    }
    return appPollKeySource(keyboardReadRawByte, dappKeyboardKeys);
}

static void appResetKeyState() {
    dappTelnetKeys = DappKeyState();
    dappKeyboardKeys = DappKeyState();
    dappAbort = false;
}

static void appDelay(unsigned long waitMs) {
    unsigned long started = millis();
    while (millis() - started < waitMs) {
        appRuntimeYield();
        if (dappAbort) {
            return;   //appExecute notices the flag right after this op
        }
    }
}

static String appReadInput(const String& prompt, bool masked, bool echoInput) {
    String input = "";
    commandCursorPos = 0;

    if (telnetClient && telnetClient.connected()) {
        telnetClient.print(prompt);
    }

    while (true) {
        //checked before the line editor gets the byte, so ^X interrupts even a
        //blocked prompt instead of being swallowed as an editing keystroke
        appPollAbortChord();
        if (dappAbort) {
            commandCursorPos = 0;
            setActiveInput("", "", false);
            return "";
        }
        LineInputResult r = readLineEditedInput(input);
        if (r == LINE_NO_INPUT) {
            r = readKeyboardLineEditedInput(input);
        }
        setActiveInput(prompt, input, masked);
        appRuntimeYield();
        //appRuntimeYield() skips its own drawDisplayFrame() call while a canvas app is
        //active, to keep a frame mid-construction (between CLS/PUT and FLIP) from
        //flashing onto the panel half-built. That gate also caught this fully-formed,
        //already-FLIPped frame just sitting behind a blocking INPUT prompt (e.g.
        //tracker-music's save-browser "S"/"N" prompts) -- so every keystroke, including
        //backspace, edited the buffer correctly but was never pushed to the panel, and
        //only the state at the moment of the *next* unrelated FLIP ever became visible.
        if (dappCanvasActive) {
            drawDisplayFrame();
        }

        if (r == LINE_SUBMITTED) {
            String submitted = input;
            submitted.trim();
            commandCursorPos = 0;
            if (echoInput) {
                outLine(prompt + (masked ? "[hidden]" : submitted), C_CYAN);
            }
            //otherwise the submitted text sits in the command bar under a canvas app
            //(e.g. tracker-music's save-name prompt) until something else overwrites it
            setActiveInput("", "", false);
            return submitted;
        }
    }
}

static bool appCompare(long left, const String& op, long right) {
    if (op == "==" || op == "=") return left == right;
    if (op == "!=" || op == "<>") return left != right;
    if (op == ">") return left > right;
    if (op == "<") return left < right;
    if (op == ">=") return left >= right;
    if (op == "<=") return left <= right;
    return false;
}

static long appRandomRange(long low, long high) {
    if (high < low) {
        long tmp = low;
        low = high;
        high = tmp;
    }

    uint32_t span = (uint32_t)(high - low + 1);
    if (span == 0) {
        return low;
    }
    return low + (long)(esp_random() % span);
}

//   Load-time opcode decode. appExecute used to recover the opcode by substring'ing the
//   line and walking a chain of ~70 String comparisons, on every execution of every line --
//   with GOTO at position ~70 and IF at ~75, so the ops an inner loop leans on were the
//   most expensive to recognise. The name is resolved to one of these once, at load, and
//   the chain below compares integers instead.
enum DappOpcode : uint8_t {
    DAPP_OP_UNKNOWN = 0,
    DAPP_OP_LABEL,
    DAPP_OP_PRINT,
    DAPP_OP_ECHO,
    DAPP_OP_COLOR,
    DAPP_OP_LED,
    DAPP_OP_WAVE,
    DAPP_OP_WAVESTOP,
    DAPP_OP_CLEAR,
    DAPP_OP_CLS,
    DAPP_OP_WAIT,
    DAPP_OP_SLEEP,
    DAPP_OP_SET,
    DAPP_OP_ADD,
    DAPP_OP_SUB,
    DAPP_OP_MUL,
    DAPP_OP_DIV,
    DAPP_OP_MOD,
    DAPP_OP_EXPR,
    DAPP_OP_DIM,
    DAPP_OP_LIFE,
    DAPP_OP_SETSTR,
    DAPP_OP_APPEND,
    DAPP_OP_CHR,
    DAPP_OP_HEX,
    DAPP_OP_JSONESC,
    DAPP_OP_JSONGET,
    DAPP_OP_SUBSTR,
    DAPP_OP_LEN,
    DAPP_OP_CHARAT,
    DAPP_OP_INPUT,
    DAPP_OP_INPUTSECRET,
    DAPP_OP_KEY,
    DAPP_OP_CANVAS,
    DAPP_OP_ENDCANVAS,
    DAPP_OP_PUT,
    DAPP_OP_FLIP,
    DAPP_OP_FOPEN,
    DAPP_OP_FCLOSE,
    DAPP_OP_FREAD,
    DAPP_OP_FREADB,
    DAPP_OP_FWRITE,
    DAPP_OP_FWRITEB,
    DAPP_OP_FSEEK,
    DAPP_OP_FTELL,
    DAPP_OP_FSIZE,
    DAPP_OP_FEXISTS,
    DAPP_OP_FDELETE,
    DAPP_OP_FLIST,
    DAPP_OP_FMKDIR,
    DAPP_OP_FCOPY,
    DAPP_OP_FMOVE,
    DAPP_OP_HTTPCLEAR,
    DAPP_OP_HTTPHEADER,
    DAPP_OP_HTTPGET,
    DAPP_OP_HTTPPOST,
    DAPP_OP_BUFNEW,
    DAPP_OP_BUFFREE,
    DAPP_OP_BUFCLEAR,
    DAPP_OP_BUFAT,
    DAPP_OP_BUFSUB,
    DAPP_OP_BUFWRITE,
    DAPP_OP_BUFSCAN,
    DAPP_OP_BUFTAKE,
    DAPP_OP_BUFSAVE,
    DAPP_OP_BUFLOAD,
    DAPP_OP_HTTPGETBUF,
    DAPP_OP_URLABS,
    DAPP_OP_URLPART,
    DAPP_OP_HTMLOPEN,
    DAPP_OP_HTMLFEED,
    DAPP_OP_HTMLCLOSE,
    DAPP_OP_HTMLTEXT,
    DAPP_OP_HTMLSTR,
    DAPP_OP_DAPPER,
    DAPP_OP_TIME,
    DAPP_OP_RAND,
    DAPP_OP_GOTO,
    DAPP_OP_GOSUB,
    DAPP_OP_RETURN,
    DAPP_OP_IF,
    DAPP_OP_IFEQ,
    DAPP_OP_IFNE,
    DAPP_OP_EXIT,
    DAPP_OP_END,
    DAPP_OP_COUNT
};

//   Parallel to the enum above -- index with the opcode to get the source spelling back.
//   Only the cold error paths need it; the hot path never turns an opcode back into text.
static const char* const DAPP_OPCODE_NAMES[DAPP_OP_COUNT] = {
    "",
    "LABEL",
    "PRINT",
    "ECHO",
    "COLOR",
    "LED",
    "WAVE",
    "WAVESTOP",
    "CLEAR",
    "CLS",
    "WAIT",
    "SLEEP",
    "SET",
    "ADD",
    "SUB",
    "MUL",
    "DIV",
    "MOD",
    "EXPR",
    "DIM",
    "LIFE",
    "SETSTR",
    "APPEND",
    "CHR",
    "HEX",
    "JSONESC",
    "JSONGET",
    "SUBSTR",
    "LEN",
    "CHARAT",
    "INPUT",
    "INPUTSECRET",
    "KEY",
    "CANVAS",
    "ENDCANVAS",
    "PUT",
    "FLIP",
    "FOPEN",
    "FCLOSE",
    "FREAD",
    "FREADB",
    "FWRITE",
    "FWRITEB",
    "FSEEK",
    "FTELL",
    "FSIZE",
    "FEXISTS",
    "FDELETE",
    "FLIST",
    "FMKDIR",
    "FCOPY",
    "FMOVE",
    "HTTPCLEAR",
    "HTTPHEADER",
    "HTTPGET",
    "HTTPPOST",
    "BUFNEW",
    "BUFFREE",
    "BUFCLEAR",
    "BUFAT",
    "BUFSUB",
    "BUFWRITE",
    "BUFSCAN",
    "BUFTAKE",
    "BUFSAVE",
    "BUFLOAD",
    "HTTPGETBUF",
    "URLABS",
    "URLPART",
    "HTMLOPEN",
    "HTMLFEED",
    "HTMLCLOSE",
    "HTMLTEXT",
    "HTMLSTR",
    "DAPPER",
    "TIME",
    "RAND",
    "GOTO",
    "GOSUB",
    "RETURN",
    "IF",
    "IFEQ",
    "IFNE",
    "EXIT",
    "END",
};

static const char* appOpcodeName(uint8_t opcode) {
    return (opcode < DAPP_OP_COUNT) ? DAPP_OPCODE_NAMES[opcode] : "";
}

//   Linear, but it runs once per line at load rather than once per execution, so the
//   table stays in source order (which groups related ops) instead of being sorted.
static uint8_t appOpcodeFromName(const String& name) {
    for (uint8_t i = 1; i < DAPP_OP_COUNT; i++) {
        if (name == DAPP_OPCODE_NAMES[i]) {
            return i;
        }
    }
    return DAPP_OP_UNKNOWN;
}

//   Which argument of a jump-taking instruction holds the label, so the resolve pass below
//   knows where to look. -1 for everything else.
static int appJumpLabelArg(uint8_t opcode) {
    switch (opcode) {
        case DAPP_OP_GOTO:
        case DAPP_OP_GOSUB: return 0;
        case DAPP_OP_IFEQ:
        case DAPP_OP_IFNE:  return 3;
        case DAPP_OP_IF:    return 4;
        default:            return -1;
    }
}

static bool appLoad(File& file, DappProgram& program) {
    DappLine* lines = program.lines;
    DappLabel* labels = program.labels;
    int& lineCount = program.lineCount;
    int& labelCount = program.labelCount;
    bool reachedExecutable = false;
    lineCount = 0;
    labelCount = 0;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        if (line.endsWith("\r")) {
            line.remove(line.length() - 1);
        }

        if (lineCount >= DAPP_MAX_LINES) {
            outLine("run: too many app lines (max " + String(DAPP_MAX_LINES) + ")", C_RED);
            return false;
        }

        DappLine& record = lines[lineCount];
        record.opcode = DAPP_OP_UNKNOWN;
        record.jumpTarget = -1;

        //trimmed once here rather than on every execution of the line -- appExecute used to
        //re-trim, re-split and re-uppercase the same text every time it came round a loop
        String trimmed = trimCopy(line);
        if (isAppCommentOrBlank(trimmed) || trimmed.startsWith(":")) {
            if (isAppCommentOrBlank(trimmed)) {
                if (!reachedExecutable) {
                    appApplyMetadataDirective(trimmed, program);
                }
            } else {
                reachedExecutable = true;
                if (labelCount < DAPP_MAX_LABELS) {
                    labels[labelCount++] = { trimmed.substring(1), lineCount };
                }
            }
            //blanks, comments and ':' labels all execute as no-ops
            record.opcode = DAPP_OP_LABEL;
        } else {
            reachedExecutable = true;
            int space = trimmed.indexOf(' ');
            String opName = (space >= 0) ? trimmed.substring(0, space) : trimmed;
            opName.toUpperCase();
            record.arg = (space >= 0) ? trimCopy(trimmed.substring(space + 1)) : String("");
            record.opcode = appOpcodeFromName(opName);

            if (record.opcode == DAPP_OP_UNKNOWN) {
                //kept only for the runtime error message, which still fires when -- and only
                //when -- the line is actually reached, exactly as it did before
                record.opText = opName;
            } else if (record.opcode == DAPP_OP_LABEL && space >= 0 && labelCount < DAPP_MAX_LABELS) {
                labels[labelCount++] = { record.arg, lineCount };
            }
        }

        lineCount++;
        if ((lineCount & 0x1f) == 0) appRuntimeYield(false);
    }

    //   Second pass: a GOTO may name a label defined further down, so targets can only be
    //   resolved once every label is known. This is what turns a jump from a linear scan of
    //   every label -- with a String compare each -- into an array index.
    //
    //   A label that does not resolve is deliberately left at -1 rather than reported here.
    //   Doing otherwise would break an app whose dead branch names a missing label, which
    //   used to run fine; the runtime path falls back to appFindLabel and reports it there,
    //   only if the line is ever reached.
    for (int i = 0; i < lineCount; i++) {
        int labelArg = appJumpLabelArg(lines[i].opcode);
        if (labelArg < 0) {
            continue;
        }
        String parts[5];
        int count = splitCommand(lines[i].arg, parts, 5);
        if (labelArg == 0) {
            lines[i].jumpTarget = (int16_t)appFindLabel(labels, labelCount, lines[i].arg);
        } else if (count > labelArg) {
            lines[i].jumpTarget = (int16_t)appFindLabel(labels, labelCount, parts[labelArg]);
        }
        if ((i & 0x1f) == 0) appRuntimeYield(false);
    }

    return true;
}

static bool appExecute(DappProgram& program) {
    DappLine* lines = program.lines;
    DappLabel* labels = program.labels;
    DappStringVar* stringVars = program.stringVars;
    const int lineCount = program.lineCount;
    const int labelCount = program.labelCount;
    int pc = 0;
    long steps = 0;
    int color = C_WHITE;

    while (pc >= 0 && pc < lineCount) {
        if (dappAbort) {
            outLine("run: aborted (^X)", C_YELLOW);
            return false;
        }
        if (++steps > DAPP_MAX_STEPS) {
            outLine("run: stopped after " + String(DAPP_MAX_STEPS) + " steps without a WAIT (possible loop)", C_RED);
            return false;
        }
        if (steps % DAPP_STEPS_PER_YIELD == 0) {
            appRuntimeYield();   //service the board so a long loop can't starve the watchdog
        }

        //   All of this -- trim, split, uppercase, match -- happened at load. What is left
        //   per instruction is an array index and an integer compare, where it used to be
        //   four String allocations before the opcode had even been recognised.
        const DappLine& current = lines[pc];
        const uint8_t op = current.opcode;
        const String& arg = current.arg;
        const int16_t jumpTarget = current.jumpTarget;
        pc++;
        const int lineNumber = pc;   //1-based, captured before any jump moves pc

        if (op == DAPP_OP_LABEL) {
            continue;
        } else if (op == DAPP_OP_PRINT || op == DAPP_OP_ECHO) {
            outLine(appExpandText(arg, program), color);
        } else if (op == DAPP_OP_COLOR) {
            color = appColorByName(arg);
        } else if (op == DAPP_OP_LED) {
            String parts[3];
            int count = splitCommand(arg, parts, 3);
            if (count < 3) {
                outLine("run: LED needs <red> <green> <blue>", C_RED);
                return false;
            }
            if (!rearLedAvailable()) {
                outLine("run: LED unavailable on this build (set REAR_RGB_LED_PIN)", C_RED);
                return false;
            }
            ledSetAppOverrideRgbLong(appValueOf(parts[0], program),
                                     appValueOf(parts[1], program),
                                     appValueOf(parts[2], program));
        } else if (op == DAPP_OP_WAVE) {
            String parts[4];
            int count = splitCommand(arg, parts, 4);
            if (count < 4) {
                outLine("run: WAVE needs <channel> sine|triangle|square|sawtooth|noise|off <hz> <level>", C_RED);
                return false;
            }
            int channel = (int)appValueOf(parts[0], program);
            String waveform = appStringOperand(parts[1], program);
            long frequency = appValueOf(parts[2], program);
            long level = appValueOf(parts[3], program);
            String loweredWaveform = waveform;
            loweredWaveform.toLowerCase();
            bool validWaveform = loweredWaveform == "off" || loweredWaveform == "sine" ||
                loweredWaveform == "sin" || loweredWaveform == "triangle" ||
                loweredWaveform == "tri" || loweredWaveform == "square" ||
                loweredWaveform == "sq" || loweredWaveform == "sawtooth" ||
                loweredWaveform == "saw" || loweredWaveform == "noise";
            if (channel < 1 || channel > 3 || !validWaveform ||
                frequency < 1 || frequency > 12000 || level < 0 || level > 100) {
                outLine("run: WAVE needs channel 1..3, hz 1..12000, level 0..100, and a valid waveform", C_RED);
                return false;
            }
            dappSynthSetChannel(channel, waveform, frequency, level);
        } else if (op == DAPP_OP_WAVESTOP) {
            dappSynthEnd();
        } else if (op == DAPP_OP_CLEAR || op == DAPP_OP_CLS) {
            //while a canvas is up this means "blank the grid", not "wipe the scrollback the
            //canvas is drawn over" -- the latter would be visible only after ENDCANVAS
            if (dappCanvasActive) {
                appCanvasClear();
            } else {
                outClearScreen();
            }
        } else if (op == DAPP_OP_WAIT || op == DAPP_OP_SLEEP) {
            unsigned long waitMs = (unsigned long)appValueOf(arg, program);
            appDelay(waitMs);
            if (waitMs > 0) {
                steps = 0;   //a paced loop isn't a runaway one -- see DAPP_MAX_STEPS
            }
        } else if (op == DAPP_OP_SET || op == DAPP_OP_ADD || op == DAPP_OP_SUB || op == DAPP_OP_MUL || op == DAPP_OP_DIV || op == DAPP_OP_MOD) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: " + String(appOpcodeName(op)) + " needs <name> <value>", C_RED);
                return false;
            }
            long value = appValueOf(parts[1], program);
            if ((op == DAPP_OP_DIV || op == DAPP_OP_MOD) && value == 0) {
                outLine("run: " + String(appOpcodeName(op)) + " by zero", C_RED);
                return false;
            }
            long* target = appNumericTarget(program, parts[0]);
            if (!target) {
                continue;   //fault recorded; the check at the bottom of the loop reports it
            }
            if (op == DAPP_OP_SET) *target = value;
            else if (op == DAPP_OP_ADD) *target += value;
            else if (op == DAPP_OP_SUB) *target -= value;
            else if (op == DAPP_OP_MUL) *target *= value;
            else if (op == DAPP_OP_DIV) *target /= value;
            else *target %= value;
        } else if (op == DAPP_OP_EXPR) {
            //not split with splitCommand: the expression is the whole rest of the line,
            //spaces and all (tinyexpr skips whitespace itself), so it is taken verbatim
            int split = arg.indexOf(' ');
            if (split < 0) {
                outLine("run: EXPR needs <name> <expression>", C_RED);
                return false;
            }
            String targetToken = arg.substring(0, split);
            String expression = appExpandNumericText(trimCopy(arg.substring(split + 1)), program);
            if (program.fault.length() > 0) {
                continue;
            }

            int err = 0;
            double result = te_interp(expression.c_str(), &err);
            if (err != 0) {
                outLine("run: EXPR cannot evaluate: " + expression, C_RED);
                return false;
            }
            long* target = appNumericTarget(program, targetToken);
            if (!target) {
                continue;
            }
            *target = (long)(result >= 0 ? result + 0.5 : result - 0.5);
        } else if (op == DAPP_OP_DIM) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: DIM needs <name> <size>", C_RED);
                return false;
            }
            if (!appDimArray(program, parts[0], appValueOf(parts[1], program))) {
                return false;
            }
        } else if (op == DAPP_OP_LIFE) {
            String parts[4];
            int count = splitCommand(arg, parts, 4);
            if (count < 4) {
                outLine("run: LIFE needs <current-array> <next-array> <cols> <rows>", C_RED);
                return false;
            }
            if (!appLifeStep(program, parts[0], parts[1],
                             (int)appValueOf(parts[2], program),
                             (int)appValueOf(parts[3], program))) {
                return false;
            }
            steps = 0;   //native LIFE work yields internally, so it is not a runaway loop
        } else if (op == DAPP_OP_SETSTR) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: SETSTR needs <name> <text>", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            appSetStringValue(stringVars, slot, appExpandText(parts[1], program));
        } else if (op == DAPP_OP_APPEND) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: APPEND needs <name> <text>", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            appAppendStringValue(stringVars, slot, appExpandText(parts[1], program));
        } else if (op == DAPP_OP_CHR) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: CHR needs <name> <code>", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            long code = appValueOf(parts[1], program);
            appSetStringValue(stringVars, slot, (code >= 32 && code < 127) ? String((char)code) : String(" "));
        } else if (op == DAPP_OP_HEX) {
            String parts[3];
            int count = splitCommand(arg, parts, 3);
            if (count < 2) {
                outLine("run: HEX needs <name> <value> [width]", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            long width = count >= 3 ? appValueOf(parts[2], program) : 2;
            if (width < 1 || width > 8) {
                outLine("run: HEX width must be 1..8", C_RED);
                return false;
            }
            char formatted[9];
            snprintf(formatted, sizeof(formatted), "%0*lX", (int)width,
                     (unsigned long)(uint32_t)appValueOf(parts[1], program));
            appSetStringValue(stringVars, slot, String(formatted));
        } else if (op == DAPP_OP_JSONESC || op == DAPP_OP_JSONGET) {
            String parts[3];
            int count = splitCommand(arg, parts, 3);
            if (count < (op == DAPP_OP_JSONESC ? 2 : 3)) {
                outLine("run: " + String(appOpcodeName(op)) + (op == DAPP_OP_JSONESC
                    ? " needs <name> <text>" : " needs <name> <json> <path>"), C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            String result;
            dappJsonOk = op == DAPP_OP_JSONESC
                ? (appJsonEscape(appStringOperand(parts[1], program), result) ? 1 : 0)
                : (appJsonGetPath(appStringOperand(parts[1], program),
                                  appStringOperand(parts[2], program), result) ? 1 : 0);
            appSetStringValue(stringVars, slot, result);
        } else if (op == DAPP_OP_SUBSTR) {
            String parts[4];
            int count = splitCommand(arg, parts, 4);
            if (count < 4) {
                outLine("run: SUBSTR needs <name> <text> <start> <count>", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            String source = appStringOperand(parts[1], program);
            long start = appValueOf(parts[2], program);
            long want = appValueOf(parts[3], program);
            if (start < 0) start = 0;
            if (start > (long)source.length()) start = source.length();
            if (want < 0) want = 0;
            long end = start + want;
            if (end > (long)source.length()) end = source.length();
            appSetStringValue(stringVars, slot, source.substring(start, end));
        } else if (op == DAPP_OP_LEN || op == DAPP_OP_CHARAT) {
            String parts[3];
            int count = splitCommand(arg, parts, 3);
            int needed = (op == DAPP_OP_LEN) ? 2 : 3;
            if (count < needed) {
                outLine("run: " + String(appOpcodeName(op)) + (op == DAPP_OP_LEN ? " needs <name> <text>" : " needs <name> <text> <index>"), C_RED);
                return false;
            }
            String source = appStringOperand(parts[1], program);
            long value = 0;
            if (op == DAPP_OP_LEN) {
                value = source.length();
            } else {
                long index = appValueOf(parts[2], program);
                value = (index >= 0 && index < (long)source.length()) ? (long)(uint8_t)source[index] : 0;
            }
            long* target = appNumericTarget(program, parts[0]);
            if (!target) {
                continue;
            }
            *target = value;
        } else if (op == DAPP_OP_INPUT || op == DAPP_OP_INPUTSECRET) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 1) {
                outLine("run: " + String(appOpcodeName(op)) + " needs <name> [prompt]", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            String prompt = count >= 2 ? appExpandText(parts[1], program) : parts[0] + "> ";
            appSetStringValue(stringVars, slot, appReadInput(prompt, op == DAPP_OP_INPUTSECRET, program.echoInput));
            steps = 0;   //waiting on a human is not a runaway loop
        } else if (op == DAPP_OP_KEY) {
            if (arg.length() == 0) {
                outLine("run: KEY needs <name>", C_RED);
                return false;
            }
            long key = appPollKey();
            long* target = appNumericTarget(program, arg);
            if (!target) {
                continue;
            }
            *target = key;
        } else if (op == DAPP_OP_CANVAS) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: CANVAS needs <cols> <rows>", C_RED);
                return false;
            }
            if (!appCanvasBegin((int)appValueOf(parts[0], program), (int)appValueOf(parts[1], program))) {
                return false;
            }
        } else if (op == DAPP_OP_ENDCANVAS) {
            appCanvasEnd();
        } else if (op == DAPP_OP_PUT) {
            String parts[3];
            int count = splitCommand(arg, parts, 3);
            if (count < 3) {
                outLine("run: PUT needs <col> <row> <text>", C_RED);
                return false;
            }
            if (!dappCanvasActive) {
                outLine("run: PUT needs a CANVAS first", C_RED);
                return false;
            }
            appCanvasPut((int)appValueOf(parts[0], program),
                         (int)appValueOf(parts[1], program),
                         appExpandText(parts[2], program),
                         color);
        } else if (op == DAPP_OP_FLIP) {
            if (!dappCanvasActive) {
                outLine("run: FLIP needs a CANVAS first", C_RED);
                return false;
            }
            appCanvasFlip();
        } else if (op == DAPP_OP_FOPEN) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: FOPEN needs <path> read|write|append|update", C_RED);
                return false;
            }
            String mode = parts[1];
            mode.toLowerCase();
            const char* fsMode;
            bool readable;
            bool writable;
            if (mode == "read" || mode == "r") { fsMode = "r"; readable = true; writable = false; }
            else if (mode == "write" || mode == "w") { fsMode = "w"; readable = false; writable = true; }
            else if (mode == "append" || mode == "a") { fsMode = "a"; readable = false; writable = true; }
            else if (mode == "update" || mode == "rw" || mode == "r+") { fsMode = "r+"; readable = true; writable = true; }
            else {
                outLine("run: FOPEN mode must be read, write, append, or update", C_RED);
                return false;
            }
            appFileOpen(appExpandText(parts[0], program), fsMode, readable, writable);
        } else if (op == DAPP_OP_FCLOSE) {
            appFileClose();
        } else if (op == DAPP_OP_FREAD) {
            if (arg.length() == 0) {
                outLine("run: FREAD needs <name>", C_RED);
                return false;
            }
            if (!dappFileOpen || !dappFileReadable) {
                outLine("run: FREAD needs a file FOPENed for read", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, arg);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            appSetStringValue(stringVars, slot, appFileReadLine());
        } else if (op == DAPP_OP_FREADB) {
            if (arg.length() == 0) {
                outLine("run: FREADB needs <name>", C_RED);
                return false;
            }
            if (!dappFileOpen || !dappFileReadable) {
                outLine("run: FREADB needs a file FOPENed for read or update", C_RED);
                return false;
            }
            long* target = appNumericTarget(program, arg);
            if (!target) continue;
            *target = appFileReadByte();
        } else if (op == DAPP_OP_FWRITE) {
            if (!dappFileOpen || !dappFileWritable) {
                outLine("run: FWRITE needs a file FOPENed for write or append", C_RED);
                return false;
            }
            String text = appExpandText(arg, program);
            ledPulseStorageWrite(dappFileIsSd);
            size_t wrote = dappFile.print(text);
            wrote += dappFile.print("\n");
            if (wrote != text.length() + 1) {
                outLine("run: FWRITE failed (filesystem full?)", C_RED);
                return false;
            }
        } else if (op == DAPP_OP_FWRITEB) {
            if (!dappFileOpen || !dappFileWritable) {
                outLine("run: FWRITEB needs a file FOPENed for write, append, or update", C_RED);
                return false;
            }
            long value = appValueOf(arg, program);
            if (arg.length() == 0 || value < 0 || value > 255) {
                outLine("run: FWRITEB needs one byte value (0..255)", C_RED);
                return false;
            }
            ledPulseStorageWrite(dappFileIsSd);
            if (dappFile.write((uint8_t)value) != 1) {
                outLine("run: FWRITEB failed (filesystem full?)", C_RED);
                return false;
            }
        } else if (op == DAPP_OP_FSEEK) {
            if (!dappFileOpen) {
                outLine("run: FSEEK needs an open file", C_RED);
                return false;
            }
            long offset = appValueOf(arg, program);
            if (arg.length() == 0 || offset < 0) {
                outLine("run: FSEEK needs an absolute offset >= 0", C_RED);
                return false;
            }
            dappFok = dappFile.seek((uint32_t)offset, SeekSet) ? 1 : 0;
            dappFeof = 0;
        } else if (op == DAPP_OP_FTELL || op == DAPP_OP_FSIZE) {
            if (!dappFileOpen) {
                outLine("run: " + String(appOpcodeName(op)) + " needs an open file", C_RED);
                return false;
            }
            if (arg.length() == 0) {
                outLine("run: " + String(appOpcodeName(op)) + " needs <name>", C_RED);
                return false;
            }
            long* target = appNumericTarget(program, arg);
            if (!target) continue;
            *target = (op == DAPP_OP_FTELL) ? (long)dappFile.position() : (long)dappFile.size();
        } else if (op == DAPP_OP_FEXISTS) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: FEXISTS needs <name> <path>", C_RED);
                return false;
            }
            RoutedPath r;
            long exists = 0;
            if (appFileRoute(appExpandText(parts[1], program), r)) {
                ledPulseStorageRead(r.isSd);
                if (r.fs->exists(r.realPath)) {
                    exists = 1;
                }
            }
            long* target = appNumericTarget(program, parts[0]);
            if (!target) {
                continue;
            }
            *target = exists;
        } else if (op == DAPP_OP_FDELETE) {
            if (arg.length() == 0) {
                outLine("run: FDELETE needs <path>", C_RED);
                return false;
            }
            RoutedPath r;
            dappFok = 0;
            if (appFileRoute(appExpandText(arg, program), r)) {
                ledPulseStorageWrite(r.isSd);
                if (r.fs->remove(r.realPath)) {
                    dappFok = 1;
                }
            }
        } else if (op == DAPP_OP_FLIST) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: FLIST needs <directory> <output-file>", C_RED);
                return false;
            }
            appFileListToFile(appStringOperand(parts[0], program), appStringOperand(parts[1], program));
        } else if (op == DAPP_OP_FMKDIR) {
            if (arg.length() == 0) {
                outLine("run: FMKDIR needs <path>", C_RED);
                return false;
            }
            dappFok = dappStorageMkdir(resolvePath(cwd, appStringOperand(arg, program))) ? 1 : 0;
        } else if (op == DAPP_OP_FCOPY || op == DAPP_OP_FMOVE) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: " + String(appOpcodeName(op)) + " needs <source> <destination>", C_RED);
                return false;
            }
            String source = resolvePath(cwd, appStringOperand(parts[0], program));
            String destination = resolvePath(cwd, appStringOperand(parts[1], program));
            dappFok = (op == DAPP_OP_FCOPY ? dappStorageCopy(source, destination)
                                      : dappStorageMove(source, destination)) ? 1 : 0;
        } else if (op == DAPP_OP_HTTPCLEAR) {
            appHttpClearHeaders();
        } else if (op == DAPP_OP_HTTPHEADER) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2 || !appHttpSetHeader(appStringOperand(parts[0], program),
                                                appStringOperand(parts[1], program))) {
                outLine("run: HTTPHEADER needs a safe <name> <value> (max 8 headers)", C_RED);
                return false;
            }
        } else if (op == DAPP_OP_HTTPGET || op == DAPP_OP_HTTPPOST) {
            String parts[4];
            int count = splitCommand(arg, parts, 4);
            int needed = op == DAPP_OP_HTTPGET ? 2 : 3;
            if (count < needed) {
                outLine("run: " + String(appOpcodeName(op)) + (op == DAPP_OP_HTTPGET
                    ? " needs <name> <http-or-https-url> [max-bytes]"
                    : " needs <name> <http-or-https-url> <body> [max-bytes]"), C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            int maximumIndex = op == DAPP_OP_HTTPGET ? 2 : 3;
            long maximum = count > maximumIndex
                ? appValueOf(parts[maximumIndex], program) : DAPP_MAX_STRING_LEN;
            if (maximum < 1 || maximum > DAPP_MAX_STRING_LEN) {
                outLine("run: " + String(appOpcodeName(op)) + " max-bytes must be 1.." + String(DAPP_MAX_STRING_LEN), C_RED);
                return false;
            }
            String response;
            String body = op == DAPP_OP_HTTPPOST ? appStringOperand(parts[2], program) : String("");
            appHttpRequest(op == DAPP_OP_HTTPPOST ? "POST" : "GET",
                           appStringOperand(parts[1], program), body,
                           (size_t)maximum, response);
            appSetStringValue(stringVars, slot, response);
            steps = 0;  //the network wait is a real yield, not a runaway loop
        } else if (op == DAPP_OP_BUFNEW) {
            long bytes = arg.length() ? appValueOf(arg, program) : (long)DAPP_BUF_DEFAULT_BYTES;
            if (bytes < 1 || bytes > (long)DAPP_BUF_MAX_BYTES) {
                outLine("run: BUFNEW size must be 1.." + String((long)DAPP_BUF_MAX_BYTES), C_RED);
                return false;
            }
            dappBufOk = appBufAlloc((size_t)bytes) ? 1 : 0;
        } else if (op == DAPP_OP_BUFFREE) {
            appBufFree();
            dappBufOk = 1;
        } else if (op == DAPP_OP_BUFCLEAR) {
            dappBufLen = 0;
            dappBufOk = dappBuf ? 1 : 0;
        } else if (op == DAPP_OP_BUFAT) {
            String parts[2];
            if (splitCommand(arg, parts, 2) < 2) {
                outLine("run: BUFAT needs <name> <position>", C_RED);
                return false;
            }
            long* target = appNumericTarget(program, parts[0]);
            if (!target) continue;
            long position = appValueOf(parts[1], program);
            //out of range reads 0 rather than stopping: a scan loop tests $buflen itself
            *target = (dappBuf && position >= 0 && position < (long)dappBufLen)
                ? (long)dappBuf[position] : 0;
        } else if (op == DAPP_OP_BUFSUB) {
            String parts[3];
            if (splitCommand(arg, parts, 3) < 3) {
                outLine("run: BUFSUB needs <name> <position> <count>", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            long position = appValueOf(parts[1], program);
            long count = appValueOf(parts[2], program);
            String piece = "";
            if (dappBuf && position >= 0 && position < (long)dappBufLen && count > 0) {
                if (count > DAPP_MAX_STRING_LEN) count = DAPP_MAX_STRING_LEN;
                if (position + count > (long)dappBufLen) count = (long)dappBufLen - position;
                piece.reserve(count);
                for (long i = 0; i < count; i++) {
                    //the buffer holds bytes and a String cannot carry a NUL through the
                    //print path, so a slice stops where the text does
                    char ch = (char)dappBuf[position + i];
                    if (ch == 0) break;
                    piece += ch;
                }
            }
            appSetStringValue(stringVars, slot, piece);
        } else if (op == DAPP_OP_BUFWRITE) {
            String parts[2];
            if (splitCommand(arg, parts, 2) < 2) {
                outLine("run: BUFWRITE needs <position> <text>", C_RED);
                return false;
            }
            long position = appValueOf(parts[0], program);
            String text = appStringOperand(parts[1], program);
            dappBufOk = 0;
            if (dappBuf && position >= 0 && position + (long)text.length() <= (long)dappBufCap) {
                memcpy(dappBuf + position, text.c_str(), text.length());
                if (position + (long)text.length() > (long)dappBufLen) {
                    dappBufLen = position + text.length();
                }
                dappBufOk = 1;
            }
        } else if (op == DAPP_OP_BUFSCAN || op == DAPP_OP_BUFTAKE) {
            //   Token-at-a-time scanning. A per-byte loop in script costs ~25 interpreter
            //   steps per byte; these stop on a character class so the script pays per
            //   token instead, which for most text formats is roughly an order of
            //   magnitude fewer steps for the same walk.
            String parts[4];
            int count = splitCommand(arg, parts, 4);
            bool take = op == DAPP_OP_BUFTAKE;
            int needed = take ? 3 : 2;
            if (count < needed) {
                outLine(take ? "run: BUFTAKE needs <strname> <numname> <position> [stopset]"
                             : "run: BUFSCAN needs <numname> <position> [stopset]", C_RED);
                return false;
            }
            int slot = take ? appEnsureStringVar(stringVars, parts[0]) : -1;
            if (take && slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            long* target = appNumericTarget(program, parts[take ? 1 : 0]);
            if (!target) continue;
            long position = appValueOf(parts[take ? 2 : 1], program);
            //an empty stop set means whitespace, which is what most scans want
            String stops = count > needed ? appStringOperand(parts[needed], program) : String("");

            if (position < 0) position = 0;
            String piece = "";
            while (dappBuf && position < (long)dappBufLen) {
                uint8_t byte = dappBuf[position];
                bool stop = stops.length() == 0 ? (byte <= 32) : (stops.indexOf((char)byte) >= 0);
                if (stop) break;
                if (take && piece.length() < DAPP_MAX_STRING_LEN) piece += (char)byte;
                position++;
            }
            *target = position;
            if (take) appSetStringValue(stringVars, slot, piece);
        } else if (op == DAPP_OP_BUFSAVE || op == DAPP_OP_BUFLOAD) {
            if (arg.length() == 0) {
                outLine("run: " + String(appOpcodeName(op)) + " needs <path>", C_RED);
                return false;
            }
            RoutedPath r;
            dappBufOk = 0;
            if (dappFileOpen) {
                outLine("run: " + String(appOpcodeName(op)) + " needs the script's file handle closed (FCLOSE)", C_RED);
                return false;
            }
            if (dappBuf && appFileRoute(appExpandText(arg, program), r)) {
                if (op == DAPP_OP_BUFSAVE) {
                    ledPulseStorageWrite(r.isSd);
                    File f = r.fs->open(r.realPath, "w");
                    if (f && !f.isDirectory()) {
                        dappBufOk = f.write(dappBuf, dappBufLen) == dappBufLen ? 1 : 0;
                        f.close();
                    }
                } else {
                    ledPulseStorageRead(r.isSd);
                    File f = r.fs->open(r.realPath, "r");
                    if (f && !f.isDirectory()) {
                        size_t want = f.size() < dappBufCap ? f.size() : dappBufCap;
                        dappBufLen = f.read(dappBuf, want);
                        dappBufOk = 1;
                        f.close();
                    }
                }
            }
        } else if (op == DAPP_OP_HTTPGETBUF) {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 1) {
                outLine("run: HTTPGETBUF needs <http-or-https-url> [max-bytes]", C_RED);
                return false;
            }
            if (!dappBuf && !appBufAlloc(DAPP_BUF_DEFAULT_BYTES)) {
                outLine("run: HTTPGETBUF could not allocate a buffer", C_RED);
                return false;
            }
            long maximum = count > 1 ? appValueOf(parts[1], program) : (long)dappBufCap;
            if (maximum < 1 || maximum > (long)dappBufCap) maximum = (long)dappBufCap;

            dappBufLen = 0;
            size_t savedCap = dappBufCap;
            dappBufCap = (size_t)maximum;   //the sink stops at the caller's limit, not the block's
            DappBufSink sink;
            appHttpPerform("GET", appStringOperand(parts[0], program), "", &sink);
            dappBufCap = savedCap;
            dappHttpLength = (long)dappBufLen;
            dappHttpTruncated = sink.truncated ? 1 : 0;
            //a short write left bytes unread on the socket, so it cannot be pooled
            if (sink.truncated) appHttpSessionEnd();
            steps = 0;
        } else if (op == DAPP_OP_URLABS) {
            String parts[3];
            if (splitCommand(arg, parts, 3) < 3) {
                outLine("run: URLABS needs <name> <base-url> <href>", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            //"" means "not a followable http(s) target" -- the caller tests the result
            appSetStringValue(stringVars, slot,
                appHtmlResolveUrl(appStringOperand(parts[1], program),
                                  appStringOperand(parts[2], program)));
        } else if (op == DAPP_OP_URLPART) {
            String parts[3];
            if (splitCommand(arg, parts, 3) < 3) {
                outLine("run: URLPART needs <name> <url> scheme|origin|host|path|dir", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            String scheme, origin, path;
            appUrlSplit(appStringOperand(parts[1], program), scheme, origin, path);
            String which = appStringOperand(parts[2], program);
            which.toLowerCase();
            String value = "";
            if (which == "scheme") value = scheme;
            else if (which == "origin") value = origin;
            else if (which == "host") value = origin.length() > scheme.length() + 3
                ? origin.substring(scheme.length() + 3) : String("");
            else if (which == "path") value = path;
            else if (which == "dir") {
                int q = path.indexOf('?');
                if (q >= 0) path = path.substring(0, q);
                int lastSlash = path.lastIndexOf('/');
                value = origin + (lastSlash >= 0 ? path.substring(0, lastSlash + 1) : String("/"));
            } else {
                outLine("run: URLPART part must be scheme, origin, host, path or dir", C_RED);
                return false;
            }
            appSetStringValue(stringVars, slot, value);
        } else if (op == DAPP_OP_HTMLOPEN) {
            String parts[5];
            int count = splitCommand(arg, parts, 5);
            if (count < 3) {
                outLine("run: HTMLOPEN needs <textpath> <linkpath|-> <base-url> [wrapcol] [maxlinks]", C_RED);
                return false;
            }
            //   The runtime has one script file handle and the renderer needs two of its
            //   own. Rather than interleave them -- which is what browse.dapp had to do,
            //   closing and reopening its spool around every link -- this simply refuses
            //   to start while the script is holding a file.
            if (dappFileOpen) {
                outLine("run: HTMLOPEN needs the script's file handle closed (FCLOSE)", C_RED);
                return false;
            }
            appHtmlClose();
            dappHtmlOk = 0;
            if (!appHtmlBegin(appExpandText(parts[0], program),
                              appExpandText(parts[1], program),
                              appStringOperand(parts[2], program),
                              count > 3 ? (int)appValueOf(parts[3], program) : 76,
                              count > 4 ? (int)appValueOf(parts[4], program) : 200)) {
                outLine("run: HTMLOPEN cannot write its output files", C_RED);
                return false;
            }
            dappHtmlOk = 1;
        } else if (op == DAPP_OP_HTMLFEED) {
            String parts[3];
            int count = splitCommand(arg, parts, 3);
            if (count < 1 || !dappHtml.active) {
                outLine(dappHtml.active
                    ? "run: HTMLFEED needs url <url> | buf [pos count] | text <string>"
                    : "run: HTMLFEED without HTMLOPEN", C_RED);
                return false;
            }
            String kind = parts[0];
            kind.toLowerCase();
            if (kind == "url") {
                if (count < 2) {
                    outLine("run: HTMLFEED url needs <url>", C_RED);
                    return false;
                }
                DappHtmlSink sink;
                appHttpPerform("GET", appStringOperand(parts[1], program), "", &sink);
                dappHttpLength = dappHtmlBytes;
                steps = 0;
            } else if (kind == "buf") {
                long position = count > 1 ? appValueOf(parts[1], program) : 0;
                long length = count > 2 ? appValueOf(parts[2], program)
                                        : (long)dappBufLen - position;
                if (position < 0) position = 0;
                if (position + length > (long)dappBufLen) length = (long)dappBufLen - position;
                if (dappBuf && length > 0) dappHtml.feed(dappBuf + position, (size_t)length);
            } else if (kind == "text") {
                if (count < 2) {
                    outLine("run: HTMLFEED text needs <string>", C_RED);
                    return false;
                }
                String text = appStringOperand(parts[1], program);
                dappHtml.feed((const uint8_t*)text.c_str(), text.length());
            } else {
                outLine("run: HTMLFEED source must be url, buf or text", C_RED);
                return false;
            }
        } else if (op == DAPP_OP_HTMLCLOSE) {
            appHtmlClose();
        } else if (op == DAPP_OP_HTMLTEXT) {
            //   The whole browser fetch in one line: connect, stream the body through the
            //   renderer, close. Nothing is buffered in between, so the page size is
            //   bounded by the filesystem rather than by anything allocated here -- which
            //   is what makes the ranged-window walk browse.dapp used unnecessary.
            String parts[5];
            int count = splitCommand(arg, parts, 5);
            if (count < 3) {
                outLine("run: HTMLTEXT needs <url> <textpath> <linkpath|-> [wrapcol] [maxlinks]", C_RED);
                return false;
            }
            if (dappFileOpen) {
                outLine("run: HTMLTEXT needs the script's file handle closed (FCLOSE)", C_RED);
                return false;
            }
            String url = appStringOperand(parts[0], program);
            appHtmlClose();
            dappHtmlOk = 0;
            //the page's own URL is the base every relative href resolves against
            if (!appHtmlBegin(appExpandText(parts[1], program),
                              appExpandText(parts[2], program), url,
                              count > 3 ? (int)appValueOf(parts[3], program) : 76,
                              count > 4 ? (int)appValueOf(parts[4], program) : 200)) {
                outLine("run: HTMLTEXT cannot write its output files", C_RED);
                return false;
            }
            dappHtmlOk = 1;
            DappHtmlSink sink;
            appHttpPerform("GET", url, "", &sink);
            dappHttpLength = dappHtmlBytes;
            appHtmlClose();
            steps = 0;
        } else if (op == DAPP_OP_HTMLSTR) {
            String parts[3];
            int count = splitCommand(arg, parts, 3);
            if (count < 2) {
                outLine("run: HTMLSTR needs <name> url <url> | buf | text <string>", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            appHtmlClose();
            dappHtml.reset();
            dappHtml.toString = true;
            dappHtml.haveText = false;
            dappHtml.haveLink = false;   //links need a file to number against
            dappHtml.wrapcol = 76;
            dappHtml.active = true;
            dappHtmlOk = 1;

            String kind = parts[1];
            kind.toLowerCase();
            if (kind == "url") {
                if (count < 3) {
                    outLine("run: HTMLSTR url needs <url>", C_RED);
                    return false;
                }
                dappHtml.base = appStringOperand(parts[2], program);
                DappHtmlSink sink;
                appHttpPerform("GET", dappHtml.base, "", &sink);
                steps = 0;
            } else if (kind == "buf") {
                if (dappBuf) dappHtml.feed(dappBuf, dappBufLen);
            } else if (kind == "text") {
                if (count < 3) {
                    outLine("run: HTMLSTR text needs <string>", C_RED);
                    return false;
                }
                String text = appStringOperand(parts[2], program);
                dappHtml.feed((const uint8_t*)text.c_str(), text.length());
            } else {
                dappHtml.active = false;
                outLine("run: HTMLSTR source must be url, buf or text", C_RED);
                return false;
            }
            dappHtml.flushWord();
            dappHtml.flushLine();
            dappHtml.active = false;
            appSetStringValue(stringVars, slot, dappHtml.out);
            dappHtml.out = "";
        } else if (op == DAPP_OP_DAPPER) {
            //A deliberately narrow bridge to the verified package manager. This is not
            //a shell escape: the only reachable actions are the subcommands accepted by
            //handleDapperCommand(), so package validation, rollback and path ownership
            //stay in Dapper.ino rather than being reimplemented in script.
            String commandLine = "dapper ";
            commandLine += appExpandText(arg, program);
            String commandParts[8];
            int commandPartCount = splitCommand(commandLine, commandParts, 8);
            handleDapperCommand(commandParts, commandPartCount);
            steps = 0;  //downloads and catalog walks service the runtime internally
        } else if (op == DAPP_OP_TIME) {
            //NTP sync (ntpEnsureClock, SysInfo.ino -- shared with Dapper's own HTTPS
            //certificate-date check) broken down with gmtime_r into $time* fields. UTC:
            //this board has no timezone setting, so scripts wanting local time do their
            //own offset with ADD/SUB. No WiFi precheck, same as HTTPGET/HTTPPOST -- it
            //just times out into $timeok=0 if there's no network, rather than a second
            //error path to keep in sync with the real one.
            dappTimeOk = 0;
            String timeError;
            if (ntpEnsureClock(timeError, 8000, appRuntimeYield)) {
                time_t now = time(nullptr);
                struct tm timeInfo;
                gmtime_r(&now, &timeInfo);
                dappTimeOk = 1;
                dappTimeEpoch = (long)now;
                dappTimeYear = timeInfo.tm_year + 1900;
                dappTimeMonth = timeInfo.tm_mon + 1;
                dappTimeDay = timeInfo.tm_mday;
                dappTimeHour = timeInfo.tm_hour;
                dappTimeMinute = timeInfo.tm_min;
                dappTimeSecond = timeInfo.tm_sec;
                dappTimeWeekday = timeInfo.tm_wday;
            }
            steps = 0;  //the NTP wait is a real yield, not a runaway loop
        } else if (op == DAPP_OP_RAND) {
            String parts[3];
            int count = splitCommand(arg, parts, 3);
            if (count < 2) {
                outLine("run: RAND needs <name> <max> or <name> <min> <max>", C_RED);
                return false;
            }

            long value = 0;
            if (count == 2) {
                long maxExclusive = appValueOf(parts[1], program);
                if (maxExclusive <= 0) {
                    outLine("run: RAND max must be greater than 0", C_RED);
                    return false;
                }
                value = appRandomRange(0, maxExclusive - 1);
            } else {
                value = appRandomRange(appValueOf(parts[1], program), appValueOf(parts[2], program));
            }

            long* target = appNumericTarget(program, parts[0]);
            if (!target) {
                continue;
            }
            *target = value;
        } else if (op == DAPP_OP_GOTO) {
            //resolved at load; the scan only runs for a label that did not resolve, which is
            //an error path anyway
            int target = (jumpTarget >= 0) ? jumpTarget : appFindLabel(labels, labelCount, arg);
            if (target < 0) {
                outLine("run: label not found: " + arg, C_RED);
                return false;
            }
            pc = target;
        } else if (op == DAPP_OP_GOSUB) {
            int target = (jumpTarget >= 0) ? jumpTarget : appFindLabel(labels, labelCount, arg);
            if (target < 0) {
                outLine("run: label not found: " + arg, C_RED);
                return false;
            }
            if (program.callDepth >= DAPP_MAX_CALL_DEPTH) {
                outLine("run: GOSUB nested deeper than " + String(DAPP_MAX_CALL_DEPTH) +
                        " (a RETURN is probably missing)", C_RED);
                return false;
            }
            //pc has already advanced past the GOSUB, so this is where RETURN resumes
            program.callStack[program.callDepth++] = pc;
            pc = target;
        } else if (op == DAPP_OP_RETURN) {
            if (program.callDepth <= 0) {
                outLine("run: RETURN without GOSUB", C_RED);
                return false;
            }
            pc = program.callStack[--program.callDepth];
        } else if (op == DAPP_OP_IF) {
            String parts[5];
            int count = splitCommand(arg, parts, 5);
            String jumpOp = (count >= 4) ? parts[3] : "";
            jumpOp.toUpperCase();
            if (count < 5 || (jumpOp != "GOTO" && jumpOp != "GOSUB")) {
                outLine("run: IF syntax is IF <left> <op> <right> GOTO|GOSUB <label>", C_RED);
                return false;
            }
            if (appCompare(appValueOf(parts[0], program), parts[1], appValueOf(parts[2], program))) {
                int target = (jumpTarget >= 0) ? jumpTarget : appFindLabel(labels, labelCount, parts[4]);
                if (target < 0) {
                    outLine("run: label not found: " + parts[4], C_RED);
                    return false;
                }
                if (jumpOp == "GOSUB") {
                    if (program.callDepth >= DAPP_MAX_CALL_DEPTH) {
                        outLine("run: GOSUB nested deeper than " + String(DAPP_MAX_CALL_DEPTH), C_RED);
                        return false;
                    }
                    program.callStack[program.callDepth++] = pc;
                }
                pc = target;
            }
        } else if (op == DAPP_OP_IFEQ || op == DAPP_OP_IFNE) {
            String parts[4];
            int count = splitCommand(arg, parts, 4);
            String jumpOp = (count >= 3) ? parts[2] : "";
            jumpOp.toUpperCase();
            if (count < 4 || (jumpOp != "GOTO" && jumpOp != "GOSUB")) {
                outLine("run: " + String(appOpcodeName(op)) + " syntax is " + String(appOpcodeName(op)) + " <left> <right> GOTO|GOSUB <label>", C_RED);
                return false;
            }
            bool equal = appStringOperand(parts[0], program) == appStringOperand(parts[1], program);
            if ((op == DAPP_OP_IFEQ && equal) || (op == DAPP_OP_IFNE && !equal)) {
                int target = (jumpTarget >= 0) ? jumpTarget : appFindLabel(labels, labelCount, parts[3]);
                if (target < 0) {
                    outLine("run: label not found: " + parts[3], C_RED);
                    return false;
                }
                if (jumpOp == "GOSUB") {
                    if (program.callDepth >= DAPP_MAX_CALL_DEPTH) {
                        outLine("run: GOSUB nested deeper than " + String(DAPP_MAX_CALL_DEPTH), C_RED);
                        return false;
                    }
                    program.callStack[program.callDepth++] = pc;
                }
                pc = target;
            }
        } else if (op == DAPP_OP_EXIT || op == DAPP_OP_END) {
            return true;
        } else {
            outLine("run: unknown app command: " + current.opText, C_RED);
            return false;
        }

        //single check for every helper that can only report failure out of band -- an
        //out-of-range array index, or running out of variable slots mid-expression
        if (program.fault.length() > 0) {
            outLine("run: " + program.fault + " (line " + String(lineNumber) + ")", C_RED);
            return false;
        }
    }

    return true;
}

void handleRunCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: run <app|path.dapp>");
        outLine("Upload apps to /sd/apps over FTP, then run <name>.");
        return;
    }

    String nativeTarget = parts[1];
    nativeTarget.toLowerCase();
    if (nativeTarget == "wifi-manager" || nativeTarget == "wifi-manager.app") {
        runWifiManagerApp();
        return;
    }

    File file;
    String resolved;
    if (!appOpenByName(parts[1], file, resolved)) {
        outLine("run: app not found: " + parts[1], C_RED);
        outLine("Try 'apps' to list installed .dapp files.");
        return;
    }

    DappProgram program;
    if (!program.alloc()) {
        file.close();
        outLine("run: not enough memory to load the app", C_RED);
        return;
    }

    outLine("Running " + resolved, C_GREEN);
    ledPulseStorageRead(resolved.startsWith("/sd/"));
    bool loaded = appLoad(file, program);
    file.close();
    if (!loaded) {
        ledClearAppOverride();
        return;
    }

    //stale bytes from before the app started would arrive as its first keypresses
    appResetKeyState();
    dappFok = 0;
    dappFeof = 0;
    dappHttpCode = 0;
    dappHttpLength = 0;
    dappHttpTruncated = 0;
    dappHttpOk = 0;
    dappJsonOk = 0;
    dappBufOk = 0;
    dappHtmlOk = 0;
    dappHtmlLines = 0;
    dappHtmlLinks = 0;
    dappHtmlBytes = 0;
    appHttpClearHeaders();

    bool ok = appExecute(program);

    //unconditional: an app that faulted mid-frame still has to hand the terminal back,
    //and one that stopped mid-read must not leave a dangling handle
    appCanvasEnd();
    appFileClose();
    appHtmlClose();
    appBufFree();
    //the pooled socket belongs to the app that opened it: the next one gets a clean
    //connection rather than inheriting whatever origin this was still talking to
    appHttpSessionEnd();
    dappSynthEnd();
    appHttpClearHeaders();
    ledClearAppOverride();
    outLine(ok ? "[app exited]" : "[app stopped]", ok ? C_GREEN : C_RED);
}
