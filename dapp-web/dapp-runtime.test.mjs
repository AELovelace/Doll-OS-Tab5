import assert from "node:assert/strict";
import { readFile, readdir } from "node:fs/promises";
import test from "node:test";

import { DappRuntime } from "./dapp-runtime.js";

class MemoryFiles {
  constructor(files = {}) {
    this.files = { ...files };
    this.directories = new Set(["/", "/apps", "/sd", "/sd/apps"]);
    for (const path of Object.keys(files)) {
      const parts = path.split("/").filter(Boolean);
      let current = "";
      for (const part of parts.slice(0, -1)) {
        current += `/${part}`;
        this.directories.add(current);
      }
    }
  }
  exists(path) { return Object.hasOwn(this.files, path); }
  read(path) { return this.files[path] ?? null; }
  write(path, content) { this.files[path] = content; return true; }
  delete(path) {
    if (!this.exists(path)) return false;
    delete this.files[path];
    return true;
  }
  mkdir(path) {
    const parent = path.slice(0, path.lastIndexOf("/")) || "/";
    if (this.directories.has(path) || !this.directories.has(parent)) return false;
    this.directories.add(path);
    return true;
  }
  list(path) {
    if (!this.directories.has(path)) return null;
    const prefix = path === "/" ? "/" : `${path}/`;
    const rows = new Map();
    for (const directory of this.directories) {
      if (!directory.startsWith(prefix) || directory === path) continue;
      const rest = directory.slice(prefix.length);
      if (!rest.includes("/")) rows.set(rest, { name: rest, directory: true, size: 0 });
    }
    for (const [file, content] of Object.entries(this.files)) {
      if (!file.startsWith(prefix)) continue;
      const rest = file.slice(prefix.length);
      if (!rest.includes("/")) rows.set(rest, { name: rest, directory: false, size: content.length });
    }
    return [...rows.values()];
  }
  copy(source, destination) {
    if (!this.exists(source) || this.exists(destination)) return null;
    const parent = destination.slice(0, destination.lastIndexOf("/")) || "/";
    if (!this.directories.has(parent)) return null;
    this.files[destination] = this.files[source];
    return destination;
  }
  move(source, destination) {
    const copied = this.copy(source, destination);
    if (!copied) return null;
    delete this.files[source];
    return copied;
  }
}

function runtimeFor(files, events = {}) {
  const io = {
    output() {},
    clear() {},
    status() {},
    input() {},
    canvas() {},
    endCanvas() {},
    wave: value => (events.waves ||= []).push(value),
    waveStop: () => { events.stops = (events.stops || 0) + 1; }
  };
  return new DappRuntime(io, files);
}

test("raw file opcodes preserve NUL, newline, and 0xff while update patches in place", async () => {
  const original = String.fromCharCode(0x00, 0x0a, 0xff, 0x41);
  const files = new MemoryFiles({ "/apps/test.bin": original });
  const runtime = runtimeFor(files);
  const result = await runtime.run(`
FOPEN "/apps/test.bin" update
FSIZE size
FREADB b0
FREADB b1
FREADB b2
FSEEK 2
FWRITEB 17
FTELL pos
FSEEK 0
FREADB again
HEX shown 255 2
FCLOSE
END`);

  assert.equal(result.ok, true);
  assert.equal(runtime.numbers.get("size"), 4);
  assert.equal(runtime.numbers.get("b0"), 0);
  assert.equal(runtime.numbers.get("b1"), 10);
  assert.equal(runtime.numbers.get("b2"), 255);
  assert.equal(runtime.numbers.get("pos"), 3);
  assert.equal(runtime.numbers.get("again"), 0);
  assert.equal(runtime.strings.get("shown"), "FF");
  assert.deepEqual([...files.read("/apps/test.bin")].map(ch => ch.charCodeAt(0)), [0, 10, 17, 65]);
});

test("@echo off tells the input UI not to echo submitted lines", async () => {
  const echoFlags = [];
  const io = {
    output() {}, clear() {}, status() {}, canvas() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve, _masked, echoInput) {
      echoFlags.push(echoInput);
      resolve("quiet");
    }
  };
  const runtime = new DappRuntime(io, new MemoryFiles());
  const result = await runtime.run("# @echo off\nINPUT line\nEND");

  assert.equal(result.ok, true);
  assert.deepEqual(echoFlags, [false]);
  assert.equal(runtime.strings.get("line"), "quiet");
});

test("WAVE awaits the browser adapter and audiook reflects live readiness", async () => {
  let ready = false;
  let waveFinished = false;
  const io = {
    output() {}, clear() {}, status() {}, input() {}, canvas() {}, endCanvas() {}, waveStop() {},
    async wave() {
      await Promise.resolve();
      ready = true;
      waveFinished = true;
    }
  };
  const runtime = new DappRuntime(io, new MemoryFiles(), { audiook: () => Number(ready) });
  const result = await runtime.run("SET before $audiook\nWAVE 1 square 440 80\nSET after $audiook\nEND");
  assert.equal(result.ok, true);
  assert.equal(waveFinished, true);
  assert.equal(runtime.numbers.get("before"), 0);
  assert.equal(runtime.numbers.get("after"), 1);
});

test("HTTPGET bounds the decoded body and reports status", async (t) => {
  const previousFetch = globalThis.fetch;
  t.after(() => { globalThis.fetch = previousFetch; });
  globalThis.fetch = async () => ({
    status: 200,
    ok: true,
    arrayBuffer: async () => new TextEncoder().encode("abcdef").buffer
  });

  const runtime = runtimeFor(new MemoryFiles());
  const result = await runtime.run(`HTTPGET body "https://example.test/" 4\nEND`);
  assert.equal(result.ok, true);
  assert.equal(runtime.strings.get("body"), "abcd");
  assert.equal(runtime.httpcode, 200);
  assert.equal(runtime.httplen, 4);
  assert.equal(runtime.httptruncated, 1);
  assert.equal(runtime.httpok, 1);
});

test("HTTPPOST sends configured headers and JSON helpers round-trip chat content", async (t) => {
  const previousFetch = globalThis.fetch;
  let request;
  t.after(() => { globalThis.fetch = previousFetch; });
  globalThis.fetch = async (url, options) => {
    request = { url, options };
    const response = JSON.stringify({ choices: [{ message: { content: "hello Doll" } }] });
    return {
      status: 200,
      ok: true,
      arrayBuffer: async () => new TextEncoder().encode(response).buffer
    };
  };

  const runtime = runtimeFor(new MemoryFiles());
  const result = await runtime.run(`
CHR q 34
SETSTR prompt "say "
APPEND prompt $q
APPEND prompt "hello$q"
JSONESC safe $prompt
SETSTR body "{"
APPEND body $q
APPEND body "model$q:"
APPEND body $q
APPEND body "demo$q,"
APPEND body $q
APPEND body "messages$q:[{"
APPEND body $q
APPEND body "role$q:"
APPEND body $q
APPEND body "user$q,"
APPEND body $q
APPEND body "content$q:"
APPEND body $q
APPEND body $safe
APPEND body $q
APPEND body "}]}"
HTTPHEADER "Authorization" "Bearer test-key"
HTTPHEADER "Content-Type" "application/json"
HTTPPOST raw "https://example.test/v1/chat/completions" $body 4096
JSONGET answer $raw "choices[0].message.content"
END`);

  assert.equal(result.ok, true);
  assert.equal(request.url, "https://example.test/v1/chat/completions");
  assert.equal(request.options.method, "POST");
  assert.equal(request.options.headers.Authorization, "Bearer test-key");
  assert.equal(request.options.headers["Content-Type"], "application/json");
  assert.equal(request.options.credentials, "omit");
  assert.equal(request.options.referrerPolicy, "no-referrer");
  assert.equal(JSON.parse(request.options.body).messages[0].content, 'say "hello"');
  assert.equal(runtime.strings.get("answer"), "hello Doll");
  assert.equal(runtime.jsonok, 1);
});

