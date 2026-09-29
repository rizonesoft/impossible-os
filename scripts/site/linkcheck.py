#!/usr/bin/env python3
"""Check every external link on the published site for rot (the Sphinx
`linkcheck` equivalent).

Internal links are checked on every commit by `build.py --check` (lint Check 30);
external ones cannot be, because the network is slow, flaky and not ours. So this
runs WEEKLY (.github/workflows/linkcheck.yml) and on demand, never per commit.

What is collected: every http(s) link a reader can follow, from docs/**/*.md
(Markdown links, autolinks, images, and href/src in raw HTML, but not URLs in code
spans or code blocks) and from the rendered gh-pages/ templates (href/src, except
<link rel="preconnect|dns-prefetch">, which name a host, not a page).

How each is judged, after retries:
  DEAD        404 or 410, the host name does not exist (EAI_NONAME/EAI_NODATA),
              or the link is malformed as written: no host, a bad port, or anything
              the HTTP client refuses to form a request from.
              Fails the run: this is rot, and the page must be fixed.
  UNVERIFIED  anything ambiguous: 401/403/429, 5xx, timeouts, TLS errors, refused
              connections, temporary DNS failure (EAI_AGAIN), a redirect with
              no usable destination or to a non-HTTP URL. Reported with its
              reason, never failed and never counted as healthy: bot walls and
              outages are not rot, and failing on them would teach people to
              ignore the check.
Each attempt sends HEAD, then GET when HEAD is refused (400/403/405/501) or says
gone (404/410), since many servers refuse HEAD but serve the page. Redirects are
followed by hand (at most 10, http(s) only) and no response body is ever read.
Each link is first put in the form a browser requests, by Node's WHATWG URL
parser (trimmed, dot segments resolved, components encoded), and each redirect
Location is resolved by it the same way; a link that parser rejects is
malformed as written. Node is required: if it is missing or fails, the run
stops with exit 2 rather than judge links by an approximation.
A link is retried until an attempt succeeds and the last attempt's answer
stands, except that a request the client refuses to form is final at once.
Through a proxy, only an HTTP answer can make a link DEAD. At most 4 requests
go to one host at a time, redirect hops included, and the whole run has a time
budget (default 900 s, inside the workflow's 20 minutes): links still
unanswered then are reported UNVERIFIED rather than lost to a cancelled job.

Allowlist: scripts/site/linkcheck-allow.txt, one URL prefix per line followed by
`  # reason` (the reason is required). Allowlisted links are not fetched; an
entry that matches no link is reported as stale.

Usage:
  python3 scripts/site/linkcheck.py [--retries N] [--wait SECS] [--timeout SECS] [--budget SECS] [--list]
Exit: 0 no dead links, 1 dead links (or a malformed allowlist), 2 usage error or the
URL parser (Node) missing or failing.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import http.client
import json
import re
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from html.parser import HTMLParser
from urllib.parse import urlsplit

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build as B  # noqa: E402  (sibling: vendored markdown-it, facts, tracked files)

ALLOWLIST = B.REPO / "scripts" / "site" / "linkcheck-allow.txt"
USER_AGENT = "Mozilla/5.0 (compatible; impossible-os-linkcheck/1; +https://impossibleos.co/)"
HTTP_RE = re.compile(r"^https?://", re.I)
PER_HOST = 4        # concurrent requests to one host, so a page full of GitHub links is not a burst
MAX_REDIRECTS = 10

DEAD, UNVERIFIED, OK = "DEAD", "UNVERIFIED", "OK"
MD = B.make_md()


# --------------------------------------------------------------------------
# Collection
# --------------------------------------------------------------------------

class _LinkParser(HTMLParser):
    """href/src of real tags. HTMLParser handles quoting, comments and exact
    attribute names, so a URL in a comment, inside another attribute's value or
    in `data-href` is never taken for a link."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.found: list[tuple[str, int]] = []

    def handle_starttag(self, tag, attrs):
        a = {k.lower(): v or "" for k, v in attrs}
        if tag == "link" and {"preconnect", "dns-prefetch"} & set(a.get("rel", "").lower().split()):
            return   # a connection hint names a host, not a page
        for key in ("href", "src"):
            if HTTP_RE.match(a.get(key, "")):
                self.found.append((a[key], self.getpos()[0]))

    handle_startendtag = handle_starttag


