'use strict';

import fs, { createReadStream } from 'node:fs';
import crypto from 'node:crypto';
import path from 'node:path';

const fsp = fs.promises;

const DEFAULT_TIMEOUT_MS = 1200;
const DEFAULT_CLEANUP_TIMEOUT_MS = 700;
const DEFAULT_MAX_EVENTS = 128;
const DEFAULT_SUMMARY_FRAMES = 12;

function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

function withTimeout(promise, ms, label) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`${label} timed out after ${ms}ms`)), ms);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

function finitePositive(value, fallback) {
  return Number.isFinite(value) && value > 0 ? Math.floor(value) : fallback;
}

async function ensureParent(filePath) {
  await fsp.mkdir(path.dirname(path.resolve(filePath)), { recursive: true });
}

async function writeJson(filePath, value) {
  await ensureParent(filePath);
  await fsp.writeFile(filePath, `${JSON.stringify(value, null, 2)}\n`, 'utf8');
}

function workerIndexFromName(name) {
  const match = String(name || '').match(/(?:em-pthread-|worker[-_ ]?)(\d+)/i);
  return match ? Number(match[1]) : null;
}

function parseWorkerState(response) {
  if (response?.exceptionDetails) {
    return { error: response.exceptionDetails.text || 'Runtime.evaluate raised an exception' };
  }
  const remote = response?.result;
  if (!remote || !Object.prototype.hasOwnProperty.call(remote, 'value')) {
    return { error: remote?.description || 'Runtime.evaluate returned no by-value result' };
  }
  let value = remote.value;
  if (typeof value === 'string') {
    try {
      value = JSON.parse(value);
    } catch (_) {
      return { error: 'Runtime.evaluate returned invalid worker state JSON', raw: value };
    }
  }
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    return { error: 'Runtime.evaluate returned an unexpected worker state', raw: value };
  }
  return value;
}

function boundedFrame(frame) {
  const location = frame?.location || {};
  return {
    functionName: frame?.functionName || '',
    url: frame?.url || '',
    scriptId: location.scriptId ?? null,
    lineNumber: location.lineNumber ?? null,
    columnNumber: location.columnNumber ?? null
  };
}

function summarizeCapture(capture, frameLimit) {
  const pauses = [];
  for (const event of capture.events || []) {
    if (event.method !== 'Debugger.paused') continue;
    const frames = event.params?.callFrames || [];
    pauses.push({
      reason: event.params?.reason || null,
      frameCount: frames.length,
      frames: frames.slice(0, frameLimit).map(boundedFrame),
      framesOmitted: Math.max(0, frames.length - frameLimit)
    });
  }
  return {
    roles: capture.roles,
    arrayIndex: capture.arrayIndex,
    url: capture.url || null,
    workerName: capture.workerName || null,
    workerIndex: capture.workerIndex ?? null,
    workerIndexSource: capture.workerIndexSource || null,
    state: capture.state || null,
    eventCount: capture.events?.length || 0,
    eventsDropped: capture.eventsDropped || 0,
    pauseCount: capture.pauseCount || 0,
    pauses
  };
}

/**
 * Capture only workers owned by this Puppeteer page. Worker identity prefers
 * runtime `workerID`, then `self.name`; array-index fallback is explicitly
 * labeled unverified. Full event params keep every captured frame; the
 * returned stack summary is bounded. Required `outputPath` saves full JSON.
 *
 * Options: workerIndex/workerId (numeric runtime ID, or string worker name),
 * workerName, timeoutMs, cleanupTimeoutMs, settleMs, maxEvents,
 * summaryFrameLimit, and outputPath.
 */
