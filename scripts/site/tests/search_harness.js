// Drives the docs template's real inline script against a minimal DOM stub, so
// the search combobox is tested as shipped rather than re-implemented in a test.
// stdin: {"script": "<the template script, %ROOT% already substituted>",
//         "files": {"search.json": "...", "search-<hash>.json": "..."},
//         "cached": {"file name": "what the browser cache serves unless the fetch says cache: no-cache"},
//         "steps": [["input", "text"] | ["key", "ArrowDown"] | ["blur", null] | ["click", <option index>] |
//                   ["flush", null] | ["wait", <ms>] | ["snap", "label"]],
//         "fail_once": ["name of a file whose first fetch fails", ...],
//         "stall_once": ["name of a file whose first fetch never answers", ...],
//         "delay": {"file name": <ms before its fetch answers>},
//         "files2": {the files a ["deploy", null] step swaps in},
//         "version": {"version": "<data-version>", "page": "<data-page>"} adds the version picker,
//         "heads": ["full URL a HEAD request finds", ...],
//         "head_delay": {"full URL": <ms before its HEAD answers>}, "head_fail": ["full URL whose HEAD rejects", ...],
//         "attrs": {"q": {"data-search-page": "search.html"}} sets attributes before the script runs,
//         "page": {"search": "?q=..."} makes this the search results page, opened at that query string,
//         "deadline_ms": <ms> stands in for the page's 15 s shard deadline}
// Results page step: ["more", null] presses its "Show more results" button.
// Version picker steps: ["change", <option index>] picks a version; ["note-click", null] follows the
// old-version notice's link.
// stdout: {"snaps": {"label": {expanded, active, shown, options: [{id, href, selected, text, html}], msg, status, href, renders,
//                               page: the results page's HTML, pageRenders: times it was drawn,
//                               url: its last history.replaceState URL, value: the box's text,
//                               picker: {hidden, options: [{value, text, selected}]}}},
//          "errors": [every unhandled promise rejection, which the page would swallow silently],
//          "fetches": {"file name": times fetched}, "revalidated": {"file name": times fetched with cache: no-cache},
//          "aborted": {"file name": times the page aborted a request for it}}
'use strict';
const vm = require('vm');

const input = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const errors = [];
process.on('unhandledRejection', (e) => errors.push(String(e && e.stack || e)));
const failOnce = new Set(input.fail_once || []);
const stallOnce = new Set(input.stall_once || []);

function unescape(s) {
  return s.replace(/&quot;/g, '"').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
}

class El {
  constructor(id) { this.id = id; this.attrs = {}; this.style = {}; this.hidden = false; this.textContent = '';
                    this.handlers = {}; this._html = ''; this.options = []; this.value = ''; this.tagName = 'DIV';
                    this.renders = 0; }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
  removeAttribute(k) { delete this.attrs[k]; }
  addEventListener(t, f) { (this.handlers[t] = this.handlers[t] || []).push(f); }
  fire(t, e) { (this.handlers[t] || []).forEach((f) => f(e)); }
  focus() { doc.activeElement = this; }
  closest(sel) {
    if (sel === '.search') return this.inSearch ? this : null;
    return sel === '[role=option]' && this.attrs.role === 'option' ? this : null;
  }
  set innerHTML(h) {
    this._html = h;
    if (h) this.renders++;
    this.options = [];
    const re = /<a role="option" id="([^"]+)"[^>]*href="([^"]*)">(.*?)<\/a>/g;
    let m;
    while ((m = re.exec(h))) {
      const o = new El(m[1]);
      o.inSearch = true;
      o.attrs = { role: 'option', 'aria-selected': 'false', href: unescape(m[2]) };
      o.text = unescape(m[3].replace(/<[^>]+>/g, ' '));
      o.html = m[3];
      this.options.push(o);
    }
  }
  get innerHTML() { return this._html; }
  querySelectorAll(sel) { return sel === '[role=option]' ? this.options : []; }
  appendChild(c) { this.children = (this.children || []).concat([c]); }
}

const els = {};
['theme', 'q', 'results-pop', 'results', 'results-msg', 'search-status'].forEach((id) => {
  els[id] = new El(id);
  els[id].inSearch = id !== 'theme';
});
els.q.tagName = 'INPUT';
Object.entries((input.attrs || {}).q || {}).forEach(([k, v]) => els.q.setAttribute(k, v));
if (input.page) els['search-page'] = new El('search-page');
if (input.version) {
  els.version = new El('version');
  els.version.hidden = true;
  els.version.dataset = input.version;
  els['version-note'] = new El('version-note');
  els['version-note'].hidden = true;
}
const docHandlers = {};
const doc = {
  documentElement: { dataset: {} },
  activeElement: null,
  getElementById: (id) => els[id] || null,
  addEventListener: (t, f) => { (docHandlers[t] = docHandlers[t] || []).push(f); },
  querySelector: () => null,
  createElement: () => new El('x'),
  head: { appendChild() {} },
};
doc.activeElement = { tagName: 'BODY' };
const loc = { href: 'start', search: input.page ? input.page.search : undefined, pathname: '/docs/search.html' };
const hist = { url: null, replaceState(state, title, url) { this.url = url; } };

