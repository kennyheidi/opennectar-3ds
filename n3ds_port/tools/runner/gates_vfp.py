# Adapted for Open Nectar 3DS (n3ds_port) from T2K's tools/runner/gates_vfp.py
# (https://github.com/colbyshores/t2k, commit 94964668, MIT License -- see LICENSE.t2k).
# Every project-specific path, thread site, helper name, standing exemption and
# profile-stage mapping now comes from contracts/project.json instead of being
# spelled here; the rules themselves (R1-R11, T1/T2, the exemption grammar, the
# corpus clocks) are T2K's, unchanged. Edits are marked "n3ds:".
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from dataclasses import asdict, dataclass, field
from datetime import date
from pathlib import Path
from typing import Iterator, Literal, Mapping, Sequence

Severity = Literal["MUST", "SHOULD"]
Status = Literal["PASS", "FAIL"]

# An @vfp-exempt pin is a CLAIM that integerizing the site changes pixels or hit
# tests, backed by a `Verified <date>`. A claim with no expiry is a claim nobody
# rechecks: AGENTS.md names reflexive stamping as the failure mode of a large
# exemption corpus, and an undated-or-never-rechecked pin is how that happens.
# Past PIN_MAX_AGE_DAYS the pin is a MUST finding, and the remedy is the one the
# doctrine already requires -- a fresh measured capture, then bump the date.
PIN_MAX_AGE_DAYS = int(os.environ.get("VFP_PIN_MAX_AGE_DAYS", "180"))
# The hot-path corpus is "top 15 of a CAPTURED profile" (AGENTS.md 2). Against a
# stale capture that phrase means nothing, so the corpus must declare when it was
# captured and go stale on the same clock.
CORPUS_MAX_AGE_DAYS = int(os.environ.get("VFP_CORPUS_MAX_AGE_DAYS", "45"))

# The two clocks above exist to DEMAND A RE-MEASUREMENT. Once the project is
# frozen that demand cannot be satisfied -- nobody is taking another OG-profile
# capture -- so past the freeze they stop being a prompt and become permanent
# red, and a permanently red gate is worse than a stale corpus because it trains
# the reader to skip the gate. So the staleness clocks are waived from this date.
# ONLY the staleness clocks: every substantive rule (R2/R3/R4/R6/R11, exemption
# shape, register pressure) still runs, so editing hot-path code still fails on
# its merits. Green after a freeze means "clean under the freeze", NOT "freshly
# measured" -- which is why the waiver is reported, not silent.
# Set VFP_FROZEN_AT="" to put the clocks back and resume perf work.
# n3ds: Open Nectar is NOT frozen -- perf work is live -- so the clocks run unless
# contracts/project.json (or VFP_FROZEN_AT) says otherwise.
def _project_frozen_default() -> str:
    try:
        return str(json.loads((Path(__file__).resolve().parent / "contracts" / "project.json").read_text(encoding="utf-8")).get("frozen_at", ""))
    except (OSError, ValueError):
        return ""
PROJECT_FROZEN_AT = os.environ.get("VFP_FROZEN_AT", _project_frozen_default()).strip()


def _frozen() -> str | None:
    """Freeze date, or None if the staleness clocks are live."""
    return PROJECT_FROZEN_AT or None


def _age_days(iso: str, today: date | None = None) -> int | None:
    try:
        y, m, d = (int(x) for x in iso.strip().split("-"))
        return (today or date.today()).toordinal() - date(y, m, d).toordinal()
    except (ValueError, AttributeError):
        return None


RUNNER_DIR = Path(__file__).resolve().parent
# n3ds: the project description (paths are relative to repo_root, which is
# relative to this directory).
PROJECT_FILE = RUNNER_DIR / "contracts" / "project.json"
PROJECT: dict = json.loads(PROJECT_FILE.read_text(encoding="utf-8")) if PROJECT_FILE.exists() else {}
REPO_ROOT = (RUNNER_DIR / str(PROJECT.get("repo_root", "../.."))).resolve()
CONTRACTS_DIR = RUNNER_DIR / "contracts"
DEFAULT_CORPUS = CONTRACTS_DIR / "hotpath_corpus.json"
DEFAULT_LOG = RUNNER_DIR / "logs" / "vfp_audit.json"
# n3ds: the build has no Makefile ARCH line; R10 reads the files that carry the
# compile flags (toolchain file + CMakeLists) and checks the flags themselves.
BUILD_FLAG_FILES = tuple(REPO_ROOT / f for f in PROJECT.get("build_flag_files", []))
BUILD_DIR = REPO_ROOT / str(PROJECT.get("build_dir", "build-3ds"))
PERF_LOG_DIR = REPO_ROOT / str(PROJECT.get("perf_log_dir", "docs/validation"))
MAKEFILE_3DS = BUILD_FLAG_FILES[0] if BUILD_FLAG_FILES else REPO_ROOT / "Makefile"
SRC_ROOTS = tuple(REPO_ROOT / d for d in PROJECT.get("src_roots", []))
EXCLUDED_PREFIXES = tuple(REPO_ROOT / d for d in PROJECT.get("excluded", []))

