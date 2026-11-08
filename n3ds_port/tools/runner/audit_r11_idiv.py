#!/usr/bin/env python3
"""
audit_r11_idiv.py -- the R11 hand-check, mechanised.

    python3 tools/runner/audit_r11_idiv.py [--all] [--json]

AGENTS.md R11 forbids INTEGER division or modulo by a RUNTIME value in a hot
loop: ARMv6 has no integer divide instruction, so `/` or `%` on integers is a
`bl __aeabi_idiv` / `__aeabi_idivmod` -- tens of cycles PLUS a call barrier that
spills the loop's live registers. A compile-time-constant divisor is fine; GCC
turns it into a magic-number multiply.

R11 closes by saying, in the contract's own words:

    "The gates do NOT detect this yet. Until they do it is a hand-check.
     Detection is cheap: an integer % or / whose divisor is neither a literal
     nor constexpr, inside a corpus-hot loop."

This is that. It is a SCREEN, NOT A PROOF, and the distinction is the whole
reason it prints what it filtered:

  * C++ cannot be typed by regex, so "is this integer division" is inferred from
    the shape of the operands. A float divide (hardware `vdiv`, R7's concern,
    NOT R11's) is excluded when either side looks like a float; that inference
    can be wrong in both directions.
  * "Inside a hot loop" is approximated TWICE OVER: "in a loop" means "at or
    after the first for/while in the file" (there is no brace tracking -- see
    the comment on depth_stack in scan(), which explains why that is the useful
    behaviour), and "hot" means "in a file the corpus does not mark
    cold_always". The corpus is a file-level list, not a per-function profile.

So a hit is a CANDIDATE for a human to confirm against a real profile, and a
clean run means "nothing obvious", never "proven absent". It exists to turn a
whole-tree hand-check into a short list, which is the difference between a rule
that gets checked and one that does not.

Exit code 1 if any candidate is found, so it can gate.

FOUR FILTER FIXES, 2026-10-14. An audit found three real R11 sites this screen
was structurally blind to, each hidden by a DIFFERENT filter, and every one of
them was invisible for a reason that had been written down as a deliberate choice:

  * `float_expr` accepted a two-letter variable name as PROOF of a float operand,
    so `int vv = ...; vv % engine.lane_count` (entity_geometry.cpp:941) was
    called a float expression. FLOATISH is now operand-aware: a name match is
    only float evidence when that operand is not declared `int` in the file.
  * `constant_divisor` accepted any `const int` declared in ANY file as a
    compile-time constant in EVERY file, so a local `const int n = e.lane_count`
    made every `x % n` in the tree look like a magic-number multiply
    (arcade_flipper.cpp:102). The default const set is now `constexpr` only --
    see collect_constants(), whose own docstring already called the old default
    "KNOWN TO OVER-TRUST" while leaving it in force.
  * `not_in_loop` LATCHED FORWARD at the first loop in a file, so straight-line
    code above that first loop was invisible (arcade_pulsar.cpp:181, inside
    `stepOneLane`, called from `move_enemies`, sat above the file's first `for`
    at line 307). The loop test is now a property of the file, not of a position
    in it.
  * The fourth fix goes the OTHER WAY, and a screen needs both directions or its
    precision claim is unfalsifiable: `float_expr` only ever looked at the operand
    BEFORE the operator and only at a fixed list of float-SHAPED prefixes, so a
    divide whose floatness sat on the far side (`radial = o / len`,
    line_geometry.cpp:648) or on a name the prefix list never imagined
    (`(int)(slen / step)`, popup_fx.h:453) was reported as an integer divide.
    Eleven of the 28 blessed baseline candidates were float divides -- VFPv2 does
    hardware `vdiv`, so those are R7's business and were never R11's. A name
    DECLARED float in the file now decides it from either side of the operator
    (float_operand_names()). Precision and recall are the same dial; turning only
    one is how a screen ends up crying wolf until nobody reads it.

Every fix WIDENS the net and the candidate count went up as a result. That is the
point: a screen whose filters only ever get tuned to shrink its output has
stopped looking. A site a human has actually reviewed is suppressed by an
`@vfp-exempt R11` pin (AGENTS.md 5), which this screen now honours -- see
collect_pins().

SIGNATURES ARE LINE-INDEPENDENT (--sig). The baseline used to key on
`file:line`, which made it a lie about the code: any edit ABOVE a site shifted
every later line and reported the SAME site as a NEW hazard. On 2026-09-26 that
produced 13 "new" candidates that were 13 already-blessed sites moved by ~3
lines, and the gate's only remedy was to re-bless -- which trains the reader to
wave the gate through rather than to read it. A baseline that must be rewritten
after every unrelated edit is not a baseline.

The signature is therefore `file + divisor + normalized statement text`: it
survives line shifts, and it still CHANGES when the site changes, because the
statement text is the site. What it cannot see is a site that moves to another
file or changes its divisor -- and that is a real change that should be
re-blessed deliberately, not silently.
"""

