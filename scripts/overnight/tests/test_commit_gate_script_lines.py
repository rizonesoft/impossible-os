#!/usr/bin/env python3
"""section_commit_gate reads a multi-line script per command, not as one commit.

v22, live 2026-11-07: a call holding `git commit -q -F f` and a later `grep -n` was
refused as `git commit --no-verify (or -n)`, because shlex treats an unquoted
newline as whitespace.
"""
import pathlib
import sys

HOOKS = pathlib.Path(__file__).resolve().parents[3] / ".claude/hooks"
sys.path.insert(0, str(HOOKS))
import section_commit_gate as gate  # noqa: E402

gate._ensure_crc()

CLEAN = (
    "git commit -q -F f\ngrep -n x y",
    "git commit -q -F f;grep -n x y",
    "git commit -q -F f && grep -n x y",
    "git commit -F - <<'E'\nsubject\n\n-n\nE\ngrep -n x y",
    "git commit -m 'one\n-n\ntwo' -- a.md",
    "git commit -mn -- a.md",
    "git commit -m x -- -n",           # after `--`, a pathspec
    "git commit --end-of-options -n",
    "git -c alias.ci='commit -m' ci -n",  # the alias's -m takes the command's -n
    "git commit -qa -m x",
    "cat > f <<'E'\nit's -n\nE\ngit commit -F f",
)
REFUSED = (
    "/usr/bin/env git commit -n -m x",  # a wrapper by path (env runs the binary, not a shim)
    "git commit -n -F f",
    "git commit --no-verify -F f",
    "git commit -qn -F f",
    "git commit -an -m x",
    "git commit -nm x",
    "git -c alias.ci='commit -n' ci -m x",
    "git commit \\\n  -n -F f",
    "ls\ngit commit -n -m x",
    "git commit -q -F f\ngit commit -n -m x",
    "cat > f <<'E'\nit's\nE\ngit commit -n -F f",
    "bash -c 'git commit -n -m x'",
    "git commit -m 'a\nb' -n",
    # Review, 2026-11-07, rounds 1 and 2: inputs an earlier version hid by dropping text.
    "echo '<<EOF' ;\ngit commit -n -m x",
    "echo $((1<<x))\ntrue ; git commit -n -m x\nx\n",
    "echo foo\\ # hi ; git commit -n -m x",
    "cat <<A <<B\nbody\nA\nbody\nB\ngit commit -n -m x",
    "# don't\ngit commit -n -m x",
    # Round 3: a separator inside a substitution does not end the commit.
    "git commit -m x$(printf a;printf b) -n",
    "git commit -m x$(printf a\nprintf b) -n",
    "git commit -m x`printf a;printf b` -n",
    "git commit -m x${y:-;} -n",
    # Round 5: an inline comment can hold a quote, so a `#` keeps HEAD's reading.
    "git commit -q # '\ngit commit -n -m x",
    # Round 4: an inner quote is not the outer quote's end.
    'git commit -m "x$(printf "a;b")" -n',
    'git commit -m "x$(printf "a\nb")" -n',
)


def test_other_commands_lend_no_flag():
    for cmd in CLEAN:
        assert gate._bash_is_git_commit(cmd) == (True, False), cmd


def test_refusal_a_real_no_verify_is_still_seen():
    for cmd in REFUSED:
        assert gate._bash_is_git_commit(cmd) == (True, True), cmd


def test_no_commit_is_no_commit():
    assert gate._bash_is_git_commit("grep -n x y\nls -n") == (False, False)


