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
      context: event.context || null,
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
 * workerName, timeoutMs, cleanupTimeoutMs, workerStartTimeoutMs, settleMs,
 * maxEvents, summaryFrameLimit, outputPath, pauseOnExceptions ("uncaught" or
 * "all"), trigger (async callback), and waitForPauseMs.
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
  const pauseOnExceptions = options.pauseOnExceptions ?? false;
  if (pauseOnExceptions !== false && pauseOnExceptions !== 'uncaught' && pauseOnExceptions !== 'all') {
    throw new TypeError('pauseOnExceptions must be false, "uncaught", or "all"');
  }
  if (options.trigger !== undefined && typeof options.trigger !== 'function') {
    throw new TypeError('trigger must be an async callback');
  }
  const waitForPauseMs = pauseOnExceptions
    ? Math.min(60000, finitePositive(options.waitForPauseMs, 30000))
    : 0;
  const workerStartTimeoutMs = Math.min(15000, finitePositive(options.workerStartTimeoutMs, 5000));
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
  let workerObjects = await withTimeout(Promise.resolve(page.workers()), timeoutMs, 'page.workers');
  const failures = [];
  let triggerAttempted = false;
  if (!workerObjects.length && typeof options.trigger === 'function') {
    triggerAttempted = true;
    try {
      await withTimeout(Promise.resolve(options.trigger()), timeoutMs, 'debug trigger before worker creation');
    } catch (error) {
      failures.push(`debug trigger: ${String(error?.message || error)}`);
    }
    const deadline = Date.now() + workerStartTimeoutMs;
    while (!workerObjects.length && Date.now() < deadline) {
      await delay(Math.min(50, deadline - Date.now()));
      workerObjects = await withTimeout(Promise.resolve(page.workers()), timeoutMs, 'page.workers after trigger');
    }
  }
  function makeRecord(worker, arrayIndex) {
    return {
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
      runtimeEnabled: false,
      pauseOnExceptionsEnabled: false,
      listeners: []
    };
  }
  const records = workerObjects.map(makeRecord);

  async function inspectRecord(record, identify = true) {
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
      if (!identify) return;
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
            arguments: moduleExists && Array.isArray(Module.arguments)
              ? Module.arguments.slice(0, 32) : null,
            programArgs: typeof programArgs !== 'undefined' && Array.isArray(programArgs)
              ? programArgs.slice(0, 32) : null,
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
  }
  await Promise.all(records.map(record => inspectRecord(record, !pauseOnExceptions)));

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
  if (!pauseOnExceptions && !workerZero.length) failures.push('worker 0 was not found on this page');
  if (!pauseOnExceptions && !requestedRecords.length) {
    failures.push(`requested guest worker ${String(requested)} was not found on this page`);
  }

  const selectedByWorker = new Map();
  const initialSelection = pauseOnExceptions ? records : [...workerZero, ...requestedRecords];
  for (const record of initialSelection) {
    if (workerZero.includes(record)) record.roles.push('worker0');
    if (requestedRecords.includes(record)) record.roles.push('requested');
    if (!record.roles.length) record.roles.push('exception-watch');
    selectedByWorker.set(record.arrayIndex, record);
  }
  const selected = [...selectedByWorker.values()];
  const knownWorkers = new Set(records.map(record => record.worker));

  function addListener(record, eventName, handler) {
    if (typeof record.client.on !== 'function') {
      throw new Error(`worker.client[${record.arrayIndex}] does not expose event listeners`);
    }
    record.client.on(eventName, handler);
    record.listeners.push([eventName, handler]);
  }
  let resolveObservation;
  const observation = new Promise(resolve => { resolveObservation = resolve; });
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
    if (hasFrames && record.events.length < maxEvents) {
      record.events.push({ at: Date.now(), method, params: eventParams });
    } else if (hasFrames) {
      record.eventsDropped++;
    }
    if (method === 'Debugger.paused') {
      resolveObservation({ record, method });
    }
  }
  async function capturePausedContexts() {
    for (const record of selected) {
      for (const event of record.events) {
        if (event.method !== 'Debugger.paused' ||
            !['exception', 'promiseRejection'].includes(event.params?.reason)) continue;
        const frame = event.params.callFrames?.[0];
        if (!frame?.callFrameId) continue;
        try {
          const response = await withTimeout(record.client.send('Debugger.evaluateOnCallFrame', {
            callFrameId: frame.callFrameId,
            expression: `(() => {
              const event = typeof e === 'undefined' ? null : e;
              const reason = event && event.reason;
              const caught = typeof ex === 'undefined' ? null : ex;
              const error = event && (event.error || reason) || caught;
              const limit = value => typeof value === 'string' ? value.slice(0, 8192) : value;
              return {
                eventMessage: event && limit(event.message),
                filename: event && limit(event.filename),
                lineno: event && event.lineno,
                colno: event && event.colno,
                reasonName: reason && limit(reason.name),
                reasonMessage: reason && limit(reason.message),
                reasonStack: reason && limit(reason.stack),
                caughtName: caught && limit(caught.name),
                caughtMessage: caught && limit(caught.message),
                caughtStack: caught && limit(caught.stack),
                errorName: error && limit(error.name),
                errorMessage: error && limit(error.message),
                errorStack: error && limit(error.stack),
                workerID: typeof workerID === 'undefined' ? null : workerID,
                threadName: typeof self === 'undefined' ? null : self.name,
                browserUseWebGL: typeof Browser === 'undefined' ? null : Browser.useWebGL,
                glContextPresent: typeof GLctx !== 'undefined' && Boolean(GLctx),
                webglFramebuffer: (() => {
                  try {
                    const gl = typeof GLctx === 'undefined' ? null : GLctx;
                    if (!gl || typeof gl.checkFramebufferStatus !== 'function') return null;
                    const attachment = point => {
                      try {
                        const type = gl.getFramebufferAttachmentParameter(
                          gl.FRAMEBUFFER, point, gl.FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE);
                        const object = gl.getFramebufferAttachmentParameter(
                          gl.FRAMEBUFFER, point, gl.FRAMEBUFFER_ATTACHMENT_OBJECT_NAME);
                        const result = {
                          type,
                          kind: type === gl.RENDERBUFFER ? 'renderbuffer' :
                            type === gl.TEXTURE ? 'texture' : 'none',
                          objectPresent: object !== null
                        };
                        if (type === gl.RENDERBUFFER && object) {
                          const previous = gl.getParameter(gl.RENDERBUFFER_BINDING);
                          try {
                            gl.bindRenderbuffer(gl.RENDERBUFFER, object);
                            result.storage = {
                              width: gl.getRenderbufferParameter(gl.RENDERBUFFER, gl.RENDERBUFFER_WIDTH),
                              height: gl.getRenderbufferParameter(gl.RENDERBUFFER, gl.RENDERBUFFER_HEIGHT),
                              internalFormat: gl.getRenderbufferParameter(
                                gl.RENDERBUFFER, gl.RENDERBUFFER_INTERNAL_FORMAT),
                              samples: gl.RENDERBUFFER_SAMPLES === undefined ? null :
                                gl.getRenderbufferParameter(gl.RENDERBUFFER, gl.RENDERBUFFER_SAMPLES)
                            };
                          } finally {
                            gl.bindRenderbuffer(gl.RENDERBUFFER, previous);
                          }
                        }
                        return result;
                      } catch (error) {
                        return {error: {name: error.name, message: error.message}};
                      }
                    };
                    const attachments = {};
                    for (const [name, point] of [
                      ['color0', gl.COLOR_ATTACHMENT0],
                      ['depth', gl.DEPTH_ATTACHMENT],
                      ['stencil', gl.STENCIL_ATTACHMENT]
                    ]) attachments[name] = attachment(point);
                    return {
                      status: gl.checkFramebufferStatus(gl.FRAMEBUFFER),
                      maxSamples: gl.MAX_SAMPLES === undefined ? null :
                        gl.getParameter(gl.MAX_SAMPLES),
                      framebufferBound: gl.getParameter(gl.FRAMEBUFFER_BINDING) !== null,
                      drawFramebufferBound: gl.DRAW_FRAMEBUFFER_BINDING === undefined ? null :
                        gl.getParameter(gl.DRAW_FRAMEBUFFER_BINDING) !== null,
                      readFramebufferBound: gl.READ_FRAMEBUFFER_BINDING === undefined ? null :
                        gl.getParameter(gl.READ_FRAMEBUFFER_BINDING) !== null,
                      attachments
                    };
                  } catch (error) {
                    return {error: {name: error.name, message: error.message}};
                  }
                })(),
                glImmediateMaxTextures: typeof GLImmediate === 'undefined' ? null : GLImmediate.MAX_TEXTURES,
                activeTexture: typeof GLImmediate === 'undefined' || !GLImmediate.TexEnvJIT
                  ? null : GLImmediate.TexEnvJIT.getActiveTexture(),
                texUnitState: (() => {
                  try {
                    return GLImmediate.TexEnvJIT.getTexType(GLImmediate.TexEnvJIT.getActiveTexture());
                  } catch (error) {
                    return {name: error.name, message: error.message};
                  }
                })(),
                command: typeof cmd === 'undefined' ? null : cmd,
                messageKeys: typeof msgData === 'object' && msgData ? Object.keys(msgData).slice(0, 16) : [],
                handlerMessage: typeof message === 'undefined' ? null : limit(message)
              };
            })()`,
            returnByValue: true,
            awaitPromise: false
          }), timeoutMs, `Debugger.evaluateOnCallFrame worker[${record.arrayIndex}]`);
          event.context = response?.result?.value ?? null;
        } catch (error) {
          event.contextError = String(error?.message || error);
        }
      }
    }
  }
  async function enableRecord(record) {
    if (!record.client) {
      failures.push(`worker[${record.arrayIndex}] has no usable worker.client CDP session`);
      return;
    }
    try {
      addListener(record, 'Debugger.paused', params => recordEvent(record, 'Debugger.paused', params));
      addListener(record, 'Debugger.resumed', params => recordEvent(record, 'Debugger.resumed', params));
      addListener(record, 'Runtime.exceptionThrown', params =>
        recordEvent(record, 'Runtime.exceptionThrown', params));
    } catch (error) {
      failures.push(`worker[${record.arrayIndex}] event listener: ${String(error?.message || error)}`);
    }
    if (!record.listeners.some(([name]) => name === 'Debugger.paused')) return;
    try {
      await withTimeout(record.client.send('Debugger.enable'), timeoutMs,
        `Debugger.enable worker[${record.arrayIndex}]`);
      record.debuggerEnabled = true;
    } catch (error) {
      failures.push(`Debugger.enable worker[${record.arrayIndex}]: ${String(error?.message || error)}`);
    }
    if (!pauseOnExceptions || !record.debuggerEnabled) return;
    try {
      await withTimeout(record.client.send('Runtime.enable'), timeoutMs,
        `Runtime.enable worker[${record.arrayIndex}]`);
      record.runtimeEnabled = true;
    } catch (error) {
      failures.push(`Runtime.enable worker[${record.arrayIndex}]: ${String(error?.message || error)}`);
    }
    try {
      await withTimeout(record.client.send('Debugger.setPauseOnExceptions', {
        state: pauseOnExceptions
      }), timeoutMs, `Debugger.setPauseOnExceptions worker[${record.arrayIndex}]`);
      record.pauseOnExceptionsEnabled = true;
    } catch (error) {
      failures.push(`Debugger.setPauseOnExceptions worker[${record.arrayIndex}]: ${String(error?.message || error)}`);
    }
  }
  async function discoverNewWorkers() {
    let currentWorkers;
    try {
      currentWorkers = await withTimeout(Promise.resolve(page.workers()), timeoutMs,
        'page.workers while watching exceptions');
    } catch (error) {
      failures.push(`page.workers while watching exceptions: ${String(error?.message || error)}`);
      return;
    }
    for (let arrayIndex = 0; arrayIndex < currentWorkers.length; arrayIndex++) {
      const worker = currentWorkers[arrayIndex];
      if (knownWorkers.has(worker)) continue;
      knownWorkers.add(worker);
      const record = makeRecord(worker, arrayIndex);
      records.push(record);
      await inspectRecord(record, false);
      if (record.workerIndex === null && arrayIndex === 0) {
        record.workerIndex = 0;
        record.workerIndexSource = 'page.workers()[0] (unverified fallback)';
      } else if (record.workerIndex === null && Number.isInteger(requested) && arrayIndex === requested) {
        record.workerIndex = requested;
        record.workerIndexSource = `page.workers()[${requested}] (unverified fallback)`;
      }
      if (record.workerIndex === 0) record.roles.push('worker0');
      if (Number.isInteger(requested) && record.workerIndex === requested) {
        record.roles.push('requested');
      } else if (typeof requested === 'string' &&
          (record.workerName === requested || record.url === requested)) {
        record.roles.push('requested');
      }
      if (!record.roles.length) record.roles.push('exception-watch');
      selected.push(record);
      selectedByWorker.set(record.arrayIndex, record);
      await enableRecord(record);
    }
  }

  try {
    for (const record of selected) await enableRecord(record);
    if (typeof options.trigger === 'function' && !triggerAttempted) {
      triggerAttempted = true;
      try {
        await withTimeout(Promise.resolve(options.trigger()), timeoutMs, 'debug trigger');
      } catch (error) {
        failures.push(`debug trigger: ${String(error?.message || error)}`);
      }
    }
    if (pauseOnExceptions) {
      let observed = false;
      const deadline = Date.now() + waitForPauseMs;
      while (waitForPauseMs && Date.now() < deadline) {
        await discoverNewWorkers();
        if (selected.some(record => record.paused)) {
          observed = true;
          break;
        }
        observed = await Promise.race([
          observation.then(() => true),
          delay(Math.min(50, deadline - Date.now())).then(() => false)
        ]);
        if (observed) break;
      }
      if (!observed) failures.push(`no exception pause observed within ${waitForPauseMs}ms`);
      await capturePausedContexts();
    } else {
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
    }
  } finally {
    for (const record of selected) {
      if (!record.client) continue;
      if (record.pauseOnExceptionsEnabled) {
        try {
          await withTimeout(record.client.send('Debugger.setPauseOnExceptions', { state: 'none' }),
            cleanupTimeoutMs, `Debugger.setPauseOnExceptions cleanup worker[${record.arrayIndex}]`);
        } catch (error) {
          failures.push(`Debugger.setPauseOnExceptions cleanup worker[${record.arrayIndex}]: ${String(error?.message || error)}`);
        }
      }
      try {
        await withTimeout(record.client.send('Debugger.resume'), cleanupTimeoutMs,
          `Debugger.resume worker[${record.arrayIndex}]`);
      } catch (error) {
        if (/timed out/i.test(String(error?.message || error))) {
          failures.push(`Debugger.resume cleanup worker[${record.arrayIndex}] timed out`);
        }
      }
      if (record.runtimeEnabled) {
        try {
          await withTimeout(record.client.send('Runtime.disable'), cleanupTimeoutMs,
            `Runtime.disable worker[${record.arrayIndex}]`);
        } catch (error) {
          failures.push(`Runtime.disable cleanup worker[${record.arrayIndex}]: ${String(error?.message || error)}`);
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
      record.runtimeEnabled = false;
      record.pauseOnExceptionsEnabled = false;
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
    debugMode: {
      pauseOnExceptions: pauseOnExceptions || 'none',
      waitForPauseMs,
      workerStartTimeoutMs
    },
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
    debugMode: fullCapture.debugMode,
    requested: fullCapture.requested,
    workersSeen: fullCapture.workersSeen,
    summary: fullCapture.summary,
    failures,
    fullOutputPath: path.resolve(options.outputPath)
  };
}

const FAILURE_RE = /\b(?:fatal|error(?=\s*[:=])|failed to (?:dlopen|open|load|link)\b|failure|abort(?:ed|ing)?|exception|assert(?:ion)?|uncaught|unable(?: to)?|cannot|(?:Eval|Range|Reference|Syntax|Type|URI|NotSupported|Security|Runtime|DataClone|Abort|InvalidState|Network|NotAllowed|QuotaExceeded|IndexSize|Operation)Error)\b/i;
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
