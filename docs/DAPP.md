# DOLL-OS .dapp Apps

`.dapp` files are text executables for the DOLL-OS shell. DOLL-OS looks for them in both
places:

```text
/sd/apps   on the SD card (upload with FTP into /apps)
/apps      on internal flash (LittleFS on the SPIFFS-labeled partition)
```

Normal sketch upload and flash upload are separate on this board:

```text
Upload          flashes firmware only
LittleFS/SPIFFS uploads the filesystem image
```

So `data/apps/<name>.dapp` only lands on the device when you run a dedicated
filesystem upload. Built-in firmware-seeded apps are written to `/system/apps`
on boot; `/sd/apps` and `/apps` override a built-in app with the same name.
This firmware bundle also seeds a plain text copy of this guide to
`/docs/dapp.txt` on LittleFS.

Use:

```text
apps
run hello
run /apps/hello.dapp
run /sd/apps/hello.dapp
```

## Example

```text
# /sd/apps/hello.dapp
COLOR cyan
PRINT "hello from a DOLL-OS app"
PRINT "cwd=$cwd ip=$ip battery=$battery%"
INPUT name "name> "
PRINT "hi, $name"
RAND lucky 1 100
PRINT "lucky number: $lucky"
WAIT 750
COLOR pink
PRINT "tiny executable acquired"
EXIT
```

## Editing

`edit /sd/apps/hello.dapp` syntax highlights the file on both the panel and a
connected telnet client. Comments are grey, commands cyan, labels pink, quoted
strings green, `$variables` yellow, and numbers orange. Highlighting keys off the
`.dapp` extension, so saving under a new name with `^O` switches it on or off.
An opcode that stays white is one `run` will reject as unknown.

`edit --repo snake` downloads the compatible repository version into the editor
without installing it first. `^O` writes it to the suggested app path, or to any
path you type into the save prompt.

## Metadata Directives

Package metadata lives in leading comments such as `# @id hello`. AppRunner also
honors `# @echo off`, which keeps `INPUT` and `INPUTSECRET` from copying the
submitted prompt line into scrollback. Use it for chat-style apps that print the
submitted text later through their own display flow. The default is echo on.

## Commands

