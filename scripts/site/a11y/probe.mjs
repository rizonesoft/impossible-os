// Opens one page of a built docs site in Chromium and prints what a reader's
// browser made of it, for scripts/site/tests/test_build.py to assert on:
//   node scripts/site/a11y/probe.mjs <site dir> <path with ?query and #fragment> [#fragment]
// A second fragment is navigated to after load, as choosing another section of
// the same page from its search results does (the page does not reload).
// stdout: {"marks": [{"text", "section", "code"}], "text": the article's textContent,
//          "pre": [every <pre>'s textContent],
//          "search": the results page's text or null, "injected": whether an <img>
//          or <script> appeared inside #search-page or <article>, "errors": [page errors]}
// "section" is the id of the H2/H3 a mark sits under ("" before the first).
import { createServer } from 'node:http';
import { readFileSync, existsSync, statSync } from 'node:fs';
import { join, resolve, sep } from 'node:path';
import { chromium } from 'playwright-core';

const [root, path, then] = process.argv.slice(2);
if (!root || !path) { console.error('usage: probe.mjs <site dir> <path>'); process.exit(2); }
const base = resolve(root);
const server = createServer((req, res) => {
  let p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  if (p.endsWith('/')) p += 'index.html';
  const file = resolve(join(base, p));
  if ((file !== base && !file.startsWith(base + sep)) || !existsSync(file) || !statSync(file).isFile()) {
    res.writeHead(404);
    return res.end();
  }
  res.writeHead(200, { 'content-type': file.endsWith('.html') ? 'text/html; charset=utf-8' : 'application/json' });
  res.end(readFileSync(file));
});
await new Promise((ok) => server.listen(0, '127.0.0.1', ok));
const origin = `http://127.0.0.1:${server.address().port}`;
const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });
try {
  const ctx = await browser.newContext();
  await ctx.route('**/*', (r) => (r.request().url().startsWith(origin) ? r.continue() : r.abort()));
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e.message || e)));
  await page.goto(origin + path, { waitUntil: 'load' });
  // The results page says when it is finished: data-state done, failed, empty or unavailable.
  if (path.includes('search.html')) {
    await page.waitForFunction(() => ['done', 'failed', 'empty', 'unavailable']
      .includes(document.getElementById('search-page').getAttribute('data-state')), null, { timeout: 30000 });
  }
  await page.waitForLoadState('load');
  if (then) {
    await page.evaluate((h) => { location.hash = h; }, then);
    await page.waitForFunction((h) => location.hash === h, then);
    await page.waitForTimeout(100);
  }
  const out = await page.evaluate(() => {
    const heads = [...document.querySelectorAll('article h2, article h3')];
    const marks = [...document.querySelectorAll('article mark')].map((m) => {
      const before = heads.filter((h) => h.compareDocumentPosition(m) & Node.DOCUMENT_POSITION_FOLLOWING);
      return { text: m.textContent, section: before.length ? before[before.length - 1].id : '', code: !!m.closest('pre, code') };
    });
    const sp = document.getElementById('search-page');
    return { marks, text: document.querySelector('article').textContent,
             pre: [...document.querySelectorAll('article pre')].map((p) => p.textContent),
             search: sp ? sp.textContent : null,
             injected: !!document.querySelector('#search-page img, #search-page script, article img[src="x"], article b.injected') };
  });
  out.errors = errors;
  process.stdout.write(JSON.stringify(out));
} finally {
  await browser.close();
  server.close();
}