async function captureWorkers(page, options = {}) {
  if (!page || typeof page.workers !== 'function' || typeof page.browser !== 'function') {
    throw new TypeError('captureWorkers requires a Puppeteer Page with workers() and browser()');
  }
  const timeoutMs = finitePositive(options.timeoutMs, DEFAULT_TIMEOUT_MS);
  const cleanupTimeoutMs = finitePositive(options.cleanupTimeoutMs, DEFAULT_CLEANUP_TIMEOUT_MS);
  const maxEvents = finitePositive(options.maxEvents, DEFAULT_MAX_EVENTS);
  const frameLimit = finitePositive(options.summaryFrameLimit, DEFAULT_SUMMARY_FRAMES);
  const settleMs = Math.min(1000,
    Math.max(0, Number.isFinite(options.settleMs) ? Math.floor(options.settleMs) : 40));
  const requested = options.workerIndex ?? options.workerId ?? options.workerName;
  if (typeof options.outputPath !== 'string' || !options.outputPath) {
    throw new TypeError('captureWorkers requires a string outputPath to preserve the full CDP capture');
  }

  const browser = page.browser();
  let browserVersion = null;
  if (browser && typeof browser.version === 'function') {
    try {
      browserVersion = await withTimeout(Promise.resolve(browser.version()), timeoutMs, 'browser.version');
    } catch (_) {}
  }
  const pageUrl = typeof page.url === 'function' ? page.url() : null;
  const workerObjects = await withTimeout(Promise.resolve(page.workers()), timeoutMs, 'page.workers');
  const records = workerObjects.map((worker, arrayIndex) => ({
    worker,
    arrayIndex,
    client: null,
    url: null,
    workerName: null,
    workerIndex: null,
    workerIndexSource: null,
    state: null,
    identityError: null,
    roles: [],
    events: [],
    eventsDropped: 0,
    pauseCount: 0,
    paused: false,
    debuggerEnabled: false,
    listeners: []
  }));
  const failures = [];

  await Promise.all(records.map(async record => {
    const worker = record.worker;
    try {
      record.url = typeof worker.url === 'function' ? await worker.url() :
        (typeof worker.url === 'string' ? worker.url : null);
    } catch (_) {}
    try {
      let client = worker.client;
      if (typeof client === 'function') client = client.call(worker);
      record.client = await withTimeout(Promise.resolve(client), timeoutMs,
        `worker.client[${record.arrayIndex}]`);
      if (!record.client || typeof record.client.send !== 'function') {
        throw new Error('worker.client does not expose CDPSession.send');
      }
      const evaluated = await withTimeout(record.client.send('Runtime.evaluate', {
        expression: `(() => {
          const name = typeof self.name === 'string' ? self.name : null;
          const id = typeof workerID === 'number' ? workerID : null;
          const moduleExists = typeof Module !== 'undefined';
          return JSON.stringify({
            workerIndex: id,
            name,
            url: typeof self.location === 'object' ? self.location.href : null,
            runtimeInitialized: moduleExists && typeof runtimeInitialized === 'boolean'
              ? runtimeInitialized : null,
            modulePresent: moduleExists,
            wasmMemory: typeof wasmMemory !== 'undefined' && Boolean(wasmMemory)
          });
        })()`,
        returnByValue: true,
        awaitPromise: false
      }), timeoutMs, `Runtime.evaluate worker[${record.arrayIndex}]`);
      record.state = parseWorkerState(evaluated);
      if (typeof record.state.name === 'string') record.workerName = record.state.name;
      if (typeof record.state.url === 'string') record.url = record.state.url;
      if (Number.isInteger(record.state.workerIndex)) {
        record.workerIndex = record.state.workerIndex;
        record.workerIndexSource = 'Runtime.evaluate workerID';
      } else {
        const namedIndex = workerIndexFromName(record.workerName);
        if (namedIndex !== null) {
          record.workerIndex = namedIndex;
          record.workerIndexSource = 'self.name';
        }
      }
    } catch (error) {
      record.identityError = String(error?.message || error);
      record.state = { error: record.identityError };
    }
  }));

  // Names are a useful identity when a worker is blocked and Runtime.evaluate
  // times out. Only fall back to page.workers() ordering after identity checks;
  // that ordering is explicitly not evidence of the guest worker ID.
  const workerZero = records.filter(record => record.workerIndex === 0);
  if (!workerZero.length && records[0] && records[0].workerIndex === null) {
    records[0].workerIndex = 0;
    records[0].workerIndexSource = 'page.workers()[0] (unverified fallback)';
    workerZero.push(records[0]);
  }
  let requestedRecords;
  if (Number.isInteger(requested)) {
    requestedRecords = records.filter(record => record.workerIndex === requested);
    if (!requestedRecords.length && records[requested] && records[requested].workerIndex === null) {
      const fallback = records[requested];
      fallback.workerIndex = requested;
      fallback.workerIndexSource = `page.workers()[${requested}] (unverified fallback)`;
      requestedRecords = [fallback];
    }
  } else {
    requestedRecords = records.filter(record =>
      record.workerName === requested || record.url === requested);
  }
  if (!workerZero.length) failures.push('worker 0 was not found on this page');
  if (!requestedRecords.length) failures.push(`requested guest worker ${String(requested)} was not found on this page`);

  const selectedByWorker = new Map();
  for (const record of [...workerZero, ...requestedRecords]) {
    if (!record.client) {
      failures.push(`worker[${record.arrayIndex}] has no usable worker.client CDP session`);
      continue;
    }
    if (workerZero.includes(record)) record.roles.push('worker0');
    if (requestedRecords.includes(record)) record.roles.push('requested');
    selectedByWorker.set(record.arrayIndex, record);
  }
  const selected = [...selectedByWorker.values()];

  function addListener(record, eventName, handler) {
    if (typeof record.client.on !== 'function') {
      throw new Error(`worker.client[${record.arrayIndex}] does not expose event listeners`);
    }
    record.client.on(eventName, handler);
    record.listeners.push([eventName, handler]);
  }
  function recordEvent(record, method, params) {
    const eventParams = params || {};
    if (method === 'Debugger.paused') {
      record.paused = true;
      record.pauseCount++;
    } else if (method === 'Debugger.resumed') {
      record.paused = false;
    }
    const hasFrames = method === 'Debugger.paused' ||
      method === 'Runtime.exceptionThrown' ||
      Object.prototype.hasOwnProperty.call(eventParams, 'callFrames') ||
      Object.prototype.hasOwnProperty.call(eventParams, 'stackTrace');
    if (!hasFrames && method !== 'Debugger.resumed') return;
    if (record.events.length < maxEvents) {
      record.events.push({ at: Date.now(), method, params: eventParams });
    } else {
      record.eventsDropped++;
    }
  }

  try {
    // Subscribe on every selected worker before enabling any debugger. In
    // particular, Debugger.enable itself may cause Debugger.paused.
    for (const record of selected) {
      try {
        addListener(record, 'Debugger.paused', params => recordEvent(record, 'Debugger.paused', params));
        addListener(record, 'Debugger.resumed', params => recordEvent(record, 'Debugger.resumed', params));
        addListener(record, 'Runtime.exceptionThrown', params =>
          recordEvent(record, 'Runtime.exceptionThrown', params));
      } catch (error) {
        failures.push(`worker[${record.arrayIndex}] event listener: ${String(error?.message || error)}`);
      }
    }
    for (const record of selected) {
      if (!record.listeners.some(([name]) => name === 'Debugger.paused')) continue;
      try {
        await withTimeout(record.client.send('Debugger.enable'), timeoutMs,
          `Debugger.enable worker[${record.arrayIndex}]`);
        record.debuggerEnabled = true;
      } catch (error) {
        failures.push(`Debugger.enable worker[${record.arrayIndex}]: ${String(error?.message || error)}`);
      }
    }
    if (settleMs) await delay(settleMs);
    for (const record of selected) {
      if (!record.debuggerEnabled || record.paused) continue;
      try {
        await withTimeout(record.client.send('Debugger.pause'), timeoutMs,
          `Debugger.pause worker[${record.arrayIndex}]`);
      } catch (error) {
        failures.push(`Debugger.pause worker[${record.arrayIndex}]: ${String(error?.message || error)}`);
      }
    }
    if (settleMs) await delay(settleMs);
  } finally {
    // Resume every selected worker even when no pause event was observed: a
    // pause command can time out after the worker has entered the paused state.
    for (const record of selected) {
      if (!record.client) continue;
      try {
        await withTimeout(record.client.send('Debugger.resume'), cleanupTimeoutMs,
          `Debugger.resume worker[${record.arrayIndex}]`);
      } catch (error) {
        if (/timed out/i.test(String(error?.message || error))) {
          failures.push(`Debugger.resume cleanup worker[${record.arrayIndex}] timed out`);
        }
      }
      try {
        await withTimeout(record.client.send('Debugger.disable'), cleanupTimeoutMs,
          `Debugger.disable worker[${record.arrayIndex}]`);
      } catch (error) {
        if (/timed out/i.test(String(error?.message || error))) {
          failures.push(`Debugger.disable cleanup worker[${record.arrayIndex}] timed out`);
        }
      }
      record.debuggerEnabled = false;
      for (const [eventName, listener] of record.listeners) {
        if (typeof record.client.off === 'function') record.client.off(eventName, listener);
        else if (typeof record.client.removeListener === 'function') {
          record.client.removeListener(eventName, listener);
        }
      }
      record.listeners = [];
    }
  }

  const captures = selected.map(record => ({
    roles: record.roles,
    arrayIndex: record.arrayIndex,
    url: record.url,
    workerName: record.workerName,
    workerIndex: record.workerIndex,
    workerIndexSource: record.workerIndexSource,
    state: record.state,
    pauseCount: record.pauseCount,
    events: record.events,
    eventsDropped: record.eventsDropped
  }));
  const fullCapture = {
    capturedAt: new Date().toISOString(),
    pageUrl,
    browserVersion,
    requested: Number.isInteger(requested) ? { workerIndex: requested } : { workerName: requested },
    workersSeen: records.map(record => ({
      arrayIndex: record.arrayIndex,
      url: record.url,
      workerName: record.workerName,
      workerIndex: record.workerIndex,
      workerIndexSource: record.workerIndexSource,
      state: record.state,
      identityError: record.identityError
    })),
    captures,
    summary: captures.map(capture => summarizeCapture(capture, frameLimit)),
    failures
  };
  await writeJson(options.outputPath, fullCapture);
  return {
    capturedAt: fullCapture.capturedAt,
    pageUrl,
    browserVersion,
    requested: fullCapture.requested,
    workersSeen: fullCapture.workersSeen,
    summary: fullCapture.summary,
    failures,
    fullOutputPath: path.resolve(options.outputPath)
  };
}

