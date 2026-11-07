// Automated accessibility audit of the built site (see docs/infrastructure/docs-search-and-accessibility.md).
//
//   node scripts/site/a11y/audit.mjs <site dir>             audit every page
//   node scripts/site/a11y/audit.mjs <site dir> control     prove the audit fails on planted defects
//
// <site dir> is the output of `python3 scripts/site/build.py --out <dir>`. Every
// HTML page (docs and landing pages) is loaded in Chromium in each of VARIANTS and
// checked by axe-core (WCAG 2.2 A and AA plus its best-practice rules) and by
// checks axe does not make:
//   skip-link      the first Tab lands on a visible skip link whose target exists
//   focus-visible  every element Tab can reach shows a focus indicator, judged from
//                  rendered pixels, on screen and not covered by another element
//                  (a closed drawer must be inert)
//   aria-current   docs pages only: the navigation marks exactly the current page,
//                  and none on a page it does not list (the search results page)
//   motion         under prefers-reduced-motion: nothing moves
// The search surfaces are audited in their populated states too: the expanded
// combobox, the results page and a page opened from a result with its match marked.
// Requests leave only for the local server, so fonts and the diagram library
// never decide a verdict. Exit 0 = no findings; 1 = findings; 2 = the audit
// itself could not run.
import { createServer } from 'node:http';
import { readFileSync, readdirSync, statSync, existsSync } from 'node:fs';
import { join, extname, resolve, sep } from 'node:path';
import { inflateSync } from 'node:zlib';
import { createRequire } from 'node:module';
import { chromium } from 'playwright-core';