def html_links(text: str, where: str, line0: int, out: dict[str, list[str]]) -> None:
    p = _LinkParser()
    p.feed(text)
    p.close()
    for url, line in p.found:
        out.setdefault(url.split("#", 1)[0], []).append(f"{where}:{line0 + line - 1}")


def markdown_links(text: str, where: str, out: dict[str, list[str]]) -> None:
    for tok in MD.parse(text, {}):
        line = (tok.map[0] + 1) if tok.map else 1
        if tok.type == "html_block":
            html_links(tok.content, where, line, out)
        if tok.type != "inline":
            continue
        for c in tok.children or []:
            url = ""
            if c.type == "link_open":
                url = c.attrGet("href") or ""
            elif c.type == "image":
                url = c.attrGet("src") or ""
            elif c.type == "html_inline":
                html_links(c.content, where, line, out)
            if HTTP_RE.match(url):
                out.setdefault(url.split("#", 1)[0], []).append(f"{where}:{line}")


def collect() -> dict[str, list[str]]:
    """{url without fragment: [file:line, ...]} over docs/ and the rendered gh-pages/."""
    out: dict[str, list[str]] = {}
    for path in B.tracked_files("docs/*.md"):
        markdown_links(path.read_text(encoding="utf-8"), path.relative_to(B.ROOT).as_posix(), out)
    facts = B.load_project()
    for path in B.tracked_files("gh-pages/*.html"):
        rel = path.relative_to(B.ROOT).as_posix()
        text = path.read_text(encoding="utf-8")
        # Templates are checked as published: {{repo_url}} resolved. %ROOT%-style
        # docs-template placeholders are relative and never match http(s).
        text = B.render_template(text, dict(facts, feature_cards="", design_tokens_css=""), rel, [])
        html_links(text, rel, 1, out)
    return out


# --------------------------------------------------------------------------
# Allowlist
# --------------------------------------------------------------------------

def load_allowlist(path: Path) -> tuple[list[str], list[str]]:
    """(prefixes, errors). Each entry is `<url prefix>  # <reason>`."""
    prefixes, errors = [], []
    if not path.exists():
        return prefixes, errors
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.fullmatch(r"(\S+)\s+#\s*(.*)", line)
        url, reason = (m.group(1), m.group(2)) if m else (line, "")
        if not HTTP_RE.match(url) or len(reason.strip()) < 8:
            errors.append(f"{path.name}:{n}: expected '<http(s) url prefix>  # <reason>', got {raw!r}")
            continue
        prefixes.append(url)
    return prefixes, errors


# --------------------------------------------------------------------------
# Checking
# --------------------------------------------------------------------------

def malformed(url: str) -> str:
    """Why `url` cannot be requested as written ("" when it can): it must parse,
    name a host, and carry a numeric port if it has one. Checked on the links
    as written, where a malformed one is a broken link (DEAD); a malformed
    redirect Location is the far server's problem (UNVERIFIED)."""
    try:
        parts = urlsplit(url)
        parts.port   # raises on a non-numeric or out-of-range port
    except ValueError as e:
        return str(e)
    return "" if parts.hostname else "no host name"