const fetches = {};
const revalidated = {};
const aborted = {};
function fetchStub(url, opts) {
  if (opts && opts.method === 'HEAD') {
    const found = (input.heads || []).includes(url);
    const fail = (input.head_fail || []).includes(url);
    return new Promise((resolve, reject) => setTimeout(() => (fail ? reject(new Error('network'))
      : resolve({ ok: found, status: found ? 200 : 404 })), (input.head_delay || {})[url] || 1));
  }
  const name = url.replace(/^.*\//, '');
  fetches[name] = (fetches[name] || 0) + 1;
  if (opts && opts.cache === 'no-cache') revalidated[name] = (revalidated[name] || 0) + 1;
  // An abort rejects a request still waiting and is counted whenever it comes,
  // including after an error status arrived (its body is then never read).
  let rejectIt = null;
  if (opts && opts.signal) {
    opts.signal.addEventListener('abort', () => {
      aborted[name] = (aborted[name] || 0) + 1;
      if (rejectIt) rejectIt(new Error('AbortError'));
    });
  }
  if (stallOnce.has(name)) { stallOnce.delete(name); return new Promise((_, reject) => { rejectIt = reject; }); }   // never answers
  return new Promise((resolve, reject) => { rejectIt = reject; later(() => {
    if (failOnce.has(name)) {
      failOnce.delete(name);
      resolve({ ok: false, status: 503, json: () => Promise.reject(new Error('503')) });
    } else if (name in (input.cached || {}) && !(opts && opts.cache === 'no-cache')) {
      resolve({ ok: true, status: 200, json: () => Promise.resolve(JSON.parse(input.cached[name])) });
    } else if (!(name in input.files)) {
      resolve({ ok: false, status: 404, json: () => Promise.reject(new Error('404')) });
    } else {
      resolve({ ok: true, status: 200, json: () => Promise.resolve(JSON.parse(input.files[name])) });
    }
  }, (input.delay || {})[name] || 1); });
}
// A long timer (a stalled fetch, the page's shard deadline) never keeps the
// harness alive; "deadline_ms" shortens the page's own long timers so a test can
// reach the deadline without waiting 15 s.
function later(f, ms) {
  const t = setTimeout(f, ms);
  if (ms >= 10000) t.unref();
  return t;
}
function pageTimeout(f, ms) { return later(f, ms >= 10000 && input.deadline_ms ? input.deadline_ms : ms); }

const ctx = vm.createContext({
  document: doc, location: loc, history: hist, fetch: fetchStub, RegExp, localStorage: { setItem() {} },
  matchMedia: () => ({ matches: false }), setTimeout: pageTimeout, clearTimeout, Promise, AbortController, Array, Object, JSON, Math, Error,
});
vm.runInContext(input.script, ctx);

function key(k) {
  const e = { key: k, defaultPrevented: false, preventDefault() { this.defaultPrevented = true; } };
  els.q.fire('keydown', e);
}
function flush() { return new Promise((r) => setTimeout(r, 60)); }   // past the 16 ms refresh coalescing
function snap() {
  const list = els.results;
  return {
    expanded: els.q.getAttribute('aria-expanded'),
    active: els.q.getAttribute('aria-activedescendant'),
    shown: els['results-pop'].style.display === 'block',
    options: list.options.map((o) => ({ id: o.id, href: o.getAttribute('href'),
                                        selected: o.getAttribute('aria-selected'), text: o.text, html: o.html })),
    page: els['search-page'] ? els['search-page'].innerHTML : null,
    pageRenders: els['search-page'] ? els['search-page'].renders : null,
    url: hist.url,
    value: els.q.value,
    msg: els['results-msg'].hidden ? '' : els['results-msg'].textContent,
    status: els['search-status'].textContent,
    href: loc.href,
    renders: list.renders,          // non-empty result lists drawn so far: one per scoring pass
    picker: els.version ? { hidden: els.version.hidden,
                            options: (els.version.children || []).map((o) => ({ value: o.value, text: o.textContent,
                                                                                selected: !!o.selected })) } : null,
    note: els['version-note'] ? { hidden: els['version-note'].hidden,
                                  text: (els['version-note'].children || []).map((c) => c.textContent).join(''),
                                  href: ((els['version-note'].children || [])[1] || { attrs: {} }).attrs.href || null }
                              : null,
  };
}

(async () => {
  const snaps = {};
  for (const [op, arg] of input.steps) {
    if (op === 'input') { els.q.value = arg; els.q.fire('input', {}); }
    else if (op === 'key') key(arg);
    else if (op === 'blur') els.q.fire('blur', { relatedTarget: null });
    else if (op === 'click') els.results.fire('click', { target: els.results.options[arg] });
    else if (op === 'more') els['search-page'].fire('click', { target: { id: 'search-more' } });
    else if (op === 'deploy') input.files = input.files2;          // the site is redeployed under the page
    else if (op === 'change') { els.version.value = els.version.children[arg].value; els.version.fire('change', {}); }
    else if (op === 'note-click') {
      const e = { defaultPrevented: false, preventDefault() { this.defaultPrevented = true; } };
      els['version-note'].children[1].fire('click', e);
    }
    else if (op === 'flush') await flush();
    else if (op === 'wait') await new Promise((r) => setTimeout(r, arg));
    else if (op === 'snap') snaps[arg] = snap();
  }
  process.stdout.write(JSON.stringify({ snaps, errors, fetches, revalidated, aborted }));
})();
