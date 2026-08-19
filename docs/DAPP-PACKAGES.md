# DOLL-OS `.dapp` Packages and Repositories

Status: proposed version 1 specification

This document defines how DOLL-OS identifies, publishes, installs, updates, and
removes `.dapp` applications. A package remains one plain-text `.dapp` file. The
package metadata is a comment header at the beginning of that file, so packaged
apps remain readable, editable, directly executable, and backward-compatible
with AppRunner versions that predate the package manager.

The on-device package manager is called **Dapper** and uses `/.dapper` for its
private state.

The first package format intentionally has no archives, native code, install
scripts, or package dependencies. Those features would add memory, security,
and rollback costs without helping the current single-file app model.

## 1. Version model

Three versions are independent and must not be substituted for one another.

| Version | Example | Meaning |
| --- | --- | --- |
| Package format | `1` | Syntax and required fields of the metadata header and repository records |
| App version | `1.3.0` | Version of one app, assigned by its publisher |
| AppRunner API | `1.8.0` | Language behavior, opcodes, built-ins, and runtime contract implemented by the firmware |

Firmware versions are deliberately not package compatibility versions. A
firmware release may change Wi-Fi, display, or shell code without changing the
AppRunner API.

### 1.1 Package format versions

Package format versions are positive integers. DOLL-OS package manager version
1 accepts only `@dapp-format 1`. A future incompatible metadata or repository
schema increments this integer.

Unknown metadata fields must be ignored. This lets format 1 gain optional fields
without making old clients reject otherwise compatible packages.

### 1.2 App versions

App versions use stable Semantic Versioning triples: `MAJOR.MINOR.PATCH`.

- `PATCH` fixes the app without intentionally changing its public behavior or
  save-data format.
- `MINOR` adds backward-compatible content or behavior.
- `MAJOR` may make incompatible behavior or save-data changes.

The version-1 device client only resolves stable numeric triples. Repository
tooling may publish prereleases such as `2.0.0-beta.1`, but they are installed
only when the user requests that exact version. Normal install and update
operations ignore prereleases.

Published versions are immutable. Changing a released `.dapp` requires a new
app version, even for a one-character correction.

### 1.3 AppRunner API versions

AppRunner API versions also use `MAJOR.MINOR.PATCH`.

- `PATCH` fixes an interpreter defect without intentionally changing valid app
  behavior.
- `MINOR` adds backward-compatible opcodes, built-ins, or higher resource
  limits.
- `MAJOR` removes an API or changes the meaning of a previously valid app.

Reducing a documented resource limit is a breaking change. Increasing one is
backward-compatible.

Each firmware exposes these compile-time values:

```cpp
#define DOLL_BOARD_ID "m5cardputer"
#define DAPP_RUNTIME_VERSION "1.3.0"
#define DAPP_PACKAGE_FORMAT 1
```

The shell command `dapper runtime` prints all three values. Package selection uses
the numeric version components; it must not compare version strings
lexicographically.

The initial version assignments are:

| Firmware family | Board ID | Initial AppRunner API |
| --- | --- | --- |
| M5Cardputer DOLL-OS | `m5cardputer` | `1.3.0` |
| Freenove FNK0104 DOLL-OS | `fnk0104` | `1.5.0` |
| M5Stack Tab5 DOLL-OS | `m5stack-tab5` | `1.9.0` |

These assignments describe the checked-in implementations summarized in
section 4. The FNK0104 implementation contains the complete `1.0.0` command set
plus the `1.1.0`, `1.2.0`, `1.3.0`, `1.4.0`, `1.4.1`, `1.4.2`, and `1.5.0`
extensions.

## 2. Board identity

Board IDs describe a DOLL-OS runtime family, not an Arduino FQBN or a particular
screen panel.

| Board ID | Hardware family |
| --- | --- |
| `m5cardputer` | M5Cardputer using the M5Stamp-S3, 8 MB flash, no PSRAM |
| `fnk0104` | Freenove FNK0104-series ESP32-S3 display boards, 16 MB flash and PSRAM |
| `m5stack-tab5` | M5Stack Tab5 using the ESP32-P4, 16 MB flash, 32 MB PSRAM, and a 1280x720 display |

