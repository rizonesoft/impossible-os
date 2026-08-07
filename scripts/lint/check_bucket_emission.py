#!/usr/bin/env python3
"""Read the resolver's DECLARED emitted-bucket set, and prove it is the truth.

WHY (section 20, replacing section 18's evidence). `identity-gate.sh` adjudicates
bucket RETIREMENT, and the only proof it had that a bucket is no longer produced
was a SOURCE-TEXT search for the quoted literal in the emitters. Text absence is
not proof of non-emission: an executable-only refactor to `PRE_RESOLUTION_BUCKETS[1]`,
a concatenation, or a table lookup preserves behaviour and leaves the literal
absent -- so a two-commit sequence (refactor to indirect emission, then a
data-only retirement) passed the search while the resolver could still emit a
name the published vocabulary no longer declares. Every active retirement was
therefore refused outright, which made the gate a wedge rather than a gate.

This replaces the EVIDENCE, not the rule. The rule -- retire only when the
emitters can no longer produce the name -- is unchanged; what changes is that
the question is now answered from a DECLARATION instead of a text search.

WHAT IS CHECKED. `ref_resolution.py` declares one module-level

    EMITTED_BUCKETS = frozenset({Bucket.UNPAIRED_REF, ..., Bucket.MISSING_FILE})

and this checker pins that exact AST shape: assigned once, at module level,
never rebound or mutated, every element a plain `Bucket.<MEMBER>` attribute, and
every member present in the inert `snapshot_protocol.json` vocabulary. That is
what makes the emitted set a STATIC property, readable without executing the
resolver and without inferring anything from source text.

WHY THE SET IS DECLARED RATHER THAN DISCOVERED. Collecting every `Bucket.<MEMBER>`
access in the file was the first design and was rejected in design review as
bypassable: `next(iter(Bucket))`, or an alias `B = Bucket` followed by
`B.PATH_ESCAPE`, emits a bucket while the file holds no literal, no subscript,
no dynamic construction and no `Bucket.<MEMBER>` expression at all. The
discovered set would then omit a member the resolver can still emit, and a later
retirement would recreate the exact latent failure this work exists to close.

WHY A STATIC CHECK IS NOT CLAIMED TO BE SUFFICIENT ON ITS OWN. No AST rule can
enumerate every way to compute a string. The static half is paired with a
RUNTIME half -- `ref_resolution._check_emitted`, called from both outcome
constructors -- which refuses any bucket that is not a `Bucket` member of
EMITTED_BUCKETS. The runtime half makes the declaration TRUE; this checker makes
it READABLE. Neither is load-bearing alone, and the retirement proof needs both.

The bans below are therefore defence in depth over a runtime guarantee, not a
substitute for one. They keep the source honest so a reader (and the gate) can
trust the declaration at face value.

Exit codes mirror the sibling gates:
    0  the contract holds
    1  a VIOLATION: the declaration is missing, malformed, or contradicted
    2  usage error
    3  INFRASTRUCTURE: the check could not run. NEVER read as a pass, and in
       particular never read as proof that a bucket is absent -- an unreadable
       or unparseable emitter is the fail-open shape fixture 22af exists to
       close.
"""
from __future__ import annotations

import argparse
import ast
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
EMITTER = REPO_ROOT / "scripts/todo-graph/ref_resolution.py"
PROTOCOL = REPO_ROOT / "scripts/todo-graph/snapshot_protocol.json"

DECLARATION = "EMITTED_BUCKETS"
ENUM_NAME = "Bucket"
# Names that hold the vocabulary as an ordered sequence. Indexing any of them
# reaches a bucket without naming it, which is one of the two shapes section 20
# was written against.
_BUCKET_SEQUENCES = ("PRE_RESOLUTION_BUCKETS", "POST_RESOLUTION_BUCKETS",
                     "ALL_BUCKETS", DECLARATION, ENUM_NAME)
# The outcome types whose construction the runtime guard bounds. Their
# namedtuple copy/factory helpers do NOT route through the checked `__new__`.
_OUTCOME_TYPES = ("Verdict", "RefResult")

EXIT_OK, EXIT_VIOLATION, EXIT_USAGE, EXIT_INFRA = 0, 1, 2, 3


class ContractInfra(RuntimeError):
    """Infrastructure: the check itself could not run."""


