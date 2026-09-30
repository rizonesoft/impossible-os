// Drives the docs template's real inline script against a minimal DOM stub, so
// the search combobox is tested as shipped rather than re-implemented in a test.
// stdin: {"script": "<the template script, %ROOT% already substituted>",
//         "files": {"search.json": "...", "search-<hash>.json": "..."},
//         "cached": {"file name": "what the browser cache serves unless the fetch says cache: no-cache"},
//         "steps": [["input", "text"] | ["key", "ArrowDown"] | ["blur", null] | ["click", <option index>] |
//                   ["flush", null] | ["wait", <ms>] | ["snap", "label"]],
//         "fail_once": ["name of a file whose first fetch fails", ...],
//         "delay": {"file name": <ms before its fetch answers>},
//         "files2": {the files a ["deploy", null] step swaps in},
//         "version": {"version": "<data-version>", "page": "<data-page>"} adds the version picker,
//         "heads": ["full URL a HEAD request finds", ...],
//         "head_delay": {"full URL": <ms before its HEAD answers>}, "head_fail": ["full URL whose HEAD rejects", ...]}
// Version picker steps: ["change", <option index>] picks a version.
// stdout: {"snaps": {"label": {expanded, active, shown, options: [{id, href, selected, text}], msg, status, href, renders,
//                               picker: {hidden, options: [{value, text, selected}]}}},
//          "errors": [every unhandled promise rejection, which the page would swallow silently],
//          "fetches": {"file name": times fetched}, "revalidated": {"file name": times fetched with cache: no-cache}}
'use strict';
const vm = require('vm');

const input = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const errors = [];
process.on('unhandledRejection', (e) => errors.push(String(e && e.stack || e)));
const failOnce = new Set(input.fail_once || []);

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
if (input.version) {
  els.version = new El('version');
  els.version.hidden = true;
  els.version.dataset = input.version;
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
const loc = { href: 'start' };

const fetches = {};
const revalidated = {};
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
  return new Promise((resolve) => setTimeout(() => {
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
  }, (input.delay || {})[name] || 1));
}

const ctx = vm.createContext({
  document: doc, location: loc, fetch: fetchStub, localStorage: { setItem() {} },
  matchMedia: () => ({ matches: false }), setTimeout, Promise, Array, Object, JSON, Math, Error,
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
                                        selected: o.getAttribute('aria-selected'), text: o.text })),
    msg: els['results-msg'].hidden ? '' : els['results-msg'].textContent,
    status: els['search-status'].textContent,
    href: loc.href,
    renders: list.renders,          // non-empty result lists drawn so far: one per scoring pass
    picker: els.version ? { hidden: els.version.hidden,
                            options: (els.version.children || []).map((o) => ({ value: o.value, text: o.textContent,
                                                                                selected: !!o.selected })) } : null,
  };
}

(async () => {
  const snaps = {};
  for (const [op, arg] of input.steps) {
    if (op === 'input') { els.q.value = arg; els.q.fire('input', {}); }
    else if (op === 'key') key(arg);
    else if (op === 'blur') els.q.fire('blur', { relatedTarget: null });
    else if (op === 'click') els.results.fire('click', { target: els.results.options[arg] });
    else if (op === 'deploy') input.files = input.files2;          // the site is redeployed under the page
    else if (op === 'change') { els.version.value = els.version.children[arg].value; els.version.fire('change', {}); }
    else if (op === 'flush') await flush();
    else if (op === 'wait') await new Promise((r) => setTimeout(r, arg));
    else if (op === 'snap') snaps[arg] = snap();
  }
  process.stdout.write(JSON.stringify({ snaps, errors, fetches, revalidated }));
})();