def classify_error(exc: BaseException) -> tuple[str, str]:
    """(verdict, reason) for a request that raised. Only a definite answer is DEAD."""
    if isinstance(exc, urllib.error.HTTPError):
        if exc.code in (404, 410):
            return DEAD, f"HTTP {exc.code}"
        return UNVERIFIED, f"HTTP {exc.code}"
    reason = exc.reason if isinstance(exc, urllib.error.URLError) else exc
    if isinstance(reason, socket.gaierror):
        definite = {getattr(socket, n) for n in ("EAI_NONAME", "EAI_NODATA") if hasattr(socket, n)}
        if reason.errno in definite:
            return DEAD, f"host not found ({reason.strerror or reason})"
        return UNVERIFIED, f"DNS lookup failed ({reason.strerror or reason})"
    if isinstance(reason, (socket.timeout, TimeoutError)):
        return UNVERIFIED, "timed out"
    if isinstance(reason, ssl.SSLError):
        return UNVERIFIED, f"TLS error ({reason.reason or reason})"
    if isinstance(reason, ConnectionRefusedError):
        return UNVERIFIED, "connection refused"
    if isinstance(reason, str):
        return UNVERIFIED, reason
    return UNVERIFIED, f"{type(reason).__name__}: {reason}"


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Surface a 3xx as an HTTPError instead of following it. urllib's own
    handler drains the redirect body with an unbounded read() before following;
    the scheduler follows redirects itself and never reads a body."""

    def http_error_302(self, req, fp, code, msg, headers):
        return None   # the next handler raises HTTPError; urllib never parses or drains the redirect

    http_error_301 = http_error_303 = http_error_307 = http_error_308 = http_error_302


_PHASE = threading.local()   # this thread's current request: .connected (reached a server), .proxied (routed via a proxy)


def _tracked(conn_class):
    class Tracked(conn_class):
        def connect(self):
            super().connect()
            _PHASE.connected = True
    return Tracked


class _HTTP(urllib.request.HTTPHandler):
    def http_open(self, req):
        return self.do_open(_tracked(http.client.HTTPConnection), req)


class _HTTPS(urllib.request.HTTPSHandler):
    def https_open(self, req):
        return self.do_open(_tracked(http.client.HTTPSConnection), req, context=self._context)


class _RecordingProxy(urllib.request.ProxyHandler):
    """ProxyHandler that records, per thread, whether THIS request was actually
    routed through a proxy, by urllib's own bypass rule rather than a guess at
    it. Through a proxy, a failure before any HTTP answer (a bad proxy address,
    a proxy host that does not resolve) may be the proxy's, so it proves nothing
    about the link: such a result is never DEAD. A proxy setting that fails to
    parse counts as proxied for the same reason."""

    def proxy_open(self, req, proxy, type):
        before = req.host
        try:
            return super().proxy_open(req, proxy, type)
        except Exception:
            _PHASE.proxied = True
            raise
        finally:
            if req.host != before:   # set_proxy() moved the request to the proxy
                _PHASE.proxied = True


PROXIES = urllib.request.getproxies()


def make_opener(proxies: dict) -> urllib.request.OpenerDirector:
    return urllib.request.build_opener(_RecordingProxy(proxies), _NoRedirect, _HTTP, _HTTPS)


OPENER = make_opener(PROXIES)
MALFORMED = (ValueError, http.client.InvalidURL)   # UnicodeError is a ValueError


class NotFormed(Exception):
    """The HTTP client refused to form the request (an unparsable URL, a host
    name it cannot encode, a control character) before any connection was made.
    The same exception types raised AFTER connecting (a response header the
    parser rejects) are the server's doing and are not this."""
REDIRECTS = (301, 302, 303, 307, 308)
HEAD_REFUSED = (400, 403, 404, 405, 410, 501)   # retry as GET: many servers refuse HEAD, or answer it wrongly


NODE_URL = r"""
const [links, base] = JSON.parse(require("fs").readFileSync(0, "utf8"));
const out = Object.create(null);   // no prototype: a link named __proto__ is just a key
for (const u of links) {
  try { const x = base === null ? new URL(u) : new URL(u, base); x.hash = ""; out[u] = x.href; }
  catch (e) { out[u] = null; }
}
process.stdout.write(JSON.stringify(out));
"""


class _Closed(Exception):
    """The run's budget is spent: no new work may start (internal)."""


class NormalizeError(Exception):
    """The URL parser (Node) is missing, failed or ran out of time. That is the
    checker's own failure, never evidence about a link."""


def browser_urls(urls: list[str], base: str | None = None, timeout: float = 120.0) -> dict[str, str | None]:
    """{link as written: the URL a browser requests for it (resolved against
    `base` when given, as for a redirect Location), or None when a browser
    cannot parse it}, from Node's WHATWG `URL` in one process: trimmed, tabs and
    newlines removed, dot segments resolved, backslashes read as slashes,
    components percent-encoded, host IDNA-encoded, fragment dropped. Hand-rolling
    that standard kept diverging from it, so the checker asks a real
    implementation; Node is on the dev host and the Actions runner. Raises
    NormalizeError when Node is missing, fails or exceeds `timeout` (at least 1 s,
    so Node's own start-up on a slow runner is never mistaken for a stall)."""
    if not urls:
        return {}
    node = shutil.which("node")
    if not node:
        raise NormalizeError("node not found (the link checker parses URLs with Node's WHATWG URL)")
    try:   # bytes, not text: undecodable output must become a NormalizeError, not escape as another type
        r = subprocess.run([node, "-e", NODE_URL], input=json.dumps([sorted(set(urls)), base]).encode("utf-8"),
                           capture_output=True, timeout=max(1.0, timeout))
    except subprocess.TimeoutExpired:
        raise NormalizeError(f"node URL parser took longer than {max(1.0, timeout):.1f} s") from None
    except OSError as e:
        raise NormalizeError(f"node URL parser could not run: {e}") from None
    if r.returncode != 0:
        err = r.stderr.decode("utf-8", "replace").strip()[:200]
        raise NormalizeError(f"node URL parser failed (rc {r.returncode}): {err}")
    try:
        out = json.loads(r.stdout.decode("utf-8"))
    except ValueError:   # includes UnicodeDecodeError
        raise NormalizeError("node URL parser returned unreadable output") from None
    if not isinstance(out, dict) or set(out) != set(urls):
        raise NormalizeError("node URL parser returned an incomplete answer")
    return out