```text
PRINT <text>        print text, with $variables expanded
ECHO <text>         alias for PRINT
COLOR <name>        white, red, green, yellow, blue, magenta, cyan, pink
CLEAR               clear the terminal, or the canvas if one is up
CLS                 alias for CLEAR
WAIT <ms>           pause while keeping display/radio/FTP serviced
SLEEP <ms>          alias for WAIT
SET <name> <value>  set a numeric variable
ADD <name> <value>  add to a numeric variable
SUB <name> <value>  subtract from a numeric variable
MUL <name> <value>  multiply a numeric variable
DIV <name> <value>  divide a numeric variable (integer result)
MOD <name> <value>  remainder of a numeric variable
EXPR <name> <expr>  evaluate a full arithmetic expression
RAND <n> <max>      set numeric variable n to 0..max-1
RAND <n> <min> <max> set numeric variable n to min..max
DIM <name> <size>   create a numeric array of `size` cells, all zero
LIFE <cur> <next> <cols> <rows>  advance Conway's Life in numeric arrays
SETSTR <name> <txt> set a string variable
APPEND <name> <txt> append to a string variable
CHR <name> <code>   set a string variable to one character by code
HEX <name> <value> [width]  uppercase hexadecimal text, width 1..8
SUBSTR <n> <txt> <start> <count>   slice a string into a string variable
LEN <name> <text>   character count into a numeric variable
CHARAT <n> <txt> <i> character code at index i, or 0 past the end
INPUT <name> [p]    read a line into a string variable (blocks until Enter)
INPUTSECRET <name> [p]  read a masked line into a string variable
KEY <name>          read one keypress into a numeric variable, 0 if none
LED <r> <g> <b>     set the rear RGB LED (0..255 per channel)
WAVE <ch> <kind> <hz> <level>  set synth channel 1..3; level is 0..100
WAVESTOP            silence all synth channels and release audio hardware
HTTPGET <name> <url> [max]  bounded HTTP/HTTPS GET into a string
HTTPPOST <name> <url> <body> [max]  bounded POST response into a string
HTTPHEADER <name> <value>  set/replace one of up to eight request headers
HTTPCLEAR           clear request headers
HTTPGETBUF <url> [max]  GET straight into the byte buffer, past the 4096 cap
BUFNEW [bytes]      allocate the byte buffer (default 65536, maximum 262144)
BUFFREE             release it; BUFCLEAR empties it without reallocating
BUFAT <n> <pos>     byte at pos into a numeric variable, 0 past the end
BUFSUB <name> <pos> <count>  copy a slice into a string variable
BUFWRITE <pos> <text>  copy text into the buffer at pos
BUFSCAN <n> <pos> [stops]  advance past bytes until one in stops; "" means space
BUFTAKE <s> <n> <pos> [stops]  same, and copy what was passed into a string
BUFSAVE <path>      write the buffer to a file; BUFLOAD reads one back
HTMLTEXT <url> <textpath> <linkpath|-> [wrap] [maxlinks]  fetch and render a page
HTMLOPEN <textpath> <linkpath|-> <base> [wrap] [maxlinks]  start a render
HTMLFEED url <url> | buf [pos count] | text <string>  add a source to the render
HTMLCLOSE           finish the render and close its files
HTMLSTR <name> url <url> | buf | text <s>  render into a string variable
URLABS <name> <base> <href>  absolute URL, or "" if it is not followable
URLPART <name> <url> scheme|origin|host|path|dir  one piece of a URL
DAPPER <action> [args]  call the verified Dapper package manager
JSONESC <name> <text>  escape text for insertion inside a JSON string
JSONGET <name> <json> <path>  extract a JSON value; $jsonok reports success
FOPEN <path> <mode> open a file: read, write, append, or update
FCLOSE              close it (automatic when the app ends)
FREAD <name>        read one line into a string variable; $feof goes 1 at end
FREADB <name>       read one raw byte (0..255) into a numeric variable
FWRITE <text>       write a line, with $variables expanded
FWRITEB <value>     write one raw byte (0..255)
FSEEK <offset>      move to an absolute byte offset; $fok reports success
FTELL <name>        current byte offset into a numeric variable
FSIZE <name>        open file size into a numeric variable
FEXISTS <n> <path>  1 into numeric variable n if the path exists
FDELETE <path>      delete a file; $fok reports success
FLIST <dir> <output> snapshot children as D|name|0 or F|name|bytes
FMKDIR <path>       create one directory; $fok reports success
FCOPY <src> <dst>   copy one file without overwriting
FMOVE <src> <dst>   move one file without overwriting
CANVAS <cols> <rows> switch the display to a character grid
ENDCANVAS           leave canvas mode, restoring the terminal
PUT <col> <row> <text>  draw text into the canvas in the current COLOR
FLIP                push the canvas to the panel and telnet client
LABEL <name>        define a jump target
:<name>             shorthand label
GOTO <name>         jump to a label
GOSUB <name>        call a label, returning to the next line
RETURN              return from the most recent GOSUB
IF <l> <op> <r> GOTO|GOSUB <name>
IFEQ <l> <r> GOTO|GOSUB <name>
IFNE <l> <r> GOTO|GOSUB <name>
EXIT                leave the app
END                 alias for EXIT
```

`IF` supports `=`, `==`, `!=`, `<>`, `<`, `<=`, `>`, and `>=`.
`RAND roll 6` returns `0..5`; `RAND roll 1 6` returns `1..6`.
`IFEQ` and `IFNE` compare strings. Quote string literals that contain spaces.

## Arrays

`DIM` makes a numeric array. Cells are read as `$name[index]` anywhere a value is
accepted, and written by using `name[index]` where a variable name goes:

```text
DIM well 200
SET well[0] 3
EXPR i $y * 10 + $x
SET well[$i] $well[0]
IF $well[$i] <> 0 GOTO occupied
```

The index is itself a value, so `$board[$row]` and `$board[$a[1]]` both work. An
index outside the array stops the app with the offending line number rather than
quietly reading zero. All arrays share a pool of 8192 cells.

`LIFE cur nxt 38 20` is a native Conway's Game of Life step for dense grids.
Both arrays must be `DIM`'d with at least `cols * rows` cells. It reads `cur`,
writes the next generation through `nxt`, then copies the result back into
`cur`.

## Arithmetic

`SET`/`ADD`/`SUB`/`MUL`/`DIV`/`MOD` each take one value, which is enough for
counters and awkward for anything else. `EXPR` takes a whole expression instead
and hands it to the same evaluator the `calc` command uses:

```text
EXPR index $row * 10 + $col
EXPR wrapped ($angle + 360) % 360
EXPR level floor($lines / 10) + 1
```