const FAILURE_RE = /\b(?:fatal|error|failed|failure|abort(?:ed|ing)?|exception|assert(?:ion)?|uncaught|unable|cannot|[A-Z][A-Za-z0-9]*Error)\b|\bPROXY\s+THROW\b/i;
const SDL_RE = /\bSDL_[A-Za-z0-9_]+\b/i;

function repeatKey(line) {
  return line.trim().replace(/\bguest-import\s+#\d+\b/gi, 'guest-import #<n>');
}
/** Return a bounded excerpt around the first failure/SDL line. If outputPath is
 * supplied, save the unmodified complete input log before summarizing it. */
async function extractEvidence(text, options = {}) {
  if (typeof text !== 'string') throw new TypeError('extractEvidence requires log text');
  if (options.outputPath) {
    await ensureParent(options.outputPath);
    await fsp.writeFile(options.outputPath, text, 'utf8');
  }
  const lines = text.split(/\r?\n/);
  const failureIndex = lines.findIndex(line => FAILURE_RE.test(line));
  const sdlIndex = lines.findIndex(line => SDL_RE.test(line));
  const anchor = failureIndex >= 0 ? failureIndex : sdlIndex;
  const before = finitePositive(options.contextBefore, 5);
  const after = finitePositive(options.contextAfter, 12);
  const maxLines = finitePositive(options.maxLines, 32);
  const maxLineLength = finitePositive(options.maxLineLength, 600);
  let start = anchor < 0 ? 0 : Math.max(0, anchor - before);
  let end = anchor < 0 ? 0 : Math.min(lines.length, anchor + after + 1);
  if (end - start > maxLines) {
    const left = Math.min(before, Math.floor(maxLines / 2));
    start = Math.max(0, anchor - left);
    end = Math.min(lines.length, start + maxLines);
    if (end - start < maxLines) start = Math.max(0, end - maxLines);
  }
  const seen = new Set();
  const excerpt = [];
  let omittedRepeated = 0;
  let omittedLongChars = 0;
  if (anchor >= 0) {
    for (let index = start; index < end; index++) {
      const line = lines[index];
      const key = repeatKey(line);
      if (key && seen.has(key)) {
        omittedRepeated++;
        continue;
      }
      if (key) seen.add(key);
      let bounded = line;
      if (bounded.length > maxLineLength) {
        omittedLongChars += bounded.length - maxLineLength;
        bounded = `${bounded.slice(0, maxLineLength)}…`;
      }
      excerpt.push({ lineNumber: index + 1, text: bounded });
    }
  }
  return {
    totalLines: lines.length,
    firstFailure: failureIndex >= 0 ? {
      lineNumber: failureIndex + 1,
      text: lines[failureIndex].slice(0, maxLineLength)
    } : null,
    firstSDLLine: sdlIndex >= 0 ? { lineNumber: sdlIndex + 1, text: lines[sdlIndex].slice(0, maxLineLength) } : null,
    excerpt,
    omittedRepeated,
    omittedLongChars,
    fullOutputPath: options.outputPath || null
  };
}

async function hashArtifact(filePath) {
  const resolvedPath = path.resolve(filePath);
  const hash = crypto.createHash('sha256');
  let bytes = 0;
  for await (const chunk of createReadStream(resolvedPath)) {
    hash.update(chunk);
    bytes += chunk.length;
  }
  return { path: resolvedPath, sha256: hash.digest('hex'), bytes };
}

function artifactEntries(artifacts) {
  if (Array.isArray(artifacts)) {
    return artifacts.map(item => [item?.name, item?.path]).filter(([name, file]) =>
      typeof name === 'string' && typeof file === 'string');
  }
  if (!artifacts || typeof artifacts !== 'object') return [];
  return Object.entries(artifacts).map(([name, value]) => [
    name,
    typeof value === 'string' ? value : value?.path
  ]).filter(([, file]) => typeof file === 'string');
}

/** Merge a checkpoint update without discarding existing facts/unknown fields,
 * then record named artifact identities as SHA-256 plus byte length. */
async function updateCheckpoint(checkpointPath, update = {}, artifacts = {}) {
  if (!update || typeof update !== 'object' || Array.isArray(update)) {
    throw new TypeError('checkpoint update must be an object');
  }
  let current = {};
  try {
    current = JSON.parse(await fsp.readFile(checkpointPath, 'utf8'));
    if (!current || typeof current !== 'object' || Array.isArray(current)) current = {};
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }
  const identities = { ...(current.artifacts || {}) };
  for (const [name, filePath] of artifactEntries(artifacts)) {
    identities[name] = await hashArtifact(filePath);
  }
  const next = { ...current, ...update, artifacts: identities };
  await writeJson(checkpointPath, next);
  return next;
}

/** Verify checkpoint artifact identities and report only changed/missing names. */
async function validateCheckpoint(checkpointPath) {
  let checkpoint;
  try {
    checkpoint = JSON.parse(await fsp.readFile(checkpointPath, 'utf8'));
  } catch (error) {
    if (error.code === 'ENOENT') {
      return { ok: false, checked: 0, unchanged: [], changed: [], missing: ['checkpoint'] };
    }
    throw error;
  }
  const identities = checkpoint?.artifacts && typeof checkpoint.artifacts === 'object' ? checkpoint.artifacts : {};
  const unchanged = [];
  const changed = [];
  const missing = [];
  for (const [name, identity] of Object.entries(identities)) {
    if (!identity || typeof identity.path !== 'string') {
      changed.push(name);
      continue;
    }
    try {
      const actual = await hashArtifact(identity.path);
      if (actual.sha256 !== identity.sha256 || actual.bytes !== identity.bytes) changed.push(name);
      else unchanged.push(name);
    } catch (error) {
      if (error.code === 'ENOENT' || error.code === 'ENOTDIR') missing.push(name);
      else throw error;
    }
  }
  return {
    ok: changed.length === 0 && missing.length === 0,
    checked: Object.keys(identities).length,
    unchanged,
    changed,
    missing
  };
}

export { captureWorkers, extractEvidence, updateCheckpoint, validateCheckpoint };