import argparse
import json
import os
import re
import sys

# n3ds: adapted from T2K (MIT, see LICENSE.t2k): repo root, corpus and swept trees come from contracts/project.json.
_RUNNER = os.path.dirname(os.path.abspath(__file__))
_PROJECT = json.load(open(os.path.join(_RUNNER, "contracts", "project.json")))
ROOT = os.path.normpath(os.path.join(_RUNNER, _PROJECT.get("repo_root", "../..")))
CORPUS = os.path.join(_RUNNER, "contracts", "hotpath_corpus.json")
TREES = list(_PROJECT.get("src_roots", []))
EXTS = (".cpp", ".h", ".inc")

# A divisor that is one of these is compile-time constant -> magic-number
# multiply, not a call. Collected from the tree rather than hardcoded so a new
# constant does not become a false positive.
CONST_DECL = re.compile(
    r"\b(?:constexpr|const)\s+(?:static\s+)?(?:unsigned\s+|signed\s+)?"
    r"(?:int|long|short|size_t|u?int\d+_t|char)\s+([A-Za-z_]\w*)\s*=")
# The strict variant: `constexpr` only. A plain `const` on a local is a local,
# not a named constant, and treating it as one is what makes `i % n` read as
# "divided by a compile-time constant". See collect_constants().
CONST_DECL_STRICT = re.compile(
    r"\bconstexpr\s+(?:static\s+)?(?:unsigned\s+|signed\s+)?"
    r"(?:int|long|short|size_t|u?int\d+_t|char)\s+([A-Za-z_]\w*)\s*=")
ENUM_MEMBER = re.compile(r"^\s*([A-Z][A-Z0-9_]{2,})\s*(?:=|,)")

# `a / b` or `a % b` where the divisor is a bare identifier or a member read
# (`engine.lane_count`) -- i.e. a RUNTIME value. A numeric-literal divisor is
# excluded by the pattern itself.
#
# The divisor is restricted to `name`, `name.name` and `name::name`: allowing
# brackets and arrows swept up array subscripts as "divisors" and produced
# nonsense like `divisor=ws.count]`.
#
# `::` MUST BE IN THE CHAIN, and leaving it out did not merely miss a shape --
# IT DEFEATED THE CONSTANT-DIVISOR FILTER AND MANUFACTURED FALSE POSITIVES.
# Without it, `% RendererC3D::NUM_TEXSETS` matched with the divisor captured as
# `RendererC3D` -- a CLASS NAME, which is in neither `consts` nor upper-case, so
# the filter that exists precisely to drop compile-time constants never fired.
# Eleven `static constexpr int NUM_TEXSETS` sites were reported as R11 hazards,
# plus `rand() % std::max(...)` as "modulo by std". A screen whose misparse
# invents hits teaches its reader to distrust the real ones.
DIV = re.compile(r"[/%]\s*(?!/|\*|=)\s*([A-Za-z_]\w*(?:(?:\.|::)\w+)*)\s*(?![\w.(\[]|::)")

# A qualified name's CONSTANT-NESS lives in its last component: what decides
# whether `Foo::BAR` is a magic-number multiply is `BAR`, not `Foo`.
QUALIFIER = re.compile(r"::|\.")

# static_assert is evaluated by the COMPILER. Whatever it divides costs nothing
# at run time, so it can never be an R11 hazard.
STATIC_ASSERT = re.compile(r"\bstatic_assert\b")