def header_url(value: str) -> str:
    """A Location header as the bytes the server sent, non-ASCII bytes
    percent-escaped, which is how Chromium reads one: http.client decodes header
    bytes as Latin-1, so a raw UTF-8 `/cafe\u0301`-style path would otherwise be
    encoded twice. Existing `%xx` escapes and ASCII pass through unchanged."""
    try:
        raw = value.encode("latin-1")
    except UnicodeEncodeError:
        return value
    return "".join(chr(b) if b < 0x80 else f"%{b:02X}" for b in raw)


def request(url: str, method: str, timeout: float) -> tuple[int, str | None]:
    """ONE request, no redirect followed and no body read: (status, the Location
    of a redirect, or None). Anything but http(s) is refused before connecting."""
    if not HTTP_RE.match(url):
        raise urllib.error.URLError(f"redirect to a non-HTTP URL: {url[:80]}")
    _PHASE.connected = _PHASE.proxied = False
    try:
        req = urllib.request.Request(url, method=method, headers={"User-Agent": USER_AGENT, "Accept": "*/*"})
        with OPENER.open(req, timeout=timeout) as r:
            return r.status, None
    except MALFORMED as e:
        if getattr(_PHASE, "connected", False):
            raise
        raise NotFormed(str(e)) from e
    except urllib.error.HTTPError as e:
        location = e.headers.get("Location") if e.code in REDIRECTS else None
        e.close()
        return e.code, (header_url(location) if location else None)


def verdict_for(code: int) -> tuple[str, str]:
    if 200 <= code < 300:
        return OK, ""
    if 300 <= code < 400:
        return UNVERIFIED, f"HTTP {code} without a usable Location"
    return (DEAD if code in (404, 410) else UNVERIFIED), f"HTTP {code}"


@dataclasses.dataclass
class Job:
    """One pending request for one link: its first try, the next hop of a
    redirect chain, the GET after a refused HEAD, or a retry waiting out its
    backoff (`not_before`)."""
    origin: str          # the link as written (results are keyed by it)
    url: str             # what this request fetches
    start: str = ""      # what the link's first request fetches (the browser's form of `origin`)
    method: str = "HEAD"
    hops: int = 0
    attempt: int = 0
    not_before: float = 0.0


def resolve_location(location: str, base: str, timeout: float) -> str | None:
    return browser_urls([location], base=base, timeout=timeout)[location]