# A script shlex cannot read (automation hardening, untokenizable commit line). Each of
# these is a real commit carrying -n that an earlier reading passed as no -n or no commit.
BASH_QUOTED_NO_VERIFY = (
    "git commit -m $'a\\';b' -n",                          # the section's own example
    "git commit -m $'it\\'s\nbody' -n",                   # an ANSI-C quote spanning lines
    "git -C 'some dir' commit -m $'it\\'s' -n",            # a quoted global-option value
    "env FOO=bar git commit -m $'it\\'s' -n",              # a wrapper and its argument
    "ls; git -C ../r commit -n -m $'q\\''",                # after a separator
    'git commit -m "$(cat f)" -n',                         # a substitution is a word, not the end
    "git commit -m \"$(printf '%s' \"it's\")\" -n",         # quotes nested in a substitution
    "x=$(git commit -n -m y)",                             # a commit inside a substitution runs
    "echo `git commit -n -m y`",
    "if git commit -n -m x; then :; fi",
    "{ git commit -n -m x; }",
    "(cd r && git commit -n -m x)",
    # Round review: each of these was read as clean or as no commit.
    'git commit -n -m x\necho "unterminated',             # a later syntax error: line 1 ran
    "cat <<EOF\n$(git commit -n -m x)\nEOF",             # an unquoted heredoc body expands
    "echo $(( $(git commit -n -m x) + 1 ))",              # inside arithmetic
    "echo ${x:-$(git commit -n -m x)}",                    # inside a parameter expansion
    "git commit -m x $'\\x2dn'",                          # ANSI-C hex escape for -n
    "git commit -m x $'\\055n'",                          # ANSI-C octal escape for -n
    "git commit>/dev/null -n -m x",                        # an attached redirection
    ">/dev/null git commit -n -m x",                       # a leading redirection
    "git commit 2>&1 -n -m x",
    "git commit &>log -n -m x",
    "git commit <<<x -n -m y",
    # Second round review.
    'git commit -m "$(printf %s "$(printf ")")")" -n',      # quotes nested in a quoted substitution
    "cat <<EOF\n$(\ngit commit -n -m x\n)\nEOF",           # a substitution spanning heredoc lines
    "cat <<$'EOF'\ntext\nEOF\ngit commit -n -m x",         # an ANSI-C quoted delimiter
    "git commit -m x $'--no-verify\\0suffix'",             # a NUL ends the fragment's text
    "echo " + "${x:-" * 500 + "$(git commit -n -m x)" + "}" * 500,  # deep nesting
    # Third round review.
    "cat <<$'E\\x4fF'\ntext\nEOF\ngit commit -n -m x",       # an escape in an ANSI-C delimiter
    "git commit -m x $'\\0'-n",                            # a NUL ends only its own fragment
    "echo " + "${x:-" * 30 + "$(git commit -m x $'\\x2dn')" + "}" * 30,  # too deep: refused
    # Fourth round review.
    "cat <<EOF\nE\\\nOF\ngit commit -n -m x",               # a continued delimiter line
    'cat <<"E\\"F"\ntext\nE"F\ngit commit -n -m x',          # an escaped quote in a delimiter
    "git commit -m $'\\c\\\\' -n",                         # \c\\ is one character
    "git commit -m \"$(echo # '\n)\" -n",                    # a comment in a substitution
    "echo " + "${x:-" * 30 + "$(git c'ommit' -n -m x)" + "}" * 30,  # too deep, quoted word
    # Seventh round review.
    "cat <<\\\nEOF\nbody\nEOF\ngit commit -n -m x",            # a continued delimiter word
    "git commit -m $'\\x3b' -n",                             # a decoded ; is an argument
    "git commit -m ';' -n",                                   # so is a quoted one
    # Eighth round review.
    'cat <<"E\\\nOF"\nbody\nEOF\ngit commit -n -m x',            # continued in a quoted delimiter
    'bash -c "git commit -m \';\' -n"',                         # a quoted ; inside bash -c
    "eval \"git commit -m \\$'\\\\x3b' -n\"",                   # and inside eval
    # Ninth round review.
    "bash <<'EOF'\ngit commit -n -m x\nEOF",                  # a heredoc a shell runs
    "FOO=1 bash -s <<EOF\ngit commit -n -m x\nEOF",           # with flags and an assignment
    # Tenth round review.
    "bash 0<<'EOF'\ngit commit -n -m x\nEOF",                 # fd 0 named
    "bash -s -- arg <<'EOF'\ngit commit -n -m x\nEOF",        # -s with positional arguments
    # Eleventh round review.
    "command bash <<'EOF'\ngit commit -n -m x\nEOF",          # a wrapped shell
    "bash +x <<'EOF'\ngit commit -n -m x\nEOF",               # a plus option
    # Twelfth round review.
    "env -u UNUSED bash <<'EOF'\ngit commit -n -m x\nEOF",    # a wrapper's option value
    "nice -n 5 bash <<'EOF'\ngit commit -n -m x\nEOF",
    "/usr/bin/env bash <<'EOF'\ngit commit -n -m x\nEOF",     # a wrapper by path
    # Fourteenth round review.
    'git commit -m x $"-n"',                                  # a localized double quote
    "bash 3<<'EOF' <&3\ngit commit -n -m x\nEOF",             # a heredoc reached through fd 3
    # Fifteenth round review.
    "bash 3<<'EOF' <& 3\ngit commit -n -m x\nEOF",            # with a space
    "bash 3<<'EOF' 4<&3 <&4\ngit commit -n -m x\nEOF",        # through a second duplicate
    # Sixteenth round review.
    "bash 3<<'EOF' <&3-\ngit commit -n -m x\nEOF",            # a descriptor move
    "bash 3<<'EOF' 4<&3- <&4\ngit commit -n -m x\nEOF",
    "bash 3<<'EOF' <&\"3\"\ngit commit -n -m x\nEOF",          # a quoted source
    # Seventeenth round review.
    "bash {fd}<<'EOF' <&$fd\ngit commit -n -m x\nEOF",         # a named descriptor
    # Eighteenth round review.
    "bash <<< 'git commit -n -m x'",                          # a here-string a shell runs
    "bash -s <<< 'git commit -n -m x'",
    "bash <<< 'git commit -n -m x'; git commit -m ordinary",  # hidden before an ordinary commit
    # Nineteenth round review.
    "bash <<EOF\ngit commit \\\\-n -m x\nEOF",                 # two expansions, two backslashes
    "bash <<EOF\ngit commit \\$'\\x2dn' -m x\nEOF",              # an escaped dollar quote
    # Twentieth round review.
    "timeout 5 bash <<'EOF'\ngit commit -n -m x\nEOF",        # the commit reader's wrappers
    "timeout -k 1 5 bash <<< 'git commit -n -m x'",
    'timeout 5 bash -c "git commit -n -m x"',
    # Twenty-first round review.
    "git commit -m \"${x:-'$(git commit -n -m y)'}\"",        # in "...", the quote is text
    # Twenty-second round review.
    "git commit -m \"${x:-${y:-'$(git commit -n -m z)'}}\"",  # nested, still in "..."
    "echo ${x:-\"'$(git commit -n -m z)'\"}",                  # "..." opened inside ${...}
    # Twenty-fifth round review.
    "declare -A x=(); git commit -m \"${x[']#']:-'$(git commit -n -m inner)'}\"",  # a quoted ]
    # Twenty-sixth round review.
    "x=(abc); git commit -m \"${x['$(git commit -n -m i)']#a}\"",  # an indexed subscript runs
    # Twenty-seventh round review.
    "a=(x); echo ${#a['$(git commit -n -m x)0']}",            # a length of an element
    # Twenty-eighth round review.
    "a=abc; git commit -m ${a:'$(git commit -n -m inner)0'}",  # a substring offset
    "a=abc; git commit -m ${a:0:'$(git commit -n -m inner)1'}",  # and its length
    # Twenty-ninth round review.
    'git commit -m "$(case x in x) git commit -n -m inner;; esac)"',  # a case pattern's )
    'git commit -m "$(if true; then case x in (x) git commit -n -m i;; esac; fi)"',
    # Thirtieth round review.
    'git commit -m "$(echo # )\ngit commit -n -m inner\n)"',  # a comment's )
    "git commit -m \"$(cat <<'E'\n)\nE\ngit commit -n -m inner\n)\"",  # a heredoc's )
    # Thirty-first round review.
    "git commit -m \"$(case x in y) :;; 'esac') :;; x) git commit -n -m inner;; esac)\"",
    'git commit -m "$(case x in case) :;; x) git commit -n -m i;; esac)"',  # a pattern word
    # Thirty-second round review.
    'git commit -m "$(case esac in y|esac) git commit -n -m inner;; esac)"',  # esac after |
    'git commit -m "$(case esac in (esac) git commit -n -m inner;; esac)"',  # and after (
    # Thirty-third round review: the commit in a LATER clause.
    'git commit -m "$(case x in y|esac) :;; x) git commit -n -m inner;; esac)"',
    'git commit -m "$(case x in (esac) :;; x) git commit -n -m inner;; esac)"',
    'git commit -m "$(case x in y|case) :;; x) git commit -n -m inner;; esac)"',
    # Thirty-fourth round review (extglob patterns; the oracle runs bash -O extglob too).
    'shopt -s extglob\ngit commit -m "$(case x in @(y|z)) :;; x) git commit -n -m inner;; esac)"',
    'shopt -s extglob\ngit commit -m "$(case x in !(*(x)|+(z))) :;; x) git commit -n -m i;; esac)"',
    # Thirty-fifth round review.
    'shopt -s extglob\ngit commit -m "$(case x in @($(git commit -n -m inner))) echo ok;; esac)"',
    # Thirty-sixth round review.
    "shopt -s extglob\ngit commit -m @(<(git commit -n -m inner))",
    "shopt -s extglob\ngit commit -m @(>(git commit -n -m inner))",
    # Thirty-seventh round review.
    "shopt -s extglob\ngit commit -m @(#$(git commit -n -m inner))",  # `#` is pattern text
    "shopt -s extglob\ngit commit -m @(x|#$(git commit -n -m inner))",
    # Thirty-eighth round review.
    "a['$(git commit -n -m inner)']=x; git commit -m ordinary",  # an assignment subscript
    "a=(1); unset 'a[$(git commit -n -m inner)]'",
    # Thirty-ninth round review.
    "a=(1); unset a'[$(git commit -n -m inner)]'; git commit -m ordinary",  # split fragments
    # Fortieth round review.
    "a=(1); unset 'a[$''(git commit -n -m inner)]'; git commit -m ordinary",  # `$` + `(`
    # Forty-first round review.
    "a=(1); unset 'a[\\]$(git commit -n -m inner)]'; git commit -m ordinary",  # an escaped ]
    # Forty-second round review.
    "a['x]$(git commit -n -m x)']=1",                         # a quoted ]
    # Forty-third round review.
    "a=( ['$(git commit -n -m inner)']=x ); git commit -m ordinary",  # a compound element
    "a=( ['$(git commit -n -m inner)']=x )",
    # Forty-fourth round review.
    "echo `git commit \\\\-n -m x`",                          # a backtick removes one backslash
    "echo `gi\\\\t commit -n -m x`",
    # Forty-fifth round review.
    'echo "`git commit \\"-n\\" -m x`"',                     # an escaped " in "`...`"
    # Forty-sixth round review: a heredoc body is not double quotes.
    'cat <<EOF\n`git commit --allow-empty -m \\"subject -n #\\"`\nEOF',
    # Forty-seventh round review: "..." inside arithmetic quotes.
    'echo $(( "`git commit \\"-n\\" -m x`" + 0 ))',
    # Forty-eighth round review: and in the other arithmetic contexts.
    'a=abc; echo ${a:"`git commit \\"-n\\" -m x`"}',
    'a=abc; echo ${a:0:"`git commit \\"-n\\" -m x`"}',
    'a=(abc); echo ${a["`git commit \\"-n\\" -m x`"]}',
    # Completion pass: an arithmetic command's `<<` is a shift, not a heredoc; and git
    # reads its options as parse-options does (a unique prefix, the last of -n/--verify).
    "(( x = 1 << 2 ))\ngit commit -n -m x",
    "for (( i=0; i < 1<<1; i++ )); do git commit -n -m x; done",
    "git commit --no-veri -m x",
    "git commit --verify -n -m x",
    "git commit -m x file -n",
)
BASH_QUOTED_PLAIN = (
    # Completion pass: a value option takes its value, whatever it reads.
    "(( x = 1 << 2 ))\ngit commit -m x",
    'git commit -m "- new feature"',
    "git commit -m '-n'",
    "git commit --mess -n",
    "git commit --message=-n",
    "git commit -n --verify -m x",
    "git commit -Sn -m x",
    'a=abc; echo ${a:"`git commit -m \\"subject -n body\\"`"}',
    'echo $(( "`git commit -m \\"subject -n body\\"`" + 0 ))',
    'echo "`git commit -m \\"subject -n body\\"`"',           # one message argument
    "echo '[review-kind: x] $( backtick git commit -n'; git commit -m ordinary",  # no name[
    "shopt -s extglob\ngit commit -m @('<(git commit -n)')",  # quoted, inert
    "shopt -s extglob\ngit commit -m \"$(case x in @('$(git commit -n)')) :;; x) echo ok;; esac)\"",
    "function case { printf ok; }; git commit -m \"$('case')\"",  # a quoted word is a command
    'git commit -m "$(case=foo; echo ok)"',                   # an assignment, not a keyword
    'case-tool() { echo ok; }; git commit -m "$(case-tool x)"',  # nor a command name
    'git commit -m "$(echo case; echo esac)"; echo "$(case x in x) echo ok;; esac) -n"',
    "declare -A x=([']']=abc); git commit -m \"${x[']']#'$(git commit -n -m i)'}\"",
    "x=(abc); git commit -m \"${x[0]#'$(git commit -n -m i)'}\"",  # an array subscript
    "r=x; x=abc; git commit -m \"${!r#'$(git commit -n -m i)'}\"",  # an indirect reference
    "x=abc; git commit -m \"${x//a/'$(git commit -n -m i)'}\"",  # a pattern quotes in "..."
    "x=abc; git commit -m \"${x#'$(git commit -n -m i)'}\"",
    "x=abc; git commit -m \"${x%'$(git commit -n -m i)'}\"",
    "subject=${subject:-'$(git commit -n)'}; git commit -m \"$subject\"",  # a quoted default
    "bash <<'EOF'\ngit commit -m '\\\\-n'\nEOF",               # quoted: the body is read as written
    "git commit -q -m $'it\\'s'",
    "git commit -F - <<'E'\nwe don't\ngit commit -n\nE\necho done",  # a heredoc body runs nothing
    "git commit -m 'x' 2>&1 | tail -3",
    "git commit -q -F - > log 2>&1 <<'E'\n$(git commit -n)\nE",  # a quoted heredoc body is inert
    "git commit -m x >-n",                                  # -n is the redirection's file
    "git commit -m \"$(printf '%s' $'it\\'s')\"",              # an ANSI-C quote in a substitution
)
NOT_A_COMMIT = (
    "bash <<'EOF' -c ':'\ngit commit -n -m x\nEOF",          # -c after the heredoc: stdin is data
    "echo $'advice; git commit -n isn\\'t needed'",       # a quoted separator is not a boundary
    "echo don't git commit -n",                            # bash rejects these: nothing runs
    "say 'run git commit -n\nlater",
    "codex '[review-kind: round] see git commit -n here\nit's",
    "echo $((1 << 2)) # git commit -n",
)


