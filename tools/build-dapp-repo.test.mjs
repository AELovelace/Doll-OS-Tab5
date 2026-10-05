import assert from "node:assert/strict";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import {
  buildRepository,
  RepositoryValidationError,
} from "./build-dapp-repo.mjs";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const projectConfig = path.join(projectRoot, "dapper", "repository.config.json");
const projectCompatibility = path.join(projectRoot, "dapper", "compatibility-v1.json");
const m5AppRunner = path.resolve(projectRoot, "..", "DOLL-OS", "AppRunner.ino");
const tab5AppRunner = path.resolve(projectRoot, "..", "Doll-OS-Tab5", "AppRunner.ino");
const p4AppRunner = path.resolve(
  projectRoot,
  "..",
  "Doll-OS-P4",
  "firmware",
  "DollOS_P4",
  "AppRunner.ino",
);

function resolvedRuntimeOpcodes(compatibility, version) {
  const runtime = compatibility.runtimes[version];
  assert.ok(runtime, `runtime ${version} is present`);
  const inherited = runtime.extends
    ? resolvedRuntimeOpcodes(compatibility, runtime.extends)
    : [];
  return [...new Set([...inherited, ...(runtime.opcodes ?? [])])].sort();
}

function sourceOpcodes(source) {
  const opcodeTable = source.match(
    /DAPP_OPCODE_NAMES\s*\[[^\]]+\]\s*=\s*\{([\s\S]*?)\};/,
  );
  const decodedNames = opcodeTable
    ? [...opcodeTable[1].matchAll(/"([A-Z][A-Z0-9_]*)"/g)].map((match) => match[1])
    : [];
  return [
    ...new Set(
      [
        ...decodedNames,
        ...[...source.matchAll(/op\s*==\s*"([A-Z][A-Z0-9_]*)"/g)].map(
          (match) => match[1],
        ),
      ],
    ),
  ].sort();
}

function sourceIntegerConstants(source) {
  return new Map(
    // size_t and a leading `static` are both in use for these caps now, so the shape is
    // matched loosely rather than by listing every spelling
    [...source.matchAll(
      /(?:static\s+)?const (?:int|unsigned long|size_t) (DAPP_[A-Z0-9_]+)\s*=\s*(\d+)\s*;/g
    )].map(
      (match) => [match[1], Number(match[2])],
    ),
  );
}

function assertLimitsMatch(source, limits, mappings) {
  const constants = sourceIntegerConstants(source);
  for (const [constant, field] of Object.entries(mappings)) {
    assert.equal(constants.get(constant), limits[field], `${constant} matches limits.${field}`);
  }
}

function packageText({
  id = "hello",
  name = "Hello",
  version = "1.0.0",
  boards = "m5cardputer,fnk0104,m5stack-tab5,crowpanel-p4",
  runtime = ">=1.0.0 <2.0.0",
  body = 'PRINT "hello"',
} = {}) {
  return [
    "# @dapp-format 1",
    `# @id ${id}`,
    `# @name ${name}`,
    `# @version ${version}`,
    `# @boards ${boards}`,
    `# @runtime ${runtime}`,
    "# @summary Test package",
    "",
    body,
    "",
  ].join("\n");
}

async function makeFixture(t, packages, packageRoots = ["apps"]) {
  const root = await mkdtemp(path.join(os.tmpdir(), "dapper-test-"));
  t.after(async () => rm(root, { recursive: true, force: true }));
  for (const [relativePath, contents] of Object.entries(packages)) {
    const destination = path.join(root, relativePath);
    await mkdir(path.dirname(destination), { recursive: true });
    await writeFile(destination, contents, "utf8");
  }
  const configPath = path.join(root, "repository.config.json");
  await writeFile(
    configPath,
    `${JSON.stringify(
      {
        repository_format: 1,
        id: "test-repo",
        name: "Test Repository",
        canonical_url: "https://example.test/dapper/",
        catalog: "catalog-v1.ndjson",
        compatibility: projectCompatibility,
        package_roots: packageRoots,
      },
      null,
      2,
    )}\n`,
    "utf8",
  );
  return { root, configPath, outputPath: path.join(root, "dist") };
}