The FNK0104AB, FNK0104N, and FNK0104S display variants share `fnk0104` because
`.dapp` programs use the terminal and canvas abstractions rather than a panel
driver directly. If a future DOLL-OS port has materially different AppRunner
behavior or resource limits, it receives a new board ID.

Tab5 receives its own board ID even though its initial AppRunner limits match
FNK0104. Its board-specific artifacts can use 80-100 column by 30-40 row canvas
layouts and longer terminal records without making the smaller FNK panels render
those layouts illegibly.

Every package artifact declares one or more supported board IDs. There is no
wildcard board value in format 1: publishers must make compatibility explicit.

## 3. Package file

### 3.1 Metadata header

Metadata is a contiguous block of `# @` comment lines before the app's first
executable line:

```text
# @dapp-format 1
# @id hello
# @name Hello
# @version 1.0.0
# @boards m5cardputer,fnk0104
# @runtime >=1.0.0 <2.0.0
# @author Doll
# @summary A tiny greeting app
# @license MIT

PRINT "hello from DOLL-OS"
...
```

AppRunner already ignores `#` comments, so old firmware can still run a packaged
file. The package manager reads no more than the first 2,048 bytes or 32 lines
when inspecting the header. Metadata must therefore fit inside both limits.
AppRunner's line limit counts every physical line, including metadata, ordinary
comments, and blank lines.

Required fields:

| Field | Rules |
| --- | --- |
| `dapp-format` | Must be `1` |
| `id` | Lowercase ASCII matching `[a-z0-9][a-z0-9-]{0,31}` |
| `name` | Display name, 1 through 48 UTF-8 bytes |
| `version` | Stable SemVer triple for normal releases |
| `boards` | Comma-separated board IDs supported by this exact file |
| `runtime` | AppRunner compatibility constraint |

Optional format-1 fields are `author`, `summary`, `license`, `homepage`, and
`source`. Values occupy one line and cannot contain tabs or control characters.

The installed filename is `<id>.dapp`. The metadata ID, repository record ID,
and filename must agree.

### 3.2 Runtime constraints

Format 1 supports this deliberately small constraint grammar:

```text
>=MIN_VERSION <MAX_EXCLUSIVE_VERSION
```

For example, `>=1.1.0 <2.0.0` accepts compatible AppRunner 1.x releases starting
at 1.1.0. An omitted upper bound is not allowed because an app cannot safely
claim compatibility with an unknown future major API.

### 3.3 One release, multiple artifacts

An app version may have:

- one universal artifact whose `boards` field lists both current board IDs; or
- separate artifacts with the same app ID and version for different boards.

A universal artifact must satisfy the limits and opcode set of every board it
declares. Board-specific files are appropriate when the FNK0104 build uses its
larger limits or AppRunner 1.1 extensions while the M5Cardputer build uses a
smaller implementation.

## 4. Current compatibility matrix

This matrix is a snapshot of the checked-in AppRunner implementations when this
specification was written. Repository validation should generate and test this
information from source so it cannot quietly drift.

### 4.1 Opcodes

