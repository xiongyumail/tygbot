'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../core/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];

function fixture() {
  const elements = new Map(), requests = [];
  const element = id => {
    if (!elements.has(id)) elements.set(id, {
      className: '', textContent: '', innerHTML: '', hidden: true,
      classList: { contains(name) { return elements.get(id).className === name; } }, focus() {},
      replaceChildren(...nodes) { this.textContent = nodes.map(n => typeof n === 'string' ? n : n.textContent).join(''); }
    });
    return elements.get(id);
  };
  let handlers;
  const context = {
    document: { hidden: false, getElementById: element, querySelectorAll: () => [], addEventListener() {},
      createElement: () => ({ className: '', textContent: '' }) },
    TYGBotStream: class { constructor(options) { handlers = options; } resume() {} pause() {} },
    setInterval() { return 1; }, clearInterval() {}, setTimeout() { return 1; }, clearTimeout() {}, FormData, DataView,
    window: { addEventListener() {}, fetch: async (...args) => {
      requests.push(args);
      const setting = args[0] === '/settings' ? JSON.parse(args[1].body).call_linked_open : null;
      return { status: 200, ok: true,
        json: async () => setting === null ? ({ ok: true, seconds: 2.660329 })
          : ({ ok: true, call_linked_open: setting, detail: setting ? '来电联动开门已开启，下次来电生效' : '来电联动开门已关闭' }),
        text: async () => 'Update Successful!' };
    } }
  };
  context.fetch = (...args) => context.window.fetch(...args);
  vm.runInNewContext(script, context);
  return { context, element, handlers, requests };
}

test('duration rounds to two decimals, including zero and invalid data', () => {
  const f = fixture();
  for (const [value, expected] of [[2.660329, '2.66'], [1, '1.00'], [0, '0.00'],
    [0.009, '0.01'], [null, '—'], [undefined, '—'], ['2.66', '—'], [NaN, '—'], [Infinity, '—'], [-1, '—']]) {
    assert.equal(f.context.formatSeconds(value), expected);
  }
});

test('snapshot and SSE result use formatted duration without changing the raw record', () => {
  const f = fixture();
  const door = { ok: true, at: '2026-10-13 11:31:00', seconds: 2.660329, source: '网页' };
  f.handlers.onSnapshot({ login: { ok: true }, last_door: door });
  assert.match(f.element('last').textContent, /2\.66 秒 · 网页/);
  f.handlers.events.door(door);
  assert.equal(f.element('msg').textContent, '已开门 · 2.66 秒 · 网页');
  assert.equal(door.seconds, 2.660329);
  assert.equal(f.requests.length, 0);
});

test('firmware upload sends only an app image (ota.bin)', async () => {
  const f = fixture();
  const image = magic => { const b = new Uint8Array(0x40); b[0] = 0xe9; new DataView(b.buffer).setUint32(0x20, magic, true); return new Blob([b]); };
  f.element('file').files = [image(0x50)];   // factory.bin: bootloader first
  await f.element('upload').onclick();
  assert.equal(f.element('upMsg').textContent, '请选择 .ota.bin 固件');
  assert.equal(f.requests.length, 0);
  f.element('file').files = [image(0xabcd5432)];
  await f.element('upload').onclick();
  assert.equal(f.requests.length, 1);
  assert.equal(f.requests[0][0], '/update');
  assert.equal(f.element('upMsg').textContent, '升级完成，设备正在重启');
});

test('click response displays two decimals and makes exactly one mocked POST', async () => {
  const f = fixture();
  await f.element('open').onclick();
  assert.equal(f.element('msg').textContent, '已开门 · 2.66 秒');
  assert.equal(f.requests.length, 1);
  assert.equal(f.requests[0][0], '/door?from=web');
  assert.equal(f.requests[0][1].method, 'POST');
});

test('linked-open setting follows snapshots, SSE, and persists one explicit change', async () => {
  const f = fixture(), toggle = f.element('linkedOpen');
  f.handlers.onSnapshot({ call_linked_open: true });
  assert.equal(toggle.checked, true);
  f.handlers.events.settings({ call_linked_open: false });
  assert.equal(toggle.checked, false);
  toggle.checked = true;
  await toggle.onchange({ target: toggle });
  assert.equal(toggle.checked, true);
  assert.equal(toggle.disabled, false);
  assert.equal(f.requests.length, 1);
  assert.equal(f.requests[0][0], '/settings');
  assert.deepEqual(JSON.parse(f.requests[0][1].body), { call_linked_open: true });
  assert.match(f.element('settingMsg').textContent, /下次来电生效/);
});

test('selected names and matched call source replace opaque identifiers', () => {
  const f = fixture();
  f.handlers.onSnapshot({ target: { selected: true, automatic_supported: true, household: '示例小区', door: '一号门' },
    call: { ringing: true, caller: '+860000000', source: '一号门', matched: true } });
  assert.equal(f.element('householdName').textContent, '示例小区');
  assert.equal(f.element('doorName').textContent, '一号门');
  assert.equal(f.element('call').textContent, '一号门来电中');
  assert.doesNotMatch(f.element('call').textContent, /\+860000000/);
  assert.match(f.element('linkedNote').textContent, /成功后结束来电/);
  f.handlers.events.call({ ringing: false, ended_by: 'declined' });
  assert.match(f.element('settingMsg').textContent, /已发送 603/);
});
