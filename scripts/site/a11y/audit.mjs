// Automated accessibility audit of the built docs site (see docs/infrastructure/docs-search-and-accessibility.md).
//
//   node scripts/site/a11y/audit.mjs <site dir>             audit every docs page
//   node scripts/site/a11y/audit.mjs <site dir> control     prove the audit fails on planted defects
//
// <site dir> is the output of `python3 scripts/site/build.py --out <dir>`. Every
// page under <dir>/docs/ is loaded in Chromium at a desktop and a phone width in
// the light and the dark theme, and checked by axe-core (WCAG 2.2 A and AA plus
// its best-practice rules) and by three checks axe does not make:
//   skip-link      the first Tab lands on a visible skip link whose target exists
//   focus-visible  every element Tab can reach shows a focus indicator, on screen
//                  and not covered by another element (a closed drawer must be inert)
//   aria-current   the navigation marks exactly the current page, and none on a
//                  page it does not list (the search results page)
// The search surfaces are audited in their populated states too: the expanded
// combobox, the results page and a page opened from a result with its match marked.
// Requests leave only for the local server, so fonts and the diagram library
// never decide a verdict. Exit 0 = no findings; 1 = findings; 2 = the audit
// itself could not run.
import { createServer } from 'node:http';
import { readFileSync, readdirSync, statSync, existsSync } from 'node:fs';
import { join, extname, resolve, sep } from 'node:path';
import { createRequire } from 'node:module';
import { chromium } from 'playwright-core';