def step(job: Job, retries: int, wait: float, timeout: float,
         resolve=resolve_location) -> tuple[Job | None, tuple[str, str] | None]:
    """Run one request of `job`: (a follow-up job, None) or (None, final verdict).
    Any OK ends a link, and so does a request the client refused to form
    (NotFormed), since retrying cannot change it; anything else is retried from
    the start until `retries` attempts are spent, and the last answer stands.
    Through a proxy, only an HTTP answer can make a link DEAD. A NormalizeError
    from resolving a redirect is not an answer about the link: it propagates,
    and run_checks stops the run with it."""
    try:
        code, location = request(job.url, job.method, timeout)
    except NotFormed as e:
        # The HTTP client refused to even form this request (an unparsable URL, a
        # bad host name, a control character): nothing on the network was asked.
        # For the link as written that is a broken link; after a redirect it is
        # the far server's Location that is broken. Retrying cannot change it.
        first = job.url == (job.start or job.origin)
        where = "URL" if first else "redirect target"
        if getattr(_PHASE, "proxied", False):
            return None, (UNVERIFIED, f"request refused before any answer, through a proxy ({e})")
        return None, ((DEAD if first else UNVERIFIED), f"malformed {where} ({e})")
    except Exception as e:  # noqa: BLE001  (every failure is classified, none escapes)
        outcome = classify_error(e)
        if outcome[0] == DEAD and getattr(_PHASE, "proxied", False):   # e.g. the PROXY's name did not resolve
            outcome = (UNVERIFIED, f"{outcome[1]}, through a proxy")
    else:
        if location:
            if job.hops >= MAX_REDIRECTS:
                outcome = (UNVERIFIED, f"more than {MAX_REDIRECTS} redirects")
            else:
                # Resolved as a browser resolves it against the current URL. A
                # NormalizeError propagates: it stops the run, it is no verdict.
                target = resolve(location, job.url, timeout) or ""
                if target and not HTTP_RE.match(target):
                    outcome = (UNVERIFIED, f"redirect to a non-HTTP URL: {target[:80]}")
                elif not target or malformed(target):   # the scheduler keys the next hop by its host
                    outcome = (UNVERIFIED, f"HTTP {code} to a malformed Location")
                else:
                    return dataclasses.replace(job, url=target, hops=job.hops + 1), None
        elif job.method == "HEAD" and code in HEAD_REFUSED:
            return dataclasses.replace(job, url=job.start or job.origin, method="GET", hops=0), None
        else:
            outcome = verdict_for(code)
    if outcome[0] == OK or job.attempt + 1 >= max(1, retries):
        return None, outcome
    nxt = job.attempt + 1
    start = job.start or job.origin
    return Job(job.origin, start, start, attempt=nxt, not_before=time.monotonic() + wait * nxt), None


def run_checks(urls: list[str], retries: int, wait: float, timeout: float, workers: int = 16,
               budget: float = 900.0) -> dict[str, tuple[str, str]]:
    """Check every URL. Every request (first try, redirect hop, GET fallback,
    retry) is a job queued under the host it goes to, and a worker takes only a
    job whose host has a free slot (PER_HOST) and whose backoff has passed, so
    no worker ever waits while holding capacity: a stalled host delays its own
    links, never another host's. Workers are daemon threads; when `budget`
    seconds pass, every link not yet answered is reported UNVERIFIED and the run
    returns instead of hanging on a stuck connection. The URL parser failing,
    up front or while resolving a redirect, raises NormalizeError instead. At
    the deadline no new request or parser call starts; a parser call already in
    flight is waited out (up to its own timeout plus 2 s) so its failure is not
    lost, and one still running after that fails the run closed."""
    deadline = time.monotonic() + budget
    unique = sorted(set(urls))
    queues: dict[str, list[Job]] = collections.defaultdict(list)
    busy: collections.Counter = collections.Counter()
    results: dict[str, tuple[str, str]] = {}
    fatal: list[NormalizeError] = []   # the URL parser failed mid-run: stop, and raise it to the caller
    parsing = [0]                      # redirect resolutions in flight in Node
    closed = [False]                   # the budget is spent: admit no new job and no new parser call
    sent = browser_urls(unique, timeout=min(120.0, budget))   # NormalizeError propagates: no verdicts
    for u in unique:
        first = sent.get(u)
        why = "a browser cannot parse it" if first is None else malformed(first)
        if why:   # broken as written: report it, and check the rest
            results[u] = (DEAD, f"malformed URL ({why})")
        else:
            queues[urlsplit(first).netloc.lower()].append(Job(u, first, first))
    cond = threading.Condition()

    def take() -> Job | None:
        with cond:
            while len(results) < len(unique) and not fatal and not closed[0]:
                now, soonest = time.monotonic(), None
                if now >= deadline:   # the absolute deadline, not only the coordinator's flag
                    return None
                for host, q in queues.items():
                    if busy[host] >= PER_HOST:
                        continue
                    for i, job in enumerate(q):
                        if job.not_before <= now < deadline:
                            busy[host] += 1
                            return q.pop(i)
                        soonest = job.not_before if soonest is None else min(soonest, job.not_before)
                cond.wait(None if soonest is None else max(0.0, soonest - now))
            return None

    def resolve(location: str, base: str, t: float) -> str | None:
        with cond:
            if closed[0] or time.monotonic() >= deadline:
                raise _Closed()
            parsing[0] += 1
        failure = None
        try:
            return resolve_location(location, base, t)
        except NormalizeError as e:
            failure = e
            raise
        finally:
            with cond:
                # Publish a failure BEFORE the call stops counting as in flight:
                # otherwise the coordinator can see neither and return green.
                if failure is not None and failure not in fatal:
                    fatal.append(failure)
                parsing[0] -= 1
                cond.notify_all()

    def worker() -> None:
        while (job := take()) is not None:
            host = urlsplit(job.url).netloc.lower()
            follow, outcome, broken = None, (UNVERIFIED, "checker error"), None
            try:
                follow, outcome = step(job, retries, wait, timeout, resolve)
            except NormalizeError as e:
                broken = e
            except _Closed:
                pass   # the run already returned this link as unanswered
            except Exception as e:  # noqa: BLE001  (a bug must cost one link, never the host's queue)
                follow, outcome = None, (UNVERIFIED, f"checker error: {type(e).__name__}: {e}")
            finally:
                with cond:
                    busy[host] -= 1
                    if broken is not None:
                        if broken not in fatal:
                            fatal.append(broken)
                    elif follow is not None:
                        queues[urlsplit(follow.url).netloc.lower()].append(follow)
                    else:
                        results[job.origin] = outcome
                    cond.notify_all()

    for _ in range(max(1, min(workers, len(unique)))):
        threading.Thread(target=worker, daemon=True).start()
    with cond:
        while len(results) < len(unique) and not fatal:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            cond.wait(left)
        # Close admission first, so no parser call can START from here on. A call
        # already running decides whether the checker itself is broken, so wait it
        # out: each is bounded by its own `timeout`. One that fails ends the run as
        # an error; one still running when the grace ends does too (fail closed).
        closed[0] = True
        cond.notify_all()
        grace = time.monotonic() + timeout + 2
        while parsing[0] and not fatal and time.monotonic() < grace:
            cond.wait(max(0.0, grace - time.monotonic()))
        if fatal:
            raise fatal[0]
        if parsing[0]:
            raise NormalizeError(f"the URL parser was still running {timeout + 2:.0f} s after the budget ran out")
        out = dict(results)
    for u in urls:
        out.setdefault(u, (UNVERIFIED, f"not finished within the {budget:.0f} s budget"))
    return out


