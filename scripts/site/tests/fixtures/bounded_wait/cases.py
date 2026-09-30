"""Control fixture for `check_nets.py --rule bounded-wait` (never imported or run).

Every line marked `# expect: flag` must be reported and every line marked
`# expect: clean` must not; an unmarked line that is reported fails the control.
"""
import subprocess
import subprocess as sp
import threading
import urllib.request
from subprocess import check_output, run as run_it
from urllib.request import build_opener, urlopen

LIMIT = 30
MAYBE = None


def processes(cmd: list, opts: dict) -> None:
    subprocess.run(cmd)  # expect: flag
    subprocess.run(cmd, timeout=None)  # expect: flag
    subprocess.run(cmd, timeout=LIMIT)  # expect: clean
    sp.call(cmd)  # expect: flag
    sp.check_call(cmd, timeout=5)  # expect: clean
    check_output(cmd)  # expect: flag
    run_it(cmd, timeout=MAYBE)  # expect: flag
    subprocess.run(cmd, **opts)  # expect: flag
    subprocess.run(cmd, timeout=None if opts else 5)  # expect: flag
    subprocess.run(cmd,  # expect: flag
                   capture_output=True)


def pipes(cmd: list) -> None:
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)  # expect: flag
    proc.wait()  # expect: flag
    proc.wait(None)  # expect: flag
    proc.wait(10)  # expect: clean
    proc.communicate(b"x")  # expect: flag
    proc.communicate(b"x", 10)  # expect: clean
    proc.communicate(timeout=10)  # expect: clean
    waived = sp.Popen(cmd)  # deadline: communicate(timeout=10) on the next line kills it  # expect: clean
    waived.communicate(timeout=10)


def threads(worker: threading.Thread, cond: threading.Condition, deadline=None, step: float = 1.0) -> None:
    worker.join()  # expect: flag
    worker.join(timeout=5)  # expect: clean
    cond.wait_for(lambda: True)  # expect: flag
    cond.wait_for(lambda: True, 5)  # expect: clean
    left = None if deadline is None else deadline - 1
    cond.wait(left)  # expect: flag
    cond.wait(deadline)  # expect: flag
    cond.wait(step)  # expect: clean
    # deadline: every hand-off notifies this condition, and run() owns the whole-run budget
    cond.wait()  # expect: clean
    ", ".join(["a", "b"])  # expect: clean


def http(url: str, timeout: float) -> bytes:
    urllib.request.urlopen(url)  # expect: flag
    urlopen(url, None, timeout)  # expect: flag
    with urlopen(url, timeout=timeout) as r:  # deadline: read1 loop below checks the run deadline  # expect: clean
        return r.read1(1)


def make_opener():
    return build_opener()


OPENER = make_opener()
DIRECT = urllib.request.build_opener()


def openers(url: str, path) -> None:
    OPENER.open(url, timeout=5)  # expect: flag
    DIRECT.open(url, None, None)  # expect: flag
    OPENER.open(url, timeout=None)  # deadline: even a named owner cannot excuse no socket timeout  # expect: flag
    path.open()  # expect: clean
    open(path)  # expect: clean


def stale() -> None:
    pass  # deadline: nothing on this line waits on anything at all  # expect: flag


def short(cmd: list) -> None:
    subprocess.Popen(cmd)  # deadline: later  # expect: flag


def captured(cmd: list, proc) -> None:
    t = None

    def inner():
        subprocess.run(cmd, timeout=t)  # expect: flag
    waiter = lambda: proc.wait(t)  # expect: flag
    return inner, waiter


T = None


def shadowed(cmd: list) -> None:
    T = 5

    def inner():
        subprocess.run(cmd, timeout=T)  # expect: clean
    return inner


def joins(worker: threading.Thread, parts: list) -> str:
    worker.join(None)  # expect: flag
    return ", ".join(parts)  # expect: clean


class Holder:
    def __init__(self):
        self.opener = urllib.request.build_opener()

    def get(self, url: str):
        return self.opener.open(url, timeout=5)  # expect: flag


def shared_waiver(proc) -> None:
    proc.wait(); proc.communicate()  # deadline: the caller kills this process group at its limit  # expect: flag


def nested_calls(cmd: list) -> None:
    subprocess.run(cmd,  # expect: flag
                   input=check_output(cmd, timeout=None))  # deadline: the inner call only, named here  # expect: clean


