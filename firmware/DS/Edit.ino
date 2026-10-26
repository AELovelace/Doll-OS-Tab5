//   Edit.ino
//   "edit" app -- a full-screen nano-style text editor, written for DOLL-OS rather
//   than ported. See docs/PORTING.md ("Editor") for why nano itself didn't come
//   over: nano's structure is a conversation with ncurses/terminfo, and shimming
//   that is far more work than owning the render ourselves. Here there is no
//   curses layer and no ANSI parser -- one buffer, two renderers.
//
//   Like Gameboy.ino this is a full-screen takeover: `edit <file>` seizes loop()
//   and runs its own inner loop until the user quits, bypassing the mirrored
//   history ring entirely (Display.ino's ansiFilterByte drops cursor addressing,
//   so a cursor-addressed app can't go through the normal output path). Unlike
//   Gameboy.ino it draws into `frameSprite` and pushes with pushDisplayFrame(),
//   both of which are panel-variant-agnostic -- so this app works on all three
//   FNK0104 variants instead of being stubbed out on the QSPI one. Borrowing
//   frameSprite costs nothing: drawDisplayFrame() rebuilds it from scratch
//   whenever displayDirty is set, which we do on the way out.
//
//   Both surfaces render the same viewport: the panel gets the sprite, a
//   connected telnet client gets the identical grid as ANSI. Geometry comes from
//   the panel (~52x21 at the 6x8 GLCD font) because there is no NAWS negotiation
//   in TelnetServer.ino to ask the client its size -- a bigger terminal just
//   shows the grid in its top-left corner.
//
//   Key chords: because this is a takeover, input does NOT route through
//   readRawUserBytes() (RemoteSession.ino), so DOLL-OS's reserved Ctrl+T (detach) and
//   Ctrl+K (inline command) chords don't apply here and the nano keymap is free
//   to use them. ^X is the exit. Both input sources are drained each pass --
//   telnet and the DS-Slave BLE keyboard, which emits real control codes and CSI
//   sequences in its normal keystroke mode (DS-Slave.ino writeModifiedControl),
//   so no "GAME 1"-style mode switch is needed and the editor is fully usable
//   with no telnet client attached.
//
//   Not implemented yet (deliberately, see the plan): ^K/^U cut+paste, ^W search,
//   ^_ goto line, and undo. The buffer model below is built to take them.

#include "esp_heap_caps.h"

//   Storage model
//
//   One flat byte slab in PSRAM plus a rebuilt line-offset index -- not a vector
//   of String lines. A per-line String array looks simpler but puts every line's
//   character storage on the *internal* heap: enablePsramHeap() (SysInfo.ino)
//   only routes allocations >= 512 bytes out to PSRAM, and edit lines are far
//   smaller than that, so a few thousand of them would quietly eat tens of KB of
//   the scarce pool. A slab sidesteps that completely, and costs less code.
//
//   Editing is memmove + reindex. Worst case at the 128KB cap that's a ~128KB
//   move plus a ~128KB scan per keystroke -- a few ms in PSRAM, against a ~38ms
//   panel push. Not the bottleneck.
static const size_t EDIT_BUF_CAP  = 128 * 1024;
static const int    EDIT_MAX_LINES = 4000;
static const int    EDIT_TAB_WIDTH = 4;

static char*    editBuf = nullptr;
static size_t   editLen = 0;            //bytes used in editBuf
static int32_t* editLineStart = nullptr; //byte offset of each line's first char
static int      editLineCount = 0;       //always >= 1; an empty buffer is one empty line
static size_t   editCursor = 0;          //byte offset into editBuf

static int    editTopLine = 0;      //first buffer line shown in the viewport
static int    editLeftCol = 0;      //first *display* column shown (horizontal scroll)
static int    editGoalCol = -1;     //display column Up/Down try to return to; -1 = use current
static bool   editModified = false; //unsaved changes
static bool   editNeedsRender = true;
static String editPathLogical;      //the DOLL-OS-namespace path, as typed -- shown in the title bar
static String editStatus;           //transient message shown in the hint bar

//^K cut buffer. Consecutive ^K presses append, so ^K^K^K lifts three lines as one
//block and a single ^U puts them back -- nano's behavior, and the only reason this
//needs a "was the last key also a cut" flag.
static String editCutBuffer;
static bool   editLastKeyWasCut = false;

//^W search term, kept between searches so re-opening the prompt and pressing Enter
//is "find next" (there's no Alt+W on this keyboard worth binding for it)
static String editLastSearch;

//   Undo
//
//   Records live below editInsertBytes/editDeleteBytes, the two functions every
//   mutation funnels through, so cut/paste/undo-able typing all record for free
//   rather than each feature remembering to.
//
//   An insert record needs no payload (undoing it is a delete of known length);
//   a delete record has to keep the bytes it removed, allocated from PSRAM. Runs
//   of single-character typing and single-character backspacing coalesce into one
//   record each, so 20 keystrokes are one undo step rather than 20.
static const int    EDIT_UNDO_MAX = 64;
static const size_t EDIT_UNDO_TEXT_MAX = 8 * 1024;   //per-record payload cap

struct EditUndoRec {
    bool   isInsert;
    size_t at;
    size_t len;
    char*  text;         //delete records only; nullptr for inserts
    size_t cursorBefore;
};

//Allocated alongside the editor's text/index slabs. The records are cold while the
//editor is closed, so a permanent internal-.bss ring only reduced multitasking headroom.
static EditUndoRec* editUndo = nullptr;
static int  editUndoCount = 0;
static int  editUndoHead = 0;
static bool editUndoSuspended = false;   //true while an undo is being applied, so
                                          //the mutators it calls don't record it
//which kind of run may still be extended by the next matching keystroke:
//0 = none, 1 = a run of typing, 2 = a run of backspacing
static int  editUndoRun = 0;

//editUndoCount as of the last successful save; -1 means "the saved state is no
//longer reachable by undoing" (history was evicted, or new edits diverged from it).
//Comparing against it is what lets undoing back to the saved state clear the
//modified flag instead of leaving a false "unsaved changes" prompt on exit.
static int editSavedUndoDepth = 0;

//viewport geometry, computed once per launch from the panel
static int editRows = 0;
static int editCols = 0;
static int editCharW = 6;
static int editLineH = 10;

static const int EDIT_TITLE_H = 12;
static const int EDIT_HINT_H  = 12;

//   Logical keys
//
//   EditKey and EditKeyState themselves are declared in global.h, not here: the
//   Arduino builder hoists prototypes for this file's `static` functions too, so
//   a signature mentioning either type lands above any definition Edit.ino could
//   provide. Same trap, and the same fix, as LineInputResult and RadioState.
static EditKeyState editTelnetKeys;
static EditKeyState editKeyboardKeys;

//   Buffer primitives

static void editReindex() {
    editLineCount = 0;
    editLineStart[editLineCount++] = 0;
    for (size_t i = 0; i < editLen && editLineCount < EDIT_MAX_LINES; i++) {
        if (editBuf[i] == '\n') {
            editLineStart[editLineCount++] = (int32_t)(i + 1);
        }
    }
}

//offset one past the last character of `line` (i.e. at its '\n', or at editLen
//for the final line)
static size_t editLineEnd(int line) {
    if (line + 1 < editLineCount) {
        return (size_t)editLineStart[line + 1] - 1;
    }
    return editLen;
}

static int editLineLen(int line) {
    return (int)(editLineEnd(line) - (size_t)editLineStart[line]);
}

