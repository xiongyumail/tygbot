'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../core/events.js'), 'utf8');

function fixture(fetchImpl = async () => ({ ok: true, json: async () => ({ ok: true, busy: false }) })) {
  const instances = [], timers = new Map(), fetched = [], snapshots = [], doors = [];
  let timerId = 0, disconnected = 0;
  class EventSource {
    constructor(url) { this.url = url; this.handlers = {}; this.closed = false; instances.push(this); }
    addEventListener(name, callback) { this.handlers[name] = callback; }
    close() { this.closed = true; }
    emit(name, value, lastEventId = '') {
      this.handlers[name]?.({ data: JSON.stringify(value), lastEventId });
    }
  }
  const context = {
    EventSource, AbortController,
    Math: { random: () => 0, min: Math.min, pow: Math.pow, floor: Math.floor },
    setTimeout(fn, delay) { const id = ++timerId; timers.set(id, { fn, delay }); return id; },
    clearTimeout(id) { timers.delete(id); },
    fetch: async (...args) => { fetched.push(args); return fetchImpl(...args); }
  };
  vm.runInNewContext(source, context);
  const stream = new context.TYGBotStream({
    onSnapshot: value => snapshots.push(value),
    onDisconnected: () => disconnected++,
    events: { door: value => doors.push(value) }
  });
  const flush = async () => { for (let n = 0; n < 5; n++) await Promise.resolve(); };
  const nextTimer = () => {
    const [id, timer] = [...timers].sort((a, b) => a[1].delay - b[1].delay)[0];
    timers.delete(id); timer.fn(); return timer.delay;
  };
  return { stream, instances, timers, fetched, snapshots, doors, flush, nextTimer, get disconnected() { return disconnected; } };
}

test('healthy stream resumes idempotently without churning connections', () => {
  const f = fixture(); f.stream.resume(); f.stream.resume();
  assert.equal(f.instances.length, 1);
  f.instances[0].emit('snapshot', { ok: true, login: { ok: true } });
  assert.equal(f.snapshots.length, 1);
});

test('503/CLOSED retry is explicit and GET status fallback remains usable', async () => {
  const f = fixture(); f.stream.resume(); const old = f.instances[0];
  await old.onerror(); await f.flush();
  assert.ok(old.closed); assert.equal(f.disconnected, 1);
  assert.equal(f.snapshots.length, 1); assert.equal(f.timers.size, 1);
  assert.equal(f.nextTimer(), 800); assert.equal(f.instances.length, 2);
  assert.equal(f.fetched[0][0], '/status');
  assert.ok(f.fetched.every(([url, options]) => url === '/status' && !options.method));
});

test('offline status failure still retries with bounded exponential backoff', async () => {
  const f = fixture(async () => { throw new Error('offline'); }); f.stream.resume();
  const delays = [];
  for (let i = 0; i < 8; i++) {
    await f.instances.at(-1).onerror(); await f.flush();
    delays.push(f.nextTimer());
  }
  assert.deepEqual(delays, [800, 1600, 3200, 6400, 12000, 12000, 12000, 12000]);
});

test('one valid snapshot resets backoff after service recovery', async () => {
  const f = fixture(); f.stream.resume();
  await f.instances.at(-1).onerror(); f.nextTimer();
  await f.instances.at(-1).onerror(); f.nextTimer();
  f.instances.at(-1).emit('snapshot', { ok: true });
  await f.instances.at(-1).onerror(); assert.equal(f.nextTimer(), 800);
});

test('pause cancels retries and ignores late events from a closed connection', async () => {
  const f = fixture(); f.stream.resume(); const old = f.instances[0];
  await old.onerror(); f.stream.pause();
  assert.equal(f.timers.size, 0);
  old.emit('door', { ok: true }, 'deadbeef-7');
  assert.equal(f.doors.length, 0);
  f.stream.resume(); assert.equal(f.instances.length, 2);
});

test('in-flight status response cannot revive a hidden/stale page', async () => {
  let resolve;
  const pending = new Promise(r => { resolve = r; });
  const f = fixture(() => pending); f.stream.resume();
  const error = f.instances[0].onerror(); f.stream.pause();
  resolve({ ok: true, json: async () => ({ ok: true }) }); await error;
  assert.equal(f.snapshots.length, 0); assert.equal(f.timers.size, 0);
});

test('recreated stream preserves last event cursor and does not replay an action', async () => {
  const f = fixture(); f.stream.resume();
  f.instances[0].emit('door', { ok: true }, 'deadbeef-12');
  await f.instances[0].onerror(); f.nextTimer();
  assert.equal(f.instances[1].url, '/events?last_event_id=deadbeef-12');
  assert.equal(f.doors.length, 1);
  assert.ok(f.fetched.every(([url]) => url === '/status'));
});

test('malformed SSE payload is ignored without poisoning recovery state', () => {
  const f = fixture(); f.stream.resume(); const s = f.instances[0];
  s.handlers.snapshot({ data: '{broken', lastEventId: '' });
  s.emit('snapshot', null); s.emit('snapshot', []);
  assert.equal(f.snapshots.length, 0);
});

test('a synchronously failed EventSource constructor is retried', () => {
  const timers = [];
  const c = { EventSource: class { constructor() { throw new Error('fail'); } },
    Math, setTimeout: (fn, delay) => { timers.push({ fn, delay }); return 1; }, clearTimeout() {} };
  vm.runInNewContext(source, c);
  let disconnected = 0;
  new c.TYGBotStream({ onSnapshot() {}, onDisconnected() { disconnected++; } }).resume();
  assert.equal(disconnected, 1); assert.equal(timers.length, 1);
});

test('status fallback is aborted after its timeout without suppressing SSE retry', async () => {
  const f = fixture((_url, options) => new Promise((resolve, reject) => {
    options.signal.addEventListener('abort', () => reject(new Error('timeout')));
  }));
  f.stream.resume();
  const pending = f.instances[0].onerror();
  const timeoutEntry = [...f.timers].find(([, timer]) => timer.delay === 3000);
  assert.ok(timeoutEntry);
  f.timers.delete(timeoutEntry[0]); timeoutEntry[1].fn();
  await pending;
  assert.equal(f.timers.size, 1); assert.equal(f.nextTimer(), 800);
  assert.equal(f.instances.length, 2);
});