| AppRunner API | Opcodes introduced |
| --- | --- |
| `1.0.0` | `ADD`, `APPEND`, `CLEAR`, `CLS`, `COLOR`, `ECHO`, `END`, `EXIT`, `GOTO`, `IF`, `IFEQ`, `IFNE`, `INPUT`, `LABEL`, `PRINT`, `RAND`, `SET`, `SETSTR`, `SLEEP`, `WAIT` |
| `1.1.0` | `CANVAS`, `CHARAT`, `CHR`, `DIM`, `DIV`, `ENDCANVAS`, `EXPR`, `FCLOSE`, `FDELETE`, `FEXISTS`, `FLIP`, `FOPEN`, `FREAD`, `FWRITE`, `GOSUB`, `KEY`, `LEN`, `MOD`, `MUL`, `PUT`, `RETURN`, `SUB`, `SUBSTR` |
| `1.2.0` | *(no new opcodes; pre-LED package boundary)* |
| `1.3.0` | `LED` |
| `1.4.0` | `FREADB`, `FWRITEB`, `FSEEK`, `FTELL`, `FSIZE`, `HEX`, `INPUTSECRET`, `HTTPGET`, `HTTPPOST`, `HTTPHEADER`, `HTTPCLEAR`, `JSONESC`, `JSONGET`, `WAVE`, `WAVESTOP` |
| `1.4.1` | `LIFE` |
| `1.4.2` | *(no new opcodes; adds `# @echo off` syntax feature)* |
| `1.5.0` | `BUFAT`, `BUFCLEAR`, `BUFFREE`, `BUFLOAD`, `BUFNEW`, `BUFSAVE`, `BUFSCAN`, `BUFSUB`, `BUFTAKE`, `BUFWRITE`, `HTMLCLOSE`, `HTMLFEED`, `HTMLOPEN`, `HTMLSTR`, `HTMLTEXT`, `HTTPGETBUF`, `URLABS`, `URLPART` |
| `1.6.0` | `DAPPER` |
| `1.7.0` | `FCOPY`, `FLIST`, `FMKDIR`, `FMOVE` |
| `1.8.0` | *(no new opcodes; adds the `sawtooth` waveform to `WAVE`)* |
| `1.9.0` | `TIME` (also raises resource limits -- see §4.3) |

Aliases are included as opcodes because they are accepted directly by the
interpreter. AppRunner 1.9.0 is a strict opcode superset of 1.7.0.

AppRunner 1.1.0 also extends `IF`, `IFEQ`, and `IFNE` so their taken branch may
use `GOSUB` as well as `GOTO`. A validator must check opcode syntax and not only
the first word of each line.

AppRunner 1.4.2 adds the `# @echo off` metadata directive. It is not an opcode,
but package validation treats it as a syntax feature because older runtimes
ignore the comment and keep echoing submitted `INPUT` lines.

### 4.2 Built-in values

| AppRunner API | Numeric built-ins | String-expansion built-ins |
| --- | --- | --- |
| `1.0.0` | `$battery`, `$heap`, `$millis`, `$seconds`, `$wifi` | `$battery`, `$cwd`, `$heap`, `$ip`, `$millis`, `$seconds`, `$wifi` |
| `1.1.0` additions | `$feof`, `$fok`, `$kup`, `$kdown`, `$kleft`, `$kright`, `$kenter`, `$kesc`, `$kback`, `$ktab`, `$kspace` | `$feof`, `$fok` |
| `1.3.0` additions | `$ledok` | `$ledok` |
| `1.4.0` additions | `$audiook`, `$httpok`, `$httpcode`, `$httplen`, `$httptruncated`, `$jsonok` | `$audiook`, `$httpok`, `$httpcode`, `$httplen`, `$httptruncated`, `$jsonok` |
| `1.9.0` additions | `$timeok`, `$timeepoch`, `$timeyear`, `$timemonth`, `$timeday`, `$timehour`, `$timeminute`, `$timesecond`, `$timeweekday` | `$timeok`, `$timeepoch`, `$timeyear`, `$timemonth`, `$timeday`, `$timehour`, `$timeminute`, `$timesecond`, `$timeweekday` |

The named key values are numeric-only and deliberately expand to empty text when
printed as strings.

### 4.3 Resource limits by board

| Limit | `m5cardputer` | `fnk0104` | `m5stack-tab5` |
| --- | ---: | ---: | ---: |
| Lines | 1,200 | 4,000 | 4,000 |
| Labels | 192 | 512 | 512 |
| Numeric variables | 64 | 128 | 128 |
| String variables | 32 | 64 | 64 |
| String length | 128 | 4,096 | 4,096 |
| Arrays | 16 | 32 | 32 |
| Shared array cells | 4,096 | 16,384 | 16,384 |
| Nested `GOSUB` calls | 64 | 64 | 64 |
| Canvas size | 40 x 22 | 120 x 60 | 120 x 60 |
| Byte buffer | Not supported | 262,144 | 262,144 |
| Non-yielding step guard | 250,000 | 1,000,000 | 1,000,000 |