const require = createRequire(import.meta.url);
const AXE = readFileSync(require.resolve('axe-core/axe.min.js'), 'utf8');
const TAGS = ['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'wcag22aa', 'best-practice'];
const VIEWPORTS = [['desktop', 1280, 900], ['phone', 375, 812]];
const THEMES = ['light', 'dark'];
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
function docsPages(root) {
  const out = [];
  (function walk(dir, rel) {
    for (const name of readdirSync(dir).sort()) {
      const full = join(dir, name);
      if (statSync(full).isDirectory()) walk(full, `${rel}${name}/`);
      else if (name.endsWith('.html')) out.push(`/docs/${rel}${name}`);
    }
  })(join(root, 'docs'), '');
  return process.env.A11Y_ONLY ? out.filter((u) => u.includes(process.env.A11Y_ONLY)) : out;
}

// Runs in the page: every element Tab can reach (inside `scope`, a selector, when
// given), focused in turn.
function focusFindings(scope) {
  const out = [];
  const EDGES = ['Top', 'Right', 'Bottom', 'Left'];
  const look = (el) => {
    const c = getComputedStyle(el);
    return { outline: [c.outlineStyle, c.outlineWidth, c.outlineColor, c.outlineOffset].join(' '), shadow: c.boxShadow,
             bg: c.backgroundColor, color: c.color,
             deco: [c.textDecorationLine, c.textDecorationStyle, c.textDecorationColor, c.textDecorationThickness].join(' '),
             edges: EDGES.map((e) => [c[`border${e}Style`], c[`border${e}Width`], c[`border${e}Color`]].join(' ')) };
  };
  const seen = (color) => color !== 'transparent' && !/^rgba\(.*,\s*0\)$/.test(color);
  const layers = (shadow) => (shadow === 'none' ? [] : shadow.match(/rgba?\([^)]*\)[^,]*/g) || []).map((l) => l.trim());
  // A shadow layer paints when it has a visible colour and a non-zero offset, blur or spread.
  const paints = (layer) => {
    const color = layer.match(/rgba?\([^)]*\)/)[0];
    return seen(color) && (layer.slice(color.length).match(/-?[\d.]+px/g) || []).some((n) => parseFloat(n) !== 0);
  };
  // Colours as {r, g, b, a} (0-255, alpha 0-1), from rgb()/rgba() or Chromium's
  // color(srgb ...) form (what color-mix() computes to).
  const rgba = (c) => {
    let m = /^rgba?\(([^)]*)\)/.exec(c);
    if (m) { const v = m[1].split(/[\s,/]+/).filter(Boolean).map(Number); return { r: v[0], g: v[1], b: v[2], a: v.length > 3 ? v[3] : 1 }; }
    m = /^color\(srgb ([^)]*)\)/.exec(c);
    if (m) { const v = m[1].split(/[\s/]+/).filter(Boolean).map(Number); return { r: v[0] * 255, g: v[1] * 255, b: v[2] * 255, a: v.length > 3 ? v[3] : 1 }; }
    return null;
  };
  const over = (top, under) => ({ r: top.r * top.a + under.r * (1 - top.a), g: top.g * top.a + under.g * (1 - top.a),
                                  b: top.b * top.a + under.b * (1 - top.a), a: 1 });
  // What sits behind an element's outline: its ancestors' backgrounds, composited
  // down to an opaque one (white if the page itself paints none).
  const backdrop = (el) => {
    const layers = [];
    for (let n = el.parentElement; n; n = n.parentElement) {
      const c = rgba(getComputedStyle(n).backgroundColor);
      if (c && c.a > 0) { layers.push(c); if (c.a >= 1) break; }
    }
    return layers.reduceRight((under, top) => over(top, under), { r: 255, g: 255, b: 255, a: 1 });
  };
  const lum = (c) => [c.r, c.g, c.b].map((v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; })
    .reduce((s, v, i) => s + v * [0.2126, 0.7152, 0.0722][i], 0);
  // WCAG 1.4.11: a focus indicator needs 3:1 against what is behind it. An
  // indicator painted INSIDE the control (an inset shadow, an outline with a
  // negative offset) sits on the control's own background; one outside it sits
  // on the ancestors'.
  // `ctx` carries the element's captured background and caches its backdrop, so
  // the ancestor walk runs at most once per focused element.
  const stands = (color, ctx, inside) => {
    const c = rgba(color);
    if (!c || c.a === 0) return false;
    const own = inside ? rgba(ctx.bg) : null;
    let bg = own && own.a >= 1 ? own : (ctx.backdrop || (ctx.backdrop = backdrop(ctx.el)));
    if (own && own.a > 0 && own.a < 1) bg = over(own, bg);
    const x = lum(over(c, bg)), y = lum(bg);
    return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05) >= 3;
  };
  // A border edge paints when it has a style, a width and a visible colour.
  const edgePaints = (c, e) => !['none', 'hidden'].includes(c[`border${e}Style`]) &&
    parseFloat(c[`border${e}Width`]) > 0 && seen(c[`border${e}Color`]);
  const sel = 'a[href], button, input, select, textarea, summary, [tabindex]:not([tabindex="-1"])';
  const describe = (el) => el.tagName.toLowerCase() + (el.id ? '#' + el.id : '') +
    (el.getAttribute('href') ? `[href="${el.getAttribute('href')}"]` : '') +
    (el.textContent.trim() ? ` "${el.textContent.trim().slice(0, 40)}"` : '');
  // Start with nothing focused, so each element's unfocused look is its real one
  // (the skip-link check has just tabbed to the first element).
  if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
  for (const el of (scope ? document.querySelector(scope) : document).querySelectorAll(sel)) {
    if (el.disabled || el.closest('[hidden]') || getComputedStyle(el).visibility === 'hidden') continue;
    if (!el.getClientRects().length) continue;               // display: none, not in the tab order
    const before = look(el);
    el.focus();
    if (document.activeElement !== el) continue;              // not focusable (inert), so not reachable
    // An indicator is a VISIBLE change on focus: a transparent outline, or a
    // shadow the element always wears, is no indicator at all.
    const after = look(el), cs = getComputedStyle(el);
    const ctx = { el, bg: after.bg, backdrop: null };
    const outline = cs.outlineStyle !== 'none' && parseFloat(cs.outlineWidth) > 0 &&
      stands(cs.outlineColor, ctx, parseFloat(cs.outlineOffset) < 0) &&
      (after.outline !== before.outline);
    // Only what focus ADDED counts: a new shadow layer that paints and stands out,
    // or a border edge that changed and paints.
    const old = new Set(layers(before.shadow));
    const shadow = layers(after.shadow).some((l) => !old.has(l) && paints(l) &&
      stands(l.match(/rgba?\([^)]*\)/)[0], ctx, /\binset\b/.test(l)));
    const border = EDGES.some((e, i) => after.edges[i] !== before.edges[i] && edgePaints(cs, e));
    // Text: a new colour that shows, or a decoration (an underline) that paints.
    const text = (after.color !== before.color && seen(cs.color)) ||
      (after.deco !== before.deco && cs.textDecorationLine !== 'none' && seen(cs.textDecorationColor) &&
       parseFloat(cs.textDecorationThickness) !== 0);
    const fill = (after.bg !== before.bg && seen(cs.backgroundColor)) || border || text;
    const ring = outline || shadow || fill;
    const r = el.getBoundingClientRect();
    const onScreen = r.width > 0 && r.height > 0 && r.right > 0 && r.bottom > 0 &&
      r.left < innerWidth && r.top < innerHeight;
    if (!ring) out.push(`focus-visible: no focus indicator on ${describe(el)}`);
    if (!onScreen) { out.push(`focus-visible: focus lands off screen on ${describe(el)}`); continue; }
    // Covered: no line box of the element (a link can wrap) shows it at its centre.
    let top = null;
    const shown = [...el.getClientRects()].some((b) => {
      const x = Math.min(Math.max(b.left + b.width / 2, 0), innerWidth - 1);
      const y = Math.min(Math.max(b.top + b.height / 2, 0), innerHeight - 1);
      top = document.elementFromPoint(x, y);
      return !top || top === el || el.contains(top) || top.contains(el);
    });
    if (!shown) out.push(`focus-visible: ${describe(el)} is covered by ${describe(top)} when focused`);
  }
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
  const res = await page.evaluate((tags) => window.axe.run(document, {
    runOnly: { type: 'tag', values: tags }, resultTypes: ['violations', 'incomplete'] }), TAGS);
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