static int editCursorLine() {
    int lo = 0, hi = editLineCount - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if ((size_t)editLineStart[mid] <= editCursor) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

//   Tab handling. A real '\t' is stored in the buffer (so Makefiles and the like
//   survive a round trip) and expanded to the next EDIT_TAB_WIDTH stop only at
//   render time -- which means buffer columns and display columns differ, and
//   every cursor/scroll calculation has to go through this one function.
//
//   Expands `line` into `out`. If bufCol >= 0, returns the display column that
//   buffer column lands on; otherwise returns the line's total display width.
static int editExpandLine(int line, String& out, int bufCol) {
    out = "";
    const size_t start = (size_t)editLineStart[line];
    const size_t end = editLineEnd(line);
    int mark = -1;

    for (size_t i = start; i < end; i++) {
        if ((int)(i - start) == bufCol) {
            mark = (int)out.length();
        }
        char c = editBuf[i];
        if (c == '\t') {
            int pad = EDIT_TAB_WIDTH - ((int)out.length() % EDIT_TAB_WIDTH);
            for (int p = 0; p < pad; p++) out += ' ';
        } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) {
            out += c;
        } else {
            out += '?';   //same policy as Display.ino's ansiFilterByte -- the panel is ASCII
        }
    }
    if (bufCol >= 0 && mark < 0) {
        mark = (int)out.length();   //cursor sits at (or past) end of line
    }
    return (bufCol >= 0) ? mark : (int)out.length();
}

//display column of the cursor on its own line
static int editCursorDisplayCol() {
    int line = editCursorLine();
    String tmp;
    return editExpandLine(line, tmp, (int)(editCursor - (size_t)editLineStart[line]));
}

//buffer column on `line` whose display column is nearest to `wantDisplayCol` --
//the inverse of editExpandLine, used by Up/Down so the cursor tracks a visual
//column through tab-indented lines instead of drifting
static int editBufColForDisplayCol(int line, int wantDisplayCol) {
    const int len = editLineLen(line);
    String tmp;
    for (int c = 0; c <= len; c++) {
        if (editExpandLine(line, tmp, c) >= wantDisplayCol) {
            return c;
        }
    }
    return len;
}

//   Undo ring

static int editUndoSlot(int logicalIndex) {
    return (editUndoHead + logicalIndex) % EDIT_UNDO_MAX;
}

static void editUndoFreeSlot(int slot) {
    if (editUndo[slot].text) {
        heap_caps_free(editUndo[slot].text);
        editUndo[slot].text = nullptr;
    }
}

static void editUndoClear() {
    if (!editUndo) {
        editUndoCount = 0;
        editUndoHead = 0;
        editUndoRun = 0;
        editSavedUndoDepth = -1;
        return;
    }
    for (int i = 0; i < EDIT_UNDO_MAX; i++) {
        editUndoFreeSlot(i);
    }
    editUndoCount = 0;
    editUndoHead = 0;
    editUndoRun = 0;
    editSavedUndoDepth = -1;
}

//recomputes the modified flag from how far the undo stack sits from the last save
static void editRefreshModified() {
    editModified = (editSavedUndoDepth < 0) || (editUndoCount != editSavedUndoDepth);
}

static char* editUndoAllocText(size_t n) {
    char* p = (char*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p) p = (char*)heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

//appends a record. `removed` is the payload for delete records (ignored for
//inserts). runKind matches editUndoRun's encoding: pass 0 for an operation that
//must start its own record and end any run in progress.
static void editUndoPush(bool isInsert, size_t at, size_t len, const char* removed,
                         size_t cursorBefore, int runKind) {
    if (editUndoSuspended) {
        return;
    }

    //an op we can't record leaves a hole, and every older record's offsets are
    //only valid if everything after them is replayed first -- so drop the history
    //rather than keep a stack that would corrupt the buffer when applied
    if (!isInsert && len > EDIT_UNDO_TEXT_MAX) {
        editUndoClear();
        editRefreshModified();
        return;
    }

    //extend the record on top instead of pushing, when this is the next keystroke
    //in a run of the same kind and it's contiguous with what's already there
    if (runKind != 0 && runKind == editUndoRun && editUndoCount > 0) {
        const int top = editUndoSlot(editUndoCount - 1);
        EditUndoRec& r = editUndo[top];
        if (isInsert && r.isInsert && at == r.at + r.len) {
            r.len += len;                       //typing forward
            editRefreshModified();
            return;
        }
        if (!isInsert && !r.isInsert && at + len == r.at && r.len + len <= EDIT_UNDO_TEXT_MAX) {
            //backspacing leftward: the newly removed bytes belong in front of
            //what this record already holds, and the record's start moves back
            char* grown = editUndoAllocText(r.len + len);
            if (grown) {
                memcpy(grown, removed, len);
                memcpy(grown + len, r.text, r.len);
                heap_caps_free(r.text);
                r.text = grown;
                r.len += len;
                r.at = at;
                editRefreshModified();
                return;
            }
            //allocation failed -- fall through and push a fresh record instead
        }
    }

    char* payload = nullptr;
    if (!isInsert) {
        payload = editUndoAllocText(len);
        if (!payload) {
            editUndoClear();
            editRefreshModified();
            return;
        }
        memcpy(payload, removed, len);
    }

    int slot;
    if (editUndoCount < EDIT_UNDO_MAX) {
        slot = editUndoSlot(editUndoCount);
        editUndoCount++;
    } else {
        //ring full: the oldest record falls off, so the saved state is no longer
        //reachable by undoing and the depth comparison stops being meaningful
        slot = editUndoHead;
        editUndoFreeSlot(slot);
        editUndoHead = (editUndoHead + 1) % EDIT_UNDO_MAX;
        editSavedUndoDepth = -1;
    }
    //Editing after undoing back past the save point discards the very records that
    //led to it, so the saved content is no longer anywhere on this stack.
    //
    //The comparison reads oddly because editUndoCount was already incremented above:
    //the depth this record was pushed *at* is editUndoCount - 1, and the save is
    //unreachable once that is below it -- editUndoCount - 1 < depth, i.e. the test
    //below. Using '>' here instead would miss the exact case this exists for (save
    //at depth 5, undo to 4, type: the new record #5 is not the saved one) and leave
    //a modified buffer reporting itself clean on exit.
    if (editSavedUndoDepth >= editUndoCount) {
        editSavedUndoDepth = -1;
    }

    editUndo[slot].isInsert = isInsert;
    editUndo[slot].at = at;
    editUndo[slot].len = len;
    editUndo[slot].text = payload;
    editUndo[slot].cursorBefore = cursorBefore;
    editUndoRun = runKind;
    editRefreshModified();
}

//runKind is the undo-coalescing hint: 1 for a keystroke that may extend a run of
//typing, 2 for one that may extend a run of backspacing, 0 for anything that must
//stand as its own undo step (Enter, Tab, paste, forward-delete, cut).
static bool editInsertBytes(const char* s, size_t n, int runKind) {
    if (editLen + n > EDIT_BUF_CAP) {
        editStatus = "Buffer full (128KB limit)";
        return false;
    }
    //a newline that would push past the line index has nowhere to be recorded,
    //and a silently truncated index corrupts every offset after it
    if (editLineCount >= EDIT_MAX_LINES) {
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '\n') {
                editStatus = "Line limit reached (" + String(EDIT_MAX_LINES) + ")";
                return false;
            }
        }
    }
    const size_t cursorBefore = editCursor;
    memmove(editBuf + editCursor + n, editBuf + editCursor, editLen - editCursor);
    memcpy(editBuf + editCursor, s, n);
    editLen += n;
    editReindex();
    editUndoPush(true, editCursor, n, nullptr, cursorBefore, runKind);
    editCursor += n;
    return true;
}

static void editDeleteBytes(size_t at, size_t n, int runKind) {
    if (at >= editLen || n == 0) {
        return;
    }
    if (at + n > editLen) {
        n = editLen - at;
    }
    const size_t cursorBefore = editCursor;
    editUndoPush(false, at, n, editBuf + at, cursorBefore, runKind);   //before the bytes go
    memmove(editBuf + at, editBuf + at + n, editLen - at - n);
    editLen -= n;
    if (editCursor >= at + n)      editCursor -= n;
    else if (editCursor > at)      editCursor = at;
    editReindex();
}