def load_vocabulary(path: Path) -> tuple:
    """The declared bucket names, read as DATA. Never imports the protocol."""
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, RecursionError) as exc:
        raise ContractInfra(f"cannot read the vocabulary {path}: {exc}") from exc
    if not isinstance(raw, dict):
        raise ContractInfra(f"{path.name}: top level is not an object")
    names = []
    for field in ("pre_resolution_buckets", "post_resolution_buckets"):
        half = raw.get(field)
        if not isinstance(half, list):
            raise ContractInfra(f"{path.name}: {field!r} is not a list")
        names.extend(v for v in half if isinstance(v, str) and v)
    if not names:
        raise ContractInfra(f"{path.name}: declares no buckets at all")
    return tuple(names)


def _parse(path: Path) -> ast.Module:
    try:
        return ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    except (OSError, SyntaxError, ValueError, RecursionError) as exc:
        # RAISE, never return an empty tree. The caller's question is "can this
        # emitter still produce bucket X", and an empty tree answers "no" -- so
        # a swallowed parse error would read as PROOF OF ABSENCE and approve a
        # retirement. That inversion is the same one fixture 22af pins for an
        # unreadable emitter.
        raise ContractInfra(f"cannot parse {path}: {exc}") from exc


def _docstring_nodes(tree: ast.Module) -> set:
    """ids of the Constant nodes that are docstrings.

    Docstrings are exempt from the literal ban: the resolver documents its
    buckets by name in prose, and flagging that would be a lint that fails
    correct code -- the failure mode this repo removed a `parked-ownerless`
    detector and a consumer-side bucket-literal ban for.
    """
    out = set()
    for node in ast.walk(tree):
        if not isinstance(node, (ast.Module, ast.FunctionDef,
                                 ast.AsyncFunctionDef, ast.ClassDef)):
            continue
        body = getattr(node, "body", None) or []
        if (body and isinstance(body[0], ast.Expr)
                and isinstance(body[0].value, ast.Constant)
                and isinstance(body[0].value.value, str)):
            out.add(id(body[0].value))
    return out


def _folded_strings(node: ast.AST):
    """Every string this expression evaluates to WITHOUT running anything.

    Constant-folds `+` chains and f-strings built purely from constants, so the
    concatenation shape (`"path" + "_escape"`) cannot hide a bucket name from
    the literal ban.
    """
    if isinstance(node, ast.Constant):
        if isinstance(node.value, str):
            yield node.value
        return
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        left = list(_folded_strings(node.left))
        right = list(_folded_strings(node.right))
        for a in left:
            for b in right:
                yield a + b
        return
    if isinstance(node, ast.JoinedStr):
        parts = []
        for v in node.values:
            if isinstance(v, ast.Constant) and isinstance(v.value, str):
                parts.append(v.value)
            else:
                return          # a runtime piece: not statically a bucket name
        yield "".join(parts)


def _name_of(node: ast.AST):
    """`X` for a bare name, `X` for `mod.X`; None for anything else."""
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        return node.attr
    return None


def declaration_members(tree: ast.Module, path: Path) -> list:
    """The `Bucket.<MEMBER>` members of the single EMITTED_BUCKETS assignment.

    Returns (members, violations). A malformed declaration yields NO members --
    the caller must treat that as a violation and never as an empty emitted set,
    because an empty set would approve every retirement at once.
    """
    violations = []
    assignments = []
    for node in ast.walk(tree):
        targets = []
        if isinstance(node, ast.Assign):
            targets = node.targets
        elif isinstance(node, (ast.AnnAssign, ast.AugAssign)):
            targets = [node.target]
        for t in targets:
            if isinstance(t, ast.Name) and t.id == DECLARATION:
                assignments.append((node, t))
        # A rebind through any other route is equally fatal to the declaration.
        if isinstance(node, ast.Delete):
            for t in node.targets:
                if isinstance(t, ast.Name) and t.id == DECLARATION:
                    violations.append(
                        f"{path.name}:{node.lineno}: `{DECLARATION}` is deleted; "
                        f"the declaration must be a single immutable binding")

    top_level = {id(n) for n in tree.body}
    if len(assignments) != 1:
        violations.append(
            f"{path.name}: expected exactly ONE assignment to `{DECLARATION}`, "
            f"found {len(assignments)} "
            f"(lines {[n.lineno for n, _ in assignments] or 'none'}). The "
            f"emitted-bucket set is the retirement proof, so it must have one "
            f"unambiguous definition.")
        return [], violations

    node, _target = assignments[0]
    if isinstance(node, ast.AugAssign):
        violations.append(
            f"{path.name}:{node.lineno}: `{DECLARATION}` is built by augmented "
            f"assignment; the declaration must be one literal frozenset so it "
            f"can be read without executing the module")
        return [], violations
    if id(node) not in top_level:
        violations.append(
            f"{path.name}:{node.lineno}: `{DECLARATION}` is assigned inside a "
            f"nested scope; a conditional or function-local declaration is not "
            f"a static property")
        return [], violations

    value = node.value
    if not (isinstance(value, ast.Call)
            and _name_of(value.func) == "frozenset"
            and len(value.args) == 1
            and not value.keywords
            and isinstance(value.args[0], (ast.Set, ast.List, ast.Tuple))):
        violations.append(
            f"{path.name}:{node.lineno}: `{DECLARATION}` must be "
            f"`frozenset({{{ENUM_NAME}.MEMBER, ...}})` with a literal element "
            f"set. A comprehension, a splat, or a value derived from another "
            f"collection would make the emitted set depend on runtime state, "
            f"which is exactly the inference this check exists to avoid.")
        return [], violations

    members = []
    for elt in value.args[0].elts:
        if (isinstance(elt, ast.Attribute) and isinstance(elt.value, ast.Name)
                and elt.value.id == ENUM_NAME):
            members.append(elt.attr)
        else:
            violations.append(
                f"{path.name}:{getattr(elt, 'lineno', node.lineno)}: every "
                f"element of `{DECLARATION}` must be a plain "
                f"`{ENUM_NAME}.MEMBER` attribute; got "
                f"{ast.dump(elt)[:80]}")
    return members, violations