test("WAVE exposes three-channel settings and app exit always stops audio", async () => {
  const events = {};
  const runtime = runtimeFor(new MemoryFiles(), events);
  const result = await runtime.run(`WAVE 2 triangle 330 25\nEND`);
  assert.equal(result.ok, true);
  assert.deepEqual(events.waves, [{ channel: 2, waveform: "triangle", frequency: 330, level: 25 }]);
  assert.equal(events.stops, 1);
});

test("host policy can block HTTP before fetch", async (t) => {
  const previousFetch = globalThis.fetch;
  let fetched = false;
  t.after(() => { globalThis.fetch = previousFetch; });
  globalThis.fetch = async () => {
    fetched = true;
    throw new Error("should not fetch");
  };
  const outputs = [];
  const io = {
    output: text => outputs.push(text), clear() {}, status() {}, input() {}, canvas() {}, endCanvas() {}, waveStop() {},
    authorizeHttp: () => false
  };
  const runtime = new DappRuntime(io, new MemoryFiles());
  const result = await runtime.run(`HTTPGET body "https://example.test/" 32\nEND`);
  assert.equal(result.ok, true);
  assert.equal(fetched, false);
  assert.equal(runtime.httpcode, -2);
  assert.ok(outputs.includes("HTTP request blocked by browser policy"));
});

test("LIFE advances a Conway grid in native runtime code", async () => {
  const runtime = runtimeFor(new MemoryFiles());
  const result = await runtime.run(`
DIM cur 25
DIM nxt 25
SET cur[7] 1
SET cur[12] 1
SET cur[17] 1
LIFE cur nxt 5 5
SET a $cur[11]
SET b $cur[12]
SET c $cur[13]
SET d $cur[7]
END`);

  assert.equal(result.ok, true);
  assert.equal(runtime.numbers.get("a"), 1);
  assert.equal(runtime.numbers.get("b"), 1);
  assert.equal(runtime.numbers.get("c"), 1);
  assert.equal(runtime.numbers.get("d"), 0);
});

