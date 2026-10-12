// Headless legibility check. Serves the built page, steps a loud full-scale bass
// signal, an overload above 1, and a music-like phrase, then reads the canvas.
// Requires Chrome. Run from the repo: node web/test/legibility.mjs [outDir]
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const dist = path.resolve(here, "../dist");
const harness = path.join(here, "harness.html");
const outDir = path.resolve(process.argv[2] || path.join(here, "out"));
fs.mkdirSync(outDir, { recursive: true });

// Ceilings on a 0..1 luminance. Before this change the loud 3D scene measured
// average 1.0 and near-white 1.0 (every pixel). Near-white is luminance >= 0.92.
const CEILING = {
  average: 0.32,
  peak: 0.94,
  nearWhite: 0.004,
};

const server = http.createServer((req, res) => {
  const url = req.url.split("?")[0];
  const file = url === "/harness.html" ? harness : path.join(dist, decodeURIComponent(url));
  const allowed = file === harness || file.startsWith(dist + path.sep);
  if (!allowed) {
    res.writeHead(403);
    res.end("no");
    return;
  }
  fs.readFile(file, (err, data) => {
    if (err) {
      res.writeHead(404);
      res.end("missing");
      return;
    }
    const type = file.endsWith(".js") ? "text/javascript" : "text/html";
    res.writeHead(200, { "content-type": type, "cache-control": "no-store" });
    res.end(data);
  });
});

await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const port = server.address().port;
const debugPort = 9341;
const profile = fs.mkdtempSync("/tmp/wavis-chrome-");

const chrome = spawn("/usr/bin/google-chrome", [
  "--headless=new",
  `--remote-debugging-port=${debugPort}`,
  `--user-data-dir=${profile}`,
  "--no-first-run",
  "--no-default-browser-check",
  "--disable-dev-shm-usage",
  "--no-sandbox",
  "--use-angle=swiftshader",
  "--enable-unsafe-swiftshader",
  "--window-size=1100,700",
  "--hide-scrollbars",
  "--force-device-scale-factor=1",
  "about:blank",
], { stdio: "ignore" });

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function version() {
  for (let i = 0; i < 40; ++i) {
    try {
      const res = await fetch(`http://127.0.0.1:${debugPort}/json/version`);
      if (res.ok) return await res.json();
    } catch {
      // Chrome is still starting.
    }
    await sleep(150);
  }
  throw new Error("chrome debug port did not open");
}

const info = await version();
const ws = new WebSocket(info.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  ws.addEventListener("open", resolve);
  ws.addEventListener("error", reject);
});

let seq = 0;
const pending = new Map();
ws.addEventListener("message", (event) => {
  const msg = JSON.parse(event.data);
  if (msg.id && pending.has(msg.id)) {
    const waiter = pending.get(msg.id);
    pending.delete(msg.id);
    if (msg.error) waiter.reject(new Error(JSON.stringify(msg.error)));
    else waiter.resolve(msg.result);
  }
});

function send(method, params = {}, sessionId) {
  const id = ++seq;
  const payload = { id, method, params };
  if (sessionId) payload.sessionId = sessionId;
  ws.send(JSON.stringify(payload));
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
}

const { targetId } = await send("Target.createTarget", { url: "about:blank" });
const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
await send("Page.enable", {}, sessionId);
await send("Runtime.enable", {}, sessionId);
await send("Emulation.setDeviceMetricsOverride", {
  width: 1100,
  height: 700,
  deviceScaleFactor: 1,
  mobile: false,
}, sessionId);

async function evaluate(expression) {
  const result = await send("Runtime.evaluate", {
    expression,
    awaitPromise: true,
    returnByValue: true,
    timeout: 120000,
  }, sessionId);
  if (result.exceptionDetails)
    throw new Error(JSON.stringify(result.exceptionDetails.exception || result.exceptionDetails));
  return result.result.value;
}

async function openHarness() {
  await send("Page.navigate", { url: `http://127.0.0.1:${port}/harness.html` }, sessionId);
  for (let i = 0; i < 50; ++i) {
    const ready = await evaluate("typeof window.__wavisRun === 'function' && typeof window.wavisStep === 'function'");
    if (ready) return;
    await sleep(100);
  }
  throw new Error("harness did not load");
}

async function shoot(name) {
  const shot = await send("Page.captureScreenshot", { format: "png" }, sessionId);
  const file = path.join(outDir, name);
  fs.writeFileSync(file, Buffer.from(shot.data, "base64"));
  return file;
}

const scenarios = [
  ["loud-radial", "radial", "loud", 48, 42, 6],
  ["quiet-radial", "radial", "quiet", 48, 42, 6],
  ["overload-radial", "radial", "overload", 48, 42, 6],
  ["music-radial", "radial", "music", 96, 12, 12],
  ["loud-wave", "oscilloscope", "loud", 36, 30, 6],
  ["loud-bars", "spectrum", "loud", 36, 30, 6],
];

const report = [];
const failures = [];
for (const [name, mode, kind, frames, settle, sampleEvery] of scenarios) {
  await openHarness();
  await evaluate(`window.__MODE__ = ${JSON.stringify(mode)}; true`);
  const stats = await evaluate(`window.__wavisRun(${JSON.stringify(kind)}, ${frames}, ${settle}, ${sampleEvery})`);
  const file = await shoot(`${name}.png`);
  const row = { name, file, ...stats };
  report.push(row);
  console.log(JSON.stringify(row));
  if (kind !== "quiet") {
    if (stats.average > CEILING.average)
      failures.push(`${name} average ${stats.average.toFixed(3)} > ${CEILING.average}`);
    if (stats.peak > CEILING.peak)
      failures.push(`${name} peak ${stats.peak.toFixed(3)} > ${CEILING.peak}`);
    if (stats.nearWhite > CEILING.nearWhite)
      failures.push(`${name} near-white ${stats.nearWhite.toFixed(4)} > ${CEILING.nearWhite}`);
  }
}

const byName = Object.fromEntries(report.map((row) => [row.name, row]));
const loud = byName["loud-radial"];
const quiet = byName["quiet-radial"];
const music = byName["music-radial"];
const overload = byName["overload-radial"];
if (!(loud.average > quiet.average + 0.015))
  failures.push(`loud radial average ${loud.average.toFixed(3)} did not rise above quiet ${quiet.average.toFixed(3)}`);
if (!(loud.highlight > quiet.highlight + 0.03))
  failures.push(`loud highlight ${loud.highlight.toFixed(3)} did not rise above quiet ${quiet.highlight.toFixed(3)}`);
if (!(loud.p10 < 0.2 && loud.p90 > loud.p10 + 0.08))
  failures.push(`loud radial contrast p10 ${loud.p10.toFixed(3)} p90 ${loud.p90.toFixed(3)} is flat`);
const musicSpan = Math.max(music.maxAverage - music.minAverage, music.maxHighlight - music.minHighlight);
if (!(musicSpan > 0.02))
  failures.push(`music luminance did not move (avg ${music.minAverage.toFixed(3)}..${music.maxAverage.toFixed(3)}, highlight ${music.minHighlight.toFixed(3)}..${music.maxHighlight.toFixed(3)})`);
if (!(overload.average < CEILING.average && overload.nearWhite < CEILING.nearWhite))
  failures.push("overload escaped the luminance ceiling");

fs.writeFileSync(path.join(outDir, "report.json"), JSON.stringify({ ceiling: CEILING, report, failures }, null, 2));
ws.close();
chrome.kill();
server.close();
if (failures.length) {
  console.error(failures.join("\n"));
  process.exit(1);
}
console.log("legibility ceilings held");