def check(url: str, retries: int, wait: float, timeout: float) -> tuple[str, str]:
    """(verdict, reason) for one link; see run_checks."""
    return run_checks([url], retries, wait, timeout)[url]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--retries", type=int, default=3)
    ap.add_argument("--wait", type=float, default=5.0, help="backoff base between attempts (seconds)")
    ap.add_argument("--timeout", type=float, default=15.0, help="per network operation (seconds)")
    ap.add_argument("--budget", type=float, default=900.0,
                    help="whole-run limit; links still unanswered are reported UNVERIFIED (workflow timeout is 20 min)")
    ap.add_argument("--list", action="store_true", help="print the collected links and exit")
    args = ap.parse_args(argv)

    links = collect()
    allow, errors = load_allowlist(ALLOWLIST)
    if args.list:
        for url in sorted(links):
            print(f"{url}  {links[url][0]}")
        print(f"linkcheck: {len(links)} external link(s)", file=sys.stderr)
        return 0
    used = {p for p in allow if any(u.startswith(p) for u in links)}
    todo = [u for u in links if not any(u.startswith(p) for p in allow)]
    try:
        results = run_checks(todo, args.retries, args.wait, args.timeout, budget=args.budget)
    except NormalizeError as e:
        print(f"ERROR: linkcheck cannot run: {e}", file=sys.stderr)
        return 2
    dead = {u: r for u, r in results.items() if r[0] == DEAD}
    unverified = {u: r for u, r in results.items() if r[0] == UNVERIFIED}
    for u in sorted(unverified):
        print(f"UNVERIFIED  {u}  ({unverified[u][1]})  {', '.join(links[u][:3])}")
    for u in sorted(dead):
        print(f"DEAD        {u}  ({dead[u][1]})  {', '.join(links[u][:3])}")
    for p in sorted(set(allow) - used):
        print(f"STALE ALLOW {p}  (matches no link; remove it from {ALLOWLIST.name})")
    for e in errors:
        print(f"ERROR: {e}", file=sys.stderr)
    ok = len(results) - len(dead) - len(unverified)
    print(f"linkcheck: {len(links)} external link(s): {ok} ok, {len(dead)} dead, {len(unverified)} unverified, "
          f"{len(links) - len(todo)} allowlisted", file=sys.stderr)
    return 1 if dead or errors else 0


if __name__ == "__main__":
    sys.exit(main())