//applies the newest record and pops it
static void editApplyUndo() {
    if (editUndoCount == 0) {
        editStatus = "Nothing to undo";
        return;
    }
    const int slot = editUndoSlot(editUndoCount - 1);

    editUndoSuspended = true;
    if (editUndo[slot].isInsert) {
        editDeleteBytes(editUndo[slot].at, editUndo[slot].len, 0);
        editCursor = editUndo[slot].at;
    } else {
        editCursor = editUndo[slot].at;
        editInsertBytes(editUndo[slot].text, editUndo[slot].len, 0);
        editCursor = editUndo[slot].cursorBefore;
    }
    editUndoSuspended = false;

    if (editCursor > editLen) editCursor = editLen;
    editUndoFreeSlot(slot);
    editUndoCount--;
    editUndoRun = 0;
    editGoalCol = -1;
    editRefreshModified();
    editStatus = "Undid 1 change (" + String(editUndoCount) + " left)";
}

//   Movement

static void editScrollToCursor() {
    const int line = editCursorLine();
    if (line < editTopLine)                editTopLine = line;
    if (line >= editTopLine + editRows)    editTopLine = line - editRows + 1;
    if (editTopLine < 0)                   editTopLine = 0;

    const int col = editCursorDisplayCol();
    if (col < editLeftCol)                 editLeftCol = col;
    if (col >= editLeftCol + editCols)     editLeftCol = col - editCols + 1;
    if (editLeftCol < 0)                   editLeftCol = 0;
}

static void editMoveVertical(int delta) {
    const int line = editCursorLine();
    if (editGoalCol < 0) {
        editGoalCol = editCursorDisplayCol();
    }
    int target = line + delta;
    if (target < 0) target = 0;
    if (target >= editLineCount) target = editLineCount - 1;
    editCursor = (size_t)editLineStart[target] + editBufColForDisplayCol(target, editGoalCol);
}

static void editMoveLeft() {
    editGoalCol = -1;
    if (editCursor > 0) editCursor--;
}

static void editMoveRight() {
    editGoalCol = -1;
    if (editCursor < editLen) editCursor++;
}

static void editMoveHome() {
    editGoalCol = -1;
    editCursor = (size_t)editLineStart[editCursorLine()];
}

static void editMoveEnd() {
    editGoalCol = -1;
    editCursor = editLineEnd(editCursorLine());
}

//   Cut / paste

static const size_t EDIT_CUT_MAX = 32 * 1024;

//^K: lifts the whole line the cursor is on, regardless of column (nano's default,
//i.e. without "cutfromcursor"). Consecutive presses append, so a run of them takes
//a block out that a single ^U puts back.
static void editDoCut() {
    const int line = editCursorLine();
    size_t start = (size_t)editLineStart[line];
    size_t end = editLineEnd(line);

    const bool hasNewline = (end < editLen && editBuf[end] == '\n');
    size_t removeFrom = start;
    size_t removeTo = hasNewline ? end + 1 : end;

    if (removeTo == removeFrom) {
        editStatus = "Nothing to cut";
        return;
    }
    //the final line carries no newline of its own, so deleting just its text would
    //leave the line before it terminated by a newline that now ends the buffer --
    //a dangling empty last line. Take the preceding newline with it instead.
    if (!hasNewline && start > 0 && editBuf[start - 1] == '\n') {
        removeFrom = start - 1;
    }

    if (!editLastKeyWasCut) {
        editCutBuffer = "";
    }
    if (editCutBuffer.length() + (end - start) + 1 > EDIT_CUT_MAX) {
        editStatus = "Cut buffer full (32KB)";
        return;
    }

    //always store the line with a trailing newline, whether or not the buffer had
    //one there -- that's what makes a paste land as a whole line anywhere it goes
    editCutBuffer.reserve(editCutBuffer.length() + (end - start) + 1);
    for (size_t i = start; i < end; i++) {
        editCutBuffer += editBuf[i];
    }
    editCutBuffer += '\n';

    editCursor = removeFrom;
    editDeleteBytes(removeFrom, removeTo - removeFrom, 0);
    editGoalCol = -1;
    editLastKeyWasCut = true;
    editStatus = "Cut buffer: " + String((unsigned long)editCutBuffer.length()) + " bytes";
}

//^U: pastes the cut buffer at the cursor
static void editDoUncut() {
    if (editCutBuffer.length() == 0) {
        editStatus = "Cut buffer is empty";
        return;
    }
    if (editInsertBytes(editCutBuffer.c_str(), editCutBuffer.length(), 0)) {
        editStatus = "Pasted " + String((unsigned long)editCutBuffer.length()) + " bytes";
    }
    editGoalCol = -1;
}

//   Search
//
//   Case-insensitive, because the alternative on a BLE keyboard driving a 52-column
//   screen is mostly frustration. Returns the match offset or -1.
static long editFindFrom(const char* needle, size_t nlen, size_t from) {
    if (nlen == 0 || nlen > editLen) {
        return -1;
    }
    for (size_t i = from; i + nlen <= editLen; i++) {
        size_t j = 0;
        while (j < nlen &&
               tolower((unsigned char)editBuf[i + j]) == tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == nlen) {
            return (long)i;
        }
    }
    return -1;
}

//   Key decoding -- one byte in, at most one logical key out. Shared by the main
//   loop and by the modal prompts below, so both speak the same vocabulary.
static bool editDecodeByte(EditKeyState& st, uint8_t b, EditKey& key, char& ch) {
    key = EK_NONE;
    ch = 0;

    if (st.esc == UESC_GOT_ESC) {
        if (b == '[') {
            st.esc = UESC_GOT_CSI;
            st.params = "";
            return false;
        }
        //ESC + printable is how both a telnet client and DS-Slave (writeModifiedAscii,
        //alt branch) spell Alt+key, so Alt+U reaches us as ESC 'u' -- nano's undo.
        //The usual terminal ambiguity applies: a bare Escape followed by typing 'u'
        //is indistinguishable from Alt+U, which is true in nano itself too.
        st.esc = UESC_NONE;
        if (b == 'u' || b == 'U') {
            key = EK_UNDO;
            return true;
        }
        return false;
    }
    if (st.esc == UESC_GOT_CSI) {
        if (b < 0x40 || b > 0x7E) {
            st.params += (char)b;
            return false;
        }
        const String p = st.params;
        st.esc = UESC_NONE;

        //Ctrl+Up/Down keeps nudging radio volume here, same as it does at the
        //shell prompt and in raw ssh/telnet sessions -- one gesture everywhere
        if (p == "1;5" && (b == 'A' || b == 'B')) {
            radioAdjustVolume(b == 'A' ? 1 : -1);
            return false;
        }
        switch ((char)b) {
            case 'A': key = EK_UP;    return true;
            case 'B': key = EK_DOWN;  return true;
            case 'C': key = EK_RIGHT; return true;
            case 'D': key = EK_LEFT;  return true;
            case 'H': key = EK_HOME;  return true;
            case 'F': key = EK_END;   return true;
            case '~':
                if (p == "1" || p == "7") key = EK_HOME;
                else if (p == "4" || p == "8") key = EK_END;
                else if (p == "3") key = EK_DELETE;
                else if (p == "5") key = EK_PGUP;
                else if (p == "6") key = EK_PGDN;
                return key != EK_NONE;
            default: return false;   //unbound sequence (function keys, csi-u from DS-Slave)
        }
    }

    if (b == 0x1B) {
        st.esc = UESC_GOT_ESC;
        return false;
    }

    //CR/LF pairing, same rule processLineEditByte() uses: a CRLF is one Enter
    if (b == '\r') {
        st.lastByteWasCR = true;
        key = EK_ENTER;
        return true;
    }
    if (b == '\n') {
        if (st.lastByteWasCR) {
            st.lastByteWasCR = false;
            return false;
        }
        key = EK_ENTER;
        return true;
    }
    st.lastByteWasCR = false;

    switch (b) {
        case 0x0F: key = EK_SAVE;      return true;   //^O
        case 0x18: key = EK_EXIT;      return true;   //^X
        case 0x03: key = EK_CANCEL;    return true;   //^C
        case 0x01: key = EK_HOME;      return true;   //^A
        case 0x05: key = EK_END;       return true;   //^E
        case 0x19: key = EK_PGUP;      return true;   //^Y
        case 0x16: key = EK_PGDN;      return true;   //^V
        case 0x04: key = EK_DELETE;    return true;   //^D
        case 0x0B: key = EK_CUT;       return true;   //^K -- free here; DOLL-OS's inline-command
                                                       //reservation only applies to sessions
                                                       //that route through readRawUserBytes()
        case 0x15: key = EK_UNCUT;     return true;   //^U
        case 0x17: key = EK_SEARCH;    return true;   //^W
        case 0x1F: key = EK_GOTO;      return true;   //^_ (Ctrl+Shift+- / Ctrl+/)
        case 0x07: key = EK_HELP;      return true;   //^G
        case 0x1A: key = EK_UNDO;      return true;   //^Z -- no job control here, and it's
                                                       //far more discoverable than Alt+U
        case 0x09: key = EK_TAB;       return true;
        case 0x08:
        case 0x7F: key = EK_BACKSPACE; return true;
    }
    if (b >= 0x20 && b < 0x7F) {
        key = EK_CHAR;
        ch = (char)b;
        return true;
    }
    return false;   //other control bytes: no binding, dropped
}