def undeclared_references(tree: ast.Module, path: Path, declared) -> list:
    """Every `Bucket.<MEMBER>` outside the declaration must be DECLARED.

    WITHOUT THIS THE DECLARATION IS A LIST, NOT A CONTRACT. Removing a member
    from `EMITTED_BUCKETS` while leaving its emission sites in place produced
    ZERO violations (Codex adversarial, section 20 round 2): the resolver still
    named the bucket at four sites, but the retirement gate reads only the
    declaration, so the following data-only retirement was approved. The stale
    branch then raises the first time a ref reaches it -- `BucketContractError`
    while the name is still in the vocabulary, `AttributeError` once the enum
    drops it -- taking out both real consumers. That is exactly the dormant
    latent failure this section exists to close, wearing a new shape.

    The clearing-path fixture could not have caught it: it retires a bucket that
    was never emitted anywhere, so no site was left behind to go stale.
    """
    out = []
    declared_members = {name.upper() for name in declared}
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Attribute)
                and isinstance(node.value, ast.Name)
                and node.value.id == ENUM_NAME):
            continue
        if node.attr in declared_members:
            continue
        out.append(
            f"{path.name}:{node.lineno}: `{ENUM_NAME}.{node.attr}` is named "
            f"here but is NOT in `{DECLARATION}`. Either declare it, or remove "
            f"this emission site -- a site the declaration does not cover is "
            f"how a retired bucket keeps a live code path.")
    return out


def emission_bans(tree: ast.Module, path: Path, vocabulary) -> list:
    """Shapes that would reach a bucket without naming its member."""
    out = []
    exempt = _docstring_nodes(tree)
    vocab = set(vocabulary)

    for node in ast.walk(tree):
        # (a) a bucket name spelled as a string, or built by folding constants.
        if isinstance(node, (ast.Constant, ast.BinOp, ast.JoinedStr)):
            if id(node) in exempt:
                continue
            for text in _folded_strings(node):
                if text in vocab:
                    out.append(
                        f"{path.name}:{node.lineno}: the bucket name {text!r} "
                        f"appears as a string; name it as "
                        f"`{ENUM_NAME}.{text.upper()}` so the declared emitted "
                        f"set stays the single readable record of what this "
                        f"module can produce")
                    break
        # (b) indexing the vocabulary reaches a bucket without naming it. This
        #     is the shape section 18's text search could not see.
        if isinstance(node, ast.Subscript):
            seq = _name_of(node.value)
            if seq in _BUCKET_SEQUENCES:
                out.append(
                    f"{path.name}:{node.lineno}: `{seq}` is indexed; an indexed "
                    f"emission produces a bucket that no declaration mentions, "
                    f"which is precisely what defeated the source-text proof")
        # (c) namedtuple construction routes that skip the checked `__new__`.
        #     A probe drove a retired bucket through every one of these (Codex
        #     adversarial, section 20). `_make` is overridden on both outcome
        #     types, but banning the shapes keeps a future refactor from
        #     reaching for `tuple.__new__` -- the one route Python cannot take
        #     away -- and documents why the overrides exist.
        if isinstance(node, ast.Attribute) and node.attr in (
                "_make", "_replace", "_asdict", "_fields_defaults"):
            owner = _name_of(node.value)
            if owner in _OUTCOME_TYPES:
                out.append(
                    f"{path.name}:{node.lineno}: `{owner}.{node.attr}` bypasses "
                    f"or copies an outcome without going through the checked "
                    f"constructor; build the outcome directly")
        if isinstance(node, ast.Call):
            fname = _name_of(node.func)
            if (fname == "__new__" and isinstance(node.func, ast.Attribute)
                    and _name_of(node.func.value) in ("tuple", "object")):
                out.append(
                    f"{path.name}:{node.lineno}: `tuple.__new__` constructs an "
                    f"outcome without the bucket check; this is the one route "
                    f"the runtime guard cannot close, so it is banned here")
            if fname == ENUM_NAME:
                out.append(
                    f"{path.name}:{node.lineno}: `{ENUM_NAME}(...)` constructs a "
                    f"member from a value; write the member literally")
            if fname == "getattr" and node.args and _name_of(
                    node.args[0]) == ENUM_NAME:
                out.append(
                    f"{path.name}:{node.lineno}: `getattr({ENUM_NAME}, ...)` "
                    f"reaches a member by computed name; write it literally")
    return out


