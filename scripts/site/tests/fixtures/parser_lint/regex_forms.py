"""Control fixture for scripts/site/check_parsers.py (lint Check 31).

Never imported or run. Each marked line is a regex the check must judge as
marked: `# expect: flag` lines read HTML or URLs and must be reported,
`# expect: clean` lines must not be. The control fails if either goes wrong.
"""

import re
import re as _re
from re import compile as rx, search

A = re.compile(r"<a\s+href=")  # expect: flag
B = re.compile(r"<[Aa]\b[^>]*\b(?:href|src)\s*=\s*[^>]+>")  # expect: flag
C = re.findall(r"\b(?:href|srcset)\s*=\s*\"([^\"]*)\"", "")  # expect: flag
D = _re.compile(r"<(\w+)")  # expect: flag
E = rx(r"^https?://", re.I)  # expect: flag
F = search(r"^[a-z][a-z0-9+.-]*:", "")  # expect: flag
G = re.compile(r"paypal\.com/donate\?id=(\w+)")  # expect: flag
H = re.match(r"mailto:(.*)", "")  # expect: flag
I = re.sub(r"<li>\[ \]", "", "")  # expect: flag
J = re.compile(r"(?P<scheme>[a-z]+)\:\/\/")  # expect: flag
K = re.compile(r"<!--.*?-->", re.S)  # expect: flag

_TAG = r"<([A-Za-z][\w:-]*)"
_ATTRS = r"(\s+[^\s=>]+)*"
L = re.compile(_TAG + _ATTRS + r">")  # expect: flag


class Holder:
    _VAL = r"(?:\"[^\"]*\"|[^\s>]+)"
    ATTR = re.compile(r"(\s+)(src)=" + _VAL)  # expect: flag


M = re.compile(r"(?<!`)\{\{([a-z_]+)\}\}")  # expect: clean
N = re.compile(r"(?P<name>[a-z]+)=(?P<value>\d+)")  # expect: clean
O = re.match(r"\d{4}-\d{2}-\d{2}(T\d{2}:\d{2})?", "")  # expect: clean
P = re.split(r"\s+(?=[a-z_]+=)", "")  # expect: clean
Q = re.compile(r"docs/design/([a-z0-9_-]+\.md)(?:#([A-Za-z0-9_-]+))?")  # expect: clean
R = re.compile(r"src/kernel/[a-z_]+\.c")  # expect: clean
S = re.compile(r"a\s+<\s+b")  # expect: clean
T = re.compile(r"<!-- keep -->")  # parser-allow: a fixture waiver with a long enough reason  # expect: clean
U = re.compile(r"<b>")  # parser-allow: short  # expect: flag
V = 1  # parser-allow: a waiver on a line with no regex is stale  # expect: flag
W = "<a href=x>"  # a string that no regex reads  # expect: clean
# parser-allow: a standalone waiver covers the statement on the next line
X = re.compile(r"<i>")  # expect: clean
Y = 2  # parser-allow: a trailing waiver never reaches the next line  # expect: flag
Z = re.compile(r"<u>")  # expect: flag
AA = re.compile(r'\bid="([^"]*)"')  # expect: flag
AB = re.compile(r"\bclass\s*=\s*['\"]([^'\"]*)")  # expect: flag
AC = re.compile(r"(?<=<)a\b[^>]*>")  # expect: flag
_OPEN = "<a"
AD = re.compile(rf"{_OPEN}\b[^>]*>")  # expect: flag
AE = re.compile(f"{_OPEN}")  # expect: flag
AF = re.compile(r"covers=(\S+)")  # expect: clean
AG = re.compile(r"(?P<key>[a-z_]+)=(?P<value>[^\s]+)")  # expect: clean
AH = re.compile(rf"{'x'}\d+")  # expect: clean
AI = re.compile(r"<\s*(\w+)[^>]*>")  # expect: flag
AJ = re.compile(r"<\s*/?\s*[a-z][^>]*>")  # expect: flag
AK = re.compile(r"<\s*img\b[^>]*>")  # expect: flag
AL = re.compile(r"x < \d+")  # expect: clean