test("the shipped synth app renders and exits through its normal key loop", async () => {
  const events = {};
  const files = new MemoryFiles();
  let runtime;
  let sentExit = false;
  const io = {
    output() {}, clear() {}, status() {}, input() {}, endCanvas() {},
    canvas() {
      if (!sentExit) {
        sentExit = true;
        runtime.keyQueue.push(120);
      }
    },
    wave: value => (events.waves ||= []).push(value),
    waveStop: () => { events.stops = (events.stops || 0) + 1; }
  };
  runtime = new DappRuntime(io, files);
  const source = await readFile(new URL("../apps/synth.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);
  assert.equal(result.ok, true);
  assert.equal(events.waves.length, 3);
  assert.ok(events.stops >= 1);
});

test("the shipped hex editor reads a binary page and exits without changing it", async () => {
  const original = String.fromCharCode(0, 10, 255, 65);
  const files = new MemoryFiles({ "/apps/test.bin": original });
  let runtime;
  let sentExit = false;
  const io = {
    output() {}, clear() {}, status() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve("/apps/test.bin"); },
    canvas() {
      if (!sentExit) {
        sentExit = true;
        runtime.keyQueue.push(120);
      }
    }
  };
  runtime = new DappRuntime(io, files);
  const source = await readFile(new URL("../apps/hex.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);
  assert.equal(result.ok, true);
  assert.equal(files.read("/apps/test.bin"), original);
});

async function runGrotto2({ answers, randoms = [] }) {
  const files = new MemoryFiles();
  const outputs = [];
  const frames = [];
  let runtime;
  let queuedStart = false;
  const originalRandom = Math.random;
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  Math.random = () => randoms.length ? randoms.shift() : 0;
  globalThis.setTimeout = (callback) => {
    queueMicrotask(callback);
    return 0;
  };
  globalThis.clearTimeout = () => {};
  try {
    const io = {
      output(text) { outputs.push(text); },
      clear() {}, status() {}, endCanvas() {}, wave() {}, waveStop() {},
      input(_prompt, resolve) { resolve(answers.shift() ?? ""); },
      canvas(canvas) {
        const text = chatCanvasText(canvas);
        if (text.trim()) frames.push({ cols: canvas.cols, rows: canvas.rows, text });
        if (!queuedStart && text.trim()) {
          queuedStart = true;
          runtime.pushKey({ key: "Enter", ctrlKey: false });
        }
      }
    };
    runtime = new DappRuntime(io, files);
    const source = await readFile(new URL("../apps/grotto2.dapp", import.meta.url), "utf8");
    const result = await runtime.run(source);
    return { result, runtime, files, outputs, frames };
  } finally {
    Math.random = originalRandom;
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
  }
}

test("The Cursed Grotto II reaches victory and saves its score", async () => {
  const answers = [
    "", "Doll", "s", "s", "s", "s", "1",
    "1", "1", "1", "1", "1", "1", "1", "1",
    "d", "d"
  ];
  const { result, runtime, files, outputs, frames } = await runGrotto2({ answers });

  assert.equal(result.ok, true, result.error?.message);
  assert.ok(outputs.some(line => line.includes("V I C T O R Y")));
  assert.equal(runtime.numbers.get("zone"), 12);
  assert.equal(runtime.numbers.get("px"), 6);
  assert.equal(runtime.numbers.get("py"), 5);
  assert.equal(files.read("/apps/grotto2.hs"), "#1060\n");
  assert.ok(frames.some(frame => frame.cols === 80 && frame.rows === 30
    && frame.text.includes("THE ABYSSAL DEPTHS")), "wide title is rendered");
  assert.ok(frames.some(frame => frame.cols === 84 && frame.rows === 36
    && frame.text.includes("ADVENTURER")), "wide map dashboard is rendered");
});

test("The Cursed Grotto II random encounters return to movement", async () => {
  const answers = [
    "", "Doll", "s", "2", "s", "s", "s", "1",
    "1", "1", "1", "1", "1", "1", "1", "1",
    "d", "d"
  ];
  const randoms = [0, 0.9, 0, 0.9];
  const { result, outputs } = await runGrotto2({ answers, randoms });

  assert.equal(result.ok, true, result.error?.message);
  assert.ok(outputs.some(line => line.includes("giant cave bat")));
  assert.ok(outputs.some(line => line.includes("V I C T O R Y")));
});

test("the shipped LLM chat app masks its key and parses a Chat Completions reply", async (t) => {
  const previousFetch = globalThis.fetch;
  let request;
  t.after(() => { globalThis.fetch = previousFetch; });
  globalThis.fetch = async (url, options) => {
    request = { url, options };
    const response = JSON.stringify({ choices: [{ message: { content: "Hi from the model" } }] });
    return { status: 200, ok: true, arrayBuffer: async () => new TextEncoder().encode(response).buffer };
  };

  const answers = ["test-key", 'hello "Doll"', "/quit"];
  const outputs = [];
  const masked = [];
  const io = {
    output(text) { outputs.push(text); }, clear() {}, status() {}, canvas() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve, isMasked) {
      masked.push(isMasked);
      resolve(answers.shift());
    }
  };
  const runtime = new DappRuntime(io, new MemoryFiles());
  const source = await readFile(new URL("../apps/llm-chat.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true);
  assert.deepEqual(masked, [true, false, false]);
  assert.equal(request.url, "http://192.168.1.50:8000/v1/chat/completions");
  assert.equal(request.options.headers.Authorization, "Bearer test-key");
  assert.equal(JSON.parse(request.options.body).messages[0].content, 'hello "Doll"');
  assert.ok(outputs.includes("llm> Hi from the model"));
});

function chatCanvasText(canvas) {
  return canvas.cells.map(row => row.map(cell => cell.char).join("").trimEnd()).join("\n");
}

// Drives the shipped DappChat through its blocking login prompts and into the
// canvas loop, against a fake room the test can mutate mid-flight. onFrame runs
// on every FLIP and is how a test types: push("h") queues a keypress the way the
// panel would. The frame cap keeps a broken build from hanging the suite.
async function runDappChat({ room = [], onFrame, sendHook, answers = ["Doll", "secret", ""] } = {}) {
  const polls = [];
  const sends = [];
  const previousFetch = globalThis.fetch;
  globalThis.fetch = async (url, options = {}) => {
    let body;
    if (url.endsWith("/auth")) {
      body = { ok: true, created: false, token: "chat-token" };
    } else if (url.endsWith("/send")) {
      const sent = JSON.parse(options.body);
      sends.push(sent);
      body = sendHook ? sendHook(sent, room) : { ok: true, id: room.length };
    } else {
      polls.push(url);
      const since = Number(new URL(url).searchParams.get("since")) || 0;
      const rows = since > 0 ? room.filter(m => m.id > since) : room.slice(-10);
      body = {
        ok: true,
        last_id: rows.length ? rows[rows.length - 1].id : since,
        messages: rows
      };
    }
    return {
      status: 200,
      ok: true,
      arrayBuffer: async () => new TextEncoder().encode(JSON.stringify(body)).buffer
    };
  };

  const queue = [...answers];
  const frames = [];
  let runtime;
  const io = {
    output() {}, clear() {}, status() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(queue.shift() ?? ""); },
    canvas(canvas) {
      const text = chatCanvasText(canvas);
      // CANVAS hands back a blank grid when the screen is created, before the
      // app has drawn anything. Only FLIP frames are worth looking at.
      if (!text.trim()) return;
      frames.push(text);
      const push = key => runtime.pushKey({ key, ctrlKey: false });
      if (frames.length > 300) { push("Escape"); return; }
      onFrame?.(text, frames.length, push);
    }
  };
  runtime = new DappRuntime(io, new MemoryFiles());
  const source = await readFile(new URL("../apps/dappchat.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);
  globalThis.fetch = previousFetch;
  return { result, polls, sends, frames };
}

test("DappChat refreshes the room on its own clock with nobody touching a key", async () => {
  // The whole point of the rewrite: a message posted by someone else has to
  // appear while this unit sits idle, with no Enter and no blank line.
  const room = [{ id: 1, user: "Asuka", text: "hello Doll" }];
  const { result, frames, polls } = await runDappChat({
    room,
    onFrame(text, frame, push) {
      if (frame === 1) room.push({ id: 2, user: "Rei", text: "arrived later" });
      if (text.includes("Rei: arrived later")) push("Escape");
    }
  });

  assert.equal(result.ok, true);
  assert.ok(frames[0].includes("Asuka: hello Doll"), "history drawn on the first frame");
  assert.ok(frames.some(f => f.includes("Rei: arrived later")), "later message arrived unprompted");
  assert.ok(polls.length >= 2, `expected repeat polls, got ${polls.length}`);
  // No key was ever pressed except the Escape that ended the test.
  assert.ok(frames.at(-1).includes("Doll>"), "prompt row still drawn");
});

test("DappChat edits its input line from KEY without blocking the room", async () => {
  const room = [{ id: 1, user: "Asuka", text: "hello Doll" }];
  const { result, frames, sends } = await runDappChat({
    room,
    onFrame(text, frame, push) {
      if (frame === 1) { push("h"); push("i"); push("!"); }
      if (text.includes("Doll> hi!")) push("Backspace");
      if (text.includes("Doll> hi") && !text.includes("Doll> hi!")) push("Escape");
    }
  });

  assert.equal(result.ok, true);
  assert.ok(frames.some(f => f.includes("Doll> hi!")), "typed text echoed live");
  assert.ok(frames.some(f => f.includes("Doll> hi") && !f.includes("Doll> hi!")), "backspace edits");
  assert.equal(sends.length, 0, "nothing sent without Enter");
});

test("DappChat sends on enter and shows the message without waiting out the timer", async () => {
  const room = [];
  const { result, frames, sends } = await runDappChat({
    room,
    sendHook(sent, current) {
      current.push({ id: current.length + 1, user: "Doll", text: sent.text });
      return { ok: true, id: current.length };
    },
    onFrame(text, frame, push) {
      if (frame === 1) { push("h"); push("e"); push("y"); }
      if (text.includes("Doll> hey")) push("Enter");
      if (text.includes("Doll: hey")) push("Escape");
    }
  });

  assert.equal(result.ok, true);
  assert.equal(sends.length, 1);
  assert.equal(sends[0].text, "hey");
  assert.equal(sends[0].room, "lobby");
  assert.ok(frames.some(f => f.includes("Doll: hey")), "own message came back into the feed");
});

test("DappChat surfaces a send the server refused instead of dropping it", async () => {
  // /send answers HTTP 200 with {"ok":false} when the token has been retired by
  // the same account signing in elsewhere. Checking only $httpok made the
  // message vanish with no trace anywhere.
  const room = [];
  const { result, frames, sends } = await runDappChat({
    room,
    sendHook: () => ({ ok: false, error: "unauthorized" }),
    onFrame(text, frame, push) {
      if (frame === 1) { push("h"); push("i"); }
      if (text.includes("Doll> hi")) push("Enter");
      if (text.includes("signed out")) push("Escape");
    }
  });

  assert.equal(result.ok, true);
  assert.equal(sends.length, 1);
  assert.ok(frames.some(f => f.includes("signed out")), "refusal reported on the status row");
});

test("DappChat keeps polling while the room answers a full batch", async () => {
  // The server caps a poll at 10 messages, so a backlog needs more than one
  // request or the room stays permanently behind.
  const room = Array.from({ length: 14 }, (_, i) => (
    { id: i + 1, user: "Asuka", text: `line ${i + 1}` }
  ));
  const { result, frames, polls } = await runDappChat({
    room,
    onFrame(text, frame, push) { if (frame >= 1) push("Escape"); }
  });

  assert.equal(result.ok, true);
  // since=0 returns the newest ten, and last_id lands at the end of the room.
  assert.equal(polls[0], "https://sadgirlsclub.wtf/dappchat/poll?room=lobby&since=0");
  assert.ok(frames[0].includes("Asuka: line 14"), "landed at the end of the conversation");
});

test("DappChat retries a poll that died on a closed keep-alive socket", async (t) => {
  // The request after a pause routinely lands on a socket the server already
  // timed out. Swallowing it made a live room look silent.
  const previousFetch = globalThis.fetch;
  const polls = [];
  t.after(() => { globalThis.fetch = previousFetch; });
  globalThis.fetch = async (url) => {
    if (url.endsWith("/auth")) {
      return {
        status: 200, ok: true,
        arrayBuffer: async () => new TextEncoder().encode(
          JSON.stringify({ ok: true, created: false, token: "chat-token" })
        ).buffer
      };
    }
    polls.push(url);
    if (polls.length === 1) throw new TypeError("Failed to fetch");
    return {
      status: 200, ok: true,
      arrayBuffer: async () => new TextEncoder().encode(JSON.stringify({
        ok: true, last_id: 3, messages: [{ id: 3, user: "Asuka", text: "still here" }]
      })).buffer
    };
  };

  const answers = ["Doll", "secret", ""];
  const frames = [];
  let runtime;
  const io = {
    output() {}, clear() {}, status() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(answers.shift() ?? ""); },
    canvas(canvas) {
      const text = chatCanvasText(canvas);
      if (!text.trim()) return;
      frames.push(text);
      runtime.pushKey({ key: "Escape", ctrlKey: false });
    }
  };
  runtime = new DappRuntime(io, new MemoryFiles());
  const source = await readFile(new URL("../apps/dappchat.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true);
  assert.equal(polls.length, 2, "the dropped poll was retried");
  assert.ok(frames.some(f => f.includes("Asuka: still here")));
  assert.ok(!frames.some(f => f.includes("refresh failed")));
});

test("URLABS resolves relative, root, scheme-relative and dot-segment hrefs", async () => {
  const runtime = runtimeFor(new MemoryFiles());
  const result = await runtime.run([
    'SETSTR base "https://example.test/docs/guide/page.html?x=1"',
    'URLABS rel $base "next.html"',
    'URLABS root $base "/about"',
    'URLABS up $base "../images/logo.png"',
    'URLABS scheme $base "//cdn.example.test/a.js"',
    'URLABS abs $base "http://other.test/z"',
    'URLABS query $base "?page=2"',
    'URLABS frag $base "#section"',
    'URLABS mail $base "mailto:a@b.test"',
    'URLABS keep $base "search?q=a/b"',
    "END"
  ].join("\n"));

  assert.equal(result.ok, true);
  assert.equal(runtime.strings.get("rel"), "https://example.test/docs/guide/next.html");
  assert.equal(runtime.strings.get("root"), "https://example.test/about");
  assert.equal(runtime.strings.get("up"), "https://example.test/docs/images/logo.png");
  assert.equal(runtime.strings.get("scheme"), "https://cdn.example.test/a.js");
  assert.equal(runtime.strings.get("abs"), "http://other.test/z");
  assert.equal(runtime.strings.get("query"), "https://example.test/docs/guide/page.html?page=2");
  // fragments and non-http schemes are not followable and come back empty
  assert.equal(runtime.strings.get("frag"), "");
  assert.equal(runtime.strings.get("mail"), "");
  // a "/" inside a query is not a path segment
  assert.equal(runtime.strings.get("keep"), "https://example.test/docs/guide/search?q=a/b");
});

test("URLPART splits a url into the pieces a networked app actually needs", async () => {
  const runtime = runtimeFor(new MemoryFiles());
  const result = await runtime.run([
    'SETSTR u "https://example.test:8443/a/b/c.html?q=1"',
    'URLPART scheme $u "scheme"',
    'URLPART origin $u "origin"',
    'URLPART host $u "host"',
    'URLPART path $u "path"',
    'URLPART dir $u "dir"',
    "END"
  ].join("\n"));

  assert.equal(result.ok, true);
  assert.equal(runtime.strings.get("scheme"), "https");
  assert.equal(runtime.strings.get("origin"), "https://example.test:8443");
  assert.equal(runtime.strings.get("host"), "example.test:8443");
  assert.equal(runtime.strings.get("path"), "/a/b/c.html?q=1");
  assert.equal(runtime.strings.get("dir"), "https://example.test:8443/a/b/");
});

test("HTMLSTR renders tags, entities and non-ASCII text to wrapped ASCII", async () => {
  const runtime = runtimeFor(new MemoryFiles());
  const page = "<h1>Caf&eacute;</h1><p>one &amp; two&nbsp;three</p>"
    + "<p>naïve — déjà vu…</p>";
  const result = await runtime.run(`SETSTR page "${page}"\nHTMLSTR out text $page\nEND`);

  assert.equal(result.ok, true);
  // the panel font cannot draw above 126, so the renderer folds rather than dropping
  assert.equal(runtime.strings.get("out"), "Cafe\none & two three\nnaive - deja vu...");
});

test("the renderer ignores a bare < inside a script body", async () => {
  const runtime = runtimeFor(new MemoryFiles());
  const page = "<p>before</p><script>if (a<b) { x(); }</script><p>after</p>";
  const result = await runtime.run(`SETSTR page "${page}"\nHTMLSTR out text $page\nEND`);

  assert.equal(result.ok, true);
  // version 1 of browse.dapp swallowed everything to the next '>' here and lost "after"
  assert.equal(runtime.strings.get("out"), "before\nafter");
});

test("HTMLTEXT streams a page into rendered text plus a numbered link file", async (t) => {
  const previousFetch = globalThis.fetch;
  t.after(() => { globalThis.fetch = previousFetch; });
  const body = "<html><head><title>Home</title></head><body>"
    + "<p>Welcome to the <a href=\"/docs\">docs</a> and the <a href=\"faq.html\">faq</a>.</p>"
    + "<!-- <a href=\"/hidden\">hidden</a> -->"
    + "<style>a { color: red; }</style>"
    + "</body></html>";
  globalThis.fetch = async () => ({
    status: 200,
    ok: true,
    arrayBuffer: async () => new TextEncoder().encode(body).buffer
  });

  const files = new MemoryFiles();
  const runtime = runtimeFor(files);
  const result = await runtime.run(
    'HTMLTEXT "https://example.test/start/index.html" "/browse.txt" "/browse.lnk" 40 200\nEND'
  );

  assert.equal(result.ok, true);
  assert.equal(runtime.httpcode, 200);
  assert.equal(runtime.htmllinks, 2);
  // link N is line N of the link file, which is what browse.dapp's pager relies on
  assert.deepEqual(files.read("/browse.lnk").trimEnd().split("\n"), [
    "1 https://example.test/docs",
    "2 https://example.test/start/faq.html"
  ]);
  const text = files.read("/browse.txt");
  assert.match(text, /Home/);
  assert.match(text, /\[1\]/);
  assert.match(text, /\[2\]/);
  // commented-out and styled markup contribute neither text nor links
  assert.doesNotMatch(text, /hidden/);
  assert.doesNotMatch(text, /color/);
  for (const line of text.trimEnd().split("\n")) assert.ok(line.length <= 40);
});

test("HTMLTEXT stops collecting links at maxlinks", async (t) => {
  const previousFetch = globalThis.fetch;
  t.after(() => { globalThis.fetch = previousFetch; });
  const body = '<a href="/a">a</a><a href="/b">b</a><a href="/c">c</a>';
  globalThis.fetch = async () => ({
    status: 200,
    ok: true,
    arrayBuffer: async () => new TextEncoder().encode(body).buffer
  });

  const files = new MemoryFiles();
  const runtime = runtimeFor(files);
  const result = await runtime.run('HTMLTEXT "https://example.test/" "/t.txt" "/l.txt" 76 2\nEND');

  assert.equal(result.ok, true);
  assert.equal(runtime.htmllinks, 2);
  assert.equal(files.read("/l.txt").trimEnd().split("\n").length, 2);
});

test("HTMLOPEN refuses to run while the script holds a file handle", async () => {
  const files = new MemoryFiles({ "/x.txt": "hi" });
  const runtime = runtimeFor(files);
  const result = await runtime.run([
    'FOPEN "/x.txt" read',
    'HTMLOPEN "/t.txt" "-" "https://example.test/"',
    "END"
  ].join("\n"));

  assert.equal(result.ok, false);
  assert.match(String(result.error), /file handle closed/);
});

test("HTTPGETBUF fills the byte buffer and BUFSCAN/BUFTAKE walk it a token at a time", async (t) => {
  const previousFetch = globalThis.fetch;
  t.after(() => { globalThis.fetch = previousFetch; });
  globalThis.fetch = async () => ({
    status: 200,
    ok: true,
    arrayBuffer: async () => new TextEncoder().encode("alpha beta gamma").buffer
  });

  const runtime = runtimeFor(new MemoryFiles());
  const result = await runtime.run([
    "BUFNEW 1024",
    'HTTPGETBUF "https://example.test/words"',
    "BUFAT first 0",
    "BUFSUB head 0 5",
    "BUFTAKE word pos 0",
    "BUFTAKE second pos2 6",
    "SET filled $buflen",
    "END"
  ].join("\n"));

  assert.equal(result.ok, true);
  // the buffer is released when the app exits, so its length is captured while running
  assert.equal(runtime.numbers.get("filled"), 16);
  assert.equal(runtime.numbers.get("first"), "a".charCodeAt(0));
  assert.equal(runtime.strings.get("head"), "alpha");
  assert.equal(runtime.strings.get("word"), "alpha");
  assert.equal(runtime.numbers.get("pos"), 5);
  assert.equal(runtime.strings.get("second"), "beta");
  assert.equal(runtime.numbers.get("pos2"), 10);
});

test("BUFWRITE, BUFSAVE and BUFLOAD round-trip through the filesystem", async () => {
  const files = new MemoryFiles();
  const runtime = runtimeFor(files);
  const result = await runtime.run([
    "BUFNEW 64",
    'BUFWRITE 0 "hello"',
    'BUFSAVE "/b.bin"',
    "BUFCLEAR",
    'BUFLOAD "/b.bin"',
    "BUFSUB back 0 5",
    "SET reloaded $buflen",
    "END"
  ].join("\n"));

  assert.equal(result.ok, true);
  assert.equal(files.read("/b.bin"), "hello");
  assert.equal(runtime.strings.get("back"), "hello");
  assert.equal(runtime.numbers.get("reloaded"), 5);
});

test("the rewritten browser fetches a page, pages it, follows a link, and quits", async (t) => {
  const previousFetch = globalThis.fetch;
  const requested = [];
  t.after(() => { globalThis.fetch = previousFetch; });
  globalThis.fetch = async url => {
    requested.push(url);
    const body = requested.length === 1
      ? '<h1>Index</h1><p>Go to the <a href="page2.html">second page</a>.</p>'
      : "<h1>Second</h1><p>The end.</p>";
    return { status: 200, ok: true, arrayBuffer: async () => new TextEncoder().encode(body).buffer };
  };

  const answers = ["example.test/start/", "1", "q"];
  const outputs = [];
  const io = {
    output(text) { outputs.push(text); }, clear() {}, status() {}, canvas() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(answers.shift()); }
  };
  const runtime = new DappRuntime(io, new MemoryFiles());
  const source = await readFile(new URL("../apps/browse.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true);
  // a bare host typed at the prompt gets a scheme; the link is resolved by the renderer
  assert.equal(requested[0], "http://example.test/start/");
  assert.equal(requested[1], "http://example.test/start/page2.html");
  assert.ok(outputs.some(line => line.includes("Index")));
  assert.ok(outputs.some(line => line.includes("Second")));
  assert.ok(outputs.includes("browse: done"));
});

test("the shipped Reader saves an article, resumes it, and preserves it when refresh fails", async (t) => {
  const previousFetch = globalThis.fetch;
  let fetches = 0;
  t.after(() => { globalThis.fetch = previousFetch; });
  globalThis.fetch = async () => {
    fetches += 1;
    if (fetches > 1) {
      return {
        status: 503,
        ok: false,
        arrayBuffer: async () => new TextEncoder().encode("temporarily unavailable").buffer
      };
    }
    const paragraphs = Array.from({ length: 50 }, (_, index) => `<p>Article line ${index + 1}</p>`).join("");
    const body = `<h1>Saved Article</h1>${paragraphs}<a href="/next">Next</a>`;
    return {
      status: 200,
      ok: true,
      arrayBuffer: async () => new TextEncoder().encode(body).buffer
    };
  };

  // Add, attempt a failed refresh, open the intact article, then leave the library.
  const answers = ["a", "https://example.test/article", "Saved Article", "r", "1", "", "1", "q"];
  const outputs = [];
  const files = new MemoryFiles();
  let runtime;
  let articleFrames = 0;
  const io = {
    output(text) { outputs.push(text); }, clear() {}, status() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(answers.shift()); },
    canvas(canvas) {
      const heading = canvas.cells[0].map(cell => cell.char).join("");
      if (!heading.includes("Saved Article")) return;
      articleFrames += 1;
      runtime.pushKey({ key: articleFrames === 1 ? "ArrowDown" : "Escape", ctrlKey: false });
    }
  };
  runtime = new DappRuntime(io, files);
  const source = await readFile(new URL("../apps/reader.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true);
  assert.equal(fetches, 2);
  assert.match(files.read("/apps/reader-1-0.txt"), /Article line 50/);
  assert.match(files.read("/apps/reader-1-0.lnk"), /https:\/\/example\.test\/next/);
  assert.equal(files.exists("/apps/reader-1-1.txt"), false);
  assert.equal(files.read("/apps/reader-1.meta"), [
    "Saved Article",
    "https://example.test/article",
    "34",
    "0",
    ""
  ].join("\n"));
  assert.ok(outputs.some(line => line.includes("fetch failed (HTTP 503)")));
  assert.ok(outputs.includes("reader: library closed"));
});

test("DAPPER delegates only package-manager arguments to the runtime bridge", async () => {
  const calls = [];
  const runtime = new DappRuntime({
    output() {}, clear() {}, status() {}, input() {}, canvas() {}, endCanvas() {}, waveStop() {}
  }, new MemoryFiles(), {
    dapper: async parts => calls.push(parts)
  });
  const result = await runtime.run('SETSTR id "snake"\nDAPPER install $id --internal\nEND');

  assert.equal(result.ok, true);
  assert.deepEqual(calls, [["install", "snake", "--internal"]]);
});

test("the bundled Dappstore guides search and confirmed installation", async () => {
  const calls = [];
  const answers = ["2", "game", "", "4", "snake", "i", "INSTALL", "", "q"];
  const outputs = [];
  const io = {
    output(text) { outputs.push(text); }, clear() {}, status() {}, canvas() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(answers.shift()); }
  };
  const runtime = new DappRuntime(io, new MemoryFiles(), {
    dapper: async parts => calls.push(parts)
  });
  const source = await readFile(new URL("../apps/dappstore.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true);
  assert.deepEqual(calls, [
    ["search", "game"],
    ["info", "snake"],
    ["install", "snake", "--internal"]
  ]);
  assert.ok(outputs.includes("Dappstore closed"));
});

test("filesystem management opcodes list, mkdir, copy and move without a shell bridge", async () => {
  const files = new MemoryFiles({ "/apps/a.txt": "alpha" });
  const runtime = runtimeFor(files);
  const result = await runtime.run([
    'FLIST "/apps" "/listing.txt"',
    'FMKDIR "/apps/archive"',
    'FCOPY "/apps/a.txt" "/apps/archive/a.txt"',
    'FMOVE "/apps/archive/a.txt" "/apps/moved.txt"',
    "END"
  ].join("\n"));

  assert.equal(result.ok, true);
  assert.match(files.read("/listing.txt"), /F\|a\.txt\|5/);
  assert.equal(files.read("/apps/moved.txt"), "alpha");
  assert.equal(files.exists("/apps/archive/a.txt"), false);
});

test("the remaining ecosystem apps enter and leave through their normal UI", async () => {
  for (const id of ["requests", "control", "data", "feeds", "today", "files"]) {
    const files = new MemoryFiles();
    const answers = ["q"];
    const io = {
      output() {}, clear() {}, status() {}, canvas() {}, endCanvas() {}, waveStop() {},
      input(_prompt, resolve) { resolve(answers.shift()); }
    };
    const runtime = new DappRuntime(io, files);
    const source = await readFile(new URL(`../apps/${id}.dapp`, import.meta.url), "utf8");
    const result = await runtime.run(source);
    assert.equal(result.ok, true, `${id}: ${result.error?.message || "failed"}`);
  }
});

test("Tab5-enhanced apps render their wide primary workspaces", async () => {
  const cases = [
    { id: "calendar", cols: 84, rows: 36, marker: "TAB5 CALENDAR", keyExit: true },
    { id: "sheet", cols: 84, rows: 34, marker: "S H E E T", keyExit: true },
    { id: "paint", cols: 100, rows: 40, marker: "TOOLS + PALETTE", keyExit: true },
    { id: "files", cols: 100, rows: 40, marker: "FILES // /", keyExit: false },
    { id: "today", cols: 100, rows: 40, marker: "TAB5 DAILY DESK", keyExit: false },
    { id: "dappstore", cols: 100, rows: 40, marker: "TAB5 PACKAGE DESK", keyExit: false },
  ];

  for (const app of cases) {
    const frames = [];
    let runtime;
    let exitQueued = false;
    const io = {
      output() {}, clear() {}, status() {}, endCanvas() {}, waveStop() {},
      input(_prompt, resolve) { resolve("q"); },
      canvas(canvas) {
        const text = chatCanvasText(canvas);
        frames.push({ cols: canvas.cols, rows: canvas.rows, text });
        if (app.keyExit && !exitQueued && text.includes(app.marker)) {
          exitQueued = true;
          runtime.pushKey({ key: "Escape", ctrlKey: false });
        }
      }
    };
    runtime = new DappRuntime(io, new MemoryFiles());
    const source = await readFile(new URL(`../apps/${app.id}.dapp`, import.meta.url), "utf8");
    const result = await runtime.run(source);
    assert.equal(result.ok, true, `${app.id}: ${result.error?.message || "failed"}`);
    assert.ok(frames.some(frame => frame.cols === app.cols && frame.rows === app.rows
      && frame.text.includes(app.marker)), `${app.id} renders ${app.cols}x${app.rows}`);
  }
});

test("every shipped app declares Tab5 compatibility and every fixed canvas is wide", async () => {
  const appRoot = new URL("../apps/", import.meta.url);
  const names = (await readdir(appRoot)).filter(name => name.endsWith(".dapp"));
  assert.equal(names.length, 46);

  for (const name of names) {
    const source = await readFile(new URL(name, appRoot), "utf8");
    assert.match(source, /^# @boards .*\bm5stack-tab5\b/m, `${name} declares Tab5 support`);

    for (const match of source.matchAll(/^CANVAS (\d+) (\d+)$/gm)) {
      assert.ok(Number(match[1]) >= 80, `${name} fixed canvas is at least 80 columns`);
      assert.ok(Number(match[2]) >= 30, `${name} fixed canvas is at least 30 rows`);
    }
  }

  const chat = await readFile(new URL("dappchat.dapp", appRoot), "utf8");
  assert.match(chat, /^SET cols 100$/m);
  assert.match(chat, /^SET rows 40$/m);
});

test("Tracker Music renders its sequencer and exits with audio released", async () => {
  const files = new MemoryFiles();
  let runtime;
  let queued = false;
  let stops = 0;
  const io = {
    output() {}, clear() {}, status() {}, input() {}, endCanvas() {},
    canvas() {
      if (!queued) {
        queued = true;
        runtime.pushKey({ key: "Escape", ctrlKey: false });
      }
    },
    waveStop() { stops += 1; }
  };
  runtime = new DappRuntime(io, files);
  const source = await readFile(new URL("../apps/tracker-music.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true);
  assert.ok(stops >= 1);
});

test("Tracker Music saves per-note tones and opens help", async () => {
  const files = new MemoryFiles();
  const frames = [];
  let runtime;
  let step = 0;
  let saveFrame = false;
  const push = key => runtime.pushKey({ key, ctrlKey: false });
  const io = {
    output() {}, clear() {}, status() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve("tracker-music.dat"); },
    canvas(canvas) {
      const text = chatCanvasText(canvas);
      if (!text.trim()) return;
      frames.push(text);
      if (text.includes("TRACKER MUSIC // CONTROLS")) {
        push("x");
        return;
      }
      if (text.includes("SAVE TRACKER")) {
        if (!saveFrame) {
          saveFrame = true;
          push("s");
        }
        return;
      }
      step += 1;
      //"d" plays D# at the default octave (4) on channel 1, which both places and
      //toggles step 0 in one press (piano-row note keys, not the old 1-6 tone-select).
      //Command letters that collide with a note letter now need Shift: "S" opens the
      //save browser (lowercase "s" plays C# instead), "H" opens help (lowercase "h"
      //plays G# instead).
      if (step === 1) push("d");
      else if (step === 2) push("S");
      else if (step === 3) push("H");
      else if (step >= 4) push("Escape");
    }
  };
  runtime = new DappRuntime(io, files);
  const source = await readFile(new URL("../apps/tracker-music.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true, result.error?.message);
  assert.ok(frames.some(frame => frame.includes("TRACKER MUSIC // CONTROLS")));
  assert.ok(frames.some(frame => frame.includes("PATTERN 1/8") && frame.includes("TONE D#4")));
  const saved = files.read("/apps/tracker-music.dat").trimEnd().split("\n");
  assert.equal(saved.length, 58);
  //"2 1 4" is the default square/triangle/noise per channel, unchanged since this
  //test never presses W. Tone 39 is D#4 ((octave-1)*12+semitone = 3*12+3); octave
  //(4) is its own line in TM5, absent from the older TM4 layout this test used to
  //check.
  assert.deepEqual(saved.slice(0, 9),
    ["TM5", "120", "39 36 36", "2 1 4", "4", "0", "8", "0", "1"]);
  assert.equal(saved[9], "0000000000000000");
  assert.equal(saved[10], "1000000000000000");
  assert.equal(saved[11], "0000000000000000");
  assert.equal(saved[12], "0000000000000000");
  //TM5 tone rows are space-separated (0..107 no longer fits one ASCII digit);
  //step 0 carries the placed D#4 (39), the rest are still the default C4 (36)
  assert.equal(saved[13], "39 36 36 36 36 36 36 36 36 36 36 36 36 36 36 36");
  assert.equal(saved[14], "36 36 36 36 36 36 36 36 36 36 36 36 36 36 36 36");
  assert.equal(saved[15], "36 36 36 36 36 36 36 36 36 36 36 36 36 36 36 36");
});

test("Tracker Music switches and saves multiple patterns independently", async () => {
  const files = new MemoryFiles();
  const frames = [];
  let runtime;
  let step = 0;
  let saveFrame = false;
  const push = key => runtime.pushKey({ key, ctrlKey: false });
  const io = {
    output() {}, clear() {}, status() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve("tracker-music.dat"); },
    canvas(canvas) {
      const text = chatCanvasText(canvas);
      if (!text.trim()) return;
      frames.push(text);
      if (text.includes("SAVE TRACKER")) {
        if (!saveFrame) {
          saveFrame = true;
          push("s");
        }
        return;
      }
      step += 1;
      //"]" moves to pattern 2, "5" sets the octave those piano-row keys play in,
      //"z" plays C5 (also toggles step 0 on), "a" assigns pattern 2 into song slot
      //0, "S" (Shift -- lowercase is now a note) opens the save browser
      if (step === 1) push("]");
      else if (step === 2) push("5");
      else if (step === 3) push("z");
      else if (step === 4) push("a");
      else if (step === 5) push("S");
      else if (step >= 6) push("Escape");
    }
  };
  runtime = new DappRuntime(io, files);
  const source = await readFile(new URL("../apps/tracker-music.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true, result.error?.message);
  assert.ok(frames.some(frame => frame.includes("PATTERN 2/8") && frame.includes("TONE C 5")));
  const saved = files.read("/apps/tracker-music.dat").trimEnd().split("\n");
  //octave (5) is TM5's own header line; tone 48 is C5 ((octave-1)*12+semitone = 4*12+0)
  assert.deepEqual(saved.slice(0, 9),
    ["TM5", "120", "48 36 36", "2 1 4", "5", "1", "8", "0", "1"]);
  assert.equal(saved[9], "1000000000000000");   //song[0] = pattern 1 (0-indexed), from "a"
  assert.equal(saved[10], "0000000000000000");  //pattern 0's own steps are untouched
  assert.equal(saved[16], "1000000000000000");  //pattern 1's step 0, toggled on by "z"
  assert.equal(saved[19], "48 36 36 36 36 36 36 36 36 36 36 36 36 36 36 36");
});

test("Tracker Music migrates old one-pattern saves", async () => {
  const files = new MemoryFiles({
    "/apps/tracker-music.dat": [
      "140",
      "3 2 3",
      "1000000000000000",
      "0000000000000000",
      "0000000000000000",
      "3000000000000000",
      "2222222222222222",
      "3333333333333333",
      ""
    ].join("\n")
  });
  let runtime;
  let step = 0;
  let saveFrame = false;
  const push = key => runtime.pushKey({ key, ctrlKey: false });
  const io = {
    output() {}, clear() {}, status() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve("tracker-music.dat"); },
    canvas(canvas) {
      const text = chatCanvasText(canvas);
      if (!text.trim()) return;
      if (text.includes("SAVE TRACKER")) {
        if (!saveFrame) {
          saveFrame = true;
          push("s");
        }
        return;
      }
      step += 1;
      if (step === 1) push("S");   //Shift -- lowercase "s" is now a note key
      else if (step >= 2) push("Escape");
    }
  };
  runtime = new DappRuntime(io, files);
  const source = await readFile(new URL("../apps/tracker-music.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true, result.error?.message);
  const saved = files.read("/apps/tracker-music.dat").trimEnd().split("\n");
  //untagged saves predate the octave concept, so it comes back at its default (4);
  //the old file's tone1/2/3 (0..5, scale degrees under the old pentatonic scheme)
  //load through unreinterpreted -- now absolute low-octave pitch indices instead,
  //an accepted side effect of the chromatic-note rework, not something this
  //migration path tries to compensate for
  assert.deepEqual(saved.slice(0, 9),
    ["TM5", "140", "3 2 3", "2 1 4", "4", "0", "8", "0", "1"]);
  assert.equal(saved[9], "0000000000000000");
  assert.equal(saved[10], "1000000000000000");
  assert.equal(saved[13], "3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
});

test("Tracker Music save browser creates a directory and saves a named file", async () => {
  const files = new MemoryFiles();
  let runtime;
  let step = 0;
  let saveFrame = 0;
  const answers = ["songs", "jam.dat"];
  const push = key => runtime.pushKey({ key, ctrlKey: false });
  const io = {
    output() {}, clear() {}, status() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(answers.shift() || ""); },
    canvas(canvas) {
      const text = chatCanvasText(canvas);
      if (!text.trim()) return;
      if (text.includes("SAVE TRACKER")) {
        saveFrame += 1;
        if (saveFrame === 1) push("n");
        else if (saveFrame === 2) push("s");
        else if (saveFrame >= 3) push("Escape");
        return;
      }
      step += 1;
      if (step === 1) push("S");   //Shift -- lowercase "s" is now a note key
      else if (step >= 2) push("Escape");
    }
  };
  runtime = new DappRuntime(io, files);
  const source = await readFile(new URL("../apps/tracker-music.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true, result.error?.message);
  assert.ok(files.exists("/apps/songs/jam.dat"));
  const saved = files.read("/apps/songs/jam.dat").trimEnd().split("\n");
  assert.equal(saved[0], "TM5");
});

test("Tracker Music's load browser opens on L, lists the last save location, and loads the picked file", async () => {
  const files = new MemoryFiles();
  const frames = [];
  let runtime;
  let step = 0;
  let saveFrame = 0;
  let loadFrame = 0;
  const answers = ["songs", "jam.dat"];
  const push = key => runtime.pushKey({ key, ctrlKey: false });
  //the step-0 cell on the "CH1 SQR" (channel 1's default square-wave label) row:
  //a fixed 2-column slot beginning at column ten, "D#"/"C " for a
  //placed note or "." for empty. The cursor "@" only overlays it while the
  //cursor sits on step 0, which the ArrowRight below moves off of, so this
  //reads the real content for the rest of the run. Sliced by fixed column
  //rather than whitespace-stripped, since a natural note's trailing space
  //would otherwise collapse and throw off alignment.
  const step0Cell = frame => {
    const line = frame.split("\n").find(l => l.includes("CH1 SQR"));
    return line ? line.slice(10, 12).trim() : "";
  };
  const io = {
    output() {}, clear() {}, status() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(answers.shift() || ""); },
    canvas(canvas) {
      const text = chatCanvasText(canvas);
      if (!text.trim()) return;
      frames.push(text);
      if (text.includes("SAVE TRACKER")) {
        saveFrame += 1;
        if (saveFrame === 1) push("n");
        else if (saveFrame === 2) push("s");
        else if (saveFrame >= 3) push("Escape");
        return;
      }
      if (text.includes("LOAD TRACKER")) {
        loadFrame += 1;
        //first frame: land on the "/apps/songs" listing left behind by the save above,
        //with jam.dat as the only (and so already-selected) entry -- Enter loads it
        if (loadFrame === 1) push("Enter");
        else if (loadFrame >= 2) push("Escape");
        return;
      }
      step += 1;
      //place a distinctive note (which also switches step 0 on), move the cursor off
      //step 0 so its cell is readable, save to a custom directory via the browser,
      //clear the pattern so the in-memory state no longer proves anything, then open
      //the load browser (L) and pick the file back up. Save/Clear/Load are all Shift
      //now -- "s"/"c"/"l" are piano-row note keys (C#, E, C#+1oct) in the main view.
      if (step === 1) push("d");
      else if (step === 2) push("ArrowRight");
      else if (step === 3) push("S");
      else if (step === 4) push("C");
      else if (step === 5) push("L");
      else if (step >= 6) push("Escape");
    }
  };
  runtime = new DappRuntime(io, files);
  const source = await readFile(new URL("../apps/tracker-music.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true, result.error?.message);
  assert.ok(files.exists("/apps/songs/jam.dat"));
  assert.ok(!files.exists("/apps/tracker-music.dat"));
  assert.ok(loadFrame >= 1, "L should have opened the load browser instead of loading silently");
  const clearedIndex = frames.findIndex(frame => step0Cell(frame) === ".");
  assert.ok(clearedIndex >= 0, "clearing the pattern should have switched step 0 off before the reload");
  assert.ok(frames.slice(clearedIndex + 1).some(frame => step0Cell(frame) === "D#"),
    "loading the picked file through the browser should restore step 0's D# note");
});

test("Tracker Music cycles a channel's waveform with W, actually plays the chosen kind, and persists it through save/load", async () => {
  const files = new MemoryFiles();
  const waves = [];
  let runtime;
  let step = 0;
  let saveFrame = 0;
  let loadFrame = 0;
  let phase = "setup";
  let playMark = 0;
  const push = key => runtime.pushKey({ key, ctrlKey: false });
  const io = {
    output() {}, clear() {}, status() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve("tracker-music.dat"); },
    wave(value) { waves.push(value); },
    canvas(canvas) {
      const text = chatCanvasText(canvas);
      if (!text.trim()) return;

      if (text.includes("SAVE TRACKER")) {
        saveFrame += 1;
        if (saveFrame === 1) push("s");
        else push("Escape");
        return;
      }
      if (text.includes("LOAD TRACKER")) {
        loadFrame += 1;
        //only file in the (default) save directory, and already the selection
        if (loadFrame === 1) push("Enter");
        else push("Escape");
        return;
      }

      if (phase === "setup") {
        step += 1;
        if (step === 1) { push(" "); return; }        //toggle step 0 on, channel 1
        if (step === 2) { push("w"); return; }         //square -> sawtooth
        if (step === 3) {
          phase = "playSawtooth";
          playMark = waves.length;
          push("Enter");                               //play one loop
          return;
        }
      }
      if (phase === "playSawtooth") {
        if (waves.slice(playMark).some(w => w.channel === 1 && w.waveform === "sawtooth")) {
          phase = "opensave";
          push("Escape");                              //abort the play-through early
        }
        return;
      }
      if (phase === "opensave") {
        phase = "recycle";
        push("S");                                     //open the save browser (Shift -- "s" is a note now)
        return;
      }
      if (phase === "recycle") {
        phase = "playNoise";
        playMark = waves.length;
        push("w");                                     //sawtooth -> noise, saved value unchanged
        return;
      }
      if (phase === "playNoise") {
        phase = "playNoiseGo";
        push("Enter");
        return;
      }
      if (phase === "playNoiseGo") {
        if (waves.slice(playMark).some(w => w.channel === 1 && w.waveform === "noise")) {
          phase = "openload";
          push("Escape");
        }
        return;
      }
      if (phase === "openload") {
        phase = "playReloaded";
        push("L");                                     //open the load browser (Shift -- "l" is a note now)
        return;
      }
      if (phase === "playReloaded") {
        phase = "playReloadedGo";
        playMark = waves.length;
        push("Enter");
        return;
      }
      if (phase === "playReloadedGo") {
        if (waves.slice(playMark).some(w => w.channel === 1 && w.waveform === "sawtooth")) {
          phase = "done";
          push("Escape");
        }
        return;
      }
      push("Escape");
    }
  };
  runtime = new DappRuntime(io, files);
  const source = await readFile(new URL("../apps/tracker-music.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true, result.error?.message);
  assert.equal(phase, "done", "the run should have reached every phase in order");
  const saved = files.read("/apps/tracker-music.dat").trimEnd().split("\n");
  //"3 1 4" is wave1=sawtooth (cycled from the square default), wave2/wave3 untouched
  assert.equal(saved[3], "3 1 4");
});

test("Tracker Music save browser opens the SD app directory from root", async () => {
  const files = new MemoryFiles();
  let runtime;
  let step = 0;
  let saveFrame = 0;
  const answers = ["sdjam.dat"];
  const push = key => runtime.pushKey({ key, ctrlKey: false });
  const io = {
    output() {}, clear() {}, status() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(answers.shift() || ""); },
    canvas(canvas) {
      const text = chatCanvasText(canvas);
      if (!text.trim()) return;
      if (text.includes("SAVE TRACKER")) {
        saveFrame += 1;
        if (saveFrame === 1) push("Backspace");
        else if (saveFrame === 2) push("ArrowDown");
        else if (saveFrame === 3) push("Enter");
        else if (saveFrame === 4) push("Enter");
        else if (saveFrame === 5) push("s");
        else if (saveFrame >= 6) push("Escape");
        return;
      }
      step += 1;
      if (step === 1) push("S");   //Shift -- lowercase "s" is now a note key
      else if (step >= 2) push("Escape");
    }
  };
  runtime = new DappRuntime(io, files);
  const source = await readFile(new URL("../apps/tracker-music.dapp", import.meta.url), "utf8");
  const result = await runtime.run(source);

  assert.equal(result.ok, true, result.error?.message);
  assert.ok(files.exists("/sd/apps/sdjam.dat"));
});

test("Tracker Music sequences patterns and plays a song", async () => {
  const files = new MemoryFiles();
  const waves = [];
  let runtime;
  let step = 0;
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  globalThis.setTimeout = (callback) => {
    queueMicrotask(callback);
    return 0;
  };
  globalThis.clearTimeout = () => {};
  try {
    const push = key => runtime.pushKey({ key, ctrlKey: false });
    const io = {
      output() {}, clear() {}, status() {}, input() {}, endCanvas() {}, waveStop() {},
      wave(value) { waves.push(value); },
      canvas(canvas) {
        const text = chatCanvasText(canvas);
        if (!text.trim()) return;
        step += 1;
        //octave 2 + "n" (A) is exactly 110Hz (two octaves below A4=440); octave 3 +
        //"b" (G) rounds to 196Hz -- the same two pitches the old digit-tone-select
        //keys ("1" and "5" against the since-removed hardcoded pentatonic table)
        //used to reach, now via piano-row note keys plus an explicit octave
        if (step === 1) push("2");
        else if (step === 2) push("n");
        else if (step === 3) push(".");
        else if (step === 4) push("]");
        else if (step === 5) push("3");
        else if (step === 6) push("b");
        else if (step === 7) push("a");
        else if (step === 8) push("p");
        else if (waves.some(wave => wave.frequency === 110) && waves.some(wave => wave.frequency === 196)) {
          push("Escape");
        }
      }
    };
    runtime = new DappRuntime(io, files);
    const source = await readFile(new URL("../apps/tracker-music.dapp", import.meta.url), "utf8");
    const result = await runtime.run(source);

    assert.equal(result.ok, true, result.error?.message);
    assert.ok(waves.some(wave => wave.frequency === 110));
    assert.ok(waves.some(wave => wave.frequency === 196));
  } finally {
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
  }
});

test("Requests, Control and Feeds exercise their network and persistence paths", async (t) => {
  const previousFetch = globalThis.fetch;
  t.after(() => { globalThis.fetch = previousFetch; });

  // Requests: bounded response is displayed and persisted.
  globalThis.fetch = async () => ({
    status: 200, ok: true,
    arrayBuffer: async () => new TextEncoder().encode('{"ok":true}').buffer
  });
  let answers = ["2", "", "q"];
  let outputs = [];
  let files = new MemoryFiles();
  let runtime = new DappRuntime({
    output(text) { outputs.push(text); }, clear() {}, status() {}, canvas() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(answers.shift()); }
  }, files);
  let source = await readFile(new URL("../apps/requests.dapp", import.meta.url), "utf8");
  let result = await runtime.run(source);
  assert.equal(result.ok, true);
  assert.match(files.read("/apps/requests.last"), /"ok":true/);

  // Control: configured service is polled and logged.
  answers = ["a", "1", "API", "https://example.test/health", "x", "", "q"];
  outputs = [];
  files = new MemoryFiles();
  runtime = new DappRuntime({
    output(text) { outputs.push(text); }, clear() {}, status() {}, canvas() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(answers.shift()); }
  }, files);
  source = await readFile(new URL("../apps/control.dapp", import.meta.url), "utf8");
  result = await runtime.run(source);
  assert.equal(result.ok, true);
  assert.match(files.read("/apps/control.log"), /UP API HTTP=200/);

  // Feeds: RSS items become a durable title/link index.
  const rss = "<rss><channel><item><title>First &amp; Best</title>"
    + "<link>https://example.test/first</link></item>"
    + "<item><title>Second</title><link>https://example.test/second</link></item></channel></rss>";
  globalThis.fetch = async () => ({
    status: 200, ok: true,
    arrayBuffer: async () => new TextEncoder().encode(rss).buffer
  });
  answers = ["1", "", "q", "q"];
  files = new MemoryFiles();
  runtime = new DappRuntime({
    output() {}, clear() {}, status() {}, canvas() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(answers.shift()); }
  }, files);
  source = await readFile(new URL("../apps/feeds.dapp", import.meta.url), "utf8");
  result = await runtime.run(source);
  assert.equal(result.ok, true);
  assert.equal(files.read("/apps/feeds.index"), [
    "First & Best", "https://example.test/first", "Second", "https://example.test/second", ""
  ].join("\n"));
});

test("Data profiles a local table and Today reads shared app files", async () => {
  let answers = ["1", "/apps/sample.csv", "4", "", "q"];
  let outputs = [];
  let files = new MemoryFiles({ "/apps/sample.csv": "name,value\na,10\nb,20\n" });
  let runtime = new DappRuntime({
    output(text) { outputs.push(text); }, clear() {}, status() {}, canvas() {}, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(answers.shift()); }
  }, files);
  let source = await readFile(new URL("../apps/data.dapp", import.meta.url), "utf8");
  let result = await runtime.run(source);
  assert.equal(result.ok, true);
  assert.ok(outputs.includes("3 rows, up to 2 columns"), JSON.stringify(outputs));

  answers = ["q"];
  outputs = [];
  const frames = [];
  files = new MemoryFiles({
    "/apps/todo.txt": "[1] ship apps\n",
    "/apps/control.log": "[2] UP API HTTP=200 ms=4\n"
  });
  runtime = new DappRuntime({
    output(text) { outputs.push(text); }, clear() {}, status() {},
    canvas(canvas) { frames.push(chatCanvasText(canvas)); }, endCanvas() {}, waveStop() {},
    input(_prompt, resolve) { resolve(answers.shift()); }
  }, files);
  source = await readFile(new URL("../apps/today.dapp", import.meta.url), "utf8");
  result = await runtime.run(source);
  assert.equal(result.ok, true);
  assert.ok(frames.some(frame => frame.includes("ship apps")));
  assert.ok(frames.some(frame => frame.includes("UP API")));
});