# BLOCK COMMENTS MUST GO BEFORE THE SCAN. The first cut stripped only `//`, so
# the extremely common named-argument idiom `/*second=*/false` matched as a
# division by `false` -- two confident false positives that would have taught a
# reader to distrust the whole report.
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)

# STRING LITERALS MUST GO TOO. A printf format is full of `%u`/`%d`/`%s`, which
# read as "modulo by u". This was not hypothetical: the first clean run reported
# four confident hits that were all format specifiers -- two of them in a log
# line added earlier the same day.
STRING_LIT = re.compile(r'"(?:[^"\\\n]|\\.)*"')

# ANGLE-BRACKET INCLUDES MUST GO TOO, and for the same reason as the two above:
# `#include <glm/glm.hpp>` is not an expression, but `<glm/glm.hpp>` reads as
# "modulo by glm.hpp" to a regex that looks for `/` followed by a qualified name.
# Surfaced 2026-10-14 the moment the loop test stopped hiding the top of every
# file: five of the newly-visible candidates were include lines in
# math_utils.h, line_geometry.cpp and music_core.cpp, reported with a divisor
# that is a FILE NAME. Blank the line, keep the newline, same trick as above.
INCLUDE_LINE = re.compile(r"^[ \t]*#[ \t]*include[ \t]*<[^>]*>", re.M)

LOOP = re.compile(r"\b(for|while)\s*\(")
# Anything that makes the expression float, SPLIT IN TWO ON PURPOSE (2026-10-14).
#
# These used to be one regex, and a match of ANY part was treated as proof of a
# float expression. The literal half is proof. The name half is not:
# `[xyzuvw][A-Z_]?\w*\s*[/%]` matches the literal TEXT `vv %`, so
#
#     int vv = v + enemy.animation_vec;                        entity_geometry.cpp:940
#     vv = ((vv % engine.lane_count) + engine.lane_count) % engine.lane_count;  :941
#
# -- the exact double-wrap idiom AGENTS.md R11 calls its worst case -- was
# filtered as a float expression because of a two-letter variable name. A NAME
# match is now only float evidence when that operand is not declared `int` in the
# same file.
#
# THE LITERAL/KEYWORD HALF STAYS UNCONDITIONAL: `1.5f`, `float`, `double` on a
# line are evidence no declaration overrules. Dropping `[xyzuvw]` from the name
# half instead of making it operand-aware was measured and rejected: it added
# four genuine float divides as false positives (09_warp.inc:464,
# rail_geometry.cpp:424/633, shatter.cpp:508).
FLOAT_STRONG = re.compile(
    r"\d\.\d|\dF\b|\df\b|\bfloat\b|\bdouble\b", re.IGNORECASE)
FLOAT_NAME = re.compile(
    r"\b((?:inv|scale|alpha|ratio|frac|norm|len|dist|[xyzuvw])[A-Z_]?\w*)\s*[/%]",
    re.IGNORECASE)

# Declarations used to overrule a FLOAT_NAME match. A name counts as integer only
# if it is declared integer AND never declared float in the same file, so a file
# that reuses `v` for both cannot use one declaration to hide the other. This is
# a NAME test, not a type inference: a value that arrives as a float through a
# call is still invisible here, which is why this script is a screen, not a proof.
INT_DECL = re.compile(
    r"\b(?:unsigned\s+|signed\s+)?(?:int|long|short|size_t|u?int\d+_t|char|bool)"
    r"\s+([A-Za-z_]\w*)")
FLOAT_DECL = re.compile(r"\b(?:float|double)\s*\*?\s*([A-Za-z_]\w*)")

# `@vfp-exempt` pins are the contract's OWN suppression surface (AGENTS.md 5), so
# the screen honours them: a site an owner has pinned with R11 -- with a reason
# and a `Verified` date, which gates_vfp's R-EXEMPT / R-EXPIRED checks police --
# is not re-reported here. The grammar is gates_vfp's, not a new one.
#
# A pin covers its own line and the next PIN_REACH lines, because this tree writes
# pins on the line ABOVE the code they cover (collision.cpp:35, enemies.cpp:40).
# The reach is deliberately short: a pin is a statement about the line under it,
# not about the rest of the function.
EXEMPT_PIN = re.compile(r"/\*\s*@vfp-exempt\s+(?P<rules>[^\u2014]*?)(?:\u2014|\*/)", re.S)
RULE_ID = re.compile(r"\bR\d+\b")
PIN_REACH = 4