Board compatibility covers resource differences that API SemVer alone cannot
express. For example, a 1,300-line app using only 1.0.0 opcodes still cannot
declare `m5cardputer` until it is reduced below that board's line limit.

## 5. Repository protocol

A repository is static HTTPS content and requires no server-side application.
It contains:

```text
repo.json
catalog-v1.ndjson
packages/
  hello/
    1.0.0/
      universal.dapp
  tetris/
    1.3.0/
      fnk0104.dapp
      m5cardputer.dapp
```

`repo.json` is a small bootstrap document:

```json
{
  "repository_format": 1,
  "id": "sadgirlsclub",
  "name": "Sad Girls Club DAPP Repository",
  "canonical_url": "https://sadgirlsclub.wtf/dapper/",
  "catalog": "catalog-v1.ndjson"
}
```

The initial official repository entry point is:

```text
https://sadgirlsclub.wtf/dapper/repo.json
```

The catalog is newline-delimited JSON with one artifact per line. It can be
processed as a stream on the M5Cardputer instead of allocating the whole catalog.

```json
{"package_format":1,"id":"hello","name":"Hello","summary":"A tiny greeting app","version":"1.0.0","boards":["m5cardputer","fnk0104"],"runtime_min":"1.0.0","runtime_max_exclusive":"2.0.0","size":320,"sha256":"0123456789abcdef...","url":"packages/hello/1.0.0/universal.dapp"}
```

Separate board artifacts use separate records with the same `id` and `version`.
Their board lists must not overlap, otherwise artifact selection would be
ambiguous.

URLs are resolved relative to `repo.json`. Redirects to a different origin are
rejected in format 1.

Package format 1 uses the single canonical Sad Girls Club repository. Additional
repository configuration and priority rules are reserved for a later client.

### 5.1 Publishing rules

Repository tooling, rather than a person, generates the catalog. Publication
must fail when:

- required metadata is missing or invalid;
- the path, filename, ID, or version disagree;
- an ID/version/board combination is duplicated;
- the source uses an opcode unavailable to its minimum AppRunner version;
- the source exceeds a declared board's line or statically measurable resource
  limit;
- an artifact path already exists with different bytes; or
- the generated size or SHA-256 does not match the artifact.

The validator should report uncertain dynamic limits as warnings. It must not
pretend it can prove runtime array indexes, call depth, or executed step counts.

## 6. Package selection

Given an app ID and optional version, the client:

1. filters catalog records to the requested ID;
2. ignores prereleases unless an exact prerelease was requested;
3. keeps artifacts whose `boards` contains `DOLL_BOARD_ID`;
4. keeps artifacts whose runtime range contains `DAPP_RUNTIME_VERSION`;
5. keeps artifacts whose package format is supported;
6. chooses the highest compatible app version; and
7. rejects the result if more than one artifact remains for the selected
   ID/version/board combination.

Compatibility errors must name the failed dimension:

```text
apps: tetris 1.3.0 supports fnk0104; this device is m5cardputer
apps: tetris requires AppRunner >=1.1.0; installed runtime is 1.0.0
apps: package format 2 is newer than this client supports
```

`dapper install tetris@1.2.0` selects an exact release. `dapper install tetris`
selects the newest compatible stable release, not merely the repository's newest
release for any board.

## 7. On-device layout and ownership

Managed packages install to internal LittleFS or SD at the user's choice:

```text
/apps/<id>.dapp
/sd/apps/<id>.dapp
/.dapper/installed.ndjson
/.dapper/catalog-v1.ndjson
/.dapper/catalog.part
/.dapper/package.part
```

Dapper asks whether to save a fresh install to internal flash or the SD card
when an SD card is mounted. `--internal` and `--sd` skip the prompt, and update
operations preserve the existing managed install path. Repository/cache state
stays in internal LittleFS under `/.dapper`; package commits stage a temporary
file on the target filesystem before renaming it into place there.

The installed record contains at least repository ID, app ID, app version,
board ID, AppRunner constraint, installed path, size, and SHA-256. Package
manager commands modify only files they own. `dapper remove` removes the `.dapp`
but preserves save files such as `tetris.hs`; a future explicit `--purge` may
remove data declared by a later package format.