//pulls the next logical key from either input source, telnet first then the
//BLE-keyboard bridge -- the same precedence readRawUserBytes() uses, so the
//editor behaves identically whether or not a telnet client is attached
static bool editNextKey(EditKey& key, char& ch) {
    int raw;
    while ((raw = telnetReadFilteredByte()) != -1) {
        if (editDecodeByte(editTelnetKeys, (uint8_t)raw, key, ch)) return true;
    }
    while ((raw = keyboardReadRawByte()) != -1) {
        if (editDecodeByte(editKeyboardKeys, (uint8_t)raw, key, ch)) return true;
    }
    return false;
}

//   Rendering
//
//   Both renderers read the same viewport state and are called from one place,
//   after all pending input for this pass has been applied -- never from inside
//   a key handler. A full sprite push is ~38ms, so rendering per keystroke would
//   cap typing at ~26 keys/sec and fall behind on the bursts a telnet client
//   delivers. Coalescing is what keeps that from mattering.

//   Syntax highlighting
//
//   Only .dapp gets colored, and only because the editor already owns both
//   renderers -- there is no ANSI parser in the path to smuggle color through
//   (see the file header), so highlighting has to be a property of the render
//   rather than of the text. The model is deliberately the cheapest one that
//   works: a per-line pass producing one attribute byte per *display* column,
//   computed on the tab-expanded line so it slices identically to the text.
//   No cross-line state, so a line can be re-highlighted in isolation and the
//   viewport costs editRows passes per frame -- microseconds against a ~38ms
//   panel push.
//
//   .dapp is line-oriented (AppRunner.ino appExecute: opcode, then operands,
//   no continuations), which is what makes the no-state model exact rather
//   than an approximation.

static const char ESA_TEXT    = '.';
static const char ESA_COMMENT = 'c';
static const char ESA_KEYWORD = 'k';
static const char ESA_STRING  = 's';
static const char ESA_VAR     = 'v';
static const char ESA_NUMBER  = 'n';
static const char ESA_LABEL   = 'l';

//true while editing a file whose name ends in .dapp
static bool editSyntaxDapp = false;

//kept in step with the dispatch chain in AppRunner.ino appExecute()
static const char* const EDIT_DAPP_KEYWORDS[] = {
    "PRINT", "ECHO", "COLOR", "CLEAR", "CLS", "WAIT", "SLEEP",
    "SET", "ADD", "SUB", "MUL", "DIV", "MOD", "EXPR", "RAND",
    "DIM", "LIFE", "SETSTR", "APPEND", "CHR", "HEX", "SUBSTR", "LEN", "CHARAT",
    "INPUT", "INPUTSECRET", "KEY", "LED", "WAVE", "WAVESTOP",
    "HTTPGET", "HTTPPOST", "HTTPHEADER", "HTTPCLEAR", "HTTPGETBUF",
    "BUFNEW", "BUFFREE", "BUFCLEAR", "BUFAT", "BUFSUB", "BUFWRITE", "BUFSCAN",
    "BUFTAKE", "BUFSAVE", "BUFLOAD", "HTMLTEXT", "HTMLOPEN", "HTMLFEED",
    "HTMLCLOSE", "HTMLSTR", "URLABS", "URLPART", "JSONESC", "JSONGET", "DAPPER",
    "CANVAS", "ENDCANVAS", "PUT", "FLIP",
    "FOPEN", "FCLOSE", "FREAD", "FREADB", "FWRITE", "FWRITEB", "FSEEK", "FTELL",
    "FSIZE", "FEXISTS", "FDELETE", "FLIST", "FMKDIR", "FCOPY", "FMOVE",
    "LABEL", "GOTO", "GOSUB", "RETURN", "IF", "IFEQ", "IFNE", "EXIT", "END",
};
static const int EDIT_DAPP_KEYWORD_COUNT =
    sizeof(EDIT_DAPP_KEYWORDS) / sizeof(EDIT_DAPP_KEYWORDS[0]);

static bool editPathIsDapp(const String& path) {
    String p = path;
    p.toLowerCase();
    return p.endsWith(".dapp");
}

static bool editDappIsKeyword(const String& word) {
    for (int i = 0; i < EDIT_DAPP_KEYWORD_COUNT; i++) {
        if (word.equalsIgnoreCase(EDIT_DAPP_KEYWORDS[i])) return true;
    }
    return false;
}

static bool editIsWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

static void editFill(String& attr, int from, int to, char a) {
    for (int i = from; i < to && i < (int)attr.length(); i++) {
        if (i >= 0) attr.setCharAt(i, a);
    }
}

//marks `$name` runs inside an already-colored span, so "hi, $name" reads as a
//string with a live substitution in it rather than one flat green blob
static void editMarkVarsIn(const String& s, String& attr, int from, int to) {
    for (int i = from; i < to; i++) {
        if (s[i] != '$') continue;
        int j = i + 1;
        while (j < to && editIsWordChar(s[j])) j++;
        if (j > i + 1) editFill(attr, i, j, ESA_VAR);
        i = j - 1;
    }
}

