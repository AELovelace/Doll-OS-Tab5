# AppRunner optimization plan

> **Status: most of this is implemented.** See "What shipped" at the bottom for what landed,
> what changed from the plan, and what is still outstanding. The analysis below is kept as
> written because it is the reasoning the changes were made from.

Goal: make bigger `.dapp` apps *feel* fast on device. Two separate things contribute to
"feel", and they need different fixes:

1. **Interpreter throughput** — how many script lines/second `appExecute` retires. This is
   what makes a 200-line-per-frame game loop crawl.
2. **Frame latency** — how long `FLIP` takes to become visible, and how promptly `KEY`
   notices a keypress.

The web emulator (`dapp-web/dapp-runtime.js`) is *not* the problem: it already resolves
labels through a `Map` ([dapp-runtime.js:742](dapp-web/dapp-runtime.js#L742)) and runs on a
real CPU. Everything below is about `AppRunner.ino` on the ESP32.

---

## Where the time actually goes

### Finding 1 — the source text is re-parsed on every execution of every line

[AppRunner.ino:2491-2501](AppRunner.ino#L2491-L2501) runs per instruction:

```cpp
String line = trimCopy(lines[pc].text);          // copy #1 (trimCopy takes String by value)
int space = line.indexOf(' ');
String op  = (space >= 0) ? line.substring(0, space) : line;   // copy #2
String arg = (space >= 0) ? trimCopy(line.substring(space + 1)) : "";  // copies #3, #4
op.toUpperCase();
```

That is ~4 heap allocations before a single opcode has been recognised, and it happens
identically every time the line is reached. A `GOTO` at the top of a game loop pays this
cost thousands of times to re-derive a result that was fixed the moment the file loaded.

Then the opcode's own handler allocates again. `splitCommand`
([CommandProcessor.ino:6](CommandProcessor.ino#L6)) starts with `String working = input;`
and produces one `substring` per part. `appValueOf`
([AppRunner.ino:706](AppRunner.ino#L706)) takes its token **by value** and immediately calls
`stripMatchingQuotes`, which also takes **by value**. So do `trimCopy`
([AppRunner.ino:177](AppRunner.ino#L177)), `appFindLabel`
([AppRunner.ino:373](AppRunner.ino#L373)), `appBuiltinValue`
([AppRunner.ino:666](AppRunner.ino#L666)), `appStringValueOf`
([AppRunner.ino:730](AppRunner.ino#L730)), `appStringOperand`
([AppRunner.ino:849](AppRunner.ino#L849)), and `appNumericTarget`
([AppRunner.ino:867](AppRunner.ino#L867)).

Tallying one `IF $x > 10 GOTO loop`: ~20-25 malloc/free pairs and 100+ string comparisons
for what is morally a compare-and-jump. On the ESP32's locking best-fit allocator that is
the dominant cost of the instruction.

### Finding 2 — the hottest opcodes are at the bottom of a ~65-branch string chain

The dispatch is a linear `else if (op == "...")` chain. Counting individual `==` tests,
the ops a game loop leans on land here:

| opcode | position in chain | line |
|---|---|---|
| `PUT` | ~28 | [2792](AppRunner.ino#L2792) |
| `FLIP` | ~29 | [2807](AppRunner.ino#L2807) |
| `GOTO` | ~70 | [3402](AppRunner.ino#L3402) |
| `GOSUB` | ~71 | [3409](AppRunner.ino#L3409) |
| `IF` | ~75 | [3429](AppRunner.ino#L3429) |
| `IFEQ`/`IFNE` | ~77 | [3453](AppRunner.ino#L3453) |

Meanwhile `LED`, `WAVE`, `HTTPGET`, and the whole `HTML*`/`BUF*` family — none of which
appear in an inner loop — are tested first. The ordering is exactly inverted.

### Finding 3 — every name lookup is a linear scan with String compares

- `appFindVar` ([AppRunner.ino:386](AppRunner.ino#L386)) walks all **64** slots every call
  and does not stop at the first unused one.
- `appFindLabel` ([AppRunner.ino:373](AppRunner.ino#L373)) copies the name, trims it, then
  linearly compares against every label. A 100-label app pays 100 string compares per jump.
- `appFindArray` ([AppRunner.ino:472](AppRunner.ino#L472)) scans 16 slots — and it is called
  *from inside* `appArrayCell`, i.e. on every single array element read and write. A
  `board[$i]` in a nested loop re-resolves the name "board" each time.

### Finding 4 — `appRuntimeYield` unconditionally sleeps 1ms

[AppRunner.ino:68-83](AppRunner.ino#L68-L83) ends with `delay(1)` on **both** paths,
including `appRuntimeYield(false)` which is supposed to be the cheap "just feed the
watchdog" variant. Callers sprinkle it far more densely than the main loop's every-256-steps
budget:

- `appExpandText` / `appExpandNumericText` — every 128 characters
  ([807](AppRunner.ino#L807), [831](AppRunner.ino#L831))
- `appCanvasPut` — every 128 characters ([974](AppRunner.ino#L974))
- `appCanvasFlip` — every 256 cells ([998](AppRunner.ino#L998))

So an 800-cell canvas flip burns ~3ms of pure `delay` before anything is drawn, and any
`PRINT` of a long string stalls a millisecond per 128 chars. This is latency added for no
scheduling benefit — a `delay(1)` is not what feeds the watchdog, `esp_task_wdt_reset()` is.

### Finding 5 — `FLIP` redraws and blits the entire panel

`appCanvasFlip` ([981](AppRunner.ino#L981)) calls `markDisplayDirty()` + `drawDisplayFrame()`,
which redraws the status bar, the full canvas grid, and the command bar into the sprite,
then pushes **the whole 480x320x16bpp = 300KB sprite** over the bus
([Display.ino:723](Display.ino#L723), [Display.ino:17](Display.ino#L17)). There is no
dirty-rect tracking. A game that moves one character pays for a full-screen repaint.

The per-cell inner loop ([Display.ino:702-716](Display.ino#L702-L716)) also calls
`setTextColor` + `drawString` per non-blank cell, with no run-batching.

### Finding 6 — string ops that are quietly O(n²)

- `appSetStringValue` ([437](AppRunner.ino#L437)) does
  `vars[slot].value = value.substring(0, DAPP_MAX_STRING_LEN)` — a full allocate-and-copy on
  **every** assignment, even for a 3-character value that could never exceed the cap.
- `APPEND` ([2657](AppRunner.ino#L2657)) evaluates `stringVars[slot].value + expand(...)`,
  building a whole new string, then hands it to `appSetStringValue` which copies it *again*.
  Appending to a log line in a loop is quadratic with a 2x constant.
- `appExpandText` ([803](AppRunner.ino#L803)) builds its result with `out += text[i]` one
  character at a time, with no `reserve()` — so a long `PRINT` reallocs repeatedly.

---

## Phase 0 — measure before changing anything

Nothing below should be trusted without numbers from the actual board. Add a `PROFILE`
shell command (or a `#define DAPP_PROFILE`) that wraps the `appExecute` loop body with
`esp_timer_get_time()` and accumulates, per opcode: call count and total µs. Dump a sorted
table on `EXIT`.

That single table tells you whether a given app is interpreter-bound or `FLIP`-bound, which
decides whether Phase 1-2 or Phase 3 is the priority for it. Use `grotto2.dapp` (22KB, the
largest) and `tetris.dapp` as the benchmarks, and record a lines/second baseline so every
later phase can be quoted as a multiple of it.

---

## Phase 1 — cheap wins, no format or behaviour change

These are mechanical, individually safe, and reviewable in isolation.

1. **Pass Strings by `const&`.** Change `trimCopy`, `stripMatchingQuotes`, `appValueOf`,
   `appStringValueOf`, `appStringOperand`, `appNumericTarget`, `appFindLabel`, and
   `appBuiltinValue` to take `const String&`. Where the body mutates its parameter (they
   all currently do — `token.trim()`, `name.toLowerCase()`), restructure to compute
   offsets into the original instead of mutating a copy. This alone removes roughly a third
   of the allocations per instruction.

2. **Trim once, at load.** `appLoad` ([2419](AppRunner.ino#L2419)) already computes
   `trimmed` for its label scan — store *that* in `lines[].text` instead of the raw line,
   and delete the `trimCopy` at [2491](AppRunner.ino#L2491). Metadata directives are parsed
   in the same pass, so nothing downstream needs the untrimmed text.

3. **Reorder the dispatch chain** so `GOTO`, `IF`, `SET`/`ADD`, `PUT`, `IFEQ`, `GOSUB`,
   `RETURN`, `KEY`, `FLIP`, `EXPR`, and `PRINT` are tested first, and the `HTML*`/`BUF*`/
   `HTTP*`/`F*` families last. Zero risk, and it cuts ~70 string compares to ~5 on the
   hottest paths. Do this even though Phase 2 replaces it — it is a five-minute change that
   ships value immediately.

4. **Fix `appRuntimeYield`.** Drop `delay(1)` from the `serviceUi == false` path entirely
   (keep `esp_task_wdt_reset()`). On the `true` path, make the delay conditional on actually
   having been more than a few ms since the last yield, tracked with `millis()`, rather than
   unconditional. Also switch the *callers* from "every N characters/cells" to a time check
   — the character-count triggers in `appExpandText`, `appCanvasPut`, and `appCanvasFlip`
   have no relationship to how long the work actually took.

5. **`appFindVar` early-out.** Break the scan at the first `!used` slot, since
   `appEnsureVar` fills from the front and never frees. Same for `appFindStringVar` and
   `appFindArray`.

6. **`appSetStringValue` fast path.** `if (value.length() <= DAPP_MAX_STRING_LEN) { vars[slot].value = value; return; }` — skip the substring entirely in the common case.

*Expected:* a solid multiple on interpreter throughput, mostly from #1, #2, and #3. Validate
against the Phase 0 baseline rather than trusting this estimate.

---

## Phase 2 — compile once at load (the real fix)

This is the structural change and the one that actually makes big apps feel native. The
`.dapp` format on disk does not change at all; only the in-memory representation does.

Extend `DappLine` ([global.h:224](global.h#L224)) from a bare `String text` to a
pre-decoded record:

```cpp
struct DappLine {
    String text;          // kept, for error messages
    uint8_t  opcode;      // DAPP_OP_* enum, resolved once at load
    uint8_t  argCount;
    int16_t  labelTarget; // pre-resolved line index for GOTO/GOSUB/IF, -1 if none
    String   args[N];     // pre-split, so splitCommand never runs at execution time
};
```

Work in `appLoad`:

- **Resolve the opcode to an enum.** The `else if` chain becomes
  `switch (lines[pc].opcode)`, which the compiler turns into a jump table. Finding 2
  disappears completely.
- **Pre-split the arguments.** Every handler currently calls `splitCommand` into a local
  `String parts[N]`; that moves to load time. Finding 1's second half disappears.
- **Pre-resolve label targets.** Requires a second pass, since a `GOTO` may reference a
  label defined later. `appFindLabel` then vanishes from the hot path — a `GOTO` becomes
  `pc = lines[pc].labelTarget`. An unresolvable label becomes a *load-time* error, which is
  a usability win on its own: the app fails immediately instead of hundreds of lines in.
- **Intern variable and array names to slot indices.** Pre-assign every `$name` seen in the
  source to a fixed slot and store the index in the line record. `appFindVar` /
  `appFindArray` then become array indexing. This is the fix for Finding 3, and it is what
  makes `board[$i]` in a nested loop cheap.

Memory cost: at 4000 lines the extra fields are on the order of tens of KB, allocated from
PSRAM alongside the existing `DappProgram::alloc` blocks
([AppRunner.ino:92](AppRunner.ino#L92)), so there is room. Load time goes up slightly; it is
already doing a full pass and a `readStringUntil` per line, so this is not the bottleneck.

**Sequencing note:** do this opcode-family by opcode-family, not all at once. Add the
`opcode` field and switch over the ~15 hot opcodes first, with the existing string chain
kept as the `default:` fallback for everything else. That keeps every intermediate commit
working and lets you measure after each step.

---

## Phase 3 — the frame path

Targets `FLIP` latency, which is what makes a game feel sluggish even when the interpreter
is keeping up.

1. **Dirty-rect the canvas.** Keep a shadow copy of `dappCanvasCells` from the last flip.
   On `FLIP`, diff against it and track the bounding box (or per-row dirty flags) of what
   changed. Most frames touch a handful of cells.

2. **Push only the changed region.** `pushDisplayFrame` ([Display.ino:17](Display.ino#L17))
   currently blits the full sprite. Both backends can do a sub-rectangle
   (`Fill_Colors` already takes x/y/w/h; `pushSprite` has a windowed overload). Combined
   with #1 this is the single biggest latency win available — a full 300KB blit becomes a
   few KB.

3. **Don't repaint chrome on a canvas flip.** `drawDisplayFrame` redraws the status bar and
   command bar every time. While `dappCanvasActive`, only redraw those on their own refresh
   cadence, not on every `FLIP`.

4. **Batch the per-cell draw.** In `drawDappCanvas`
   ([Display.ino:702](Display.ino#L702)), coalesce runs of same-colour cells into one
   `drawString` call instead of one call per cell, mirroring what the telnet path already
   does for SGR codes.

5. **Build the telnet frame into a char buffer.** `appCanvasFlip` appends per character to a
   `String`; `reserve()` avoids the reallocs but not the per-append length bookkeeping. A
   plain `char*` with an index is straightforwardly faster.

6. **Coalesce back-to-back flips.** If a script calls `FLIP` more often than the panel can
   push, drop intermediate frames rather than queueing them — dropping is what keeps input
   responsive.

---

## Phase 4 — string handling

1. `reserve()` on `appExpandText`'s output (start at `text.length()` and grow), and copy
   literal *runs* between `$` references with `concat(ptr, len)` instead of appending
   character by character.
2. Give `APPEND` a genuine in-place path: `stringVars[slot].value.concat(expanded)` with a
   length check, instead of build-new-then-copy.
3. `reserve()` string variables to a useful size on first write. `appEnsureStringVar`
   already reserves 80 ([430](AppRunner.ino#L430)); that is a good instinct that the
   assignment path immediately throws away by replacing the buffer.

---

## Phase 5 — perceived responsiveness

Independent of throughput, and worth doing regardless:

- **Poll input more often than the yield cadence.** `KEY` is only as responsive as the loop
  reaches it. Consider servicing the key queue inside the every-256-steps yield so a
  keypress during a long compute stretch is already buffered when `KEY` runs.
- **Show progress on load.** A large `.dapp` reading through `readStringUntil` per line has
  a visible startup pause. A one-line "loading…" with a line count reads as fast even at the
  same duration.

---

## Suggested order

| Phase | Effort | Payoff | Risk |
|---|---|---|---|
| 0 — profiler | S | enables everything else | none |
| 1.3 — reorder chain | XS | immediate, hot paths | none |
| 1.1/1.2 — const&, trim once | S | large | low, mechanical |
| 1.4 — yield/delay fix | S | large on latency | low |
| 3.1/3.2 — dirty-rect blit | M | largest single latency win | medium |
| 2 — load-time compile | L | largest throughput win | medium, do incrementally |
| 1.5/1.6, 4 | S | moderate | low |
| 3.3-3.6, 5 | M | polish | low |

Phase 1.3 and 1.4 are worth doing today. Phase 3.1/3.2 and Phase 2 are the two changes that
decide whether big apps feel good, and they are independent — they can be done in either
order, or in parallel.

## Verification

`dapp-web/dapp-runtime.test.mjs` and `emulator-core.test.mjs` cover the JS runtime, not
`AppRunner.ino`, so they will not catch a regression here. Guard this work with:

- the Phase 0 profiler's lines/second number, recorded per phase;
- a manual pass over the heaviest apps — `grotto2`, `sheet`, `adventure`, `tetris`, `mines`
  — checking both output correctness and feel;
- particular attention to apps using arrays and `APPEND`, since Phase 2's name interning and
  Phase 4's in-place concat are the two changes most likely to alter behaviour rather than
  just timing.

---

## What shipped

Implemented in one pass and verified to compile clean for
`esp32:esp32:esp32s3` at `FNK0104AB_2P8_240x320_ILI9341` (the variant currently selected in
[config.h](config.h)). **Not yet run on hardware** — see "What to watch for" below.

### Interpreter

- **Load-time opcode decode.** `DappLine` ([global.h:224](global.h#L224)) now carries a
  `uint8_t opcode`, the pre-trimmed `arg`, and a pre-resolved `jumpTarget`. `appLoad` does
  the trim/split/uppercase/match once per line; `appExecute`'s per-instruction preamble is
  an array index and an integer compare, where it used to be four String allocations.
- **The ~70-deep string chain is now integer comparisons** against a generated
  `DAPP_OP_*` enum, so `GOTO` and `IF` no longer pay for being at the bottom of it. The
  chain was left as a chain rather than converted to a `switch`: at ~1 cycle per test the
  ordering stopped mattering once the strcmps were gone, and keeping the structure made the
  change reviewable.
- **Labels resolve at load**, in a second pass (a `GOTO` can name a label defined later).
  `appFindLabel`'s linear scan now only runs for a label that failed to resolve — which is
  an error path anyway.
- **Slot scans stop at the first free slot** (`appFindVar`, `appFindStringVar`,
  `appFindArray`), instead of always walking all 64/32/16 entries.
- **Operand normalization no longer copies.** `appValueOf`, `appStringValueOf` and
  `appNumericTarget` take `const String&` and resolve trim/quote/`$` handling as index
  bounds, so a clean token costs no allocation and a `$`-prefixed one costs a single
  substring instead of three whole-string copies.

### Latency

- **`appRuntimeYield(false)` no longer sleeps or feeds the task watchdog.** The task-WDT
  API remains compiled for Arduino linkage but is not initialized for this frame-drop
  diagnostic, and bulk-work checkpoints now return immediately. The service path still
  polls abort/input and board services but does not call `delay(1)`.
- **Partial frame pushes.** `pushDisplayFrame` ([Display.ino:17](Display.ino#L17)) diffs the
  sprite against a PSRAM shadow of what the panel was last sent and transfers only the rows
  that changed, coalesced into runs. A canvas app that moves one character now moves a few
  rows instead of the full 150KB frame.

### Strings

- `appSetStringValue` skips its allocate-and-copy when the value is already under the cap.
- `APPEND` grows the existing buffer in place via a new `appAppendStringValue`, instead of
  building `existing + addition` and then copying that again — it was quadratic with a
  factor of two on top.
- Both text expanders copy literal stretches in runs into a reserved buffer, and return
  immediately for text containing no `$` at all.

### Departures from the plan above

- **Phase 0's profiler was not built.** The changes were made from static analysis instead.
  That means the speedups are un-quantified — the profiler is still the right next step, and
  is now the only way to know which of these mattered most.
- **Arguments are not pre-split.** Storing `String args[N]` per line would have cost
  meaningful internal RAM at `DAPP_MAX_LINES` = 4000 for a benefit that shrank a lot once
  the four preamble allocations were gone. `IF`/`IFEQ` still call `splitCommand` at runtime.
- **Variable and array names are not interned to slot indices.** With the slot scans now
  early-outing, this became a smaller win than it looked; it is still the largest remaining
  interpreter item, particularly for `board[$i]` in a nested loop.
- **No dirty-rect tracking in the draw layer.** The row-diff against a shadow buffer gets
  the same transfer saving without needing every draw call to declare what it touched, and
  it cannot produce a wrong frame. Plan items 3.3 (skip chrome redraw) and 3.4 (batch
  same-colour runs) were therefore not needed and were skipped.

### What to watch for

- **The shadow buffer assumes everything reaches the panel through the sprite.**
  `Gameboy.ino` draws straight to `tft`, so it calls `displayInvalidateShadow()` on exit.
  Anything else added later that bypasses the sprite must do the same, or its region will
  be left stale on screen.
- **The `FNK0104N_3P5_320x480_ST77922` push path is `#ifdef`'d out of this build**, so the
  partial-push change to it was not compile-checked. It is the same call shape as before
  with a varying `sy`/`h`, but it needs a build on that variant before being trusted.
- A `:label` line on an app that has already hit `DAPP_MAX_LABELS` (512) is now a silent
  no-op rather than an "unknown app command" error at runtime. Reaching that cap at all is
  the real problem in either case.