def collect_constants(files):
    """Names believed to be compile-time-constant divisors.

    THE DEFAULT IS `constexpr` ONLY now (2026-10-14). It used to be every `const`,
    and this docstring admitted that was "KNOWN TO OVER-TRUST" while leaving it in
    force -- which is the shape of a filter that hides the thing it exists to
    find. The over-trust was concrete, not theoretical: the loose set admitted any
    name matching CONST_DECL ANYWHERE in the tree, so a local `const int n = ...`
    in one file made `n` a "compile-time constant" for EVERY file. The set held
    17 single-letter names -- `i`, `j`, `k`, `n`, `s`, `t`, `p`, `w` -- which are
    loop variables and parameters, not named constants. The consequence was
    that the exact idiom AGENTS.md R11 calls the worst case was INVISIBLE:

        arcade_flipper.cpp   d = ((d % n) + n) % n;     x2 calls, filtered as
                                                "constant divisor"

    `n` there is `const int n = e.lane_count` -- a RUNTIME value. A plain `const`
    on a local is a local; only `constexpr` declarations and enum members are
    trusted here, plus the UPPER_CASE convention the filter applies on its own.

    `VFP_R11_CONSTSET=legacy` restores the old loose set so you can see what it
    hides. Nothing in the gate suite should run that way.
    """
    legacy = os.environ.get("VFP_R11_CONSTSET", "").strip().lower() in (
        "legacy", "loose", "const")
    decl = CONST_DECL if legacy else CONST_DECL_STRICT
    names = set()
    for path in files:
        try:
            txt = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        names.update(decl.findall(txt))
        for line in txt.splitlines():
            m = ENUM_MEMBER.match(line)
            if m:
                names.add(m.group(1))
    return names


def load_cold():
    try:
        with open(CORPUS) as f:
            return json.load(f).get("cold_always", [])
    except OSError:
        return []


def is_cold(rel, cold):
    return any(rel.startswith(c.rstrip("/")) for c in cold)


def int_operand_names(text):
    """Names declared integer in this file, minus any declared float in it.

    Used only to overrule a FLOAT_NAME match -- see FLOAT_STRONG / FLOAT_NAME.
    """
    return set(INT_DECL.findall(text)) - set(FLOAT_DECL.findall(text))


def float_operand_names(text):
    """Names declared float/double in this file, minus any declared integer in it.

    The mirror of `int_operand_names`, and the answer to the other half of the
    same blind spot. FLOAT_NAME only ever looked at the operand BEFORE the
    operator and only at a fixed list of float-SHAPED prefixes, so a divide whose
    floatness sat on the far side, or on a name the prefix list never imagined,
    was reported as an integer divide:

        if (hasLen) radial = o / len;          line_geometry.cpp:648  (len is float)
        const int steps = (int)(slen / step);  popup_fx.h:453         (both float)

    VFPv2 does hardware float division, so these are R7's business and not R11's;
    reporting them is the screen crying wolf, and a screen that cries wolf gets
    skimmed.
    """
    return set(FLOAT_DECL.findall(text)) - set(INT_DECL.findall(text))


# Both sides of the operator, as TWO patterns: one alternation cannot do it, because
# the branch that matches `o /` consumes the `/` and the divisor after it is never
# seen -- which is exactly how `radial = o / len` kept reading as an integer divide.
OPERAND_BEFORE = re.compile(r"([A-Za-z_]\w*)\s*[/%]")
OPERAND_AFTER = re.compile(r"[/%]\s*([A-Za-z_]\w*)")


def looks_float(code, int_names, float_names=frozenset()):
    """Is the `/` or `%` on this line a float operation?

    Literals and the float/double keywords decide it outright. A float-SHAPED NAME
    decides it only if that operand is not declared integer in this file, which is
    what stops `int vv; ... vv % n` from reading as a float divide. And a name
    DECLARED float in this file decides it from either side of the operator, which
    is what stops `o / len` and `slen / step` from reading as integer divides.
    Still a name test, not a type inference: a value that arrives as a float
    through a call is invisible here, which is why this is a screen and not a proof.
    """
    if FLOAT_STRONG.search(code):
        return True
    for m in FLOAT_NAME.finditer(code):
        if m.group(1) not in int_names:
            return True
    if float_names:
        for rx in (OPERAND_BEFORE, OPERAND_AFTER):
            for m in rx.finditer(code):
                if m.group(1) in float_names:
                    return True
    return False