//fills `attr` with one attribute char per character of `s` (a tab-expanded line)
static void editHighlightDapp(const String& s, String& attr) {
    const int n = s.length();
    attr = "";
    if (n == 0) return;
    attr.reserve(n);
    for (int i = 0; i < n; i++) attr += ESA_TEXT;

    int i = 0;
    while (i < n && s[i] == ' ') i++;
    if (i >= n) return;

    //whole-line comment -- both forms AppRunner.ino's isAppCommentOrBlank accepts
    if (s[i] == '#' || (s[i] == '/' && i + 1 < n && s[i + 1] == '/')) {
        editFill(attr, i, n, ESA_COMMENT);
        return;
    }

    if (s[i] == ':') {
        //":name" shorthand label -- the colon and the name are the whole statement
        int j = i;
        while (j < n && s[j] != ' ') j++;
        editFill(attr, i, j, ESA_LABEL);
        i = j;
    } else {
        int j = i;
        while (j < n && s[j] != ' ') j++;
        const String op = s.substring(i, j);
        if (editDappIsKeyword(op)) {
            editFill(attr, i, j, ESA_KEYWORD);
            //LABEL/GOTO take a label name as their one operand
            if (op.equalsIgnoreCase("LABEL") || op.equalsIgnoreCase("GOTO") || op.equalsIgnoreCase("GOSUB")) {
                int a = j;
                while (a < n && s[a] == ' ') a++;
                int b = a;
                while (b < n && s[b] != ' ') b++;
                editFill(attr, a, b, ESA_LABEL);
                j = b;
            }
        }
        //an unrecognized opcode stays plain: that is the tell that `run` will
        //reject the line, and inventing an error color for it would be noisier
        i = j;
    }

    //operands
    while (i < n) {
        const char c = s[i];
        if (c == '"') {
            int j = i + 1;
            while (j < n && s[j] != '"') j++;
            if (j < n) j++;           //include the closing quote; unterminated runs to EOL
            editFill(attr, i, j, ESA_STRING);
            editMarkVarsIn(s, attr, i, j);
            i = j;
        } else if (c == '$') {
            int j = i + 1;
            while (j < n && editIsWordChar(s[j])) j++;
            //an array reference colors as one variable, subscript included, so
            //"$board[$i]" doesn't read as a variable followed by something unknown
            if (j < n && s[j] == '[') {
                int depth = 0;
                int k = j;
                for (; k < n; k++) {
                    if (s[k] == '[') depth++;
                    else if (s[k] == ']' && --depth == 0) { k++; break; }
                }
                if (depth == 0) j = k;
            }
            editFill(attr, i, j, ESA_VAR);
            i = j;
        } else if ((c >= '0' && c <= '9')
                   || (c == '-' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9')) {
            //only at a token boundary, so the "2" in "roll2" isn't a number
            if (i > 0 && s[i - 1] != ' ') { i++; continue; }
            int j = (c == '-') ? i + 1 : i;
            while (j < n && s[j] >= '0' && s[j] <= '9') j++;
            editFill(attr, i, j, ESA_NUMBER);
            i = j;
        } else if (i > 0 && s[i - 1] == ' ' && (c == 'G' || c == 'g')) {
            //the GOTO/GOSUB inside IF/IFEQ/IFNE, plus the label it jumps to
            int j = i;
            while (j < n && s[j] != ' ') j++;
            const String word = s.substring(i, j);
            if (word.equalsIgnoreCase("GOTO") || word.equalsIgnoreCase("GOSUB")) {
                editFill(attr, i, j, ESA_KEYWORD);
                int a = j;
                while (a < n && s[a] == ' ') a++;
                int b = a;
                while (b < n && s[b] != ' ') b++;
                editFill(attr, a, b, ESA_LABEL);
                j = b;
            }
            i = j;
        } else {
            i++;
        }
    }
}

static uint16_t editAttrColor(char a) {
    switch (a) {
        case ESA_COMMENT: return TFT_DARKGREY;
        case ESA_KEYWORD: return TFT_CYAN;
        case ESA_STRING:  return TFT_GREEN;
        case ESA_VAR:     return TFT_YELLOW;
        case ESA_NUMBER:  return TFT_ORANGE;
        case ESA_LABEL:   return TFT_PINK;
        default:          return TFT_WHITE;
    }
}

static const char* editAttrAnsi(char a) {
    switch (a) {
        case ESA_COMMENT: return "\x1b[90m";
        case ESA_KEYWORD: return "\x1b[36m";
        case ESA_STRING:  return "\x1b[32m";
        case ESA_VAR:     return "\x1b[33m";
        case ESA_NUMBER:  return "\x1b[91m";
        case ESA_LABEL:   return "\x1b[95m";
        default:          return "\x1b[0m";
    }
}

//the visible slice of one buffer line, already tab-expanded and horizontally
//scrolled, plus its attribute row. `attr` comes back empty when highlighting is
//off -- both renderers treat "attr shorter than row" as "draw it plain".
static void editVisibleRowStyled(int line, String& row, String& attr) {
    String expanded;
    editExpandLine(line, expanded, -1);
    attr = "";
    if (editSyntaxDapp) {
        editHighlightDapp(expanded, attr);
    }
    if (editLeftCol >= (int)expanded.length()) {
        row = "";
        attr = "";
        return;
    }
    row = expanded.substring(editLeftCol);
    if (attr.length() > 0) attr = attr.substring(editLeftCol);
    if ((int)row.length() > editCols) {
        row = row.substring(0, editCols);
        if (attr.length() > 0) attr = attr.substring(0, editCols);
    }
}

static String editVisibleRow(int line) {
    String row, attr;
    editVisibleRowStyled(line, row, attr);
    return row;
}

static String editTitleText() {
    String t = editPathLogical;
    if (editModified) t += " *";
    return t;
}

static String editPositionText() {
    return String(editCursorLine() + 1) + "," + String(editCursorDisplayCol() + 1)
         + "  " + String(editLineCount) + "L " + String((unsigned long)editLen) + "B";
}

static String editHintText() {
    if (editStatus.length() > 0) {
        return editStatus;
    }
    return "^G Help  ^O Save  ^X Exit  ^K Cut  ^W Find  ^Z Undo";
}

static void editRenderPanel() {
    frameSprite.fillSprite(TFT_BLACK);
    frameSprite.setTextDatum(TL_DATUM);

    //title bar -- same pink accent the shell status bar uses, inverted
    frameSprite.fillRect(0, 0, DISPLAY_WIDTH, EDIT_TITLE_H, TFT_PINK);
    frameSprite.setTextColor(TFT_BLACK, TFT_PINK);
    frameSprite.drawString(editTitleText(), DISPLAY_PADDING, 2);
    frameSprite.setTextDatum(TR_DATUM);
    frameSprite.drawString(editPositionText(), DISPLAY_WIDTH - DISPLAY_PADDING, 2);
    frameSprite.setTextDatum(TL_DATUM);

    //text area
    const int textTop = EDIT_TITLE_H + 2;
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    for (int r = 0; r < editRows; r++) {
        const int line = editTopLine + r;
        if (line >= editLineCount) break;
        String row, attr;
        editVisibleRowStyled(line, row, attr);
        if (row.length() == 0) continue;
        const int y = textTop + r * editLineH;
        if (attr.length() != row.length()) {
            frameSprite.drawString(row, DISPLAY_PADDING, y);
            continue;
        }
        //one drawString per run of same-colored characters; the font is fixed
        //width, so a run's x is just its column times editCharW
        int i = 0;
        while (i < (int)row.length()) {
            int j = i;
            while (j < (int)row.length() && attr[j] == attr[i]) j++;
            frameSprite.setTextColor(editAttrColor(attr[i]), TFT_BLACK);
            frameSprite.drawString(row.substring(i, j),
                                   DISPLAY_PADDING + i * editCharW, y);
            i = j;
        }
        frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    }

    //block cursor, blinking on the same half-period the shell command bar uses
    if (((millis() / DISPLAY_CURSOR_BLINK_MS) & 1UL) == 0) {
        const int cLine = editCursorLine();
        const int cCol  = editCursorDisplayCol();
        const int r = cLine - editTopLine;
        const int c = cCol - editLeftCol;
        if (r >= 0 && r < editRows && c >= 0 && c < editCols) {
            const int x = DISPLAY_PADDING + c * editCharW;
            const int y = textTop + r * editLineH;
            frameSprite.fillRect(x, y, editCharW, frameSprite.fontHeight(), TFT_WHITE);
            const String row = editVisibleRow(cLine);
            if (c < (int)row.length()) {
                frameSprite.setTextColor(TFT_BLACK, TFT_WHITE);
                frameSprite.drawString(String(row[c]), x, y);
                frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
            }
        }
    }

    //hint bar
    const int hintY = DISPLAY_HEIGHT - EDIT_HINT_H;
    frameSprite.fillRect(0, hintY, DISPLAY_WIDTH, EDIT_HINT_H, TFT_BLACK);
    frameSprite.drawFastHLine(0, hintY, DISPLAY_WIDTH, TFT_PINK);
    frameSprite.setTextColor(editStatus.length() ? TFT_YELLOW : TFT_CYAN, TFT_BLACK);
    frameSprite.drawString(editHintText(), DISPLAY_PADDING, hintY + 2);
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);

    pushDisplayFrame();
}