# Every heredoc of a shell reading stdin is read as a script, though bash runs only the
# one its descriptors leave on stdin: these bodies are data to bash and still refused.
KNOWN_FALSE_REFUSALS = (
    # Quoted text inside a word-opening `name[` is read as a subscript, which runs its
    # substitutions.
    "echo 'a[$(git commit -n -m x)]'",
    "git commit -m 'a[0] documents $(git commit -n)'",  # read whole, past its `]`
    "declare -A a=( ['$(git commit -n -m inner)']=x )",  # an associative key is literal
    # An array subscript runs its substitutions when the array is indexed; the type is
    # not in the text, so an associative array's quoted key is refused too.
    "declare -A x=([a]=abc); git commit -m \"${x['$(git commit -n -m i)']:-ok}\"",
    "bash 3<<'EOF' 3</dev/null <&3\ngit commit -n -m x\nEOF\ngit commit -m ordinary",
    "bash <<'EOF' </dev/null\ngit commit -n -m x\nEOF\ngit commit -m ordinary",
    "bash <<'A' <<'B'\ngit commit -n -m x\nA\n:\nB\ngit commit -m ordinary",
    # Options git itself refuses: no commit happens, and the reader refuses them.
    "git commit --no-ver -m x",          # ambiguous: --no-verbose or --no-verify
    "git commit --no-verify=x -m x",
)