def identical_siblings(p, q) -> None:
    p.wait(); p.wait()  # deadline: one comment cannot own two identical waits  # expect: flag
    p.communicate(q.wait())  # deadline: nor an outer call and the wait nested in it  # expect: flag


def walrus_timeouts(cmd: list) -> None:
    subprocess.run(cmd, timeout=(t := None))  # expect: flag
    subprocess.run(cmd, timeout=t)  # expect: flag


def inline_openers(url: str) -> None:
    urllib.request.build_opener().open(url)  # expect: flag
    make_opener().open(url, timeout=5)  # expect: flag
    make_opener().open(url, timeout=5)  # deadline: status only, the caller's budget owns the run  # expect: clean


def unrelated_local_alias() -> None:
    from pathlib import Path as run_it
    run_it("x")  # expect: clean


def module_alias_still_seen(cmd: list) -> None:
    run_it(cmd)  # expect: flag


def joins_by_name(worker: threading.Thread, timeout=None) -> None:
    worker.join(timeout)  # expect: flag
    t = None
    worker.join(t)  # expect: flag
    worker.join(5)  # expect: clean

    def later():
        worker.join(t)  # expect: flag
    return later


def local_opener(url: str) -> None:
    from urllib.request import build_opener as bo
    bo().open(url)  # expect: flag


class Settings:
    import subprocess as proc_mod
    WAIT = None
    proc_mod.run(["x"])  # expect: flag
    subprocess.run(["x"], timeout=WAIT)  # expect: flag

    def method(self, cmd: list) -> None:
        proc_mod.run(cmd)  # expect: clean
        subprocess.run(cmd, timeout=WAIT)  # expect: clean


WAIT_G = None


class Shadowing:
    from pathlib import Path as run_it
    WAIT_G = 5
    results = [run_it(["x"]) for _ in (0,)]  # expect: flag
    waits = [subprocess.run(["x"], timeout=WAIT_G) for _ in (0,)]  # expect: flag

    class Inner:
        run_it(["x"])  # expect: flag


def outer_finite() -> None:
    WAIT_G = 5

    def inner():
        global WAIT_G
        subprocess.run(["x"], timeout=WAIT_G)  # expect: flag
    return inner


def outer_alias() -> None:
    from pathlib import Path as run_it

    def inner():
        global run_it
        run_it(["x"])  # expect: flag
    return inner


T_ALIAS = None
ALIAS = T_ALIAS


def alias_resolves_where_written(cmd: list) -> None:
    T_ALIAS = 5
    subprocess.run(cmd, timeout=ALIAS)  # expect: flag
    return T_ALIAS


def outer_for_class_global() -> None:
    WAIT_G = 5
    from pathlib import Path as run_it

    class Declares:
        global WAIT_G, run_it
        subprocess.run(["x"], timeout=WAIT_G)  # expect: flag
        run_it(["x"])  # expect: flag
    return Declares


T_DEF = None


def default_read_where_defined(cmd: list, timeout=T_DEF) -> None:
    T_DEF = 5
    subprocess.run(cmd, timeout=timeout)  # expect: flag
    return T_DEF


T_CLS = 5


class DefaultFromClassBody:
    T_CLS = None

    def method(self, cmd: list, timeout=T_CLS) -> None:
        subprocess.run(cmd, timeout=timeout)  # expect: flag


def reassigned_parameter(cmd: list, timeout=None) -> None:
    timeout = 5
    subprocess.run(cmd, timeout=timeout)  # expect: flag


T_WAL = 5


def walrus_in_default(cmd: list, timeout=(T_WAL := None)) -> None:
    subprocess.run(cmd, timeout=T_WAL)  # expect: flag


class WalrusInMethodDefault:
    T_W2 = 5

    def method(self, cmd: list, timeout=(T_W2 := None)):
        return T_W2

    subprocess.run(["x"], timeout=T_W2)  # expect: flag


T_ANN = 5


def walrus_in_annotation(arg: (T_ANN := None)) -> None:
    return arg


def uses_annotation_binding(cmd: list) -> None:
    subprocess.run(cmd, timeout=T_ANN)  # expect: flag


def call_in_annotation(arg: subprocess.run(["x"])) -> None:  # expect: flag
    return arg