//pads/truncates to exactly editCols so reverse-video bars fill the row cleanly
static String editFitToCols(const String& s) {
    String out = s;
    if ((int)out.length() > editCols) {
        return out.substring(0, editCols);
    }
    while ((int)out.length() < editCols) out += ' ';
    return out;
}

static void editRenderTelnet() {
    if (!telnetClient || !telnetClient.connected()) {
        return;
    }

    String out;
    out.reserve((size_t)(editCols + 8) * (editRows + 2) + 64);
    out += "\x1b[?25l\x1b[H";   //hide cursor while painting, home

    //title
    out += "\x1b[7m";
    String left = editTitleText();
    String right = editPositionText();
    String bar = left;
    while ((int)bar.length() < editCols - (int)right.length() - 1) bar += ' ';
    bar += " " + right;
    out += editFitToCols(bar);
    out += "\x1b[0m\r\n";

    //text
    for (int r = 0; r < editRows; r++) {
        const int line = editTopLine + r;
        if (line < editLineCount) {
            String row, attr;
            editVisibleRowStyled(line, row, attr);
            if (attr.length() == row.length() && row.length() > 0) {
                int i = 0;
                while (i < (int)row.length()) {
                    int j = i;
                    while (j < (int)row.length() && attr[j] == attr[i]) j++;
                    out += editAttrAnsi(attr[i]);
                    out += row.substring(i, j);
                    i = j;
                }
                out += "\x1b[0m";   //so ^[K erases in the default background
            } else {
                out += row;
            }
        }
        out += "\x1b[K\r\n";
    }

    //hint
    out += editStatus.length() ? "\x1b[33m" : "\x1b[36m";
    out += editHintText();
    out += "\x1b[0m\x1b[K";

    //park the real terminal cursor on the edit position (1-based, +2 rows for the title)
    const int cr = editCursorLine() - editTopLine + 2;
    const int cc = editCursorDisplayCol() - editLeftCol + 1;
    if (cr >= 2 && cr < editRows + 2 && cc >= 1 && cc <= editCols) {
        out += "\x1b[" + String(cr) + ";" + String(cc) + "H\x1b[?25h";
    }

    telnetClient.print(out);
}

static void editRender() {
    editScrollToCursor();
    editRenderPanel();
    editRenderTelnet();
}

//   Modal prompts (filename on ^O, save-on-exit confirmation). Both borrow the
//   hint bar and the same key decoder, and both keep rendering so the panel and
//   the telnet client stay in step while they're up.

//returns false if the user cancelled (^C or ^X)
static bool editPromptLine(const String& label, String& value) {
    while (true) {
        editStatus = label + value + "_";
        editRender();

        EditKey key;
        char ch;
        bool got = false;
        //spin until a key arrives, yielding so the idle task feeds the watchdog
        while (!got) {
            got = editNextKey(key, ch);
            if (!got) delay(2);
        }

        if (key == EK_ENTER)                       { editStatus = ""; return true; }
        if (key == EK_CANCEL || key == EK_EXIT)    { editStatus = ""; return false; }
        if (key == EK_BACKSPACE) {
            if (value.length() > 0) value.remove(value.length() - 1);
        } else if (key == EK_CHAR) {
            if (value.length() < (unsigned)COMMAND_MAX_LEN) value += ch;
        }
    }
}

//returns 'y', 'n', or 'c' (cancel)
static char editPromptYesNo(const String& label) {
    editStatus = label;
    editRender();
    while (true) {
        EditKey key;
        char ch;
        if (!editNextKey(key, ch)) {
            delay(2);
            continue;
        }
        if (key == EK_CANCEL || key == EK_EXIT) { editStatus = ""; return 'c'; }
        if (key == EK_CHAR) {
            if (ch == 'y' || ch == 'Y') { editStatus = ""; return 'y'; }
            if (ch == 'n' || ch == 'N') { editStatus = ""; return 'n'; }
        }
    }
}

//^W: prompts (prefilled with the last term, so Enter alone is "find next"), searches
//forward from just past the cursor, and wraps to the top once before giving up
static void editDoSearch() {
    String term = editLastSearch;
    if (!editPromptLine("Search: ", term) || term.length() == 0) {
        editStatus = "Cancelled";
        return;
    }
    editLastSearch = term;

    bool wrapped = false;
    long hit = editFindFrom(term.c_str(), term.length(), editCursor + 1);
    if (hit < 0) {
        hit = editFindFrom(term.c_str(), term.length(), 0);
        wrapped = true;
    }
    if (hit < 0) {
        editStatus = "\"" + term + "\" not found";
        return;
    }
    editCursor = (size_t)hit;
    editGoalCol = -1;
    editStatus = wrapped ? "Search wrapped to top" : "";
}

//^_: jump to a line number, clamped to the buffer
static void editDoGotoLine() {
    String s = "";
    if (!editPromptLine("Go to line: ", s)) {
        editStatus = "Cancelled";
        return;
    }
    s.trim();
    if (s.length() == 0) {
        editStatus = "Cancelled";
        return;
    }
    long n = s.toInt();
    if (n < 1) n = 1;
    if (n > editLineCount) n = editLineCount;
    editCursor = (size_t)editLineStart[n - 1];
    editGoalCol = -1;
    editStatus = "";
}

//   ^G help. The hint bar only has room for four chords, so this is where the rest
//   of the keymap actually lives. Modal: renders over both surfaces and waits.
static const char* EDIT_HELP_LINES[] = {
    "^O  write the buffer out (prompts for name)",
    "^X  exit (offers to save if modified)",
    "^K  cut this line   ^U  paste it back",
    "    consecutive ^K appends, so ^K^K^K lifts 3",
    "^W  search (case-insensitive, wraps)",
    "    Enter on the prefilled term = find next",
    "^_  go to line number",
    "^Z  undo   (Alt+U does the same)",
    "^A  line start      ^E  line end",
    "^Y  page up         ^V  page down",
    "^D  delete forward  ^C  show position",
    "^G  this screen",
    "",
    "Arrows, Home/End, PgUp/PgDn also work.",
    "Ctrl+Up/Down still nudges radio volume.",
    "",
    "Tabs are kept as tabs. Saves go through a",
    "temp file, so a power loss can't truncate.",
    ".dapp files are syntax highlighted.",
};
static const int EDIT_HELP_COUNT = sizeof(EDIT_HELP_LINES) / sizeof(EDIT_HELP_LINES[0]);