def test_refusal_text_bash_may_run_is_refused():
    for cmd in KNOWN_FALSE_REFUSALS:
        assert gate._bash_is_git_commit(cmd) == (True, True), cmd


def test_a_commit_is_read_as_bash_reads_it():
    for cmd in BASH_QUOTED_NO_VERIFY:
        assert gate._bash_is_git_commit(cmd) == (True, True), cmd
    for cmd in BASH_QUOTED_PLAIN:
        assert gate._bash_is_git_commit(cmd) == (True, False), cmd
    for cmd in NOT_A_COMMIT:
        assert gate._bash_is_git_commit(cmd) == (False, False), cmd


def test_the_reading_matches_what_bash_runs():
    """Oracle: run each case under bash with a git function that records each call's argv,
    then run every recorded `commit` argv through real git, whose pre-commit hook marks
    that it ran. The gate refuses exactly the commits where git skipped it."""
    import subprocess
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        _oracle(pathlib.Path(d), subprocess)


# git fails before its hooks on these, so whether one would have run is unknown.
_BEFORE_HOOKS = ("pathspec", "could not lookup commit", "ambiguous option", "unknown option",
                 "takes no value", "requires a value", "unknown switch")


def _real_git_skips_hook(repo, args, subprocess):
    """True / False: real `git commit <args>` skipped / ran pre-commit. None: git stopped
    before its hooks."""
    marker = repo / "ran"
    marker.unlink(missing_ok=True)
    env = {"PATH": "/usr/bin:/bin", "GIT_EDITOR": "true", "HOME": str(repo)}
    done = subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", *args],
                          cwd=str(repo), capture_output=True, text=True, timeout=30, env=env,
                          stdin=subprocess.DEVNULL)
    if marker.exists():
        return False
    if done.returncode == 129 or any(m in done.stderr for m in _BEFORE_HOOKS):
        return None
    return True