`+ - * / ^ %` work, along with `abs`, `floor`, `ceil`, `sqrt`, `pow`, and the
trig functions — the `calc help` list. Spaces are fine. Every `$name` is replaced
by its **numeric** value before evaluation, and the result is rounded to a whole
number on the way into the variable. Note that `/` divides as a real number:
`EXPR a $b / 10` on `b = 5` stores `1`, not `0`. Wrap it in `floor()` when you
want truncation.

## Subroutines

`GOSUB` jumps like `GOTO` but remembers where it came from, and `RETURN` goes
back to the line after the call. Nesting is allowed up to 64 deep.

```text
GOSUB redraw
GOTO done

:redraw
PUT 0 0 "score $score"
FLIP
RETURN
```

There is one rule worth stating plainly: **a routine must leave through
`RETURN`**, not by `GOTO`-ing somewhere else. Jumping out strands the return
address on the stack, and enough of those in a loop will hit the depth limit. If
a routine needs to end the round, set a variable and let the caller act on it.

## Keys and games

`INPUT` blocks until Enter, which makes it useless for anything that has to keep
moving while nobody is typing. `KEY` reads at most one keypress and returns
immediately with `0` when nothing is waiting, from either the telnet client or a
BLE keyboard on the companion DOLL-OS keyboard bridge:

```text
:loop
KEY k
IF $k = $kleft GOSUB move_left
IF $k = $kesc GOTO quit
WAIT 16
GOTO loop
```

Printable keys come back as their ASCII code (`65` for `A`, `32` for space), and
these built-ins name the rest: `$kup`, `$kdown`, `$kleft`, `$kright`, `$kenter`,
`$kesc`, `$kback`, `$ktab`, `$kspace`. They are numbers, not text — compare
against them, don't `PRINT` them. Ctrl+C and Ctrl+T both read as `$kesc`, so the
chord that leaves every other DOLL-OS screen also leaves yours. Ctrl+X is never
delivered to the script: it aborts the running app, from `KEY`, `WAIT`, or a
blocked `INPUT` alike.

`LED` sets the rear RGB LED and requires AppRunner `>=1.3.0`. Channel values are
clamped into `0..255`, so negative values become `0` and values above `255`
become `255`. Check `$ledok` first when writing portable apps that may run on
builds where rear LED control is disabled.

`WAVE` is the three-channel PCM synthesizer added in AppRunner `>=1.4.0`:

```text
WAVE 1 sine 220 25
WAVE 2 triangle 330 20
WAVE 3 noise 4000 10
WAIT 2000
WAVESTOP
```

Kinds are `sine`, `triangle`, `square`, `sawtooth`, `noise`, and `off`;
frequency is `1..12000` Hz and level is `0..100` per channel. `sawtooth`
requires AppRunner `>=1.8.0` -- older runtimes reject it as an invalid
waveform. Noise frequency controls its sample-and-hold rate. The three voices
are mixed and clamped before reaching the onboard ES8388 speaker. Starting the
synth stops/relinquishes internet radio, and leaving the app always silences
and releases the synth. `$audiook` reports whether the most recent hardware
start succeeded. `run synth` is the interactive three-channel mixer and
waveform display.

`CANVAS` replaces the scrolling terminal with a grid you address by cell. `PUT`
writes into the grid without drawing anything, and `FLIP` shows the result — so a
frame is assembled off-screen and appears at once. On the panel the grid is
scaled up to fill the terminal area, so a 10x20 playfield gets big cells and an
80x24 status screen gets small ones. `CLS` blanks the grid while a canvas is up.
`ENDCANVAS` puts the terminal back, and so does leaving the app.

```text
CANVAS 20 10
COLOR cyan
PUT 6 4 "hello"
FLIP
WAIT 1000
ENDCANVAS
```

A canvas is at most 120 by 60 cells. `run tetris` is the worked example: a well
in a 200-cell array, pieces rotated with `EXPR`, gravity paced off `$millis`, and
every frame drawn cell by cell.

Tab5 applications should normally use `84x36` for roomy calendar or grid views,
or `100x40` for multi-panel tools. Those sizes retain readable glyphs on the
1280x720 panel while exposing substantially more information than the inherited
FNK layouts. The bundled Calendar and Sheet demonstrate `84x36`; Files, Today,
Dappstore, and Paint demonstrate `100x40`. Touch is intentionally unavailable,
so every visible action must remain reachable from the keyboard and the active
focus or shortcut state must be written on the canvas.

## Files

One file can be open at a time — `FOPEN` closes any previous one, and the app
ending closes the last. Paths work exactly like shell paths: `/sd/...` is the
card, everything else is flash, relative paths resolve against the shell's
`cwd`, and `$variables` expand inside them. `update` (also `rw` or `r+`) opens
an existing file without truncating it and permits both reads and writes.