def collect_pins(raw):
    """line -> set of rule ids pinned at that line, read from the RAW text.

    Block comments are blanked before the scan (see BLOCK_COMMENT), which would
    otherwise erase the very pins this exists to honour, so the pins are read
    from the raw file first.
    """
    pins = {}
    for m in EXEMPT_PIN.finditer(raw):
        rules = set(RULE_ID.findall(m.group("rules")))
        if rules:
            line = raw.count("\n", 0, m.start()) + 1
            pins.setdefault(line, set()).update(rules)
    return pins


def pinned(pins, line, rule):
    """Does an @vfp-exempt pin naming `rule` cover this line?

    A pin covers its own line and the next PIN_REACH lines -- see EXEMPT_PIN for
    why the reach is short. This is the screen's only suppression that is not an
    inference about the code, so it is checked before the others.
    """
    for pin_line, rules in pins.items():
        if pin_line <= line <= pin_line + PIN_REACH and rule in rules:
            return True
    return False


def norm_stmt(code):
    """Whitespace-collapsed statement text: the line-independent identity of a
    site. Comments are already blanked by the caller, so this is the code only."""
    return " ".join(code.split())


def scan(show_all=False):
    cold = load_cold()
    files = []
    for tree in TREES:
        for base, _dirs, names in os.walk(os.path.join(ROOT, tree)):
            for n in sorted(names):
                if n.endswith(EXTS):
                    files.append(os.path.join(base, n))

    consts = collect_constants(files)
    hits, filtered = [], {"cold_file": 0, "not_in_loop": 0,
                          "constant_divisor": 0, "float_expr": 0,
                          "static_assert": 0, "pinned": 0}

    for path in files:
        rel = os.path.relpath(path, ROOT)
        try:
            raw = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        # Pins are read from the RAW text, BEFORE the blank below erases them.
        pins = collect_pins(raw)
        # Blank block comments out rather than deleting them, so line numbers
        # in the report still match the file.
        raw = BLOCK_COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), raw)
        raw = STRING_LIT.sub(lambda m: '"' + " " * (len(m.group(0)) - 2) + '"', raw)
        raw = INCLUDE_LINE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), raw)
        lines = raw.splitlines()
        cold_file = is_cold(rel, cold)
        int_names = int_operand_names(raw)
        float_names = float_operand_names(raw)

        # THE LOOP TEST IS FILE-LEVEL, NOT BRACE-SCOPED, AND THAT IS DELIBERATE.
        # Nothing here tracks braces or closes a loop: a hit means "this FILE has
        # a for/while in it", not "this line sits inside one".
        #
        # KEPT DELIBERATELY, AND MADE POSITION-INDEPENDENT ON 2026-10-14. The
        # hottest sites this screen found are not inside a loop of their own: five
        # of the seven `((x % n) + n) % n` sites AGENTS.md R11 credits it with are
        # straight-line code in enemies.cpp's per-enemy `_ai_el_zapper` /
        # `_ai_mushroom` / `_ai_rectangle` / `_ai_spiker` / `_ai_sp_zapper`, none of
        # which contains a loop at all -- they are hot because their CALLERS loop
        # over enemies. Brace-scoped tracking would filter every one of them out.
        #
        # What it used to be is a different thing wearing the same name: the test
        # LATCHED FORWARD, set at the first `for`/`while` and kept for the rest of
        # the file, so straight-line code ABOVE that first loop was invisible.
        # arcade_pulsar.cpp's `dst = ((dst % n) + n) % n;` at :181 -- inside
        # `stepOneLane`, which `move_enemies` calls once per enemy per tick -- sat
        # before the file's first loop at line 307 and was filtered as "not in a
        # loop". Whether a site was visible depended on where the file happened to
        # put its first loop, which is not a property of the site. Hoisting the
        # test out of the line loop keeps every case the forward latch existed to
        # cover and drops the position dependence with it.
        #
        # The cost runs the other way and is the cost the latch already accepted:
        # straight-line code that merely sits in a file that has a loop somewhere
        # is reported. Confirm the loop by eye; this is a SCREEN, not a proof.
        file_has_loop = any(LOOP.search(ln.split("//", 1)[0]) for ln in lines)

        for i, line in enumerate(lines, 1):
            code = line.split("//", 1)[0]
            if STATIC_ASSERT.search(code) and DIV.search(code):
                # Counted only when the line really held a division, so the
                # tally stays a count of SUPPRESSED HITS rather than of
                # unrelated asserts.
                filtered["static_assert"] += 1
                if not show_all:
                    continue
            for m in DIV.finditer(code):
                divisor = m.group(1).rstrip(".")
                tail    = QUALIFIER.split(divisor)[-1]
                in_loop = file_has_loop
                # A pin is checked FIRST, ahead of every inference: it is an
                # explicit human decision about this line, and the inferences
                # below are guesses. gates_vfp polices the pin's grammar, its
                # measured cost and its age, so honouring one here does not
                # mean trusting an unreviewed comment.
                if pinned(pins, i, "R11"):
                    filtered["pinned"] += 1
                    if not show_all:
                        continue
                if cold_file:
                    filtered["cold_file"] += 1
                    if not show_all:
                        continue
                elif not in_loop:
                    filtered["not_in_loop"] += 1
                    if not show_all:
                        continue
                elif (tail in consts or tail.isupper()
                      or divisor in consts or divisor.isupper()):
                    filtered["constant_divisor"] += 1
                    if not show_all:
                        continue
                elif looks_float(code, int_names, float_names):
                    filtered["float_expr"] += 1
                    if not show_all:
                        continue
                key = (rel, i, divisor)
                prev = next((h for h in hits if h["key"] == key), None)
                if prev:
                    prev["count"] += 1          # `((x%n)+n)%n` is TWO calls
                    continue
                hits.append({"key": key, "count": 1,
                             "file": rel, "line": i, "divisor": divisor,
                             "cold": cold_file, "in_loop": in_loop,
                             "stmt": norm_stmt(code),
                             "text": code.strip()[:100]})
    for h in hits:
        h["sig"] = "%s  divisor=%s  x%d  %s" % (
            h["file"], h["divisor"], h["count"], h["stmt"])
    return hits, filtered, len(files)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true",
                    help="report every division site, including the filtered ones")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--sig", action="store_true",
                    help="print only line-independent signatures (for baselining)")
    args = ap.parse_args()

    hits, filtered, nfiles = scan(args.all)
    if args.sig:
        for h in hits:
            print(h["sig"])
        return 0
    if args.json:
        json.dump({"hits": hits, "filtered": filtered, "files": nfiles},
                  sys.stdout, indent=2)
        return 1 if hits and not args.all else 0

    print("R11 audit -- integer / or % by a RUNTIME value inside a loop")
    print("  scanned %d files across %s" % (nfiles, ", ".join(TREES)))
    # PRINTING WHAT WAS FILTERED IS NOT DECORATION: a screen that reports only
    # its hits cannot be told apart from a screen whose pattern matched nothing,
    # which is the "a probe must be able to fail" rule applied to a linter.
    print("  filtered: %d in cold_always files, %d not in a loop, "
          "%d constant divisor, %d float expression, %d static_assert"
          % (filtered["cold_file"], filtered["not_in_loop"],
             filtered["constant_divisor"], filtered["float_expr"],
             filtered["static_assert"]))
    print("  pinned: %d suppressed by an @vfp-exempt R11 pin (AGENTS.md 5)"
          % filtered["pinned"])
    if not hits:
        print("\n  no candidates.")
        print("  NB this is a SCREEN, not a proof -- 'is it integer' and 'is it"
              " hot' are both\n     inferred, so a clean run means nothing"
              " obvious, never nothing present.")
        return 0
    print("\n  %d candidate(s) -- confirm each against a real profile:" % len(hits))
    for h in hits:
        mult = "  x%d calls on this line" % h["count"] if h["count"] > 1 else ""
        print("    %s:%d  divisor=%s%s" % (h["file"], h["line"], h["divisor"], mult))
        print("        %s" % h["text"])
    return 1


if __name__ == "__main__":
    sys.exit(main())