def _scratch_repo(path, subprocess):
    path.mkdir()
    hooks = path / "h"
    hooks.mkdir()
    (hooks / "pre-commit").write_text("#!/bin/sh\necho ran > ran\nexit 1\n")  # mark it, commit nothing
    (hooks / "pre-commit").chmod(0o755)
    for args in (["init", "-q"], ["config", "core.hooksPath", "h"], ["config", "user.name", "t"],
                 ["config", "user.email", "t@example.invalid"]):
        subprocess.run(["git", *args], cwd=str(path), check=True, capture_output=True, timeout=30)
    (path / "f").write_text("f\n")
    subprocess.run(["git", "add", "f"], cwd=str(path), check=True, capture_output=True, timeout=30)
    subprocess.run(["git", "-c", "core.hooksPath=/dev/null", "commit", "-q", "-m", "init"], cwd=str(path),
                   check=True, capture_output=True, timeout=30)
    return path


def _oracle(tmp_path, subprocess):
    calls = tmp_path / "calls"
    calls.mkdir()
    shim = ('git() { for a; do [ "$a" = commit ] && { printf "%s\\0" "$@" > "$(mktemp "$CALLS/c.XXXXXX")"; '
            'break; }; done; return 0; }; export -f git; export CALLS\n')
    repo = _scratch_repo(tmp_path / "gr", subprocess)
    skip = ("env ", "(cd r")  # env runs the real binary; cd r fails
    decided = undecided = refused = 0
    for cmd in BASH_QUOTED_NO_VERIFY + BASH_QUOTED_PLAIN + NOT_A_COMMIT:
        if cmd.startswith(skip):
            continue
        for old in calls.iterdir():
            old.unlink()
        subprocess.run(["bash", "-c", shim + cmd], cwd=str(tmp_path), capture_output=True, timeout=30,
                       env={"CALLS": str(calls), "PATH": "/usr/bin:/bin"})
        argvs = [f.read_bytes().decode("utf-8", "surrogateescape").split("\0")[:-1] for f in calls.iterdir()]
        verdicts = [_real_git_skips_hook(repo, argv[argv.index("commit") + 1:], subprocess) for argv in argvs]
        got = gate._bash_is_git_commit(cmd)
        assert got[0] == bool(argvs), (cmd, argvs)
        if True in verdicts or (verdicts and None not in verdicts):
            decided += 1
            refused += True in verdicts
            assert got[1] == (True in verdicts), (cmd, argvs, verdicts)
        elif verdicts:
            undecided += 1
    assert decided >= 9 * undecided and refused >= 20, (decided, undecided, refused)  # the oracle decides