`FLIST` closes the script file handle and writes a stable snapshot of one
directory. Each immediate child is `D|name|0` or `F|name|bytes`. `FMKDIR`,
`FCOPY`, and `FMOVE` are non-recursive, and copy/move refuse to overwrite an
existing destination. They enable file-manager apps without exposing arbitrary
shell commands.

```text
FOPEN "/apps/scores.txt" append
FWRITE "$name $score"
FCLOSE
```

Reading is line by line; `$feof` becomes 1 when a read finds nothing left (an
empty line in the middle of a file leaves it 0, so the two are distinguishable):

```text
FOPEN "/apps/scores.txt" read
:rl
FREAD line
IF $feof = 1 GOTO done
PRINT $line
GOTO rl
:done
FCLOSE
```

A file that can't be opened is not an error — `$fok` is 0 and the script
decides, because a missing save file is a normal situation. Misusing the handle
is an error: `FREAD` with nothing open (or a file opened for write), or `FWRITE`
on a file opened for read, stops the app like any other bug. One more thing to
remember: numbers written with `FWRITE` come back as *text* — parse digits with
`CHARAT` (the book's `str2num` routine) before doing math on them. `run tetris`
does exactly this for its persistent high score in `/apps/tetris.hs`.

Binary access does not pass bytes through a `String`: `FREADB` returns `0..255`
in a numeric variable and `$feof` distinguishes a real `0x00` byte from end of
file. `FWRITEB`, `FSEEK`, `FTELL`, and `FSIZE` make in-place editors possible.
`HEX` formats values without a hand-written conversion table. `run hex` uses all
of these to edit NULs, newlines, and every other byte without altering them.

## HTTP and HTTPS

`HTTPGET result "https://example.com/data.json" 2048` performs a GET and stores
at most the requested number of bytes (maximum 4096) in a string. `HTTPPOST`
does the same with a request body. `HTTPHEADER` sets or replaces one of eight
headers for later requests, and `HTTPCLEAR` removes them. Headers are reset when
an app starts and ends. The request
does not stop the app when the network or server fails; inspect `$httpok`,
`$httpcode`, `$httplen`, and `$httptruncated`. Up to five redirects are followed
and compressed responses are declined.

Successive requests to the same origin reuse one connection, so a script that
fetches several pages from a host pays for the TLS handshake once. The connection
is dropped when the origin changes, when a response is truncated, and when the
app exits.

## Buffers

`HTTPGET` stores into a string variable and so cannot exceed 4096 bytes. The byte
buffer is the way past that: one flat block of up to 262144 bytes that holds raw
bytes rather than text.

```text
BUFNEW 131072
HTTPGETBUF "https://example.com/big.csv"
PRINT "got $buflen of $bufcap bytes"
```

`BUFAT` reads one byte and `BUFSUB` copies a slice into a string, but walking a
document one `BUFAT` at a time costs an interpreter step per byte. `BUFSCAN` and
`BUFTAKE` stop on a character class instead, so a scan costs a step per *token*:

```text
SET pos 0
:word
BUFTAKE w pos $pos " ,"
IF $w = "" GOTO done
PRINT "$w"
ADD pos 1
IF $pos < $buflen GOTO word
:done
```

An empty stop set means whitespace. `BUFSAVE` and `BUFLOAD` move the buffer to
and from a file, and both need the script's own file handle closed first. The
buffer is released when the app exits.

## HTML

`HTMLTEXT` fetches a page and renders it to readable text in one instruction:

```text
HTMLTEXT "https://example.com/" "/page.txt" "/page.lnk" 76 200
PRINT "$htmllines lines, $htmllinks links, $htmlbytes bytes"
```

The response body is streamed through the renderer as it arrives, so the page is
never held anywhere and its size is bounded by the filesystem rather than by any
runtime limit. `/page.txt` gets word-wrapped text with each link marked `[n]` in
place; `/page.lnk` gets one `<n> <absolute-url>` line per link, in marker order,
so link *n* is line *n*. Pass `-` as the link path to skip link collection.

`HTMLOPEN`/`HTMLFEED`/`HTMLCLOSE` are the same renderer with the pieces exposed,
for rendering several sources into one file or for HTML a script already holds.
`HTMLSTR` renders into a string variable for pages small enough to want no file
at all. `$htmlok` reports whether the render started.

Relative links are resolved against the page's own URL, so what lands in the link
file is directly fetchable. `URLABS` and `URLPART` expose that resolution on its
own — `URLABS` returns `""` for anything not followable over http(s), including
fragments, `mailto:`, and `javascript:`.

The renderer skips `<script>`, `<style>`, and comments; understands entities and
UTF-8; and transliterates non-ASCII to ASCII, because the panel font draws
nothing above 126. There is no JS, CSS, imagery, or form handling. Because the
renderer opens two files of its own and the runtime allows the script only one,
`HTMLOPEN` and `HTMLTEXT` refuse to start while a script file is open.

## Dapper bridge

`DAPPER` is a narrow bridge to the built-in package manager. It accepts Dapper's
normal actions—`search`, `info`, `install`, `update`, `remove`, `refresh`,
`doctor`, and `runtime`—but cannot invoke any other shell command:

```text
DAPPER search game
DAPPER info snake
DAPPER install snake --internal
DAPPER update --all
```

The bridge uses Dapper's existing repository-identity, compatibility, HTTPS,
SHA-256, managed-path, atomic replacement, and rollback checks. Scripts should
confirm destructive actions such as `remove` with the user. Supplying
`--internal` or `--sd` to `install` avoids Dapper's own interactive location
prompt and is recommended for app front ends.

`JSONESC safe $prompt` protects quotes, backslashes, and control characters
before text is inserted between JSON quotes. `JSONGET answer $response
"choices[0].message.content"` walks object keys and array indexes. Both report
through `$jsonok` instead of stopping on malformed external data. `INPUTSECRET`
masks tokens on both the device and browser runner, but a secret still lives in
RAM until the app clears it or exits.

HTTPS traffic is encrypted, but arbitrary script URLs use the same insecure
certificate mode as ASUKA's generic URL fetch: the server certificate is not
authenticated. Dapper downloads remain on their separate CA-verified path.

## Time

`TIME` (AppRunner `>=1.9.0`) syncs the clock over NTP and reports the result
as UTC -- there's no timezone setting, so a script wanting local time adds its
own offset:

```text
TIME
IF $timeok = 0 GOTO no_clock
PRINT "$timeyear-$timemonth-$timeday $timehour:$timeminute:$timesecond UTC"
```

`$timeok` is 1 on success, 0 if there's no network or NTP didn't answer within
about 8 seconds -- check it before trusting the rest. `$timeepoch` is Unix
seconds; `$timeyear`, `$timemonth` (1-12), `$timeday`, `$timehour`,
`$timeminute`, `$timesecond`, and `$timeweekday` (0=Sunday..6=Saturday) are the
same moment already split into fields, since there's no calendar math in
`EXPR` to do it yourself. The device caches a synced clock, so a `TIME` call
after the first one in a session usually returns immediately.

## Limits

A script is read into RAM in full before its first line runs. The storage comes from
PSRAM, so the caps are roomy:

```text
4000       lines per app (same cap as the `edit` editor)
512        labels
128        numeric variables
64         string variables
32         arrays, sharing a pool of 16384 cells
64         nested GOSUB calls
1          open file at a time
120 x 60   largest canvas
4096       characters per string variable
262144     bytes in the byte buffer (BUFNEW), default 65536
1000000    executed steps between waits before the loop guard trips
```

`HTMLTEXT` is not on that list: it streams to disk and never holds the page.

Hitting one is reported on the terminal rather than failing silently. The step
guard counts instructions since the last `WAIT` or `INPUT`, on the grounds that a
runaway loop is precisely one that never yields — so a game pacing itself with
`WAIT 16` can run all day, while a bare `GOTO` loop still trips. A loop that
`WAIT`s forever on purpose (or by accident) is stopped from the keyboard instead:
Ctrl+X aborts any running app.

Built-ins usable as `$name` or numeric values:

```text
$battery
$cwd
$heap
$ip
$millis
$seconds
$wifi
$ledok
$audiook
$httpok
$httpcode
$httplen
$httptruncated
$jsonok
$buflen
$bufcap
$bufok
$htmlok
$htmllines
$htmllinks
$htmlbytes
```

Numeric only (see Keys and games): `$kup`, `$kdown`, `$kleft`, `$kright`,
`$kenter`, `$kesc`, `$kback`, `$ktab`, `$kspace`; plus the file-op status pair
`$fok` (last FOPEN/FDELETE succeeded) and `$feof` (last FREAD hit end of file).

## Interactive Example

```text
# /sd/apps/ask.dapp
COLOR pink
PRINT "tiny prompt"

:again
INPUT reply "say> "
IFEQ $reply "/quit" GOTO done
PRINT "you said: $reply"
GOTO again

:done
PRINT "bye"
EXIT
```