test("current repository sources validate", async () => {
  const result = await buildRepository({ configPath: projectConfig, checkOnly: true });
  assert.equal(result.repo.canonical_url, "https://sadgirlsclub.wtf/dapper/");

  // The published set grows; the invariants do not. Naming every app here
  // would mean editing this test to add one, so it asserts the catalog order
  // and the shape of each record instead, plus the apps that must not vanish.
  const ids = result.records.map((record) => record.id);
  assert.deepEqual(ids, [...ids].sort((left, right) => left.localeCompare(right, "en")));
  for (const id of ["2048", "adventure", "beacon", "decide", "drill", "four", "lamp", "life", "mines", "notes", "page", "plot", "sheet", "simon", "snake", "sysmon", "tetris"]) {
    assert.ok(ids.includes(id), `${id} is published`);
  }
  for (const record of result.records) {
    assert.ok(record.boards.length > 0, `${record.id} declares at least one supported board`);
    assert.match(record.sha256, /^[a-f0-9]{64}$/);
  }

  const universal = result.records.find((record) => record.id === "decide");
  assert.deepEqual(universal.boards, ["fnk0104", "m5cardputer"]);
  const tab5Edition = result.records.find(
    (record) => record.id === "decide" && record.boards.includes("m5stack-tab5"),
  );
  assert.deepEqual(tab5Edition.boards, ["m5stack-tab5"]);
  const p4Edition = result.records.find(
    (record) => record.id === "decide" && record.boards.includes("crowpanel-p4"),
  );
  assert.deepEqual(p4Edition.boards, ["crowpanel-p4"]);

  // Every FNK package needs matching widescreen artifacts; comparing the IDs keeps
  // this invariant current as packages are added instead of baking in a stale count.
  const idsForBoard = (board) => result.records
    .filter((record) => record.boards.includes(board))
    .map((record) => record.id)
    .sort((left, right) => left.localeCompare(right, "en"));
  const fnkIds = idsForBoard("fnk0104");
  const tab5Ids = idsForBoard("m5stack-tab5");
  const p4Ids = idsForBoard("crowpanel-p4");
  assert.deepEqual(p4Ids, tab5Ids, "all widescreen apps have both Tab5 and P4 editions");
  assert.deepEqual(
    fnkIds.filter((id) => !p4Ids.includes(id)),
    [],
    "every FNK app has a P4 edition",
  );
});

test("FNK0104 compatibility contract matches AppRunner source", async () => {
  const compatibility = JSON.parse(await readFile(projectCompatibility, "utf8"));
  const source = await readFile(path.join(projectRoot, "AppRunner.ino"), "utf8");
  // resolved against whatever runtime the board declares, so a runtime bump does not
  // also need this assertion edited to match
  const declared = compatibility.boards.fnk0104.runtime;
  assert.deepEqual(sourceOpcodes(source), resolvedRuntimeOpcodes(compatibility, declared));
  assertLimitsMatch(source, compatibility.boards.fnk0104.limits, {
    DAPP_MAX_LINES: "lines",
    DAPP_MAX_LABELS: "labels",
    DAPP_MAX_VARS: "numeric_variables",
    DAPP_MAX_STRING_VARS: "string_variables",
    DAPP_MAX_STRING_LEN: "string_length",
    DAPP_MAX_ARRAYS: "arrays",
    DAPP_ARRAY_POOL_CELLS: "array_cells",
    DAPP_MAX_CALL_DEPTH: "call_depth",
    DAPP_CANVAS_MAX_COLS: "canvas_columns",
    DAPP_CANVAS_MAX_ROWS: "canvas_rows",
    DAPP_MAX_STEPS: "steps",
    DAPP_BUF_MAX_BYTES: "buffer_bytes",
  });
});