def test_many_substitutions_stay_linear():
    """Performance review: sibling substitutions copied the whole token list each time."""
    import time

    def best(k):
        cmd = "git commit -m \"" + "$(:) " * k + "\" -n"
        times = []
        for _ in range(3):
            t = time.perf_counter()
            assert gate._bash_is_git_commit(cmd) == (True, True)
            times.append(time.perf_counter() - t)
        return min(times)

    small, big = best(4000), best(32000)
    assert big < small * 8 * 2.5, (small, big)  # 8x the input: linear with headroom, quadratic is ~50x


def test_nested_substitutions_stay_polynomial():
    """Performance passes: work doubled per nesting level where a close-finding lexer
    read what the body is read for again (`"a[$(...)]"` ten deep: 3 s; `@("$(...)")`
    twelve deep with 10 KB of text: over 11 s), and where a shell's script was re-read
    with every deeper one (`eval "$(...)"` twelve deep: 2.5 s)."""
    import time

    def took(wrap, depth):
        cmd = "echo git" + "x" * 1000
        for _ in range(depth):
            cmd = wrap.format(cmd)
        gate._sub_script_tokens.cache_clear()
        t = time.perf_counter()
        assert gate._bash_is_git_commit(cmd) == (False, False), wrap
        return time.perf_counter() - t

    for wrap in ('echo "a[$({})]"', 'echo @("$({})")', 'eval "$({})"', 'bash -c "$({})"'):
        shallow, deep = min(took(wrap, 4) for _ in range(3)), min(took(wrap, 12) for _ in range(3))
        assert deep < shallow * 3 * 4, (wrap, shallow, deep)  # 3x the depth: quadratic at worst


def test_continued_heredoc_lines_stay_linear():
    """Performance review: each continued heredoc line re-copied the whole logical line."""
    import time

    def best(k):
        cmd = "cat <<EOF\n" + "abcdefghijklmnopqrstuvwxyz012\\\n" * k + "\nEOF\ngit commit -n -m x"
        times = []
        for _ in range(3):
            t = time.perf_counter()
            assert gate._bash_is_git_commit(cmd) == (True, True)
            times.append(time.perf_counter() - t)
        return min(times)

    small, big = best(5000), best(40000)
    assert big < small * 8 * 2.5, (small, big)