Manually copied `.dapp` files remain runnable and appear in `apps`, but are
reported as `unmanaged`. Update and remove operations require an installed
record unless the user deletes the file with the normal filesystem commands.
Installation refuses to overwrite an unmanaged file unless the user supplies an
explicit `--force`; Dapper then takes ownership of that path.

### 7.1 Firmware-bundled apps

Firmware seeding must not overwrite a package-manager update at every boot.
Bundled apps should therefore be seeded into a fallback directory:

```text
/system/apps/<id>.dapp
```

The lookup order becomes:

1. `/sd/apps/<id>.dapp` -- managed or manually installed removable copy;
2. `/apps/<id>.dapp` -- managed or manually installed internal user copy;
3. `/system/apps/<id>.dapp` -- firmware-bundled fallback.

Installing an updated copy shadows the bundled app. Removing it reveals the
bundled version again. The `apps` listing should identify `sd`, `user`, and
`system` origins and warn when one app ID occurs in more than one location.

## 8. Transaction and integrity rules

An installation is successful only after all of these steps complete:

1. establish a usable clock when needed and connect over HTTPS with normal CA
   certificate validation;
2. stream the artifact into `/.dapper/package.part` while
   calculating SHA-256;
3. reject an oversized response before it can exhaust the filesystem;
4. verify byte count and SHA-256 against the catalog;
5. parse the downloaded header and verify its ID, version, boards, runtime range,
   and package format against the selected catalog record;
6. validate the source against the current board's known opcode and line limits;
7. copy the verified download to a staging file on the target filesystem;
8. move the previous managed file to the target filesystem's backup path;
9. move the completed staging file into its final path;
10. atomically replace the installed-package database; and
11. delete the backup only after the database commit succeeds.

On any failure after step 8, the client restores the backup. Temporary files are
safe to remove on the next package-manager invocation after an interrupted boot.

The client must never use an insecure TLS mode. SHA-256 protects an artifact
against corruption and binds it to the HTTPS-delivered catalog; it does not make
an untrusted catalog trustworthy by itself.

## 9. Shell interface

The existing `apps` and `run` commands remain the public interface:

```text
apps                              list all runnable apps and their origin
dapper runtime                    show board, AppRunner, and package-format versions
dapper refresh                    refresh and validate the cached catalog
dapper search [text]              search compatible stable releases
dapper info <id>                  show installed and available versions
dapper install <id>[@version]     ask for internal flash or SD
dapper install <id> --sd          install to /sd/apps without prompting
dapper install <id> --internal    install to /apps without prompting
dapper install <id> --force       take ownership of an unmanaged app path
dapper update [id|--all]          update managed packages only
dapper remove <id>                remove a managed package but keep app data
dapper doctor                     verify installed files and compatibility
run <id>                          run using the normal lookup order
edit --repo <id>[@version]        load a verified package into the editor
```

Search results are filtered for the current board and runtime by default.
`dapper info` may show incompatible releases, but must say why they cannot be
installed.

## 10. Implementation stages

### Stage 1: Freeze and validate the format

- Add the three compile-time identity/version constants to both firmware trees.
- Add package headers to example apps.
- Build a host-side validator and catalog generator.
- Check the opcode and resource-limit matrix in continuous integration.

### Stage 2: Read-only repository client

- Add verified HTTPS download support to the M5Cardputer build.
- Synchronize time before certificate validation when the device clock is not
  already usable.
- Stream `catalog-v1.ndjson` one record at a time.
- Implement `dapper runtime`, `dapper search`, and `dapper info` without filesystem
  mutation.

### Stage 3: Transactional package operations

- Implement install, exact-version install, update, remove, the installed
  database, SHA-256 checks, temporary files, and rollback.
- Preserve manually copied apps as unmanaged files.
- Add `dapper doctor`.

### Stage 4: Bundled-app overlay and publishing

- Move firmware-seeded apps to `/system/apps` and add the fallback lookup.
- Publish the static official repository.
- Add CI fixtures for universal, board-specific, incompatible, corrupt, and
  interrupted installations.

Dependencies, multi-file archives, install hooks, and native firmware components
remain explicitly out of scope for package format 1.