test("M5Cardputer compatibility contract matches sibling AppRunner when available", async (t) => {
  let source;
  try {
    source = await readFile(m5AppRunner, "utf8");
  } catch (error) {
    if (error.code === "ENOENT") {
      t.skip("sibling DOLL-OS checkout is not available");
      return;
    }
    throw error;
  }
  const compatibility = JSON.parse(await readFile(projectCompatibility, "utf8"));
  assert.deepEqual(sourceOpcodes(source), resolvedRuntimeOpcodes(compatibility, "1.3.0"));
  assertLimitsMatch(source, compatibility.boards.m5cardputer.limits, {
    DAPP_MAX_LINES: "lines",
    DAPP_MAX_LABELS: "labels",
    DAPP_MAX_VARS: "numeric_variables",
    DAPP_MAX_STRING_VARS: "string_variables",
    DAPP_MAX_STRING_LEN: "string_length",
    DAPP_MAX_STEPS: "steps",
  });
});

test("Tab5 compatibility contract matches sibling AppRunner", async () => {
  const compatibility = JSON.parse(await readFile(projectCompatibility, "utf8"));
  const source = await readFile(tab5AppRunner, "utf8");
  const declared = compatibility.boards["m5stack-tab5"].runtime;
  assert.deepEqual(sourceOpcodes(source), resolvedRuntimeOpcodes(compatibility, declared));
  assertLimitsMatch(source, compatibility.boards["m5stack-tab5"].limits, {
    DAPP_MAX_LINES: "lines",
    DAPP_MAX_LABELS: "labels",
    DAPP_MAX_VARS: "numeric_variables",
    DAPP_MAX_STRING_VARS: "string_variables",
    DAPP_MAX_STRING_LEN: "string_length",
    DAPP_MAX_ARRAYS: "arrays",
    DAPP_ARRAY_POOL_CELLS: "array_cells",
    DAPP_MAX_CALL_DEPTH: "call_depth",
    DAPP_CANVAS_MAX_COLS: "canvas_columns",
    DAPP_CANVAS_MAX_ROWS: "canvas_rows",
    DAPP_MAX_STEPS: "steps",
    DAPP_BUF_MAX_BYTES: "buffer_bytes",
  });
});

test("CrowPanel P4 compatibility contract matches sibling AppRunner", async () => {
  const compatibility = JSON.parse(await readFile(projectCompatibility, "utf8"));
  const source = await readFile(p4AppRunner, "utf8");
  const declared = compatibility.boards["crowpanel-p4"].runtime;
  assert.deepEqual(sourceOpcodes(source), resolvedRuntimeOpcodes(compatibility, declared));
  assertLimitsMatch(source, compatibility.boards["crowpanel-p4"].limits, {
    DAPP_MAX_LINES: "lines",
    DAPP_MAX_LABELS: "labels",
    DAPP_MAX_VARS: "numeric_variables",
    DAPP_MAX_STRING_VARS: "string_variables",
    DAPP_MAX_STRING_LEN: "string_length",
    DAPP_MAX_ARRAYS: "arrays",
    DAPP_ARRAY_POOL_CELLS: "array_cells",
    DAPP_MAX_CALL_DEPTH: "call_depth",
    DAPP_CANVAS_MAX_COLS: "canvas_columns",
    DAPP_CANVAS_MAX_ROWS: "canvas_rows",
    DAPP_MAX_STEPS: "steps",
    DAPP_BUF_MAX_BYTES: "buffer_bytes",
  });
});

test("builds a universal artifact and parseable static catalog", async (t) => {
  const fixture = await makeFixture(t, { "apps/hello.dapp": packageText() });
  const result = await buildRepository(fixture);
  assert.equal(result.records.length, 1);
  assert.equal(result.records[0].url, "packages/hello/1.0.0/universal.dapp");

  const repo = JSON.parse(await readFile(path.join(fixture.outputPath, "repo.json"), "utf8"));
  const catalogLines = (await readFile(
    path.join(fixture.outputPath, "catalog-v1.ndjson"),
    "utf8",
  ))
    .trim()
    .split("\n");
  assert.equal(repo.canonical_url, "https://example.test/dapper/");
  assert.equal(catalogLines.length, 1);
  assert.equal(JSON.parse(catalogLines[0]).id, "hello");

  const copied = await readFile(
    path.join(fixture.outputPath, "packages", "hello", "1.0.0", "universal.dapp"),
    "utf8",
  );
  assert.equal(copied, packageText());
});