def test_refusal_text_the_lexer_cannot_finish_is_read_conservatively():
    # If the lexer stops where bash would not, the rest is judged, never dropped.
    real = gate._bash_rejects_from
    gate._bash_rejects_from = lambda cmd, line: False
    try:
        assert gate._bash_is_git_commit("ls\necho 'x\ngit commit -n -m y") == (True, True)
        assert gate._bash_is_git_commit("ls\necho 'x\ngit commit -m y") == (True, True)  # unread: refused
        assert gate._bash_is_git_commit("echo 'unterminated prose") == (False, False)
    finally:
        gate._bash_rejects_from = real


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")


def _repo(path):
    import subprocess
    subprocess.run(["git", "init", "-q", str(path)], check=True, timeout=30)
    return str(path.resolve())


def test_a_bare_git_C_commit_is_judged_on_the_tree_it_commits_to(tmp_path):
    """Live 2026-11-11: a repair-worktree commit was refused on the primary tree's index."""
    other = _repo(tmp_path / "other")
    for cmd in (f"git -C {other} commit -q -F msg.txt",
                f"git -C {other} commit -m 'plain words'",
                f"git -C '{other}' commit"):
        assert str(gate._commit_target_root(cmd)) == other, cmd


def test_any_other_shape_keeps_the_gates_own_directory(tmp_path):
    other = _repo(tmp_path / "other")
    for cmd in (f"cd {other} && git commit -m x",
                f"git -C {other} status; git commit -m x",
                f"git -C {other} status\ngit commit -m x",
                f"git -C {other} commit -m x && git commit -m y",
                f"git -C {other} commit -m \"$(cat f)\"",
                f"git -C {other} log commit",
                f"git -c a=b -C {other} commit -m x",
                f"env X=1 git -C {other} commit -m x",
                f"git -C {tmp_path / 'missing'} commit -m x",
                f"git -C {other} commit -m 'unterminated",
                "git commit -m x"):
        assert gate._commit_target_root(cmd) is None, cmd


SESSION = "0a1b2c3d-test-session"


def _git(path, *args, session=SESSION):
    import os
    import subprocess
    env = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid", "GIT_COMMITTER_NAME": "t",
           "GIT_COMMITTER_EMAIL": "t@example.invalid", "PATH": os.environ["PATH"], "HOME": str(path),
           "SKIP_COUNT": "1"}
    if session:
        env["CLAUDE_CODE_SESSION_ID"] = session
    return subprocess.run(["git", "-C", str(path), *args], capture_output=True, text=True, timeout=60, env=env)


def _trees(tmp_path):
    """A primary tree and a linked worktree, both running the repository's real post-commit hook."""
    here = pathlib.Path(_repo(tmp_path / "here"))
    hooks = tmp_path / "hooks"
    hooks.mkdir()
    (hooks / "post-commit").write_text((HOOKS.parents[1] / ".githooks/post-commit").read_text())
    (hooks / "post-commit").chmod(0o755)
    (hooks / "pre-commit").write_text("#!/bin/sh\n[ ! -e refuse-commits ]\n")
    (hooks / "pre-commit").chmod(0o755)
    _git(here, "config", "core.hooksPath", str(hooks))
    assert _git(here, "commit", "-q", "--allow-empty", "-m", "0").returncode == 0
    other = tmp_path / "other"
    assert _git(here, "worktree", "add", "-q", "--detach", str(other)).returncode == 0
    return here, other.resolve()


def _receipts(here):
    common = _git(here, "rev-parse", "--path-format=absolute", "--git-common-dir").stdout.strip()
    return pathlib.Path(common) / gate.COMMIT_LANDED_DIR / SESSION