def check(emitter: Path, protocol: Path, allow_undeclared: bool = False):
    """(declared_members, violations). Raises ContractInfra if it cannot run."""
    vocabulary = load_vocabulary(protocol)
    tree = _parse(emitter)
    members, violations = declaration_members(tree, emitter)
    violations = list(violations)

    known = set(vocabulary)
    declared = []
    for member in members:
        # RECOVERED FROM THE MEMBER, not looked up in the vocabulary. The
        # identity gate reads this set from a tree MID-RETIREMENT, where the
        # name has left `snapshot_protocol.json` while the resolver still
        # declares the member -- which is exactly the state it must be able to
        # REFUSE. Resolving through the vocabulary made that state unreadable
        # and the gate reported "could not run" for what is really an invalid
        # migration; the member/name mapping is bijective (`snapshot_protocol`
        # rejects a non-lowercase bucket) so no lookup is needed.
        name = member.lower()
        if name not in known and not allow_undeclared:
            violations.append(
                f"{emitter.name}: `{DECLARATION}` names `{ENUM_NAME}.{member}`, "
                f"which the published vocabulary does not declare. A typo, or a "
                f"retirement performed in the wrong order: the member must be "
                f"removed from the emitted set BEFORE the name leaves "
                f"{protocol.name}.")
            continue
        declared.append(name)
    if len(set(declared)) != len(declared):
        violations.append(
            f"{emitter.name}: `{DECLARATION}` lists a bucket more than once")

    violations.extend(emission_bans(tree, emitter, vocabulary))
    # ONLY MEANINGFUL WITH A WELL-FORMED DECLARATION. A malformed one yields no
    # members, which would report every emission site as undeclared and bury
    # the real finding under noise.
    if members:
        violations.extend(undeclared_references(tree, emitter, declared))
    return sorted(set(declared)), violations


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Verify the resolver's declared bucket-emission contract.")
    ap.add_argument("--emitter", default=str(EMITTER),
                    help="the resolver whose emitted set is declared")
    ap.add_argument("--protocol", default=str(PROTOCOL),
                    help="the inert vocabulary file")
    ap.add_argument("--emitted-set", action="store_true",
                    help="print the declared emitted set as JSON on stdout "
                         "(the identity gate's retirement proof)")
    ap.add_argument("--allow-undeclared", action="store_true",
                    help="do not treat a declared member that is absent from "
                         "the vocabulary as a violation. For the identity "
                         "gate, which must READ the emitted set of a tree "
                         "mid-retirement in order to refuse it; lint keeps the "
                         "check, where a coherent tree is the expectation.")
    try:
        args = ap.parse_args(argv)
    except SystemExit:
        return EXIT_USAGE

    try:
        declared, violations = check(Path(args.emitter), Path(args.protocol),
                                     allow_undeclared=args.allow_undeclared)
    except ContractInfra as exc:
        sys.stderr.write(
            f"[check_bucket_emission] INFRASTRUCTURE: {exc}\n"
            f"  This is NOT evidence that any bucket is unemitted. A check that "
            f"could not run must never be read as a pass.\n")
        return EXIT_INFRA

    if violations:
        for v in violations:
            sys.stderr.write(f"[check_bucket_emission] {v}\n")
        sys.stderr.write(
            f"[check_bucket_emission] The declared emitted set is what proves a "
            f"retired bucket can no longer be produced; while these hold it "
            f"proves nothing.\n")
        return EXIT_VIOLATION

    if args.emitted_set:
        sys.stdout.write(json.dumps(declared) + "\n")
    else:
        sys.stderr.write(
            f"bucket-emission:contract ok declared={len(declared)} "
            f"({', '.join(declared)})\n")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