static void editShowHelp() {
    //panel
    frameSprite.fillSprite(TFT_BLACK);
    frameSprite.setTextDatum(TL_DATUM);
    frameSprite.fillRect(0, 0, DISPLAY_WIDTH, EDIT_TITLE_H, TFT_PINK);
    frameSprite.setTextColor(TFT_BLACK, TFT_PINK);
    frameSprite.drawString("edit -- keys", DISPLAY_PADDING, 2);

    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    for (int i = 0; i < EDIT_HELP_COUNT && i < editRows; i++) {
        frameSprite.drawString(EDIT_HELP_LINES[i], DISPLAY_PADDING,
                               EDIT_TITLE_H + 2 + i * editLineH);
    }

    const int hintY = DISPLAY_HEIGHT - EDIT_HINT_H;
    frameSprite.drawFastHLine(0, hintY, DISPLAY_WIDTH, TFT_PINK);
    frameSprite.setTextColor(TFT_CYAN, TFT_BLACK);
    frameSprite.drawString("press any key", DISPLAY_PADDING, hintY + 2);
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    pushDisplayFrame();

    //telnet
    if (telnetClient && telnetClient.connected()) {
        String out;
        out.reserve((size_t)(editCols + 8) * (EDIT_HELP_COUNT + 3) + 64);
        out += "\x1b[?25l\x1b[2J\x1b[H\x1b[7m";
        out += editFitToCols("edit -- keys");
        out += "\x1b[0m\r\n";
        for (int i = 0; i < EDIT_HELP_COUNT; i++) {
            out += EDIT_HELP_LINES[i];
            out += "\x1b[K\r\n";
        }
        out += "\x1b[36mpress any key\x1b[0m\x1b[K";
        telnetClient.print(out);
    }

    EditKey key;
    char ch;
    while (!editNextKey(key, ch)) {
        delay(2);
    }
    //the buffer view is repainted by the main loop's next render; clear the telnet
    //screen here so leftover help rows can't show through a shorter buffer
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print("\x1b[2J");
    }
    editStatus = "";
}

//   Load / save

//reads `logicalPath` into the buffer. A missing file is not an error -- it's a
//new file, same as nano. Returns false only on a real failure.
static bool editLoadFile(const String& logicalPath, String& err) {
    editLen = 0;
    editCursor = 0;

    const String resolved = resolvePath(cwd, logicalPath);
    RoutedPath r = routePath(resolved);
    if (r.isSd && !sdCardMounted) {
        err = "SD not mounted (insert card and reboot)";
        return false;
    }

    ledPulseStorageRead(r.isSd);
    File f = r.fs->open(r.realPath, "r");
    if (!f) {
        editReindex();
        return true;   //new file
    }
    if (f.isDirectory()) {
        f.close();
        err = resolved + " is a directory";
        return false;
    }
    if (f.size() > EDIT_BUF_CAP) {
        const size_t sz = f.size();
        f.close();
        err = "file is " + String((unsigned long)sz) + "B, limit is "
            + String((unsigned long)EDIT_BUF_CAP) + "B";
        return false;
    }

    ledPulseStorageRead(r.isSd);
    editLen = f.read((uint8_t*)editBuf, f.size());
    f.close();

    //CRLF files would otherwise render a trailing '?' on every line (the CR is
    //not printable), and the stray bytes would survive a save. Normalize on the
    //way in; the file is written back as LF.
    size_t w = 0;
    for (size_t i = 0; i < editLen; i++) {
        if (editBuf[i] == '\r' && i + 1 < editLen && editBuf[i + 1] == '\n') continue;
        editBuf[w++] = editBuf[i];
    }
    editLen = w;

    editReindex();
    if (editLineCount >= EDIT_MAX_LINES) {
        err = "file has more than " + String(EDIT_MAX_LINES) + " lines";
        return false;
    }
    return true;
}

//writes the buffer to `logicalPath` via a temp file + rename. Never truncates
//the target in place: this is a battery device, and a power loss partway through
//a truncating write destroys the file being edited rather than just failing.
static bool editSaveFile(const String& logicalPath, String& err) {
    const String resolved = resolvePath(cwd, logicalPath);
    RoutedPath r = routePath(resolved);
    if (r.isSd && !sdCardMounted) {
        err = "SD not mounted";
        return false;
    }

    const String tmpPath = r.realPath + ".doll-os-tmp";
    ledPulseStorageWrite(r.isSd);
    File f = r.fs->open(tmpPath, "w");
    if (!f) {
        err = "cannot open " + resolved + " for writing";
        return false;
    }

    size_t written = 0;
    while (written < editLen) {
        //chunked so a large buffer doesn't sit in one blocking write; SD_MMC and
        //LittleFS both stream fine, this just keeps each call bounded
        const size_t chunk = min((size_t)4096, editLen - written);
        ledPulseStorageWrite(r.isSd);
        const size_t n = f.write((const uint8_t*)editBuf + written, chunk);
        if (n != chunk) {
            f.close();
            ledPulseStorageWrite(r.isSd);
            r.fs->remove(tmpPath);
            err = "short write (out of space?)";
            return false;
        }
        written += n;
    }
    f.close();

    ledPulseStorageWrite(r.isSd);
    r.fs->remove(r.realPath);   //no-op if it didn't exist
    if (!r.fs->rename(tmpPath, r.realPath)) {
        ledPulseError();
        r.fs->remove(tmpPath);
        err = "rename failed";
        return false;
    }
    return true;
}

//^O: confirm the filename (prefilled, Enter accepts) then write. Updates the
//title bar to whatever was actually written, like nano's "save as".
static void editDoSave() {
    String name = editPathLogical;
    if (!editPromptLine("File name to write: ", name)) {
        editStatus = "Cancelled";
        return;
    }
    if (name.length() == 0) {
        editStatus = "Cancelled (no name)";
        return;
    }

    String err;
    if (!editSaveFile(name, err)) {
        editStatus = "Error: " + err;
        return;
    }
    editPathLogical = name;
    editSyntaxDapp = editPathIsDapp(editPathLogical);   //"save as" can change the language
    //the buffer now matches the file, so this is the depth undoing back to
    //"unmodified" has to reach (see editRefreshModified)
    editSavedUndoDepth = editUndoCount;
    editRefreshModified();
    editStatus = "Wrote " + String((unsigned long)editLen) + " bytes";
}

//^X: returns true if the editor should close
static bool editDoExit() {
    if (!editModified) {
        return true;
    }
    const char answer = editPromptYesNo("Save modified buffer?  Y / N   (^C cancels)");
    if (answer == 'c') {
        return false;
    }
    if (answer == 'n') {
        return true;
    }
    editDoSave();
    return !editModified;   //a failed save keeps the editor open with the message up
}

//   Allocation

static bool editAlloc() {
    editBuf = (char*)psramOrInternalCalloc(EDIT_BUF_CAP, 1, "editBuf");
    editLineStart = (int32_t*)psramOrInternalCalloc(EDIT_MAX_LINES, sizeof(int32_t), "editLineIndex");
    editUndo = (EditUndoRec*)psramOrInternalCalloc(EDIT_UNDO_MAX, sizeof(EditUndoRec), "editUndo");
    if (!editBuf || !editLineStart || !editUndo) {
        if (editBuf) { heap_caps_free(editBuf); editBuf = nullptr; }
        if (editLineStart) { heap_caps_free(editLineStart); editLineStart = nullptr; }
        if (editUndo) { heap_caps_free(editUndo); editUndo = nullptr; }
        return false;
    }
    return true;
}

static void editFree() {
    editUndoClear();   //each delete record owns a PSRAM payload
    if (editUndo) { heap_caps_free(editUndo); editUndo = nullptr; }
    if (editBuf) { heap_caps_free(editBuf); editBuf = nullptr; }
    if (editLineStart) { heap_caps_free(editLineStart); editLineStart = nullptr; }
}

//   Command entry point

void handleEditCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: edit <file>");
        outLine("       edit --repo <id>[@version] [--internal|--sd]");
        outLine("  Full-screen text editor. Path is a normal DOLL-OS path", C_CYAN);
        outLine("  (e.g. /sd/notes.txt, notes.txt). A missing file is", C_CYAN);
        outLine("  created on save.", C_CYAN);
        outLine("  --repo downloads a verified Dapper package into the", C_CYAN);
        outLine("  buffer; ^O saves it as /apps/<id>.dapp or /sd/apps/<id>.dapp.", C_CYAN);
        outLine("  Keys: ^G help, ^O save, ^X exit, ^K cut, ^U paste,", C_CYAN);
        outLine("        ^W find, ^_ goto line, ^Z undo, ^A/^E, ^Y/^V, ^D.", C_CYAN);
        outLine("  ^G lists the rest. Ctrl+Up/Down nudges radio volume.", C_CYAN);
        outLine("  Limits: 128KB, 4000 lines, 64 undo steps.", C_CYAN);
        outLine("  Tabs are kept as tabs; the cut buffer survives between", C_CYAN);
        outLine("  files, so ^K in one and ^U in another moves text.", C_CYAN);
        outLine("  .dapp files get syntax highlighting on both surfaces.", C_CYAN);
        return;
    }

    bool loadingFromRepo = false;
    String loadPath = parts[1];
    String loadedLabel;
    if (parts[1] == "--repo" || parts[1] == "repo") {
        if (partCount < 3) {
            outLine("Usage: edit --repo <id>[@version] [--internal|--sd]", C_RED);
            return;
        }
        String targetPreference;
        for (int i = 3; i < partCount; i++) {
            if (parts[i] == "--internal" || parts[i] == "--flash") {
                if (targetPreference.length() > 0) {
                    outLine("edit: choose only one repository save target", C_RED);
                    return;
                }
                targetPreference = "internal";
            } else if (parts[i] == "--sd") {
                if (targetPreference.length() > 0) {
                    outLine("edit: choose only one repository save target", C_RED);
                    return;
                }
                targetPreference = "sd";
            } else {
                outLine("edit: unknown repository option: " + parts[i], C_RED);
                return;
            }
        }

        String sourcePath;
        String suggestedSavePath;
        String error;
        if (!dapperFetchPackageForEdit(parts[2], targetPreference, sourcePath,
                                       suggestedSavePath, loadedLabel, error)) {
            outLine("edit: " + error, C_RED);
            return;
        }
        loadPath = sourcePath;
        editPathLogical = suggestedSavePath;
        loadingFromRepo = true;
    } else {
        editPathLogical = parts[1];
    }

    if (!editAlloc()) {
        outLine("edit: out of memory", C_RED);
        if (loadingFromRepo) {
            RoutedPath r = routePath(loadPath);
            if (!r.isSd) r.fs->remove(r.realPath);
        }
        editFree();
        return;
    }

    //geometry from the panel -- both surfaces render this grid (see the header:
    //there is no NAWS negotiation to ask a telnet client its real size)
    editCharW = max(1, (int)frameSprite.textWidth("M"));
    editLineH = max(frameSprite.fontHeight() + 2, 8);
    editCols = max(8, (DISPLAY_WIDTH - 2 * DISPLAY_PADDING) / editCharW);
    editRows = max(1, (DISPLAY_HEIGHT - EDIT_TITLE_H - EDIT_HINT_H - 4) / editLineH);

    editSyntaxDapp = editPathIsDapp(editPathLogical);
    editTopLine = 0;
    editLeftCol = 0;
    editGoalCol = -1;
    editStatus = "";
    editTelnetKeys = EditKeyState();
    editKeyboardKeys = EditKeyState();
    editUndoClear();
    //editCutBuffer and editLastSearch deliberately survive across launches -- this
    //board has no clipboard, so cutting in one file and pasting into another is the
    //only way to move text between them. Only the "appending" flag resets, so the
    //first ^K of a new session starts a fresh cut rather than extending the old one.
    editLastKeyWasCut = false;

    String err;
    if (!editLoadFile(loadPath, err)) {
        outLine("edit: " + err, C_RED);
        if (loadingFromRepo) {
            RoutedPath r = routePath(loadPath);
            if (!r.isSd) r.fs->remove(r.realPath);
        }
        editFree();
        return;
    }
    if (loadingFromRepo) {
        RoutedPath r = routePath(loadPath);
        if (!r.isSd) r.fs->remove(r.realPath);
    }
    //loading writes editBuf directly rather than going through editInsertBytes, so
    //nothing was recorded and an empty undo stack is exactly the on-disk state
    editSavedUndoDepth = loadingFromRepo ? -1 : 0;
    editRefreshModified();
    if (loadingFromRepo) {
        editStatus = "Loaded " + loadedLabel + "; ^O writes " + editPathLogical;
    }

    //take the telnet screen; the panel is taken by the first editRender() below.
    //Note we deliberately do NOT call radioService() in the loop that follows --
    //it prints through outLine(), which would scribble raw lines across both
    //surfaces mid-frame. Stream announcements queue up and land after exit, the
    //same way they do during an ssh/telnet session (RemoteSession.ino).
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print("\x1b[2J\x1b[H");
    }

    bool blinkPhase = false;
    editNeedsRender = true;

    for (;;) {
        //drain every pending key from both sources, then render once -- see the
        //rendering section header for why this coalescing is load-bearing
        EditKey key;
        char ch;
        bool quit = false;

        while (editNextKey(key, ch)) {
            editNeedsRender = true;
            editStatus = "";   //any keypress clears a stale message

            //a run of typing or backspacing is only allowed to keep coalescing into
            //one undo step while nothing else intervenes, and only consecutive ^K
            //presses append to the cut buffer rather than replacing it
            if (key != EK_CHAR && key != EK_BACKSPACE) editUndoRun = 0;
            if (key != EK_CUT) editLastKeyWasCut = false;

            switch (key) {
                case EK_CHAR:      editInsertBytes(&ch, 1, 1); editGoalCol = -1; break;
                case EK_ENTER:     editInsertBytes("\n", 1, 0); editGoalCol = -1; break;
                case EK_TAB:       editInsertBytes("\t", 1, 0); editGoalCol = -1; break;
                case EK_BACKSPACE:
                    if (editCursor > 0) editDeleteBytes(editCursor - 1, 1, 2);
                    editGoalCol = -1;
                    break;
                case EK_DELETE:    editDeleteBytes(editCursor, 1, 0); editGoalCol = -1; break;
                case EK_LEFT:      editMoveLeft(); break;
                case EK_RIGHT:     editMoveRight(); break;
                case EK_UP:        editMoveVertical(-1); break;
                case EK_DOWN:      editMoveVertical(1); break;
                case EK_PGUP:      editMoveVertical(-editRows); break;
                case EK_PGDN:      editMoveVertical(editRows); break;
                case EK_HOME:      editMoveHome(); break;
                case EK_END:       editMoveEnd(); break;
                case EK_CUT:       editDoCut(); break;
                case EK_UNCUT:     editDoUncut(); break;
                case EK_SEARCH:    editDoSearch(); break;
                case EK_GOTO:      editDoGotoLine(); break;
                case EK_UNDO:      editApplyUndo(); break;
                case EK_HELP:      editShowHelp(); break;
                case EK_SAVE:      editDoSave(); break;
                case EK_EXIT:      if (editDoExit()) quit = true; break;
                case EK_CANCEL:    editStatus = editPositionText(); break;
                default: break;
            }
            if (quit) break;
        }
        if (quit) break;

        //blink is the one reason to repaint an otherwise-unchanged frame, same
        //rule drawDisplayFrame() follows
        const bool phase = ((millis() / DISPLAY_CURSOR_BLINK_MS) & 1UL) == 0;
        if (phase != blinkPhase) {
            blinkPhase = phase;
            editNeedsRender = true;
        }

        if (editNeedsRender) {
            editRender();
            editNeedsRender = false;
        }

        ledService();
        delay(2);   //yield so CPU idle runs and feeds the task watchdog
    }

    editFree();

    //hand both surfaces back to the shell
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print("\x1b[?25h\x1b[2J\x1b[H");
    }
    displayDirty = true;
    outLine("edit: closed " + editPathLogical, C_GREEN);
    drawDisplayFrame();
    printPrompt();
}