async function newContext(browser, base, [, w, h], theme) {
  const ctx = await browser.newContext({ viewport: { width: w, height: h }, colorScheme: theme });
  await ctx.route('**/*', (route) => (route.request().url().startsWith(base) ? route.continue() : route.abort()));
  return ctx;
}

// Every page in one variant, JOBS at a time.
async function auditVariant(browser, base, urls, vp, theme, findings) {
  const ctx = await newContext(browser, base, vp, theme);
  const queue = urls.slice();
  await Promise.all(Array.from({ length: JOBS }, async () => {
    const page = await ctx.newPage();
    page.setDefaultTimeout(PAGE_TIMEOUT);
    for (let url; (url = queue.shift()) !== undefined;) {
      const tag = `${url} [${vp[0]} ${theme}]`;
      try {
        await page.goto(base + url, { waitUntil: 'load' });
        const got = [...await axeFindings(page), ...await skipFindings(page), ...await page.evaluate(currentFindings)];
        got.push(...await page.evaluate(focusFindings));
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
async function drawerFindings(page, url, vp) {
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
  f.push(...(await page.evaluate(focusFindings, 'nav.side')).map((x) => 'drawer open: ' + x));
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
async function resultsFindings(page, url) {
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
  f.push(...(await page.evaluate(focusFindings)).map((x) => 'results: ' + x));
  return f;
}

// The search surfaces in the states a reader sees them in.
async function auditStates(browser, base, vp, theme, findings) {
  const ctx = await newContext(browser, base, vp, theme);
  const page = await ctx.newPage();
  page.setDefaultTimeout(PAGE_TIMEOUT);
  const tag = (s) => `[state ${s} ${vp[0]} ${theme}]`;
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
    const f = await resultsFindings(page, base + '/docs/search.html?q=boot');
    first = await page.$eval('#search-page .hits ul a', (a) => a.getAttribute('href'));
    return f;
  });
  await run('highlighted', async () => {
    if (!first) return ['no result link to open'];
    await page.goto(new URL(first, base + '/docs/search.html').href, { waitUntil: 'load' });
    await page.waitForSelector('article mark');
    return axeFindings(page);
  });
  if (vp[0] === 'phone') await run('drawer', () => drawerFindings(page, base + '/docs/index.html', vp));
  await ctx.close();
}

// Planted defects the audit must report, each on a copy of a real page served
// at a real page's path (so the navigation lists it): low contrast; focus rings
// that are removed, transparent, always shown, a transparent shadow, an unpainted
// border, missing only in the dark theme or only in the open phone drawer; a link
// in an off-screen drawer and one under an overlay; a missing skip link and two
// links marked current; and a page the navigation does not list that marks one.
// A control that passes means the audit is blind.
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
    '<p style="position: relative"><a id="planted-covered" href="#planted-contrast">Planted covered link</a>' +
    '<span style="position: absolute; inset: 0; background: #ffffff"></span></p>' +
    '<div style="position: fixed; left: 0; top: 0; transform: translateX(-100%)"><a id="planted-offscreen" ' +
    'href="#planted-contrast">A link in a closed drawer that is not inert</a></div>');
  const b = src.replace('<a class="skip-link" href="#content">Skip to content</a>', '')
    .replace('<li><a href="coverage.html">', '<li><a aria-current="page" href="coverage.html">');
  if (b === src || !b.includes('aria-current="page" href="coverage.html"') || b.includes('class="skip-link"')) {
    throw new Error('control: could not plant the skip-link and aria-current defects into docs/index.html');
  }
  const results = readFileSync(join(root, 'docs', 'search.html'), 'utf8')
    .replace('</head>', '<style>#search-page ol.hits a:focus-visible { outline: none; }</style>\n</head>');
  const server = await serve(root, { '/docs/index.html': a, '/docs/coverage.html': b, '/docs/planted-unlisted.html': src,
                                     '/docs/search.html': results });
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });
  const findings = [];
  try {
    for (const theme of THEMES) {
      await auditVariant(browser, base, ['/docs/index.html', '/docs/coverage.html', '/docs/planted-unlisted.html'],
                         VIEWPORTS[0], theme, findings);
    }
    const ctx = await newContext(browser, base, VIEWPORTS[1], 'light');
    const page = await ctx.newPage();
    page.setDefaultTimeout(PAGE_TIMEOUT);
    (await drawerFindings(page, base + '/docs/index.html', VIEWPORTS[1])).forEach((x) => findings.push('[phone] ' + x));
    (await resultsFindings(page, base + '/docs/search.html?q=boot')).forEach((x) => findings.push('[results] ' + x));
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
                ['current on an unlisted page', /planted-unlisted\.html .*aria-current: marks 1 link\(s\) on a page the navigation does not list/]];
  const missed = want.filter(([, re]) => !findings.some((f) => re.test(f))).map(([n]) => n);
  // And a correct indicator the audit must NOT report.
  if (findings.some((f) => /focus-visible: .*#planted-leftok(?![\w-])/.test(f))) missed.push('(false failure on a correct left-border indicator)');
  if (findings.some((f) => /focus-visible: .*#planted-insetok(?![\w-])/.test(f))) missed.push('(false failure on a correct inset ring)');
  if (missed.length) {
    console.log(`a11y control: FAIL -- the audit did not report the planted ${missed.join(', ')} defect(s)`);
    findings.forEach((f) => console.log('  ' + f));
    return 1;
  }
  console.log(`a11y control: OK -- all ${want.length} planted defects were reported`);
  return 0;
}

async function audit(root) {
  const urls = docsPages(root);
  if (!urls.length) throw new Error(`no pages under ${join(root, 'docs')}`);
  const server = await serve(root);
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });
  const findings = [];
  const t0 = Date.now();
  try {
    for (const vp of VIEWPORTS) {
      for (const theme of THEMES) {
        await auditVariant(browser, base, urls, vp, theme, findings);
        await auditStates(browser, base, vp, theme, findings);
      }
    }
  } finally { await browser.close(); server.close(); }
  const secs = ((Date.now() - t0) / 1000).toFixed(0);
  const review = Object.entries(needsReview).sort((a, b) => b[1] - a[1]);
  console.log(review.length ? `a11y: axe could not decide (needs review): ${review.map(([id, n]) => `${id} ${n}`).join(', ')}`
                            : 'a11y: axe left nothing for review');
  const runs = urls.length * VIEWPORTS.length * THEMES.length;
  if (findings.length) {
    findings.forEach((f) => console.log(f));
    console.log(`a11y: FAIL -- ${findings.length} finding(s) over ${urls.length} pages x ${runs / urls.length} variants (${secs}s)`);
    return 1;
  }
  console.log(`a11y: OK -- ${urls.length} pages x ${runs / urls.length} variants plus search states, no findings (${secs}s)`);
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