test("rejects an opcode newer than the declared runtime minimum", async (t) => {
  const fixture = await makeFixture(t, {
    "apps/canvas-demo.dapp": packageText({
      id: "canvas-demo",
      name: "Canvas Demo",
      boards: "fnk0104",
      body: "CANVAS 20 10",
    }),
  });
  await assert.rejects(
    buildRepository({ configPath: fixture.configPath, checkOnly: true }),
    (error) =>
      error instanceof RepositoryValidationError && error.message.includes("source requires >=1.1.0"),
  );
});

test("rejects LED under a pre-LED runtime contract", async (t) => {
  const fixture = await makeFixture(t, {
    "apps/led-demo.dapp": packageText({
      id: "led-demo",
      name: "LED Demo",
      boards: "fnk0104",
      runtime: ">=1.2.0 <2.0.0",
      body: "LED 255 0 0",
    }),
  });
  await assert.rejects(
    buildRepository({ configPath: fixture.configPath, checkOnly: true }),
    (error) =>
      error instanceof RepositoryValidationError && error.message.includes("source requires >=1.3.0"),
  );
});

test("rejects conditional GOSUB under the 1.0 runtime contract", async (t) => {
  const fixture = await makeFixture(t, {
    "apps/branch.dapp": packageText({
      id: "branch",
      name: "Branch",
      boards: "fnk0104",
      body: "IF 1 = 1 GOSUB work",
    }),
  });
  await assert.rejects(
    buildRepository({ configPath: fixture.configPath, checkOnly: true }),
    (error) =>
      error instanceof RepositoryValidationError && error.message.includes("source requires >=1.1.0"),
  );
});

test("rejects @echo off under a pre-echo-control runtime contract", async (t) => {
  const fixture = await makeFixture(t, {
    "apps/quiet.dapp": packageText({
      id: "quiet",
      name: "Quiet",
      boards: "fnk0104",
      runtime: ">=1.4.1 <2.0.0",
      body: "# @echo off\nINPUT line",
    }),
  });
  await assert.rejects(
    buildRepository({ configPath: fixture.configPath, checkOnly: true }),
    (error) =>
      error instanceof RepositoryValidationError && error.message.includes("source requires >=1.4.2"),
  );
});

test("counts metadata and comments toward the M5Cardputer line limit", async (t) => {
  const compatibility = JSON.parse(await readFile(projectCompatibility, "utf8"));
  const lineLimit = compatibility.boards.m5cardputer.limits.lines;
  const metadataLines = 6;
  const body = Array.from({ length: lineLimit - metadataLines + 1 }, (_, index) => `# comment ${index}`).join("\n");
  const fixture = await makeFixture(t, {
    "apps/large.dapp": packageText({
      id: "large",
      name: "Large",
      boards: "m5cardputer",
      body,
    }),
  });
  await assert.rejects(
    buildRepository({ configPath: fixture.configPath, checkOnly: true }),
    (error) =>
      error instanceof RepositoryValidationError &&
      error.message.includes(`physical lines exceed m5cardputer limit ${lineLimit}`),
  );
});

test("rejects overlapping artifacts for one app version and board", async (t) => {
  const app = packageText({ boards: "fnk0104" });
  const fixture = await makeFixture(
    t,
    {
      "first/hello.dapp": app,
      "second/hello.dapp": app,
    },
    ["first", "second"],
  );
  await assert.rejects(
    buildRepository({ configPath: fixture.configPath, checkOnly: true }),
    (error) =>
      error instanceof RepositoryValidationError &&
      error.message.includes("overlaps hello.dapp for hello@1.0.0:fnk0104"),
  );
});