const require = createRequire(import.meta.url);
const AXE = readFileSync(require.resolve('axe-core/axe.min.js'), 'utf8');
const TAGS = ['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'wcag22aa', 'best-practice'];
const DESKTOP = ['desktop', 1280, 900], PHONE = ['phone', 375, 812];
// Forced colours paint from the system palette, whichever theme asked for it, so one
// dark and one light palette at the two widths cover it; reduced motion changes no
// colours, so one variant is enough. Each extra full variant costs about 75 seconds.
const VARIANTS = [{ vp: DESKTOP, theme: 'light' }, { vp: DESKTOP, theme: 'dark' },
                  { vp: PHONE, theme: 'light' }, { vp: PHONE, theme: 'dark' },
                  { vp: DESKTOP, theme: 'dark', mode: 'forced' }, { vp: PHONE, theme: 'light', mode: 'forced' },
                  { vp: DESKTOP, theme: 'light', mode: 'reduced' }];
const vname = (v) => `${v.vp[0]} ${v.theme}${v.mode ? ' ' + v.mode : ''}`;
const JOBS = Number(process.env.A11Y_JOBS || 6);
const PAGE_TIMEOUT = 30000;
const TYPES = { '.html': 'text/html; charset=utf-8', '.json': 'application/json', '.svg': 'image/svg+xml',
                '.png': 'image/png', '.jpg': 'image/jpeg', '.css': 'text/css', '.js': 'text/javascript',
                '.xml': 'application/xml', '.txt': 'text/plain' };

function serve(root, overlay = {}) {
  const base = resolve(root);
  const server = createServer((req, res) => {
    let path = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    if (path.endsWith('/')) path += 'index.html';
    if (overlay[path] !== undefined) {
      res.writeHead(200, { 'content-type': TYPES['.html'] });
      return res.end(overlay[path]);
    }
    const file = resolve(join(base, path));
    if ((file !== base && !file.startsWith(base + sep)) || !existsSync(file) || !statSync(file).isFile()) {
      res.writeHead(404);
      return res.end('not found');
    }
    res.writeHead(200, { 'content-type': TYPES[extname(file)] || 'application/octet-stream' });
    res.end(readFileSync(file));
  });
  return new Promise((ok) => server.listen(0, '127.0.0.1', () => ok(server)));
}

// A11Y_ONLY=<text> audits only the pages whose path contains it (for a quick re-check).
function walkPages(dir, rel, skip) {
  const out = [];
  (function walk(d, r) {
    for (const name of readdirSync(d).sort()) {
      const full = join(d, name);
      if (statSync(full).isDirectory()) { if (!skip.includes(`${r}${name}`)) walk(full, `${r}${name}/`); }
      else if (name.endsWith('.html')) out.push(`${rel}${r}${name}`);
    }
  })(dir, '');
  return process.env.A11Y_ONLY ? out.filter((u) => u.includes(process.env.A11Y_ONLY)) : out;
}
const docsPages = (root) => walkPages(join(root, 'docs'), '/docs/', []);
// Everything outside docs/: the home page, 404, the design mockups and the error pages.
const landingPages = (root) => walkPages(root, '/', ['docs']);

// Runs in the page: every element Tab can reach (inside `scope`, a selector, when
// given), focused in turn. Reports what geometry can decide (off screen, covered)
// and gives each element a signature: elements that share one render their focus
// change alike, so the pixels are judged once per signature (focusFindings).
// The signature carries everything that decides those pixels: the element's look
// and its pseudo-elements' before and after focus, what is behind it, which edges
// of its indicator region an overflow ancestor or the viewport clips, and its
// shorter side, since the pass mark scales with it and a fixed-size indicator
// may pass on one control and fail on another a pixel larger.
function focusWalk(scope) {
  const out = [], items = [], els = [];
  const EDGES = ['Top', 'Right', 'Bottom', 'Left'];
  const look = (el) => {
    const c = getComputedStyle(el);
    const pseudo = (p) => { const k = getComputedStyle(el, p);
      return k.content === 'none' ? '' : [k.content, k.display, k.visibility, k.position, k.top, k.right, k.bottom, k.left, k.width, k.height, k.backgroundColor, k.outline, k.boxShadow, k.borderColor, k.transform, k.opacity].join(' '); };
    return [c.outlineStyle, c.outlineWidth, c.outlineColor, c.outlineOffset, c.boxShadow, c.backgroundColor, c.color,
            c.textDecorationLine, c.textDecorationStyle, c.textDecorationColor, c.textDecorationThickness,
            ...EDGES.map((e) => [c[`border${e}Style`], c[`border${e}Width`], c[`border${e}Color`]].join(' ')),
            pseudo('::before'), pseudo('::after')].join('|');
  };
  // What sits behind an element: its ancestors' backgrounds down to the first opaque one.
  const backdrop = (el) => {
    const layers = [];
    for (let n = el.parentElement; n; n = n.parentElement) {
      const c = getComputedStyle(n).backgroundColor;
      if (c === 'transparent' || /^rgba\(.*,\s*0\)$/.test(c)) continue;
      layers.push(c);
      if (!/^rgba\(|\/ /.test(c)) break;
    }
    return layers.join('>');
  };
  // Which edges of the region a ring occupies (4px around the box) something clips.
  const clipped = (el, r) => {
    const R = 4, hit = [false, false, false, false];
    const test = (t, rt, b, l) => {
      if (r.top - R < t) hit[0] = true;
      if (r.right + R > rt) hit[1] = true;
      if (r.bottom + R > b) hit[2] = true;
      if (r.left - R < l) hit[3] = true;
    };
    test(0, innerWidth, innerHeight, 0);
    for (let a = el.parentElement; a; a = a.parentElement) {
      const o = getComputedStyle(a);
      if (o.overflowX === 'visible' && o.overflowY === 'visible') continue;
      const c = a.getBoundingClientRect();
      test(c.top, c.right, c.bottom, c.left);
    }
    return hit.map(Number).join('');
  };
  const sel = 'a[href], button, input, select, textarea, summary, [tabindex]:not([tabindex="-1"])';
  const describe = (el) => el.tagName.toLowerCase() + (el.id ? '#' + el.id : '') +
    (el.getAttribute('href') ? `[href="${el.getAttribute('href')}"]` : '') +
    (el.textContent.trim() ? ` "${el.textContent.trim().slice(0, 40)}"` : '');
  // Smooth scrolling is judged by the motion check; here it would leave each focused
  // element in flight, read as off screen and photographed mid-move.
  document.documentElement.style.setProperty('scroll-behavior', 'auto', 'important');
  // Start with nothing focused, so each element's unfocused look is its real one
  // (the skip-link check has just tabbed to the first element).
  if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
  for (const el of (scope ? document.querySelector(scope) : document).querySelectorAll(sel)) {
    if (el.disabled || el.closest('[hidden]') || getComputedStyle(el).visibility === 'hidden') continue;
    if (!el.getClientRects().length) continue;               // display: none, not in the tab order
    const before = look(el);
    el.focus();
    if (document.activeElement !== el) continue;              // not focusable (inert), so not reachable
    const after = look(el);
    const r = el.getBoundingClientRect();
    const onScreen = r.width > 0 && r.height > 0 && r.right > 0 && r.bottom > 0 &&
      r.left < innerWidth && r.top < innerHeight;
    const parent = el.parentElement;
    const sig = [el.tagName, el.className, el.type || '', parent ? parent.tagName + '.' + parent.className : '',
                 before, after, backdrop(el), clipped(el, r), Math.round(Math.min(r.width, r.height))].join('\n');
    let usable = onScreen;
    if (!onScreen) out.push(`focus-visible: focus lands off screen on ${describe(el)}`);
    else {
      // Covered: no line box of the element (a link can wrap) shows it at its centre.
      let top = null;
      const shown = [...el.getClientRects()].some((b) => {
        const x = Math.min(Math.max(b.left + b.width / 2, 0), innerWidth - 1);
        const y = Math.min(Math.max(b.top + b.height / 2, 0), innerHeight - 1);
        top = document.elementFromPoint(x, y);
        // An ancestor under the point proves nothing on its own: a link clipped by a
        // scroll box has only the page behind it, and the page contains the link.
        for (let a = el.parentElement; a; a = a.parentElement) {
          const o = getComputedStyle(a);
          if (o.overflowX === 'visible' && o.overflowY === 'visible') continue;
          const c = a.getBoundingClientRect();
          if (x < c.left || x > c.right || y < c.top || y > c.bottom) { top = a; return false; }
        }
        return !top || top === el || el.contains(top) || top.contains(el);
      });
      if (!shown) { out.push(`focus-visible: ${describe(el)} is covered by ${describe(top)} when focused`); usable = false; }
    }
    els.push(el);
    items.push({ sig, desc: describe(el), usable });
  }
  if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
  window.__a11yFocus = els;
  return { out, items };
}

// The pixels of a PNG screenshot as {w, h, px: RGBA bytes}. Chromium writes 8-bit
// RGB or RGBA, not interlaced; anything else stops the audit rather than misread.
function decodePng(buf) {
  let at = 8, w = 0, h = 0, n = 0;
  const idat = [];
  while (at < buf.length) {
    const len = buf.readUInt32BE(at), type = buf.toString('latin1', at + 4, at + 8), d = at + 8;
    if (type === 'IHDR') {
      w = buf.readUInt32BE(d); h = buf.readUInt32BE(d + 4);
      const depth = buf[d + 8], color = buf[d + 9], lace = buf[d + 12];
      n = color === 6 ? 4 : color === 2 ? 3 : 0;
      if (depth !== 8 || !n || lace) throw new Error(`unexpected PNG format (depth ${depth}, colour ${color}, interlace ${lace})`);
    } else if (type === 'IDAT') idat.push(buf.subarray(d, d + len));
    at = d + len + 4;
  }
  const raw = inflateSync(Buffer.concat(idat)), stride = w * n, px = Buffer.alloc(w * h * 4);
  let prev = Buffer.alloc(stride), cur = Buffer.alloc(stride);
  for (let y = 0, o = 0; y < h; y++) {
    const f = raw[o++];
    for (let i = 0; i < stride; i++, o++) {
      const left = i >= n ? cur[i - n] : 0, up = prev[i], ul = i >= n ? prev[i - n] : 0;
      let p = 0;
      if (f === 1) p = left;
      else if (f === 2) p = up;
      else if (f === 3) p = (left + up) >> 1;
      else if (f === 4) {
        const e = left + up - ul, a = Math.abs(e - left), b = Math.abs(e - up), c = Math.abs(e - ul);
        p = a <= b && a <= c ? left : b <= c ? up : ul;
      } else if (f !== 0) throw new Error(`unexpected PNG filter ${f}`);
      cur[i] = (raw[o] + p) & 255;
    }
    for (let x = 0; x < w; x++) {
      const q = (y * w + x) * 4;
      px[q] = cur[x * n]; px[q + 1] = cur[x * n + 1]; px[q + 2] = cur[x * n + 2]; px[q + 3] = n === 4 ? cur[x * n + 3] : 255;
    }
    [prev, cur] = [cur, prev];
  }
  return { w, h, px };
}

const lumOf = (v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; };
const LUM = Array.from({ length: 256 }, (_, i) => lumOf(i));
// How much focus changed: the pixels whose colour moved by at least 3:1 (WCAG 1.4.11's
// ratio, measured between the two states as 2.4.13 does), and the longer side of
// the box around them.
function focusChange(a, b) {
  if (a.w !== b.w || a.h !== b.h) throw new Error('screenshots differ in size');
  let area = 0, x0 = a.w, x1 = -1, y0 = a.h, y1 = -1;
  for (let y = 0, q = 0; y < a.h; y++) {
    for (let x = 0; x < a.w; x++, q += 4) {
      if (a.px[q] === b.px[q] && a.px[q + 1] === b.px[q + 1] && a.px[q + 2] === b.px[q + 2]) continue;
      const l1 = 0.2126 * LUM[a.px[q]] + 0.7152 * LUM[a.px[q + 1]] + 0.0722 * LUM[a.px[q + 2]];
      const l2 = 0.2126 * LUM[b.px[q]] + 0.7152 * LUM[b.px[q + 1]] + 0.0722 * LUM[b.px[q + 2]];
      if ((Math.max(l1, l2) + 0.05) / (Math.min(l1, l2) + 0.05) < 3) continue;
      area++;
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
    }
  }
  return { area, span: area ? Math.max(x1 - x0, y1 - y0) + 1 : 0 };
}

// One element's focus change, from two screenshots of the region around it. Passes
// when the changed pixels are at least as much as a 2px line along the box's shorter
// side, and reach that far: a full ring, an underline or a single edge of the box
// pass, a speck does not. Those are this audit's rule for "a visible indicator", not
// the 2-pixel perimeter of WCAG 2.4.13 (AAA), which rejects a one-edge indicator.
const PAD = 8;
async function measureFocus(page, i) {
  for (let attempt = 0; attempt < 3; attempt++) {
    const first = await page.evaluate((k) => {
      const el = window.__a11yFocus[k];
      el.focus();
      const r = el.getBoundingClientRect();
      return { visible: el.matches(':focus-visible'), active: document.activeElement === el,
               rect: [r.left, r.top, r.right, r.bottom], scroll: [scrollX, scrollY], vw: innerWidth, vh: innerHeight };
    }, i);
    if (!first.active) return { err: 'it did not take focus' };
    if (!first.visible) return { err: 'focus did not match :focus-visible' };
    const [l, t, r, b] = first.rect;
    const x = Math.max(0, Math.floor(l - PAD)), y = Math.max(0, Math.floor(t - PAD));
    const clip = { x, y, width: Math.min(first.vw, Math.ceil(r + PAD)) - x, height: Math.min(first.vh, Math.ceil(b + PAD)) - y };
    if (clip.width <= 0 || clip.height <= 0) return { err: 'it has no visible region' };
    // Two frames, so the state just set has painted; a bound, so a stalled page cannot hang here.
    const shot = async () => {
      await page.evaluate(() => new Promise((ok) => {
        setTimeout(ok, 250);
        requestAnimationFrame(() => requestAnimationFrame(ok));
      }));
      return page.screenshot({ clip, animations: 'disabled', type: 'png' });
    };
    await page.evaluate((k) => window.__a11yFocus[k].blur(), i);
    const off = await shot();
    const again = await page.evaluate((k) => {
      const el = window.__a11yFocus[k];
      el.focus({ preventScroll: true });
      const q = el.getBoundingClientRect();
      return { rect: [q.left, q.top, q.right, q.bottom], scroll: [scrollX, scrollY] };
    }, i);
    const on = await shot();
    await page.evaluate((k) => window.__a11yFocus[k].blur(), i);
    // Unfocused again it must look as it did: anything else that changed in between (a late
    // repaint, content still loading) would otherwise pass as a focus indicator. Judged at the
    // same 3:1, since rasterising the same state twice differs by a few bytes (measured: 5 to 112).
    const offAgain = await shot();
    // A page that moved between the two pictures compares two different places.
    const before = decodePng(off);
    if (again.scroll.join() !== first.scroll.join() || again.rect.join() !== first.rect.join() ||
        focusChange(before, decodePng(offAgain)).area > 0) {
      await page.waitForTimeout(100);
      continue;
    }
    const short = Math.min(Math.min(r, first.vw) - Math.max(l, 0), Math.min(b, first.vh) - Math.max(t, 0));
    const { area, span } = focusChange(before, decodePng(on));
    return { ok: area >= 2 * short && span >= short, why: `${area}px changed by 3:1, spanning ${span}px, on a box ${Math.round(short)}px across` };
  }
  return { err: 'the page kept moving' };
}

// The focus findings for one page state. `memo` (per browser context, so per colour
// mode and width) holds each signature's verdict; pages measure the signatures they
// see first and only then wait for those another page is measuring, so two pages
// can never wait on each other.
async function focusFindings(page, memo, scope) {
  const { out, items } = await page.evaluate(focusWalk, scope);
  const mine = new Map();
  items.forEach((it, i) => {
    if (!it.usable || memo.has(it.sig) || mine.has(it.sig)) return;
    let done;
    memo.set(it.sig, new Promise((ok) => { done = ok; }));
    mine.set(it.sig, [i, done]);
  });
  for (const [i, done] of mine.values()) {
    try { done(await measureFocus(page, i)); }
    catch (e) { done({ err: String(e.message || e).split('\n')[0] }); }
  }
  for (const it of items) {
    const v = it.usable ? await memo.get(it.sig) : null;
    // A measurement that failed is not a missing ring: a control counting it as one would pass blind.
    if (v && v.err) out.push(`focus-measure: could not measure ${it.desc}: ${v.err}`);
    else if (v && !v.ok) out.push(`focus-visible: no focus indicator on ${it.desc} (${v.why})`);
  }
  return out;
}

// Runs in the page: anything that moves under prefers-reduced-motion. A colour
// fade is fine; a transition of position or size, a keyframe animation and smooth
// scrolling are not.
function motionFindings() {
  const out = [];
  const MOVES = /^(all|transform|translate|rotate|scale|left|right|top|bottom|inset.*|margin.*|padding.*|width|height|max-height|max-width|min-height|min-width)$/;
  const secs = (v) => (v.endsWith('ms') ? parseFloat(v) / 1000 : parseFloat(v));
  const name = (el, p) => el.tagName.toLowerCase() + (el.id ? '#' + el.id : '') + (el.classList.length ? '.' + el.classList[0] : '') + p;
  const seen = new Set();
  const check = (el, p) => {
    const c = getComputedStyle(el, p || undefined);
    if (p && c.content === 'none') return;
    if (!p && c.scrollBehavior === 'smooth') seen.add(`motion: smooth scrolling on ${name(el, p)}`);
    const anim = c.animationName.split(', ').some((n, i) => n !== 'none' && secs(c.animationDuration.split(', ')[i] || '0s') > 0.01);
    const durs = c.transitionDuration.split(', ');
    const trans = c.transitionProperty.split(', ').filter((n, i) => MOVES.test(n) && secs(durs[i % durs.length]) > 0.01);
    if (anim) seen.add(`motion: keyframe animation on ${name(el, p)}`);
    if (trans.length) seen.add(`motion: transition of ${trans.join(', ')} on ${name(el, p)}`);
  };
  for (const el of document.querySelectorAll('*')) { check(el, ''); check(el, '::before'); check(el, '::after'); }
  document.getAnimations().forEach((an) => {
    if (an.playState !== 'running' || !an.effect || !an.effect.target) return;
    if (an instanceof CSSTransition && !MOVES.test(an.transitionProperty)) return;
    seen.add(`motion: running animation on ${name(an.effect.target, an.effect.pseudoElement || '')}`);
  });
  seen.forEach((x) => out.push(x));
  return out;
}

function currentFindings() {
  const nav = document.querySelector('nav.side');
  if (!nav) return ['aria-current: no nav.side navigation'];
  const marked = [...nav.querySelectorAll('[aria-current]')];
  const listed = [...nav.querySelectorAll('a[href]')].some((a) => a.pathname === location.pathname ||
    a.pathname.replace(/index\.html$/, '') === location.pathname.replace(/index\.html$/, ''));
  if (!listed) return marked.length ? [`aria-current: marks ${marked.length} link(s) on a page the navigation does not list`] : [];
  if (marked.length !== 1) return [`aria-current: ${marked.length} links marked current, want exactly 1`];
  const a = marked[0];
  if (a.getAttribute('aria-current') !== 'page') return [`aria-current="${a.getAttribute('aria-current')}", want "page"`];
  const same = a.pathname.replace(/index\.html$/, '') === location.pathname.replace(/index\.html$/, '');
  return same ? [] : [`aria-current marks ${a.pathname}, not this page`];
}

// axe's "needs review" results (it could not decide, e.g. text over an image):
// not failures, but counted per rule and printed, so the audit never passes over
// them silently.
const needsReview = {};
async function axeFindings(page) {
  if (!(await page.evaluate(() => typeof window.axe === 'object'))) await page.addScriptTag({ content: AXE });
  // WORKAROUND: axe-core 4.13 compares the authored text colour with the forced background
  // (#f0f4ff on #ffffff) while the browser paints the system text colour (black), so
  // its contrast verdict under forced colours is false. Both colours there are the reader's own.
  const res = await page.evaluate((tags) => window.axe.run(document, {
    runOnly: { type: 'tag', values: tags }, resultTypes: ['violations', 'incomplete'],
    rules: { 'color-contrast': { enabled: !matchMedia('(forced-colors: active)').matches } } }), TAGS);
  res.incomplete.forEach((v) => { needsReview[v.id] = (needsReview[v.id] || 0) + v.nodes.length; });
  return res.violations.flatMap((v) => v.nodes.map((n) => `axe ${v.id} (${v.impact}): ${v.help} at ${n.target.join(' ')}`));
}

async function skipFindings(page) {
  await page.keyboard.press('Tab');
  return page.evaluate(() => {
    const el = document.activeElement;
    if (!el || !el.matches('a.skip-link[href^="#"]')) return [`skip-link: first Tab lands on ${el ? el.tagName.toLowerCase() : 'nothing'}, not a skip link`];
    if (!document.getElementById(el.getAttribute('href').slice(1))) return ['skip-link: its target does not exist'];
    const r = el.getBoundingClientRect();
    return r.top >= 0 && r.bottom <= innerHeight && r.width > 0 ? [] : ['skip-link: not visible when focused'];
  });
}

async function newContext(browser, base, v) {
  const ctx = await browser.newContext({ viewport: { width: v.vp[1], height: v.vp[2] }, colorScheme: v.theme,
                                         forcedColors: v.mode === 'forced' ? 'active' : 'none',
                                         reducedMotion: v.mode === 'reduced' ? 'reduce' : 'no-preference' });
  await ctx.route('**/*', (route) => (route.request().url().startsWith(base) ? route.continue() : route.abort()));
  return ctx;
}

// Every page in one variant, JOBS at a time. Landing pages have no docs navigation
// to mark the current page in.
async function auditVariant(browser, base, urls, v, findings, landing = false) {
  const ctx = await newContext(browser, base, v);
  const memo = new Map();
  const queue = urls.slice();
  await Promise.all(Array.from({ length: JOBS }, async () => {
    const page = await ctx.newPage();
    page.setDefaultTimeout(PAGE_TIMEOUT);
    for (let url; (url = queue.shift()) !== undefined;) {
      const tag = `${url} [${vname(v)}]`;
      try {
        await page.goto(base + url, { waitUntil: 'load' });
        const got = [...await axeFindings(page), ...await skipFindings(page)];
        if (!landing) got.push(...await page.evaluate(currentFindings));
        if (v.mode === 'reduced') got.push(...await page.evaluate(motionFindings));
        got.push(...await focusFindings(page, memo));
        got.forEach((f) => findings.push(`${tag} ${f}`));
      } catch (e) {
        findings.push(`${tag} audit error: ${String(e.message || e).split('\n')[0]}`);
      }
    }
    await page.close();
  }));
  await ctx.close();
}

// The phone navigation drawer: inert while closed; opening it announces it as
// expanded and puts focus in it, where Tab keeps walking it; every link in it,
// groups expanded, visibly focused; closed again when focus lands outside it and
// its menu button, through the skip link, or on Escape; never inert at desktop width.
async function drawerFindings(page, url, vp, memo) {
  const f = [];
  const state = () => page.evaluate(() => [document.getElementById('side').inert,
                                            document.getElementById('menu').getAttribute('aria-expanded')]);
  const expect = async (when, inert, expanded) => {
    const [i, e] = await state();
    if (i !== inert || e !== expanded) f.push(`drawer ${when}: inert=${i} aria-expanded=${e}, want inert=${inert} aria-expanded=${expanded}`);
  };
  await page.goto(url, { waitUntil: 'load' });
  await expect('closed', true, 'false');
  await page.focus('#menu');
  await page.keyboard.press('Enter');
  await page.waitForTimeout(300);                         // the drawer slides in
  await expect('opened', false, 'true');
  // The real keyboard path: opening puts focus in the drawer, and Tab keeps walking it.
  const inDrawer = () => page.evaluate(() => document.getElementById('side').contains(document.activeElement));
  if (!(await inDrawer())) f.push('drawer: opening it did not move focus into it');
  await page.keyboard.press('Tab');
  await page.keyboard.press('Tab');
  if (!(await inDrawer())) f.push('drawer: Tab from its first link left it');
  await expect('after tabbing in it', false, 'true');
  f.push(...await axeFindings(page));
  // Every drawer link, groups expanded, visibly focused. Focus stays inside the
  // drawer throughout; focus elsewhere closes it, which is checked next.
  await page.evaluate(() => document.querySelectorAll('#side details').forEach((d) => { d.open = true; }));
  f.push(...(await focusFindings(page, memo, 'nav.side')).map((x) => 'drawer open: ' + x));
  await expect('while focus is in it', false, 'true');
  await page.evaluate(() => { const a = document.querySelector('main a[href], main [tabindex]'); if (a) a.focus(); });
  await expect('after focus left it', true, 'false');
  await page.focus('#menu');
  await page.keyboard.press('Enter');
  await page.keyboard.press('Escape');
  await expect('after Escape', true, 'false');
  if (!(await page.evaluate(() => document.activeElement && document.activeElement.id === 'menu'))) {
    f.push('drawer: Escape does not return focus to the menu button');
  }
  // The skip link, reached with the drawer open, lands on content the drawer no
  // longer covers.
  await page.focus('#menu');
  await page.keyboard.press('Enter');
  await page.focus('a.skip-link');
  await page.keyboard.press('Enter');
  await expect('after the skip link', true, 'false');
  if (!(await page.evaluate(() => document.activeElement && document.activeElement.id === 'content'))) {
    f.push('drawer: the skip link did not move focus to the content');
  }
  // Escape in the search box belongs to the search box: focus stays there.
  await page.focus('#menu');
  await page.keyboard.press('Enter');
  await page.focus('#q');
  await page.keyboard.type('boot');
  await page.keyboard.press('Escape');
  if (!(await page.evaluate(() => document.activeElement && document.activeElement.id === 'q'))) {
    f.push('drawer: Escape in the search box moved focus out of it');
  }
  await page.fill('#q', '');
  // Opened, then the window widens: the drawer is gone when it narrows again.
  // (Focusing the search box closed it unless it was already closed; reopen it.)
  if ((await state())[0]) { await page.focus('#menu'); await page.keyboard.press('Enter'); }
  await expect('open before widening', false, 'true');
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.waitForTimeout(150);                         // media-query change events fire on the next frame
  await expect('at desktop width', false, 'false');
  await page.setViewportSize({ width: vp[1], height: vp[2] });
  await page.waitForTimeout(150);
  await expect('narrowed again after opening', true, 'false');
  // Focus was on the drawer's first link: it must now be on the visible menu button.
  if (!(await page.evaluate(() => document.activeElement && document.activeElement.id === 'menu'))) {
    f.push('drawer: narrowing hid the focused drawer link without moving focus to the menu button');
  }
  return f;
}

// The results page once it has drawn: axe, then "Show more results" pressed from
// the keyboard (focus must land on the first new result), then the focus walk
// over every generated link and button.
async function resultsFindings(page, url, memo) {
  const f = [];
  await page.goto(url, { waitUntil: 'load' });
  await page.waitForFunction(() => ['done', 'failed'].includes(document.getElementById('search-page').getAttribute('data-state')));
  f.push(...await axeFindings(page));
  if (await page.$('#search-more')) {
    const before = await page.$$eval('#search-page ol.hits > li', (l) => l.length);
    await page.focus('#search-more');
    await page.keyboard.press('Enter');
    const [after, onNew] = await page.evaluate((n) => {
      const items = document.querySelectorAll('#search-page ol.hits > li');
      const a = items[n] && items[n].querySelector('h2 > a');
      return [items.length, !!a && document.activeElement === a];
    }, before);
    if (after <= before) f.push(`results: Show more results drew nothing (${before} pages before, ${after} after)`);
    if (!onNew) f.push('results: focus did not move to the first new result after Show more results');
  } else {
    f.push('results: the query did not fill more than one batch, so Show more results went unchecked');
  }
  f.push(...(await focusFindings(page, memo)).map((x) => 'results: ' + x));
  return f;
}

// The search surfaces in the states a reader sees them in.
async function auditStates(browser, base, v, findings) {
  const { vp } = v;
  const ctx = await newContext(browser, base, v);
  const memo = new Map();
  const page = await ctx.newPage();
  page.setDefaultTimeout(PAGE_TIMEOUT);
  const tag = (s) => `[state ${s} ${vname(v)}]`;
  const run = async (name, fn) => {
    try { (await fn()).forEach((f) => findings.push(`${tag(name)} ${f}`)); }
    catch (e) { findings.push(`${tag(name)} audit error: ${String(e.message || e).split('\n')[0]}`); }
  };
  await run('combobox', async () => {
    await page.goto(base + '/docs/index.html', { waitUntil: 'load' });
    if (vp[0] === 'phone') await page.click('#q'); else await page.keyboard.press('/');
    await page.keyboard.type('boot');
    await page.waitForSelector('#results [role=option]');
    await page.keyboard.press('ArrowDown');
    return axeFindings(page);
  });
  let first = null;
  await run('results-page', async () => {
    const f = await resultsFindings(page, base + '/docs/search.html?q=boot', memo);
    first = await page.$eval('#search-page .hits ul a', (a) => a.getAttribute('href'));
    return f;
  });
  await run('highlighted', async () => {
    if (!first) return ['no result link to open'];
    await page.goto(new URL(first, base + '/docs/search.html').href, { waitUntil: 'load' });
    await page.waitForSelector('article mark');
    return axeFindings(page);
  });
  if (vp[0] === 'phone') await run('drawer', () => drawerFindings(page, base + '/docs/index.html', vp, memo));
  await ctx.close();
}

// Planted defects the audit must report, each on a copy of a real page served
// at a real page's path (so the navigation lists it): low contrast; focus rings
// that are removed, transparent, always shown, a transparent shadow, an unpainted
// border, missing only in the dark theme or only in the open phone drawer; a link
// in an off-screen drawer and one under an overlay; a missing skip link and two
// links marked current; a page the navigation does not list that marks one; a focus
// speck; a ring clipped by its container, twin of an unclipped one; a ring only
// a shadow draws, which forced colours remove; motion left on under reduced motion;
// and the same core defects on the home page. A control that passes means the audit is blind.
async function control(root) {
  const src = readFileSync(join(root, 'docs', 'index.html'), 'utf8');
  const plant = (html, extra) => {
    const out = html.replace('<article>', '<article>\n' + extra);
    if (out === html) throw new Error('control: docs/index.html has no <article> to plant into');
    return out;
  };
  const a = plant(src, '<style>#planted-link:focus-visible { outline: none; } ' +
    '#planted-clear:focus-visible { outline-color: transparent; } ' +
    '#planted-shadow { box-shadow: 0 0 0 2px #155cde; } #planted-shadow:focus-visible { outline: none; } ' +
    '#planted-clearshadow:focus-visible { outline: none; box-shadow: 0 0 0 2px transparent; } ' +
    '#planted-border:focus-visible { outline: none; border-color: #155cde; } ' +
    '#planted-addlayer { box-shadow: 0 0 0 2px #155cde; } ' +
    '#planted-addlayer:focus-visible { outline: none; box-shadow: 0 0 0 2px #155cde, 0 0 0 4px transparent; } ' +
    '#planted-leftok { border-left: 3px solid transparent; } ' +
    '#planted-underline { text-decoration: none; } ' +
    '#planted-blend:focus-visible { outline: 2px solid #f6f8fc; } ' +
    '#planted-inset { background: #0f172a; color: #ffffff; } ' +
    '#planted-inset:focus-visible { outline: none; box-shadow: inset 0 0 0 2px #0f172a; } ' +
    '#planted-insetok { background: #ffffff; color: #0f172a; } ' +
    '#planted-insetok:focus-visible { outline: none; box-shadow: inset 0 0 0 2px #0a6ccf; } ' +
    '#planted-underline:focus-visible { outline: none; text-decoration: underline; text-decoration-color: transparent; } ' +
    '#planted-leftok:focus-visible { outline: none; border-left-color: #155cde; } ' +
    '#planted-layered { background: #ffffff; color: #0f172a; box-shadow: inset 0 0 0 40px #dbe4f0; } ' +
    '#planted-layered:focus-visible { outline: none; box-shadow: inset 0 0 0 2px #0a2a5c, inset 0 0 0 40px #dbe4f0; } ' +
    '#planted-speck { position: relative; display: inline-block; } ' +
    '#planted-speck:focus-visible { outline: none; } ' +
    '#planted-speck:focus-visible::after { content: ""; position: absolute; left: 0; top: 0; width: 7px; height: 7px; background: #000000; } ' +
    '.planted-box { overflow: hidden; } .planted-box a { display: block; } .planted-pad { padding: 8px; } ' +
    '#planted-forced:focus-visible { outline: none; box-shadow: 0 0 0 2px #155cde; } ' +
    // It must not actually move: a spinning block covers its neighbours and makes their verdicts race.
    '@keyframes planted-spin { to { opacity: 0.99; } } ' +
    // !important: the template's own reduced-motion rule would otherwise cancel them, as it should.
    '#planted-motion { animation: planted-spin 2s infinite !important; } ' +
    '#planted-slide { transition: transform 0.3s !important; } ' +
    '#planted-smooth { overflow: auto; height: 40px; scroll-behavior: smooth !important; } ' +
    '.planted-bar { position: relative; display: block; width: 300px; } .planted-bar:focus-visible { outline: none; } ' +
    '.planted-bar:focus-visible::after { content: ""; position: absolute; left: 0; bottom: 0; width: 34px; height: 2px; background: #0a2a5c; } ' +
    '#planted-baroff:focus-visible::after { display: none; } #planted-barfar:focus-visible::after { left: -1000px; } ' +
    '.planted-bar { height: 33px; } #planted-barnear { height: 35px; } ' +
    '@media (prefers-color-scheme: dark) { #planted-dark:focus-visible { outline: none; } } ' +
    '@media (max-width: 820px) { nav.side a:focus-visible { outline: none; } }</style>' +
    '<p id="planted-contrast" style="color: #9aa3ad; background: #ffffff">Planted low-contrast text.</p>' +
    '<p><a id="planted-link" href="#planted-contrast">Planted link without a focus ring</a></p>' +
    '<p><a id="planted-clear" href="#planted-contrast">Planted link with a transparent ring</a></p>' +
    '<p><a id="planted-shadow" href="#planted-contrast">Planted link whose ring never changes</a></p>' +
    '<p><a id="planted-clearshadow" href="#planted-contrast">Planted link with a transparent shadow</a></p>' +
    '<p><a id="planted-border" href="#planted-contrast">Planted link with an unpainted border</a></p>' +
    '<p><a id="planted-addlayer" href="#planted-contrast">Planted link that adds only a transparent layer</a></p>' +
    '<p><a id="planted-leftok" href="#planted-contrast">A correct left-border indicator</a></p>' +
    '<p><a id="planted-underline" href="#planted-contrast">Planted link that adds a transparent underline</a></p>' +
    '<p><a id="planted-blend" href="#planted-contrast">Planted link whose ring is the page colour</a></p>' +
    '<p><a id="planted-inset" href="#planted-contrast">Planted link whose inset ring is its own colour</a></p>' +
    '<p><a id="planted-insetok" href="#planted-contrast">A correct inset ring</a></p>' +
    '<p><a id="planted-dark" href="#planted-contrast">Planted link without a ring in the dark theme</a></p>' +
    '<p><a id="planted-layered" href="#planted-contrast">A correct ring over a permanent inset shadow</a></p>' +
    '<p><a id="planted-speck" href="#planted-contrast">Planted link whose only change is a speck</a></p>' +
    '<div class="planted-box planted-pad"><a id="planted-unclipped" href="#planted-contrast">A ring its container has room for</a></div>' +
    '<div class="planted-box"><a id="planted-clipped" href="#planted-contrast">Planted ring its container clips</a></div>' +
    '<p><a id="planted-forced" href="#planted-contrast">Planted ring only a shadow draws</a></p>' +
    '<p id="planted-motion">Planted spinning text</p><p id="planted-slide">Planted sliding text</p>' +
    '<div id="planted-smooth" tabindex="0">Planted smooth scroller<br><br><br><br><br><br>end</div>' +
    // Twins of one look, the passing one first, so a verdict shared between them would hide the defect.
    '<p><a class="planted-bar" id="planted-baron" href="#planted-contrast">A correct bar</a></p>' +
    '<p><a class="planted-bar" id="planted-baroff" href="#planted-contrast">Planted bar that is display none</a></p>' +
    '<p><a class="planted-bar" id="planted-barfar" href="#planted-contrast">Planted bar moved off the page</a></p>' +
    '<p><a class="planted-bar" id="planted-barnear" href="#planted-contrast">Planted bar 2px too small for its box</a></p>' +
    '<p style="position: relative"><a id="planted-covered" href="#planted-contrast">Planted covered link</a>' +
    '<span style="position: absolute; inset: 0; background: #ffffff"></span></p>' +
    '<div class="table-wrap" style="width: 300px"><table><tr><td><div style="width: 240px">x</div></td>' +
    '<td><a id="planted-clipok" style="white-space: nowrap" href="#planted-contrast">A link a wide table partly clips</a></td></tr></table></div>' +
    '<div style="position: fixed; left: 0; top: 0; transform: translateX(-100%)"><a id="planted-offscreen" ' +
    'href="#planted-contrast">A link in a closed drawer that is not inert</a></div>');
  const b = src.replace('<a class="skip-link" href="#content">Skip to content</a>', '')
    .replace('<li><a href="coverage.html">', '<li><a aria-current="page" href="coverage.html">');
  if (b === src || !b.includes('aria-current="page" href="coverage.html"') || b.includes('class="skip-link"')) {
    throw new Error('control: could not plant the skip-link and aria-current defects into docs/index.html');
  }
  const results = readFileSync(join(root, 'docs', 'search.html'), 'utf8')
    .replace('</head>', '<style>#search-page ol.hits a:focus-visible { outline: none; }</style>\n</head>');
  // The home page: no skip link, low contrast, no focus ring. A landing page is
  // only worth planting into if the walk finds them all.
  const found = landingPages(root);
  const absent = ['/index.html', '/404.html', '/design/index.html', '/design/controls.html', '/err/index.html'].filter((u) => !found.includes(u));
  if (absent.length) { console.log(`a11y control: FAIL -- the landing walk missed ${absent.join(', ')}`); return 1; }
  const homeSrc = readFileSync(join(root, 'index.html'), 'utf8');
  const home = homeSrc.replace(/<a class="skip-link"[^>]*>[^<]*<\/a>/, '').replace('</body>',
    '<style>a#planted-home:focus-visible { outline: none; }</style>' +
    '<p id="planted-home-contrast" style="color: #9aa3ad; background: #ffffff">Planted low-contrast text.</p>' +
    '<p><a id="planted-home" href="#planted-home-contrast">Planted home link without a focus ring</a></p></body>');
  if (home === homeSrc || home.includes('class="skip-link"')) throw new Error('control: could not plant into index.html');
  const server = await serve(root, { '/docs/index.html': a, '/docs/coverage.html': b, '/docs/planted-unlisted.html': src,
                                     '/docs/search.html': results, '/index.html': home });
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });
  const findings = [];
  try {
    for (const v of VARIANTS.filter((k) => k.vp === DESKTOP)) {
      await auditVariant(browser, base, ['/docs/index.html', '/docs/coverage.html', '/docs/planted-unlisted.html'], v, findings);
      await auditVariant(browser, base, ['/index.html'], v, findings, true);
    }
    const phone = VARIANTS.find((k) => k.vp === PHONE);
    const ctx = await newContext(browser, base, phone);
    const memo = new Map();
    const page = await ctx.newPage();
    page.setDefaultTimeout(PAGE_TIMEOUT);
    (await drawerFindings(page, base + '/docs/index.html', phone.vp, memo)).forEach((x) => findings.push('[phone] ' + x));
    (await resultsFindings(page, base + '/docs/search.html?q=boot', memo)).forEach((x) => findings.push('[results] ' + x));
    await ctx.close();
  } finally { await browser.close(); server.close(); }
  const want = [['color-contrast', /index\.html .*axe color-contrast .*#planted-contrast/],
                ['removed focus ring', /focus-visible: no focus indicator on a#planted-link(?![\w-])/],
                ['transparent focus ring', /focus-visible: no focus indicator on a#planted-clear(?![\w-])/],
                ['unchanging focus ring', /focus-visible: no focus indicator on a#planted-shadow(?![\w-])/],
                ['transparent focus shadow', /focus-visible: no focus indicator on a#planted-clearshadow(?![\w-])/],
                ['unpainted focus border', /focus-visible: no focus indicator on a#planted-border(?![\w-])/],
                ['transparent added shadow layer', /focus-visible: no focus indicator on a#planted-addlayer(?![\w-])/],
                ['transparent added underline', /focus-visible: no focus indicator on a#planted-underline(?![\w-])/],
                ['ring the colour of the page', /\[desktop light\] focus-visible: no focus indicator on a#planted-blend(?![\w-])/],
                // Light theme: there the page behind is light, so only reading the control's own dark
                // background catches this ring (the dark theme would flag it for the wrong reason).
                ['inset ring the colour of the control', /\[desktop light\] focus-visible: no focus indicator on a#planted-inset(?![\w-])/],
                ['result link focus ring', /\[results\] results: focus-visible: no focus indicator on a\[href="[^"]*highlight=boot/],
                ['dark-theme focus ring', /\[desktop dark\] focus-visible: no focus indicator on a#planted-dark(?![\w-])/],
                ['open-drawer focus ring', /\[phone\] drawer open: focus-visible: no focus indicator on a\[href=/],
                ['covered focus', /focus-visible: a#planted-covered(?![\w-]).* is covered by span/],
                ['off-screen focus', /focus-visible: focus lands off screen on a#planted-offscreen(?![\w-])/],
                ['missing skip link', /coverage\.html .*skip-link: first Tab lands on/],
                ['two current links', /coverage\.html .*aria-current: 2 links marked current, want exactly 1/],
                ['current on an unlisted page', /planted-unlisted\.html .*aria-current: marks 1 link\(s\) on a page the navigation does not list/],
                ['focus speck', /focus-visible: no focus indicator on a#planted-speck(?![\w-])/],
                ['clipped focus ring', /focus-visible: no focus indicator on a#planted-clipped(?![\w-])/],
                ['forced-colors focus ring', /\[desktop dark forced\] focus-visible: no focus indicator on a#planted-forced(?![\w-])/],
                ['reduced-motion animation', /\[desktop light reduced\] motion: keyframe animation on p#planted-motion/],
                ['reduced-motion transition', /\[desktop light reduced\] motion: transition of transform on p#planted-slide/],
                ['reduced-motion nested smooth scrolling', /\[desktop light reduced\] motion: smooth scrolling on div#planted-smooth/],
                ['hidden pseudo-element indicator', /\[desktop light\] focus-visible: no focus indicator on a#planted-baroff(?![\w-])/],
                ['pseudo-element indicator moved off the page', /\[desktop light\] focus-visible: no focus indicator on a#planted-barfar(?![\w-])/],
                ['fixed indicator just too small for its box', /\[desktop light\] focus-visible: no focus indicator on a#planted-barnear(?![\w-])/],
                ['home page color-contrast', /^\/index\.html .*axe color-contrast .*#planted-home-contrast/],
                ['home page missing skip link', /^\/index\.html .*skip-link: first Tab lands on/],
                ['home page focus ring', /^\/index\.html .*focus-visible: no focus indicator on a#planted-home(?![\w-])/]];
  const missed = want.filter(([, re]) => !findings.some((f) => re.test(f))).map(([n]) => n);
  // And a correct indicator the audit must NOT report. Forced colours are left out:
  // they remove shadows and repaint borders, so there these are real defects.
  const plain = findings.filter((f) => !/ forced\]/.test(f));
  if (plain.some((f) => /focus-visible: .*#planted-leftok(?![\w-])/.test(f))) missed.push('(false failure on a correct left-border indicator)');
  if (plain.some((f) => /focus-visible: .*#planted-insetok(?![\w-])/.test(f))) missed.push('(false failure on a correct inset ring)');
  // A link a wide table partly clips must scroll into view when focused (the site's
  // own behaviour: browsers leave it mostly hidden, so this fails if that handler goes).
  if (plain.some((f) => /focus-visible: .*#planted-clipok(?![\w-])/.test(f))) missed.push('(a partly clipped table link stayed covered when focused)');
  // The bar's colour suits a light page only, so it is correct in the light theme only.
  if (findings.some((f) => /\[desktop light\] focus-visible: .*#planted-baron(?![\w-])/.test(f))) missed.push('(false failure on a correct bar indicator)');
  if (plain.some((f) => /focus-visible: .*#planted-layered(?![\w-])/.test(f))) missed.push('(false failure on a ring over a permanent inset shadow)');
  if (plain.some((f) => /focus-visible: .*#planted-unclipped(?![\w-])/.test(f))) missed.push('(false failure on the unclipped twin of the clipped ring)');
  // The forced-colors plant is a defect only there: seen elsewhere it would prove nothing about that mode.
  if (findings.some((f) => /\[desktop (light|dark)\] focus-visible: .*#planted-forced(?![\w-])/.test(f))) missed.push('(the forced-colors plant failed outside forced colours)');
  if (findings.some((f) => /\/index\.html .*aria-current/.test(f) && !f.includes('/docs/'))) missed.push('(aria-current checked on a landing page)');
  if (findings.some((f) => /focus-measure: /.test(f))) missed.push('(a focus measurement failed)');
  if (missed.length) {
    console.log(`a11y control: FAIL -- the audit did not report the planted ${missed.join(', ')} defect(s)`);
    findings.forEach((f) => console.log('  ' + f));
    return 1;
  }
  console.log(`a11y control: OK -- all ${want.length} planted defects were reported`);
  return 0;
}

async function audit(root) {
  const urls = docsPages(root), landing = landingPages(root);
  // A11Y_ONLY may legitimately filter one tree away; an empty run or a missing docs tree may not pass.
  if (process.env.A11Y_ONLY ? !urls.length && !landing.length : !urls.length) throw new Error(`no pages to audit under ${root}`);
  const server = await serve(root);
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });
  const findings = [];
  const t0 = Date.now();
  try {
    for (const v of VARIANTS) {
      await auditVariant(browser, base, urls, v, findings);
      await auditVariant(browser, base, landing, v, findings, true);
      await auditStates(browser, base, v, findings);
    }
  } finally { await browser.close(); server.close(); }
  const secs = ((Date.now() - t0) / 1000).toFixed(0);
  const review = Object.entries(needsReview).sort((a, b) => b[1] - a[1]);
  console.log(review.length ? `a11y: axe could not decide (needs review): ${review.map(([id, n]) => `${id} ${n}`).join(', ')}`
                            : 'a11y: axe left nothing for review');
  const pages = urls.length + landing.length;
  if (findings.length) {
    findings.forEach((f) => console.log(f));
    console.log(`a11y: FAIL -- ${findings.length} finding(s) over ${pages} pages x ${VARIANTS.length} variants (${secs}s)`);
    return 1;
  }
  console.log(`a11y: OK -- ${pages} pages x ${VARIANTS.length} variants plus search states, no findings (${secs}s)`);
  return 0;
}

const [root, mode] = process.argv.slice(2);
if (!root || (mode && mode !== 'control')) {
  console.error('usage: audit.mjs <site dir> [control]');
  process.exit(2);
}
(mode === 'control' ? control(root) : audit(root)).then((rc) => process.exit(rc), (e) => {
  console.error(`a11y: the audit could not run: ${e.stack || e}`);
  process.exit(2);
});