def test_a_post_commit_hook_acts_only_where_this_sessions_commit_landed(tmp_path, monkeypatch):
    """Live 2026-11-11: worktree commits ran the suite in the primary tree, under a live run's matrix."""
    here, other = _trees(tmp_path)
    monkeypatch.chdir(here)                       # a harness hook always runs in the primary tree
    assert gate.commit_landed_roots(SESSION, "smoketest") == [str(here)]   # the fixture's first commit
    assert gate.commit_landed_roots(SESSION, "smoketest") == []            # drained once
    # The commit goes to the worktree, with a message no shell-text parser reads: that tree is tested.
    assert _git(other, "commit", "-q", "--allow-empty", "-m", "fix #123 $(x); y").returncode == 0
    assert gate.commit_landed_roots(SESSION, "smoketest") == [str(other)]
    assert gate.commit_landed_roots(SESSION, "smoketest-boot") == [str(here), str(other)], \
        "each consumer drains for itself"
    assert gate.commit_landed_roots(SESSION, "smoketest") == []
    # A refused commit lands nothing, however soon after the last one.
    (other / "refuse-commits").write_text("")
    assert _git(other, "commit", "-q", "--allow-empty", "-m", "refused").returncode != 0
    assert gate.commit_landed_roots(SESSION, "smoketest") == []
    (other / "refuse-commits").unlink()
    # Another session's commit, and one made outside any session, are not this session's.
    assert _git(here, "commit", "-q", "--allow-empty", "-m", "the run", session="ffffffff-other").returncode == 0
    assert _git(here, "commit", "-q", "--allow-empty", "-m", "a person", session=None).returncode == 0
    assert gate.commit_landed_roots(SESSION, "smoketest") == []
    assert gate.commit_landed_roots("ffffffff-other", "smoketest") == [str(here)]
    # A commit followed by a long tail in the same call is still seen: nothing depends on its age.
    assert _git(other, "commit", "-q", "--allow-empty", "-m", "then a long build").returncode == 0
    import os
    os.utime(_receipts(here), (1, 1))
    assert gate.commit_landed_roots(SESSION, "smoketest") == [str(other)]


def test_two_worktrees_committed_in_one_call_are_both_returned(tmp_path, monkeypatch):
    """Review round 2: the second commit used to replace the first one's receipt."""
    here, other = _trees(tmp_path)
    monkeypatch.chdir(here)
    gate.commit_landed_roots(SESSION, "smoketest")
    assert _git(other, "commit", "-q", "--allow-empty", "-m", "source in A").returncode == 0
    assert _git(here, "commit", "-q", "--allow-empty", "-m", "docs in B").returncode == 0
    assert _git(other, "commit", "-q", "--allow-empty", "-m", "A again").returncode == 0
    assert gate.commit_landed_roots(SESSION, "smoketest") == [str(other), str(here)]


def test_refusal_a_count_only_refresh_is_not_a_commit(tmp_path, monkeypatch):
    """Review round 2: a manual refresh before a refused commit published a receipt."""
    import os
    import subprocess
    here, _other = _trees(tmp_path)
    monkeypatch.chdir(here)
    gate.commit_landed_roots(SESSION, "smoketest")
    hook = HOOKS.parents[1] / ".githooks/post-commit"
    env = {"PATH": os.environ["PATH"], "HOME": str(here), "CLAUDE_CODE_SESSION_ID": SESSION, "COUNT_ONLY": "1"}
    before = _receipts(here).read_text()
    subprocess.run(["bash", str(hook)], cwd=str(here), env=env, capture_output=True, timeout=120)
    assert _receipts(here).read_text() == before
    (here / "refuse-commits").write_text("")
    assert _git(here, "commit", "-q", "--allow-empty", "-m", "refused").returncode != 0
    assert gate.commit_landed_roots(SESSION, "smoketest") == []


def test_refusal_a_bad_session_id_or_receipt_starts_nothing(tmp_path, monkeypatch):
    here, _other = _trees(tmp_path)
    monkeypatch.chdir(here)
    for bad in ("", "short", "../../etc/passwd", "a" * 65, None, 7):
        assert gate.commit_landed_roots(bad, "smoketest") == [], bad
    assert gate.commit_landed_roots(SESSION, "../x") == []
    for text in ("", "no-tab-in-this-line\n", "/no/such/tree\tmark\n", "\tmark\n", f"{here}\t\n"):
        _receipts(here).write_text(text)
        _receipts(here).with_name(f"{SESSION}.smoketest").unlink(missing_ok=True)
        assert gate.commit_landed_roots(SESSION, "smoketest") == [], text
    # A position left over from a longer file starts again from the top.
    _receipts(here).write_text(f"{here}\tmark\n")
    _receipts(here).with_name(f"{SESSION}.smoketest").write_text("99")
    assert gate.commit_landed_roots(SESSION, "smoketest") == [str(here)]


def test_both_post_commit_hooks_ask_where_the_commits_landed():
    for name, consumer in (("post_commit_smoketest.py", "smoketest"), ("post_commit_smoketest_boot.py", "smoketest-boot")):
        text = (HOOKS / name).read_text()
        assert f'scg.commit_landed_roots(d.get("session_id") or "", "{consumer}")' in text, name
        assert '"rev-parse", "--show-toplevel"' not in text, name
