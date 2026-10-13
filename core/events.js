/* SPDX-License-Identifier: Apache-2.0
 * Explicit, read-only SSE recovery. Never replays POST /door or login actions.
 */
(function (global) {
  'use strict';
  class TYGBotStream {
    constructor(options) {
      this.options = options;
      this.source = null;
      this.timer = null;
      this.generation = 0;
      this.failures = 0;
      this.lastEventId = '';
      this.paused = false;
    }
    close() {
      this.generation++;
      if (this.timer !== null) global.clearTimeout(this.timer);
      this.timer = null;
      if (this.source) this.source.close();
      this.source = null;
    }
    pause() { this.paused = true; this.close(); }
    resume() {
      // Do not churn an already healthy stream on visibility/online events.
      this.paused = false;
      if (!this.source && this.timer === null) this.connect();
    }
    schedule() {
      if (this.paused || this.timer !== null) return;
      const delay = Math.min(12000, 800 * Math.pow(2, Math.min(this.failures++, 4)))
        + Math.floor(Math.random() * 250);
      this.timer = global.setTimeout(() => { this.timer = null; this.connect(); }, delay);
    }
    connect() {
      if (this.paused) return;
      this.close();
      const generation = this.generation;
      const url = '/events' + (this.lastEventId ? '?last_event_id=' + encodeURIComponent(this.lastEventId) : '');
      let source;
      try { source = new global.EventSource(url); }
      catch (_) { this.options.onDisconnected(); this.schedule(); return; }
      this.source = source;
      const current = () => generation === this.generation && source === this.source && !this.paused;
      const on = (name, callback) => source.addEventListener(name, event => {
        if (!current()) return;
        let value;
        try { value = JSON.parse(event.data); } catch (_) { return; }
        if (!value || typeof value !== 'object' || Array.isArray(value)) return;
        if (event.lastEventId && event.lastEventId.length <= 64) this.lastEventId = event.lastEventId;
        this.failures = 0;
        callback(value);
      });
      on('snapshot', this.options.onSnapshot);
      for (const [name, callback] of Object.entries(this.options.events || {})) on(name, callback);
      source.onerror = async () => {
        if (!current()) return;
        // Close native auto-retry, so exactly one bounded recovery timer owns it.
        this.close();
        const recoveryGeneration = this.generation;
        this.options.onDisconnected();
        this.schedule();
        // A 503 subscription limit is not a dead device. Keep its current state
        // usable via GET while retrying SSE; no action request is ever replayed.
        const controller = new global.AbortController();
        const timeout = global.setTimeout(() => controller.abort(), 3000);
        try {
          const response = await global.fetch('/status', { cache: 'no-store', signal: controller.signal });
          const value = response.ok ? await response.json() : null;
          if (!this.paused && recoveryGeneration === this.generation && value && value.ok)
            this.options.onSnapshot(value);
        } catch (_) { /* Retry timer remains active during a reboot/offline period. */ }
        finally { global.clearTimeout(timeout); }
      };
    }
  }
  global.TYGBotStream = TYGBotStream;
})(typeof window === 'undefined' ? globalThis : window);