# `.inc` IS A SOURCE EXTENSION HERE, not a data or build fragment.
# `t2k_3ds/src/rendering/c3d/*.inc` (~9,000 lines) are #included by
# renderer_c3d.cpp, which the hot-path corpus lists as hot with `*`, and they hold
# real shipped code -- including a worker thread (threadCreate at
# c3d/04_setup.inc:989) and its R1 helper (c3d/03_tev.inc:368). Leaving `.inc` out
# of this set meant the corpus's hottest renderer file was swept only in the ~100
# lines that survive outside its includes, and audit_r11_idiv.py already swept
# them, so the three screens disagreed about what "the tree" is.
CXX_EXTS = {".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".inc"}
INIT_NAME_RE = re.compile(
    r"(init|load|create|destroy|reset|parse|shutdown|seed)", re.IGNORECASE
)
THREAD_CREATE_RE = re.compile(r"\bthreadCreate\s*\(")
# A worker started through the shared wrapper never spells `threadCreate` at the
# call site: `sys::Thread::start(analyzerThreadMain, ...)` in audio_analysis.cpp
# hides it inside t2k_core/src/audio/audio_thread.h:134. Matching only the
# wrapper would credit the wrong file -- the file that OWNS the worker, and so
# owes the FPSCR helper, is the caller. So call sites are matched too, but only
# for a name this file (or its include closure) declares as a Thread; without
# that restriction `.start(` matches every unrelated `start()` in the tree.
THREAD_START_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\.\s*start\s*\(")
THREAD_DECL_RE = re.compile(r"\b(?:sys::)?Thread\s+([A-Za-z_]\w*)\b")
# The FIRST argument of a creation call is the worker function itself, and a
# function definition is decidable in a way that "does the unit mention FPSCR"
# never was: `threadCreate(texPrefetchWorker, ...)` names the routine whose entry
# owes the helper, so the gate can look that routine up and read its body instead
# of crediting whatever else happens to be in the file.
THREAD_CREATE_ARG_RE = re.compile(r"\bthreadCreate\s*\(\s*([A-Za-z_]\w*)")
THREAD_START_ARG_RE = re.compile(r"\.\s*start\s*\(\s*([A-Za-z_]\w*)")
# `std::thread` is deliberately NOT a thread-site pattern. R1 is a 3DS FPSCR rule,
# and the only std::thread in the tree is the non-3DS branch of the same
# sys::Thread wrapper, whose 3DS branch is covered by THREAD_CREATE_RE.
LOCAL_INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)
# `vfp_thread_fz_dn` is the helper AGENTS.md 3 names for EVERY worker row of its
# own table, and it was not in this list -- so a worker file that calls nothing but
# that helper satisfied `fpscr_ok` only by accident, via the word "FPSCR" appearing
# in a comment. audio_analysis.cpp was exactly that case.
FPSCR_HELPER_RE = re.compile(
    r"\b(vfp_set_fz_dn|vfp_thread_fz_dn|ts_vfp_init|vfp_init_fpscr|ts_vfp_set_fpscr"
    + "".join("|" + re.escape(h) for h in PROJECT.get("fpscr_helpers", []))   # n3ds
    + r")\b"
)
FPSCR_ASM_RE = re.compile(
    r"\b(vmsr|vmrs)\b.*\bFPSCR\b"
    r"|mcr\s+p10\s*,\s*7"
    r"|FPSCR",
    re.IGNORECASE,
)
EXEMPT_RE = re.compile(
    r"/\*\s*@vfp-exempt\s+(?P<rules>R\d+(?:\s*,\s*R\d+)*)\s+—\s+"
    r"(?P<reason>.+?)\.\s*"
    r"(?:measured\s+(?P<measured>\S+)\s*\.\s*)?"
    r"Verified\s+(?P<verified>\d{4}-\d{2}-\d{2})\s*\.\s*\*/",
    re.DOTALL,
)
HOTPATH_ANN_RE = re.compile(r"/\*\s*@hotpath\s*\*/")
FUNC_RE = re.compile(
    r"(?P<head>^[\w:<>,\s\*&]+?\b(?P<name>[A-Za-z_]\w*)\s*\([^;{]*\))\s*\{",
    re.MULTILINE,
)
FOR_FLOAT_RE = re.compile(r"\bfor\s*\(\s*(?:float|double)\b")
CAST_INT_RE = re.compile(
    r"(?:static_cast|reinterpret_cast|const_cast)\s*<\s*"
    r"(?:int|unsigned|uint32_t|int32_t|int16_t|uint16_t|long|size_t|u32|s32|u16|s16)\s*>"
    r"\s*\("
    r"|\(\s*(?:int|unsigned|uint32_t|int32_t|int16_t|uint16_t|long|size_t|u32|s32|u16|s16)\s*\)"
)
DOUBLE_TYPE_RE = re.compile(r"\b(long\s+double|double)\b")
UNSUFFIXED_FLOAT_RE = re.compile(r"(?<![\w.])(?:\d+\.\d+|\d+\.|\.\d+)(?![eE][+-]?\d*)(?![fF\w.])")
LIBM_RE = re.compile(
    r"\b(?:std::)?(?:sinf?|cosf?|tanf?|atan2f?|expf?|logf?|powf?)\s*\("
)
SQRT_RE = re.compile(r"\b(?:std::)?sqrtf?\s*\(")
FAST_TRIG_RE = re.compile(r"\b(?:fastSin|fastCos|fastAtan2)\b")
FX_IDENT_RE = re.compile(r"\b([A-Za-z_]\w*(?:_fx|_q16|_q8)|fx_[A-Za-z_]\w*)\b")
FLOAT_DECL_RE = re.compile(
    r"\b(?:float|double)\s+(?:const\s+)?(?P<names>[A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)"
)
KEYWORD_STOP = {
    "if",
    "for",
    "while",
    "switch",
    "return",
    "sizeof",
    "static",
    "const",
    "void",
    "int",
    "float",
    "double",
    "bool",
    "char",
    "namespace",
    "class",
    "struct",
    "enum",
    "typedef",
    "template",
    "using",
    "inline",
    "extern",
    "public",
    "private",
    "protected",
    "virtual",
    "override",
    "operator",
    "new",
    "delete",
    "this",
    "true",
    "false",
    "nullptr",
    "NULL",
    "auto",
    "case",
    "default",
    "else",
    "do",
    "break",
    "continue",
    "goto",
}
ARCH_REQUIRED = tuple(PROJECT.get("build_flags_required", ("-march=armv6k", "-mfloat-abi=hard", "-mtp=soft")))   # n3ds: devkitPro template baseline (AGENTS.md R10)
# Every row of AGENTS.md 3's R1 table, plus the two worker sites the table only
# reached indirectly. A listed file that has no FPSCR helper in itself OR in its
# include closure is a MUST finding (see gate_r1) -- before 2026-10-14 a listed
# file lacking FPSCR produced no finding at all unless it was main_3ds.cpp, which
# made the renderer_c3d.cpp row decorative: that file contains no FPSCR text, and
# nothing checked it.
#
#   main          main_3ds.cpp            vfp_set_fz_dn()  main_3ds.cpp:284
#   music         music_3ds.cpp           threadCreate     music_3ds.cpp:275
#   sfx           sfx_3ds.cpp             threadCreate     sfx_3ds.cpp:359
#   analyzer      audio_analysis.cpp      sys::Thread      audio_analysis.cpp:452
#   tex-prefetch  c3d/04_setup.inc        threadCreate     c3d/04_setup.inc:989
#   shared        audio_thread.h          threadCreate     audio_thread.h:134/136
#   renderer      renderer_c3d.cpp        (helper is in the c3d/*.inc closure)
R1_THREAD_SITES = tuple(PROJECT.get("r1_thread_sites", []))   # n3ds: contracts/project.json
R1_MAIN_SITE = str(PROJECT.get("r1_main_site", ""))
STANDING_EXEMPTIONS: tuple[dict[str, object], ...] = tuple(   # n3ds: contracts/project.json
    {"id": e["id"], "file": e["file"], "symbols": tuple(e.get("symbols", ["*"])), "rules": tuple(e.get("rules", []))}
    for e in PROJECT.get("standing_exemptions", [])
)


@dataclass
class Finding:
    rule: str
    severity: Severity
    file: str
    line: int
    symbol: str
    hotpath: bool
    hotpath_reason: str
    exemption: str | None
    detail: str


@dataclass
class AuditResult:
    status: Status
    blockers: int
    reviews: int
    findings: list[Finding] = field(default_factory=list)
    hotpath_source: str = "seed"
    notes: list[str] = field(default_factory=list)

    def to_json(self) -> dict[str, object]:
        return {
            "status": self.status,
            "blockers": self.blockers,
            "reviews": self.reviews,
            "hotpath_source": self.hotpath_source,
            "notes": self.notes,
            "findings": [asdict(f) for f in self.findings],
        }


@dataclass
class Function:
    name: str
    start: int
    end: int
    body: str
    line: int
    header: str


@dataclass
class Exemption:
    rules: frozenset[str]
    reason: str
    measured: str | None
    verified: str
    standing: bool
    line: int
    applies_to: str


@dataclass
class HotEntry:
    rel: str
    symbols: tuple[str, ...]
    why: str
    source: str


def repo_rel(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def is_excluded(path: Path) -> bool:
    resolved = path.resolve()
    return any(_is_under(resolved, prefix) for prefix in EXCLUDED_PREFIXES)


def _is_under(path: Path, prefix: Path) -> bool:
    try:
        path.relative_to(prefix.resolve())
        return True
    except ValueError:
        return False


def iter_cxx_files(root: Path | None = None) -> Iterator[Path]:
    roots = (root,) if root is not None else SRC_ROOTS
    for base in roots:
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in CXX_EXTS:
                continue
            if is_excluded(path):
                continue
            yield path


def load_json(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def load_corpus(path: Path) -> dict[str, object]:
    if not path.exists():
        return {"version": 1, "seed": [], "cold_always": []}
    data = load_json(path)
    if not isinstance(data, dict):
        raise ValueError(f"corpus is not an object: {path}")
    return data


def strip_comments_keep_lines(text: str) -> str:
    out: list[str] = []
    i = 0
    n = len(text)
    in_str = False
    in_char = False
    in_line = False
    in_block = False
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line:
            if ch == "\n":
                in_line = False
                out.append("\n")
            else:
                out.append(" ")
            i += 1
            continue
        if in_block:
            if ch == "*" and nxt == "/":
                in_block = False
                out.append("  ")
                i += 2
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
            continue
        if in_str:
            out.append(ch)
            if ch == "\\" and nxt:
                out.append(nxt)
                i += 2
                continue
            if ch == '"':
                in_str = False
            i += 1
            continue
        if in_char:
            out.append(ch)
            if ch == "\\" and nxt:
                out.append(nxt)
                i += 2
                continue
            if ch == "'":
                in_char = False
            i += 1
            continue
        if ch == "/" and nxt == "/":
            in_line = True
            out.append("  ")
            i += 2
            continue
        if ch == "/" and nxt == "*":
            in_block = True
            out.append("  ")
            i += 2
            continue
        if ch == '"':
            in_str = True
            out.append(ch)
            i += 1
            continue
        if ch == "'":
            in_char = True
            out.append(ch)
            i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def line_of(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


def match_braces(text: str, open_idx: int) -> int:
    depth = 0
    i = open_idx
    n = len(text)
    in_str = False
    in_char = False
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_str:
            if ch == "\\" and nxt:
                i += 2
                continue
            if ch == '"':
                in_str = False
            i += 1
            continue
        if in_char:
            if ch == "\\" and nxt:
                i += 2
                continue
            if ch == "'":
                in_char = False
            i += 1
            continue
        if ch == '"':
            in_str = True
        elif ch == "'":
            in_char = True
        elif ch == "/" and nxt == "/":
            nl = text.find("\n", i)
            i = n if nl < 0 else nl
            continue
        elif ch == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return n - 1


def parse_functions(text: str) -> list[Function]:
    cleaned = strip_comments_keep_lines(text)
    functions: list[Function] = []
    for match in FUNC_RE.finditer(cleaned):
        name = match.group("name")
        if name in KEYWORD_STOP:
            continue
        brace = match.end() - 1
        if cleaned[brace] != "{":
            continue
        end = match_braces(cleaned, brace)
        start = match.start()
        functions.append(
            Function(
                name=name,
                start=start,
                end=end + 1,
                body=cleaned[brace + 1 : end],
                line=line_of(text, start),
                header=cleaned[start:brace],
            )
        )
    return functions


def parse_exemptions(text: str) -> list[Exemption]:
    found: list[Exemption] = []
    for match in EXEMPT_RE.finditer(text):
        rules = frozenset(part.strip() for part in match.group("rules").split(","))
        reason = re.sub(r"\s+", " ", match.group("reason")).strip()
        measured = match.group("measured")
        standing = "standing" in reason.lower()
        found.append(
            Exemption(
                rules=rules,
                reason=reason,
                measured=measured,
                verified=match.group("verified"),
                standing=standing,
                line=line_of(text, match.start()),
                applies_to="",
            )
        )
    return found


def exemption_for_index(text: str, exemptions: Sequence[Exemption], index: int) -> Exemption | None:
    best: Exemption | None = None
    best_dist = 10**9
    for exemption in exemptions:
        pos = 0
        for _ in range(exemption.line - 1):
            nxt = text.find("\n", pos)
            if nxt < 0:
                pos = len(text)
                break
            pos = nxt + 1
        if pos <= index and (index - pos) < best_dist:
            best = exemption
            best_dist = index - pos
    if best is None:
        return None
    if best_dist > 800:
        return None
    return best


def split_loops(body: str) -> list[tuple[int, str]]:
    loops: list[tuple[int, str]] = []
    for match in re.finditer(r"\b(for|while|do)\b", body):
        i = match.end()
        while i < len(body) and body[i] in " \t\n":
            i += 1
        if i < len(body) and body[i] == "(":
            depth = 0
            j = i
            while j < len(body):
                if body[j] == "(":
                    depth += 1
                elif body[j] == ")":
                    depth -= 1
                    if depth == 0:
                        j += 1
                        break
                j += 1
            i = j
            while i < len(body) and body[i] in " \t\n":
                i += 1
        if i < len(body) and body[i] == "{":
            end = match_braces(body, i)
            loops.append((match.start(), body[i + 1 : end]))
        else:
            semi = body.find(";", i)
            if semi >= 0:
                loops.append((match.start(), body[i : semi + 1]))
    return loops


def float_names_in(text: str) -> set[str]:
    names: set[str] = set()
    for match in FLOAT_DECL_RE.finditer(text):
        for raw in match.group("names").split(","):
            ident = raw.strip()
            if ident:
                names.add(ident)
    return names


def looks_float_expr(expr: str, float_ids: set[str]) -> bool:
    if UNSUFFIXED_FLOAT_RE.search(expr) or re.search(r"\d+(?:\.\d*)?[fF]\b", expr):
        return True
    if re.search(r"\b(?:float|double)\b", expr):
        return True
    tokens = set(re.findall(r"[A-Za-z_]\w*", expr))
    if tokens & float_ids:
        return True
    if FAST_TRIG_RE.search(expr):
        return True
    return False


def looks_int_expr(expr: str) -> bool:
    stripped = expr.strip()
    if re.fullmatch(r"\d+", stripped):
        return True
    if re.search(r"\b(?:int|unsigned|size_t|uint32_t|int32_t|u32|s32)\b", expr):
        return True
    return False


def condition_is_float(cond: str, float_ids: set[str]) -> bool:
    parts = re.split(r"(&&|\|\|)", cond)
    for part in parts:
        if re.search(r"(==|!=|<=|>=|<|>)", part) and looks_float_expr(part, float_ids):
            if looks_int_expr(part) and not looks_float_expr(part, float_ids):
                continue
            return True
        if part.strip().startswith("!") and looks_float_expr(part, float_ids):
            return True
    return looks_float_expr(cond, float_ids) and bool(
        re.search(r"(==|!=|<=|>=|<|>)", cond)
    )


def standing_covers(rel: str, symbol: str, rule: str) -> str | None:
    rel_n = rel.replace("\\", "/")
    base = rel_n.rsplit("/", 1)[-1]
    for item in STANDING_EXEMPTIONS:
        file_n = str(item["file"]).replace("\\", "/")
        file_base = file_n.rsplit("/", 1)[-1]
        path_ok = (
            rel_n == file_n
            or rel_n.endswith("/" + file_n)
            or (
                file_base in {"math_lut.h", "math_lut.cpp"}
                and base == file_base
            )
        )
        if not path_ok:
            continue
        symbols = item["symbols"]
        assert isinstance(symbols, tuple)
        if "*" not in symbols and symbol not in symbols:
            continue
        rules = item["rules"]
        assert isinstance(rules, tuple)
        if rule in rules:
            return str(item["id"])
    return None


def cold_file(rel: str, cold: Sequence[str]) -> bool:
    rel_n = rel.replace("\\", "/")
    for prefix in cold:
        p = prefix.replace("\\", "/")
        if rel_n == p or rel_n.startswith(p.rstrip("/") + "/"):
            return True
    return False


def is_init_symbol(name: str) -> bool:
    return bool(INIT_NAME_RE.search(name))


def classify_hot(
    rel: str,
    func: Function,
    seed: Mapping[str, HotEntry],
    annotated: bool,
    cold: Sequence[str],
) -> tuple[bool, str]:
    if cold_file(rel, cold):
        return False, ""
    if annotated:
        return True, "@hotpath"
    entry = seed.get(rel)
    if entry is None:
        return False, ""
    if "*" in entry.symbols:
        if is_init_symbol(func.name):
            return False, ""
        return True, entry.source
    if func.name in entry.symbols:
        return True, entry.source
    return False, ""


def seed_map(corpus: Mapping[str, object], extra: Sequence[HotEntry] | None = None) -> dict[str, HotEntry]:
    out: dict[str, HotEntry] = {}
    raw_seed = corpus.get("seed", [])
    if isinstance(raw_seed, list):
        for item in raw_seed:
            if not isinstance(item, dict):
                continue
            file_name = str(item.get("file", ""))
            symbols_raw = item.get("symbols", [])
            symbols = tuple(str(s) for s in symbols_raw) if isinstance(symbols_raw, list) else ("*",)
            why = str(item.get("why", "seed"))
            out[file_name] = HotEntry(file_name, symbols, why, "seed")
    if extra:
        for item in extra:
            out[item.rel] = item
    return out


def ingest_perf_logs(docs_dir: Path) -> tuple[list[HotEntry], str]:
    """n3ds: the profile is the console's own log.txt, saved as
    <perf_log_dir>/perf-YYYY-MM-DD-<what>.log. Each stage in project.json names a
    regex whose first group is a per-frame millisecond figure (the [lite2],
    [lite3], [prof], [audioprof] lines) and the files that stage's time is spent
    in. The 15 most expensive stages (mean over the last 8 logs) make those files
    hot -- "top 15 of a captured profile", measured, not remembered."""
    if not docs_dir.exists():
        return [], "seed"
    logs = sorted(docs_dir.glob("perf-*.log"))
    if not logs:
        return [], "seed"
    stages = [st for st in PROJECT.get("profile_stages", []) if isinstance(st, dict)]
    averages: dict[str, list[float]] = {}
    for log in logs[-8:]:
        try:
            text = log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for st in stages:
            for match in re.finditer(str(st["pattern"]), text):
                try:
                    averages.setdefault(str(st["name"]), []).append(float(match.group(1)))
                except (IndexError, ValueError):
                    continue
    if not averages:
        return [], "seed"
    ranked = sorted(
        ((name, sum(vals) / len(vals)) for name, vals in averages.items()),
        key=lambda kv: kv[1],
        reverse=True,
    )[:15]
    by_name = {str(st["name"]): st for st in stages}
    extra: list[HotEntry] = []
    for name, avg in ranked:
        if avg < float(by_name[name].get("min_ms", 0.5)):
            continue
        for rel in by_name[name].get("files", []):
            extra.append(HotEntry(str(rel), tuple(by_name[name].get("symbols", ["*"])), f"profile:{name} {avg:.1f} ms", f"profile:{name}"))
    return extra, "profile"


def gate_r10(makefile: Path | None = None) -> list[Finding]:
    """n3ds: R10 against the CMake build. The flags must be present in the files
    that set them (toolchain + CMakeLists); a soft/softfp ABI or -ffast-math /
    -Ofast anywhere in them is a MUST. Object attributes are checked separately
    by probe_objects() when a build directory is present."""
    findings: list[Finding] = []
    files = [makefile] if makefile is not None else list(BUILD_FLAG_FILES)
    blob = ""
    for f in files:
        if not f.exists():
            findings.append(Finding("R10", "MUST", repo_rel(f), 0, "flags", True, "seed", None, "build flag file missing"))
            continue
        blob += strip_comments_hash(f.read_text(encoding="utf-8", errors="replace")) + "\n"
    if not blob:
        return findings
    first = repo_rel(files[0])
    missing = [flag for flag in ARCH_REQUIRED if flag not in blob]
    if missing:
        findings.append(Finding("R10", "MUST", first, 0, "ARCH", True, "seed", None, "build flags missing " + ", ".join(missing)))
    if "-fno-math-errno" not in blob:
        findings.append(Finding("R10", "MUST", first, 0, "COMMONFLAGS", True, "seed", None, "compile flags missing -fno-math-errno"))
    if re.search(r"-mfloat-abi=(softfp|soft)\b", blob):
        findings.append(Finding("R10", "MUST", first, 0, "ARCH", True, "seed", None, "soft/softfp float ABI present"))
    if re.search(r"(?<![\w-])-(ffast-math|Ofast)\b", blob):
        findings.append(Finding("R10", "MUST", first, 0, "COMMONFLAGS", True, "seed", None, "-ffast-math / -Ofast present (AGENTS.md R10 forbids it)"))
    return findings


def strip_comments_hash(text: str) -> str:
    """n3ds: drop CMake '#' comments, so a comment that says 'never add -ffast-math' is not read as the flag."""
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


SHADER_STUB_RE = re.compile(r"_shbin\.o$")


def _shown(paths: list[Path], limit: int = 4) -> str:
    names = [repo_rel(p) for p in paths[:limit]]
    if len(paths) > limit:
        names.append(f"(+{len(paths) - limit} more)")
    return ", ".join(names)


def probe_objects(build_dir: Path) -> list[Finding]:
    """Verify the LINK IS HARD-FLOAT by reading every object's ARM attributes.

    This used to read ONE object -- `objects[0]` of an unordered glob -- and call a
    missing Tag_ABI_VFP_args a MUST violation. That made the gate order-dependent,
    and it failed on the five `*_shbin.o` shader wrappers: they are `bin2s`
    output assembled with `-x assembler-with-cpp`, i.e. a blob of precompiled
    PICA bytes with a symbol on it. A blob has no float calls, so gas never stamps
    a float ABI on it, and there is nothing for the rule to be about. Which object
    the glob returned first decided whether the gate was green, so the same tree
    passed and failed on different filesystems.

    Scanning every object is strictly stronger than sampling one: a single
    soft-float or wrong-ABI object anywhere in the link now fails, and the
    hard-float requirement is asserted against the compiled code rather than
    against whichever file happened to be first. The shader wrappers stay out of
    the requirement for what they are -- data, not code -- and are still checked
    for a softfp stamp, because a softfp blob would be a real mixed-ABI hazard.
    """
    findings: list[Finding] = []
    if not build_dir.exists():
        return findings
    objects = sorted(list(build_dir.rglob("*.o")) + list(build_dir.rglob("*.obj")))   # n3ds: CMake names objects *.cpp.obj in subfolders
    if not objects:
        return findings
    readelf = shutil.which("arm-none-eabi-readelf") or shutil.which("readelf")
    if not readelf:
        return findings

    hard: list[Path] = []
    softfp: list[Path] = []
    other_abi: list[Path] = []
    untagged_code: list[Path] = []
    no_fp_arch: list[Path] = []

    for obj in objects:
        try:
            proc = subprocess.run(
                [readelf, "-A", str(obj)],
                check=False,
                capture_output=True,
                text=True,
                timeout=15,
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        blob = proc.stdout + proc.stderr
        is_stub = bool(SHADER_STUB_RE.search(obj.name))
        if "Tag_ABI_VFP_args" not in blob:
            if not is_stub:
                untagged_code.append(obj)
        elif "VFP registers" in blob:
            hard.append(obj)
        elif "FP hardware" in blob:
            softfp.append(obj)
        else:
            other_abi.append(obj)
        if not is_stub and "Tag_FP_arch" not in blob:
            no_fp_arch.append(obj)

    if softfp:
        findings.append(
            Finding(
                "R10",
                "MUST",
                repo_rel(softfp[0]),
                0,
                "object",
                True,
                "seed",
                None,
                f"{len(softfp)} object(s) stamped softfp (Tag_ABI_VFP_args: FP hardware): "
                + _shown(softfp),
            )
        )
    if other_abi:
        findings.append(
            Finding(
                "R10",
                "MUST",
                repo_rel(other_abi[0]),
                0,
                "object",
                True,
                "seed",
                None,
                f"{len(other_abi)} object(s) carry a non-hard-float Tag_ABI_VFP_args: "
                + _shown(other_abi),
            )
        )
    if untagged_code:
        findings.append(
            Finding(
                "R10",
                "MUST",
                repo_rel(untagged_code[0]),
                0,
                "object",
                True,
                "seed",
                None,
                f"{len(untagged_code)} compiled object(s) missing Tag_ABI_VFP_args: "
                + _shown(untagged_code),
            )
        )
    if not hard:
        findings.append(
            Finding(
                "R10",
                "MUST",
                repo_rel(build_dir),
                0,
                "object",
                True,
                "seed",
                None,
                f"no object in {repo_rel(build_dir)} carries Tag_ABI_VFP_args: VFP registers",
            )
        )
    if no_fp_arch:
        findings.append(
            Finding(
                "R10",
                "MUST",
                repo_rel(no_fp_arch[0]),
                0,
                "object",
                True,
                "seed",
                None,
                f"{len(no_fp_arch)} object(s) missing VFP Tag_FP_arch: " + _shown(no_fp_arch),
            )
        )
    return findings


def fpscr_asm_write(text: str) -> bool:
    """Does this text write the FPSCR inline, rather than through a helper?

    Split out of `fpscr_ok` so "establishes FZ|DN here" can be asked of a single
    function body, where a helper definition in the same file proves nothing.
    """
    if FPSCR_ASM_RE.search(text) and re.search(r"\b(FZ|DN|0x0[23]000000|1u?\s*<<\s*24|1u?\s*<<\s*25)\b", text):
        return True
    if re.search(r"vmsr\s+FPSCR", text, re.IGNORECASE) and re.search(
        r"(1u?\s*<<\s*24).*(1u?\s*<<\s*25)|(1u?\s*<<\s*25).*(1u?\s*<<\s*24)|0x03000000",
        text,
    ):
        return True
    return False


def fpscr_ok(text: str) -> bool:
    """Does this text MENTION FPSCR setup, as a helper name or as inline asm?

    This is a presence proxy and it is deliberately weak: it is the check that
    predates the 2026-10-14 tightening, kept for the coarse "does this file carry
    FZ|DN at all" questions (the main-thread row, the rationale-comment rule).
    For a worker's entry path use `fpscr_established` / `worker_helper_verdict`.
    """
    if FPSCR_HELPER_RE.search(text):
        return True
    return fpscr_asm_write(text)


_HELPER_NAMES = r"vfp_set_fz_dn|vfp_thread_fz_dn|ts_vfp_init|vfp_init_fpscr|ts_vfp_set_fpscr" + "".join(
    "|" + re.escape(h) for h in PROJECT.get("fpscr_helpers", []))   # n3ds: the project's own helper names count too
HELPER_CALL_RE = re.compile(r"\b(" + _HELPER_NAMES + r")\s*\(\s*\)")
HELPER_DEF_TAIL_RE = re.compile(r"\b(?:void|int|static|inline|constexpr|auto)\s*$")


def fpscr_called(text: str) -> bool:
    """Is a FZ|DN helper CALLED here, rather than merely defined here?

    `fpscr_ok` answers "does this text mention FPSCR setup", and using that for
    everything is exactly what made the old gate a presence check: a worker whose
    helper is defined in its unit and never called at thread entry read as
    compliant, because the definition is in the unit. Whether the helper is
    invoked is a different question and it IS decidable -- the name followed by
    `()`, not preceded by the tail of its own declaration.

    This is the test used for an include closure, where "the unit mentions FPSCR"
    is far too weak a claim: the renderer's ten fragments mention FPSCR because
    03_tev.inc defines the helper, and that alone says nothing about the
    tex-prefetch worker ever running it.
    """
    for match in HELPER_CALL_RE.finditer(text):
        if HELPER_DEF_TAIL_RE.search(text[max(0, match.start() - 40) : match.start()]):
            continue
        return True
    return False


HELPER_DEF_RE = re.compile(
    r"\b(?:void|int|static|inline|constexpr|auto)[^\n;{}]*\b"
    r"(?:" + _HELPER_NAMES + r")\s*\("
)


def fpscr_established(text: str) -> bool:
    """Is FZ|DN actually ESTABLISHED here, rather than merely offered?

    `fpscr_ok` answers "does this text mention FPSCR setup", and that is exactly
    why the old gate could not fail: delete the helper CALL from a worker and the
    helper DEFINITION two lines above still satisfied it. Here the call has to be
    present, or the FPSCR written inline. A unit that only defines the helper
    establishes nothing -- that is audio_thread.h, which hands the helper to its
    callers and is correct to do so, and is also the shape that must not be
    mistaken for compliance in a worker file.
    """
    if fpscr_called(text):
        return True
    if HELPER_DEF_RE.search(text):
        return False
    return fpscr_asm_write(text)


def worker_name_at(code: str, index: int, kind: str) -> str | None:
    """The worker routine named by the creation call at byte `index`."""
    rx = THREAD_CREATE_ARG_RE if kind == "threadCreate" else THREAD_START_ARG_RE
    match = rx.search(code, index, index + 160)
    return match.group(1) if match else None


def worker_helper_verdict(name: str, scope: Sequence[Path]) -> bool | None:
    """Does the worker routine `name` establish FZ|DN at its own entry?

    True  -- definition found, body calls the helper or writes the FPSCR.
    False -- definition found, body does neither. This is the case the old gate
             could not see: a worker whose helper call was deleted, or a new
             worker that never had one, in a file that defines the helper anyway.
    None  -- definition not in scope, so it is undecidable here. That is the
             shared sys::Thread wrapper, whose first argument is the parameter
             `fn`; its callers each name a concrete routine and are checked at
             their own sites, so the wrapper is not a hole, just not a leaf.
    """
    for path in scope:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for fn in parse_functions(strip_comments_keep_lines(text)):
            if fn.name == name:
                return bool(fpscr_called(fn.body) or fpscr_asm_write(fn.body))
    return None


def r1_site_ok(
    path: Path, code: str, index: int, kind: str,
    closures: Mapping[str, frozenset[Path]],
) -> bool:
    """Is FZ|DN established for the worker created at byte `index` of `code`?

    The worker function named by the call is the question, and it is answerable
    more often than the old gate assumed. Only when the name is a pointer
    parameter (undecidable) does this fall back to the unit-level proxy, and even
    then it demands that the unit actually CALLS a helper -- not merely define one.
    """
    scope = sorted(closures.get(str(path.resolve()), frozenset({path})))
    name = worker_name_at(code, index, kind)
    verdict = worker_helper_verdict(name, scope) if name else None
    if verdict is not None:
        return verdict
    if fpscr_established(code):
        return True
    return fpscr_established(strip_comments_keep_lines(tu_text(path, closures)))


def thread_creation_sites(
    path: Path, clean: str, closures: Mapping[str, frozenset[Path]]
) -> list[tuple[int, str, str]]:
    """Every thread-creation site in comment-stripped `clean`: (byte, kind, severity).

    `threadCreate` is a MUST -- a libctru call, so unambiguously a 3DS worker, and
    every libctru worker in this tree does float work. A `sys::Thread::start` call
    is a SHOULD, because that wrapper compiles to std::thread off-3DS and a
    desktop-only worker is not an R1 subject at all; the name must be declared as
    a Thread in the file or its unit, or `.start(` would match every unrelated
    `start()` in the tree.
    """
    sites = [(m.start(), "threadCreate", "MUST") for m in THREAD_CREATE_RE.finditer(clean)]
    if THREAD_START_RE.search(clean):
        names = set(THREAD_DECL_RE.findall(clean))
        names |= set(
            THREAD_DECL_RE.findall(strip_comments_keep_lines(tu_text(path, closures)))
        )
        for m in THREAD_START_RE.finditer(clean):
            if m.group(1) in names:
                sites.append((m.start(), "Thread::start", "SHOULD"))
    return sorted(sites)


def r1_site_detail(kind: str) -> str:
    detail = (
        "thread-creation site whose worker never establishes FPSCR FZ|DN "
        "(worker body checked, then the include closure)"
    )
    if kind == "Thread::start":
        detail += (
            " -- review: whether this worker does float work is not"
            " statically decidable (see gate_r1)"
        )
    return detail


def _fpscr_comment_ok(text: str) -> bool:
    lowered = text.lower()
    has_denormal = "denormal" in lowered
    has_fz_dn = ("flush-to-zero" in lowered or "flush to zero" in lowered) or (
        "fz" in lowered and "dn" in lowered
    )
    has_per_thread = (
        "per-thread" in lowered
        or "per thread" in lowered
        or "on this thread" in lowered
        or "each worker" in lowered
        or "every thread" in lowered
    )
    return (has_denormal or has_fz_dn) and has_per_thread


def swept_files(base: Path) -> list[Path]:
    """Every swept source file under `base`, in a stable order."""
    out: list[Path] = []
    for src in tuple(base / d for d in PROJECT.get("src_roots", [])):   # n3ds
        if not src.exists():
            continue
        for path in sorted(src.rglob("*")):
            if path.is_file() and path.suffix in CXX_EXTS and not is_excluded(path):
                out.append(path)
    return out


def include_graph(
    files: Sequence[Path],
) -> tuple[dict[Path, set[Path]], dict[Path, set[Path]]]:
    """(includes, includers) over the quoted local #include edges of `files`."""
    present = {p.resolve(): p for p in files}
    includes: dict[Path, set[Path]] = {p: set() for p in files}
    includers: dict[Path, set[Path]] = {p: set() for p in files}
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for match in LOCAL_INCLUDE_RE.finditer(text):
            src = present.get((path.parent / match.group(1)).resolve())
            if src is not None and src != path:
                includes[path].add(src)
                includers[src].add(path)
    return includes, includers


def include_closures(files: Sequence[Path]) -> dict[str, frozenset[Path]]:
    """resolved path -> the files the compiler pastes together with it, one hop out.

    That is: the file, the local headers it #includes, every file that directly
    #includes it, and the local headers each of those includes.

    A `.inc` is not a translation unit. `t2k_3ds/src/rendering/c3d/*.inc` are
    pasted into renderer_c3d.cpp, which #includes all ten, and the R1 helper for
    the worker created in 04_setup.inc:989 lives in the SIBLING fragment
    03_tev.inc:368. A per-file check therefore reports a worker with no FPSCR
    helper that does not exist -- a false positive whose only remedies would be
    duplicating the helper or pinning it away, both worse than the finding. One
    hop up (to the .cpp that pastes it) and one hop down (to the fragments pasted
    beside it) reproduces the unit the compiler sees, which is what R1 needs.

    Deliberately NOT transitive. The transitive closure of this tree's include
    graph is one blob: measured on the current tree, `audio_thread.h` reaches 111
    of the 139 swept files, because a shared header reaches engine.h and engine.h
    reaches most of t2k_core. "The unit" would then mean "the repository", and any
    presence test over it is vacuously true -- which is not a check, it is a green
    light with a docstring. Angle-bracket includes are system headers and are not
    followed.
    """
    includes, includers = include_graph(files)
    closures: dict[str, frozenset[Path]] = {}
    for path in files:
        seen = {path} | includes[path]
        for up in includers[path]:
            seen.add(up)
            seen |= includes[up]
        closures[str(path.resolve())] = frozenset(seen)
    return closures


def tu_text(path: Path, closures: Mapping[str, frozenset[Path]]) -> str:
    """The file plus everything its include closure reaches ("" if unknown)."""
    members = closures.get(str(path.resolve()))
    if not members:
        return ""
    parts: list[str] = []
    for member in sorted(members):
        try:
            parts.append(member.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
    return "\n".join(parts)


def window_after(text: str, index: int, lines: int = 40) -> str:
    start = index
    consumed = 0
    i = index
    while i < len(text) and consumed < lines:
        if text[i] == "\n":
            consumed += 1
        i += 1
    return text[start:i]


def gate_r1(root: Path | None = None) -> list[Finding]:
    """R1 -- every float thread establishes FPSCR FZ|DN at its entry.

    Three checks. The second and third are new on 2026-10-14; none of the earlier
    behaviour was removed to make room for them.

      1. The AGENTS.md 3 table. Each listed site must exist, and now each listed
         site must actually carry the helper: a listed file with no FPSCR helper in
         itself OR anywhere in its include closure is a MUST. Before this, a listed
         file lacking FPSCR produced NO finding unless it was main_3ds.cpp, which
         made the renderer_c3d.cpp row decorative -- that file contains no FPSCR
         text at all, and nothing checked it.
      2. Every thread-creation site names the routine it starts, and the gate now
         reads THAT routine: `threadCreate(texPrefetchWorker, ...)` is checked
         against the body of `texPrefetchWorker`, wherever it is defined in the
         unit. This is the tightening that 2026-10-14 was for. The old rule asked
         whether FPSCR text appeared near the call or anywhere in the file, which
         a helper DEFINITION satisfied -- so deleting a worker's helper call left
         the gate green while the thread ran with FZ|DN off. `fpscr_established`
         (call present, or FPSCR written inline) is what counts now.
      3. A tree-wide sweep of thread-creation sites, because a table is a list
         someone has to remember to update. It finds every `threadCreate` in the
         swept trees -- including the ones inside `c3d/*.inc` -- and every
         `sys::Thread::start()` call site, which hides its `threadCreate` inside
         t2k_core/src/audio/audio_thread.h and so was invisible to both the table
         and pattern 2.

    WHAT THIS CANNOT DECIDE, AND WHAT IT DOES ABOUT IT. Two things. R1's MUST is
    scoped to a worker "that touches floats", and that is not statically
    decidable here, so the sweep flags every thread-creation site whose worker
    does not establish FZ|DN, rather than pretending to know which threads do
    arithmetic. And when the creation call's first argument is a POINTER PARAMETER
    -- the shared sys::Thread wrapper passes `fn` -- there is no body to read.
    `worker_helper_verdict` returns None for that, meaning undecidable, and the
    site falls back to the unit-level rule (some file of the unit must actually
    CALL a helper, not merely define one). That is not a hole: every caller of the
    wrapper names a concrete routine and is checked at its own site, so the
    wrapper is the one place the gate declines to answer instead of guessing.
    A `threadCreate` (a libctru call, so unambiguously a 3DS worker) is a MUST:
    every libctru worker in this tree does float work, and the remedy -- calling
    the existing helper at thread entry -- costs a non-float thread nothing. A
    `sys::Thread::start` call is a SHOULD, because that wrapper compiles to
    std::thread off-3DS and a desktop-only worker is not an R1 subject at all.
    """
    findings: list[Finding] = []
    base = root or REPO_ROOT
    swept = swept_files(base)
    closures = include_closures(swept)
    any_fpscr = False
    for rel in R1_THREAD_SITES:
        path = base / rel
        if not path.exists():
            findings.append(
                Finding(
                    "R1",
                    "MUST",
                    rel,
                    0,
                    "missing",
                    True,
                    "seed",
                    None,
                    "required thread site missing",
                )
            )
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        # Comments are stripped for the SITE checks below (offsets are preserved
        # by strip_comments_keep_lines, so indices found in `text` stay valid).
        # A comment that says "FPSCR FZ|DN" is documentation, not enforcement: it
        # is what let a worker whose helper call had been deleted keep reading as
        # compliant. The rationale-comment check above deliberately keeps using
        # the raw text -- that check is ABOUT the comment.
        code = strip_comments_keep_lines(text)
        tu = tu_text(path, closures)
        tu_code = strip_comments_keep_lines(tu)
        if fpscr_ok(text):
            any_fpscr = True
            if not _fpscr_comment_ok(text):
                findings.append(
                    Finding(
                        "R1",
                        "MUST",
                        rel,
                        0,
                        "fpscr-comment",
                        True,
                        "seed",
                        None,
                        "FPSCR code without VFPU rationale comment (denormal/FZ|DN, per-thread)",
                    )
                )
        if R1_MAIN_SITE and rel == R1_MAIN_SITE and not fpscr_ok(text):
            findings.append(
                Finding(
                    "R1",
                    "MUST",
                    rel,
                    0,
                    "main",
                    True,
                    "seed",
                    None,
                    "no FPSCR FZ|DN write on main or workers",
                )
            )
        for index, kind, severity in thread_creation_sites(path, code, closures):
            # Ask the worker itself. The creation call names the routine
            # (`threadCreate(texPrefetchWorker, ...)`), so the gate reads that
            # routine's body rather than crediting the file around it -- which is
            # what let a deleted helper call pass while the definition two lines
            # above it stayed. The routine may be defined in a sibling fragment
            # (texPrefetchWorker is in 03_tev.inc, its creation in 04_setup.inc),
            # so the lookup walks the unit, not just this file.
            if not r1_site_ok(path, code, index, kind, closures):
                findings.append(
                    Finding(
                        "R1",
                        severity,
                        rel,
                        line_of(text, index),
                        kind,
                        True,
                        "seed",
                        None,
                        r1_site_detail(kind),
                    )
                )
        # The row is only real if the helper is actually there -- and "there"
        # means called or inlined, not merely defined: a file that defines
        # `vfp_thread_fz_dn` and never runs it satisfies the table on paper only.
        # The unit counts: renderer_c3d.cpp's worker helper legitimately lives in
        # c3d/03_tev.inc, and demanding a second copy in the .cpp would be the
        # screen inventing work.
        if not fpscr_established(code) and not fpscr_established(tu_code):
            findings.append(
                Finding(
                    "R1",
                    "MUST",
                    rel,
                    0,
                    "thread-site",
                    True,
                    "seed",
                    None,
                    "listed R1 thread site has no FPSCR FZ|DN helper in the file "
                    "or its include closure",
                )
            )

    # --- every thread-creation site in the tree, not just the listed files ----
    # Files already in R1_THREAD_SITES are skipped: check 2 above already covers
    # their `threadCreate` sites with the stricter per-file rule, and reporting
    # the same site twice under two details teaches nobody anything.
    listed = set(R1_THREAD_SITES)
    for path in swept:
        rel = repo_rel(path)
        if rel in listed:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        clean = strip_comments_keep_lines(text)
        sites = thread_creation_sites(path, clean, closures)
        if not sites:
            continue
        for index, kind, severity in sites:
            if r1_site_ok(path, clean, index, kind, closures):
                continue
            detail = r1_site_detail(kind)
            findings.append(
                Finding(
                    "R1",
                    severity,
                    rel,
                    line_of(text, index),
                    kind,
                    True,
                    "seed",
                    None,
                    detail,
                )
            )
    if any_fpscr:
        findings = [
            f
            for f in findings
            if not (
                (R1_MAIN_SITE and f.file == R1_MAIN_SITE)
                and f.detail == "no FPSCR FZ|DN write on main or workers"
            )
        ]
    if not any_fpscr and not any(R1_MAIN_SITE and f.file == R1_MAIN_SITE for f in findings):
        findings.append(
            Finding(
                "R1",
                "MUST",
                R1_MAIN_SITE or "main",
                0,
                "main",
                True,
                "seed",
                None,
                "no FPSCR FZ|DN write on main or workers",
            )
        )
    return _dedupe(findings)


def _dedupe(findings: Sequence[Finding]) -> list[Finding]:
    seen: set[tuple[object, ...]] = set()
    out: list[Finding] = []
    for item in findings:
        key = (
            item.rule,
            item.file,
            item.line,
            item.symbol,
            item.detail,
        )
        if key in seen:
            continue
        seen.add(key)
        out.append(item)
    return out


def gate_exemptions(text: str, rel: str) -> list[Finding]:
    findings: list[Finding] = []
    for match in re.finditer(r"/\*\s*@vfp-exempt\b", text):
        chunk = text[match.start() : match.start() + 400]
        parsed = EXEMPT_RE.match(chunk)
        if parsed is None:
            findings.append(
                Finding(
                    "R-EXEMPT",
                    "MUST",
                    rel,
                    line_of(text, match.start()),
                    "@vfp-exempt",
                    True,
                    "@hotpath",
                    None,
                    "invalid @vfp-exempt grammar",
                )
            )
            continue
        rules = [part.strip() for part in parsed.group("rules").split(",")]
        reason = parsed.group("reason")
        measured = parsed.group("measured")
        verified = parsed.group("verified")
        standing = "standing" in reason.lower()
        must_rules = [r for r in rules if r not in {"R7", "R9"}]
        if must_rules and not measured and not standing:
            findings.append(
                Finding(
                    "R-EXEMPT",
                    "MUST",
                    rel,
                    line_of(text, match.start()),
                    "@vfp-exempt",
                    True,
                    "@hotpath",
                    None,
                    "MUST exemption missing measured cost",
                )
            )
        age = None if _frozen() else _age_days(verified)
        if age is not None and age > PIN_MAX_AGE_DAYS:
            findings.append(
                Finding(
                    "R-EXPIRED",
                    "MUST",
                    rel,
                    line_of(text, match.start()),
                    "@vfp-exempt " + ",".join(rules),
                    True,
                    "@hotpath",
                    None,
                    f"exemption verified {verified}, {age}d old > {PIN_MAX_AGE_DAYS}d "
                    "-- re-verify against a current OG-profile capture, then bump the date",
                )
            )
    return findings


def scan_function(
    rel: str,
    func: Function,
    text: str,
    exemptions: Sequence[Exemption],
    hot_reason: str,
) -> list[Finding]:
    findings: list[Finding] = []
    float_ids = float_names_in(func.header + "\n" + func.body)
    loops = split_loops(func.body)
    # n3ds fix: loop/body offsets are relative to func.body, which starts after the header and its "{"; maybe() adds them to
    # func.start, so without this every body finding pointed len(header)+1 characters too early -- before an @vfp-exempt pin
    # placed right above the loop, which then never applied.
    BB = len(func.header) + 1

    def maybe(
        rule: str,
        severity: Severity,
        rel_off: int,
        detail: str,
        *,
        untyped: bool = False,
    ) -> None:
        abs_index = func.start + rel_off
        line = line_of(text, abs_index) if 0 <= abs_index < len(text) else func.line
        cover = standing_covers(rel, func.name, rule)
        local = exemption_for_index(text, exemptions, abs_index)
        if local and rule in local.rules:
            cover = cover or "inline"
        if cover:
            return
        sev: Severity = "SHOULD" if untyped and severity == "MUST" else severity
        findings.append(
            Finding(
                rule,
                sev,
                rel,
                line,
                func.name,
                True,
                hot_reason,
                None,
                detail if not untyped else f"untyped-heuristic: {detail}",
            )
        )

    for _off, loop in loops:
        loop_abs = func.body.find(loop) if loop else 0
        for match in CAST_INT_RE.finditer(loop):
            expr = loop[match.end() : match.end() + 80]
            untyped = not looks_float_expr(expr, float_ids) and "." not in expr and "float" not in expr
            if looks_int_expr(expr) and not looks_float_expr(expr, float_ids):
                continue
            maybe("R2", "MUST", BB + max(loop_abs, 0) + match.start(), "float-to-int cast in loop body", untyped=untyped)

        for match in re.finditer(r"\b(if|while|for)\s*\(", loop):
            start = match.end() - 1
            depth = 0
            j = start
            while j < len(loop):
                if loop[j] == "(":
                    depth += 1
                elif loop[j] == ")":
                    depth -= 1
                    if depth == 0:
                        j += 1
                        break
                j += 1
            cond = loop[start:j]
            if match.group(1) == "for":
                parts = cond.strip("()").split(";")
                cond_expr = parts[1] if len(parts) >= 2 else cond
            else:
                cond_expr = cond
            if condition_is_float(cond_expr, float_ids):
                maybe(
                    "R3",
                    "MUST",
                    BB + max(loop_abs, 0) + match.start(),
                    "float comparison driving control flow in hot loop",
                    untyped=not (float_ids or UNSUFFIXED_FLOAT_RE.search(cond_expr) or re.search(r"[fF]\b", cond_expr)),
                )

        for match in re.finditer(r"\?[^;]+?:", loop):
            if looks_float_expr(match.group(0), float_ids):
                maybe(
                    "R3",
                    "MUST",
                    BB + max(loop_abs, 0) + match.start(),
                    "float ternary in hot loop",
                )

        if FOR_FLOAT_RE.search(loop) or FOR_FLOAT_RE.search(func.body[_off : _off + 80] if _off >= 0 else ""):
            maybe("R5", "MUST", BB + _off, "float-typed induction variable")

        for match in re.finditer(r"([A-Za-z_]\w*)\s*\[([^\]]+)\]", loop):
            idx = match.group(2)
            if looks_float_expr(idx, float_ids) and not looks_int_expr(idx):
                maybe("R5", "MUST", BB + max(loop_abs, 0) + match.start(), "float value used as array subscript")

        for match in re.finditer(r"(?<![=\!<>])/(?![/\*=])", loop):
            left = loop[max(0, match.start() - 40) : match.start()]
            right = loop[match.end() : match.end() + 40]
            if looks_float_expr(left, float_ids) or looks_float_expr(right, float_ids):
                if looks_int_expr(left) and looks_int_expr(right) and not looks_float_expr(left + right, float_ids):
                    continue
                maybe("R7", "SHOULD", BB + max(loop_abs, 0) + match.start(), "float division in hot loop")

        for match in SQRT_RE.finditer(loop):
            maybe("R7", "SHOULD", BB + max(loop_abs, 0) + match.start(), "sqrtf in hot loop")

    whole = func.header + "\n" + func.body
    if DOUBLE_TYPE_RE.search(whole):
        match = DOUBLE_TYPE_RE.search(whole)
        assert match is not None
        maybe("R4", "MUST", match.start(), "double/long double in hot path")
    for match in UNSUFFIXED_FLOAT_RE.finditer(whole):
        maybe("R4", "MUST", match.start(), "unsuffixed float literal promotes to double")
    for match in LIBM_RE.finditer(whole):
        maybe("R6", "MUST", match.start(), "libm transcendental in hot path")
    for match in FX_IDENT_RE.finditer(whole):
        window = whole[match.start() : match.start() + 60]
        if re.search(r"[+\-*/]", window) and looks_float_expr(window, float_ids):
            maybe("R8", "SHOULD", match.start(), "fixed-point identifier mixed with float")

    if FOR_FLOAT_RE.search(func.header + func.body[:120]):
        maybe("R5", "MUST", 0, "float-typed induction variable")

    loop_spans = []
    for off, loop in loops:
        loop_spans.append((off, off + len(loop) + 8))
    for match in re.finditer(r"(?<![=\!<>])/(?![/\*=])", func.body):
        if any(start <= match.start() <= end for start, end in loop_spans):
            continue
        left = func.body[max(0, match.start() - 40) : match.start()]
        right = func.body[match.end() : match.end() + 40]
        if looks_float_expr(left, float_ids) or looks_float_expr(right, float_ids):
            if looks_int_expr(left) and looks_int_expr(right) and not looks_float_expr(left + right, float_ids):
                continue
            maybe("R7", "SHOULD", BB + match.start(), "float division inner_loop=false")
    for match in SQRT_RE.finditer(func.body):
        if any(start <= match.start() <= end for start, end in loop_spans):
            continue
        maybe("R7", "SHOULD", BB + match.start(), "sqrtf inner_loop=false")

    return findings


def scan_file(
    path: Path,
    seed: Mapping[str, HotEntry],
    cold: Sequence[str],
    *,
    force_hot: bool = False,
) -> list[Finding]:
    rel = repo_rel(path) if _is_under(path.resolve(), REPO_ROOT) else path.name
    if force_hot:
        rel_key = rel
    else:
        rel_key = repo_rel(path)
    text = path.read_text(encoding="utf-8", errors="replace")
    findings = gate_exemptions(text, rel_key)
    exemptions = parse_exemptions(text)
    annotated_lines = {line_of(text, m.start()) for m in HOTPATH_ANN_RE.finditer(text)}
    functions = parse_functions(text)
    for func in functions:
        annotated = any(func.line - 3 <= ln <= func.line for ln in annotated_lines)
        hot, reason = classify_hot(rel_key, func, seed, annotated or force_hot, cold)
        if force_hot:
            hot, reason = True, reason or "seed"
        if not hot:
            continue
        findings.extend(scan_function(rel_key, func, text, exemptions, reason))
    if force_hot and not functions:
        dummy = Function("body", 0, len(text), strip_comments_keep_lines(text), 1, "")
        findings.extend(scan_function(rel_key, dummy, text, exemptions, "seed"))
    return findings


def fragment_hot_entries(root: Path, files: Sequence[Path], seed: dict) -> list[HotEntry]:
    """Corpus entries for `.inc` fragments, derived from the file that pastes them.

    THE BLIND SPOT THIS CLOSES IS THE BIGGEST ONE IN THE FILE. The corpus names
    `t2k_3ds/src/rendering/renderer_c3d.cpp` with `symbols: ["*"]`, and the ten
    `c3d/*.inc` fragments ARE that translation unit -- they are pasted in by
    #include, and every symbol the profile captured (`build_Stars`,
    `ts_render_frame`, `packDotColor`) is defined in a fragment, not in the .cpp.
    Matching hotness by file path therefore rated the renderer's entire body as
    cold: the .cpp is 11 KB of #include lines and setup, and the ~9,000 lines that
    actually build and issue the frame were never scanned hot by any rule in this
    module. First run over the fragments: 27 MUST and 119 SHOULD.

    Hotness is inherited, never invented: a fragment is hot only because the file
    that pastes it is in the corpus, and it inherits that entry's symbol list.
    A fragment with no hot includer stays cold, which is correct -- `#include`-ing
    a header does not make the header hot, only the pasting file's body is.
    """
    _, includers = include_graph(files)
    out: list[HotEntry] = []
    for path in files:
        if path.suffix != ".inc":
            continue
        try:
            rel = path.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            continue
        if rel in seed:
            continue
        for up in sorted(includers.get(path, ())):
            up_rel = up.resolve().relative_to(root.resolve()).as_posix()
            entry = seed.get(up_rel)
            if entry is not None:
                out.append(
                    HotEntry(
                        rel,
                        entry.symbols,
                        f"pasted into {up_rel} ({entry.why})",
                        entry.source,
                    )
                )
                break
    return out


INC_HOT_BASELINE = str(PROJECT.get("inc_hot_baseline", "n3ds_port/tools/runner/contracts/inc_hot_baseline.txt"))   # n3ds


def bless_inc_hot(root: Path) -> int:
    """Write the baseline of untriaged `.inc` hot-path sites. See INC_HOT_BASELINE."""
    corpus_path = root / "tools/runner/contracts/hotpath_corpus.json"
    corpus = json.loads(corpus_path.read_text(encoding="utf-8", errors="replace"))
    files: list[Path] = []
    for base in tuple(root / d for d in PROJECT.get("src_roots", [])):   # n3ds
        if base.exists():
            files.extend(p for p in base.rglob("*") if p.is_file() and p.suffix in CXX_EXTS and not is_excluded(p))
    seed = seed_map(corpus)
    cold = [str(x) for x in corpus.get("cold_always", [])]
    for entry in fragment_hot_entries(root, files, seed):
        seed.setdefault(entry.rel, entry)
    keys: list[str] = []
    for path in files:
        rel = path.resolve().relative_to(root.resolve()).as_posix()
        if not rel.endswith(".inc"):
            continue
        for finding in scan_file(path, seed, cold):
            keys.append(inc_hot_key(finding))
    dest = root / INC_HOT_BASELINE
    dest.write_text(INC_HOT_BASELINE_HEADER + "\n".join(sorted(keys)) + "\n", encoding="utf-8")
    return len(keys)


INC_HOT_BASELINE_HEADER = """\
# Untriaged hot-path sites in c3d/*.inc -- the renderer's actual body.
#
# WHY THIS FILE EXISTS. The corpus names renderer_c3d.cpp with symbols ["*"], and
# the ten c3d/*.inc fragments ARE that translation unit -- every symbol the
# profile captured (build_Stars, ts_render_frame, packDotColor) is defined in a
# fragment, not in the .cpp. gates_vfp matched hotness by file path, so ~9,000
# lines of per-frame renderer code were rated cold by every rule in the module.
# Adding the inheritance (fragment -> the .cpp that pastes it) found these sites
# on the first run. They are not new defects: they are old sites nobody had
# looked at, and many are the same swept-window / viewport-cull / envelope float
# predicates AGENTS.md 6 says are reviewed and pinned in the .cpp files.
#
# WHAT IT IS. A record of UNREVIEWED sites, not of accepted ones. Each line is
# `file<TAB>symbol<TAB>rule<TAB>detail`, line-independent, as a MULTISET: a key
# not listed here, or one more copy of a listed key than the baseline allows, is
# reported and blocks. The list is meant to SHRINK -- a site leaves it by being
# pinned in the fragment with a real `@vfp-exempt R<n>` reason (AGENTS.md 5) or
# fixed. Regenerate with: python3 -m tools.runner.gates_vfp --bless-inc-hot
"""


def inc_hot_baseline(path: Path) -> Counter:
    """Multiset of already-known untriaged fragment findings (see INC_HOT_BASELINE)."""
    counts: Counter = Counter()
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return counts
    for line in text.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        counts[line.strip()] += 1
    return counts


def inc_hot_key(f: Finding) -> str:
    """Line-INDEPENDENT identity for a baselined fragment finding.

    Same lesson the R11 baseline already learned: a file:line baseline reports the
    same site as new after any edit above it, and a baseline that must be rewritten
    after every unrelated edit is a baseline nobody reads. `detail` is in the key
    so a second violation of the same rule in the same function is not silently
    covered by the first, and the count is a MULTISET so a third identical site is
    one more than the baseline allows.
    """
    return f"{f.file}\t{f.symbol}\t{f.rule}\t{f.detail}"


def filter_inc_hot(findings: list[Finding], baseline_path: Path) -> tuple[list[Finding], int]:
    """Drop baselined untriaged fragment findings; return (kept, dropped)."""
    allowed = inc_hot_baseline(baseline_path)
    kept: list[Finding] = []
    dropped = 0
    for f in findings:
        if f.file.endswith(".inc"):
            key = inc_hot_key(f)
            if allowed.get(key, 0) > 0:
                allowed[key] -= 1
                dropped += 1
                continue
        kept.append(f)
    return kept, dropped


def scan_tree(
    root: Path,
    corpus: Mapping[str, object],
    *,
    ingest_profile: bool = True,
) -> tuple[list[Finding], str]:
    extra: list[HotEntry] = []
    source = "seed"
    if ingest_profile:
        extra, source = ingest_perf_logs(PERF_LOG_DIR)   # n3ds
    seed = seed_map(corpus, extra)
    cold_raw = corpus.get("cold_always", [])
    cold = [str(x) for x in cold_raw] if isinstance(cold_raw, list) else []
    findings: list[Finding] = []
    src_roots = tuple(root / d for d in PROJECT.get("src_roots", []))   # n3ds
    files: list[Path] = []
    for base in src_roots:
        if base.exists():
            files.extend(p for p in base.rglob("*") if p.is_file() and p.suffix in CXX_EXTS and not is_excluded(p))
    for entry in fragment_hot_entries(root, files, seed):
        seed.setdefault(entry.rel, entry)
    targets = {entry.rel for entry in seed.values()}
    for path in files:
        rel = path.resolve().relative_to(root.resolve()).as_posix()
        if rel not in targets and not any(rel.startswith(c.rstrip("/") + "/") for c in targets):
            if not HOTPATH_ANN_RE.search(path.read_text(encoding="utf-8", errors="replace")):
                continue
        findings.extend(scan_file(path, seed, cold))
    findings, dropped = filter_inc_hot(findings, root / INC_HOT_BASELINE)
    if dropped:
        source += f"; {dropped} baselined untriaged .inc hot-path site(s)"
    return findings, source


def gate_r9(build_dir: Path) -> list[Finding]:
    if os.environ.get("VFP_OBJDUMP") != "1":
        return []
    if not build_dir.exists():
        return []
    objdump = shutil.which("arm-none-eabi-objdump")
    if not objdump:
        return []
    findings: list[Finding] = []
    s_reg = re.compile(r"\bs([0-9]|[12][0-9]|3[01])\b")
    for obj in sorted(list(build_dir.rglob("*.o")) + list(build_dir.rglob("*.obj")))[:20]:   # n3ds
        try:
            proc = subprocess.run(
                [objdump, "-d", str(obj)],
                check=False,
                capture_output=True,
                text=True,
                timeout=20,
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        live = {m.group(0) for m in s_reg.finditer(proc.stdout)}
        if len(live) > 24:
            findings.append(
                Finding(
                    "R9",
                    "SHOULD",
                    repo_rel(obj),
                    0,
                    obj.stem,
                    True,
                    "seed",
                    None,
                    f"live VFP s-regs {len(live)} > 24",
                )
            )
    return findings


def gate_corpus_freshness(corpus: Mapping[str, object], docs_dir: Path) -> list[Finding]:
    """The corpus is a MEASUREMENT, not a preference list.

    AGENTS.md 2 defines hot as "top 15 of a captured profile" and warns that a
    hand-maintained list silently stops covering the code. That warning only
    bites if the capture has a date attached and that date is checked: an
    undated corpus reads as current forever, which is exactly the decay the rule
    describes. The newest-log cross-check exists so the date cannot be bumped
    without a capture actually landing on disk.
    """
    findings: list[Finding] = []
    corpus_rel = repo_rel(DEFAULT_CORPUS)
    captured = corpus.get("captured")
    if not isinstance(captured, str) or not captured.strip():
        findings.append(
            Finding(
                "R-CORPUS",
                "MUST",
                corpus_rel,
                0,
                "hotpath_corpus",
                True,
                "corpus",
                None,
                "corpus declares no `captured` date -- hot is a measurement "
                "(AGENTS.md 2), so the corpus must say when it was measured",
            )
        )
        return findings
    age = None if _frozen() else _age_days(captured)
    if age is not None and age > CORPUS_MAX_AGE_DAYS:
        findings.append(
            Finding(
                "R-CORPUS",
                "MUST",
                corpus_rel,
                0,
                "hotpath_corpus",
                True,
                "corpus",
                None,
                f"hot-path corpus captured {captured}, {age}d old > "
                f"{CORPUS_MAX_AGE_DAYS}d -- 'top 15 of a captured profile' is "
                "meaningless against a stale capture; re-capture and regenerate",
            )
        )
    newest = ""
    if docs_dir.exists():
        for log in docs_dir.glob("perf-*.log"):
            stamp = re.search(r"(\d{4}-\d{2}-\d{2})", log.name)
            if stamp and stamp.group(1) > newest:
                newest = stamp.group(1)
    if newest and captured > newest:
        findings.append(
            Finding(
                "R-CORPUS",
                "SHOULD",
                corpus_rel,
                0,
                "hotpath_corpus",
                True,
                "corpus",
                None,
                f"corpus claims captured {captured} but the newest perf log on "
                "disk is {0} -- the date was bumped without a capture".format(newest),
            )
        )
    return findings


def run_audit(
    root: Path | None = None,
    *,
    corpus_path: Path | None = None,
    makefile: Path | None = None,
    ingest_profile: bool = True,
    scan_sources: bool = True,
) -> AuditResult:
    root = root or REPO_ROOT
    corpus = load_corpus(corpus_path or DEFAULT_CORPUS)
    findings: list[Finding] = []
    notes: list[str] = []
    findings.extend(gate_r10(makefile))   # n3ds: toolchain + CMakeLists from project.json
    findings.extend(probe_objects(BUILD_DIR))
    findings.extend(gate_r1(root))
    findings.extend(gate_corpus_freshness(corpus, PERF_LOG_DIR))
    source = "seed"
    if scan_sources:
        scanned, source = scan_tree(root, corpus, ingest_profile=ingest_profile)
        findings.extend(scanned)
    r9 = gate_r9(BUILD_DIR)
    findings.extend(r9)
    if os.environ.get("VFP_OBJDUMP") != "1":
        notes.append("R9 omitted (VFP_OBJDUMP!=1)")
    elif not BUILD_DIR.exists():
        notes.append(f"R9 omitted (no {repo_rel(BUILD_DIR)})")
    frozen = _frozen()
    if frozen:
        notes.append(
            f"staleness clocks waived (corpus age, pin age): project frozen at "
            f"{frozen}. Substantive rules still run. VFP_FROZEN_AT='' restores them."
        )
    findings = _dedupe(findings)
    blockers = sum(1 for f in findings if f.severity == "MUST")
    reviews = sum(1 for f in findings if f.severity == "SHOULD")
    status: Status = "FAIL" if blockers else "PASS"
    return AuditResult(status, blockers, reviews, findings, source, notes)


def write_result(result: AuditResult, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(result.to_json(), indent=2) + "\n", encoding="utf-8")


def _cli(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="gates_vfp", description="VFP hot-path gates R1–R10")
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    parser.add_argument("--makefile", type=Path, default=None)
    parser.add_argument("--out", type=Path, default=DEFAULT_LOG)
    parser.add_argument("--no-profile", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument(
        "--bless-inc-hot",
        action="store_true",
        help="rewrite the baseline of untriaged .inc hot-path sites and exit",
    )
    args = parser.parse_args(argv)
    if args.bless_inc_hot:
        print(f"blessed {bless_inc_hot(args.root)} untriaged .inc hot-path site(s)")
        return 0
    result = run_audit(
        args.root,
        corpus_path=args.corpus,
        makefile=args.makefile,
        ingest_profile=not args.no_profile,
    )
    write_result(result, args.out)
    if args.json:
        json.dump(result.to_json(), sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        print(f"{result.status} blockers={result.blockers} reviews={result.reviews}")
        for note in result.notes:
            print(f"note: {note}")
        for finding in result.findings:
            print(
                f"{finding.rule} {finding.severity} {finding.file}:{finding.line} "
                f"{finding.symbol} {finding.detail}"
            )
    return 3 if result.status == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(_cli())
