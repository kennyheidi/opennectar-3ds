"""T2K gates_vfp tests, adapted to Open Nectar (python3 -m unittest tools.runner.tests.test_gates_vfp from n3ds_port/).

The rule tests run on the T2K fixtures unchanged; the repo tests run on this tree. MIT, see ../LICENSE.t2k."""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.runner.gates_vfp import (  # noqa: E402
    Finding,
    gate_r1,
    gate_r10,
    run_audit,
    scan_file,
    seed_map,
)

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "vfp"
EMPTY_SEED = seed_map({"seed": [], "cold_always": []})
HOT_SEED = seed_map(
    {
        "seed": [{"file": p.name, "symbols": ["*"], "why": "fixture"}]
        for p in FIXTURES.glob("*.cpp")
    }
)


def rules(path: Path, *, force_hot: bool = True, cold: list[str] | None = None) -> list[Finding]:
    return scan_file(path, HOT_SEED if force_hot else EMPTY_SEED, cold or [], force_hot=force_hot)


def rule_ids(findings: list[Finding], severity: str | None = None) -> set[str]:
    out = set()
    for item in findings:
        if severity and item.severity != severity:
            continue
        out.add(item.rule)
    return out


class MakefileAbiTests(unittest.TestCase):
    def test_good_makefile(self) -> None:
        findings = gate_r10(FIXTURES / "makefile_abi.mk")
        self.assertEqual(findings, [])

    def test_bad_makefile(self) -> None:
        findings = gate_r10(FIXTURES / "makefile_abi_bad.mk")
        ids = rule_ids(findings, "MUST")
        self.assertIn("R10", ids)
        blob = " ".join(f.detail for f in findings)
        self.assertTrue("soft" in blob or "mfpu" in blob or "errno" in blob)


class HotPathScanTests(unittest.TestCase):
    def test_integer_counter_float_body_ok(self) -> None:
        findings = rules(FIXTURES / "hot_ok.cpp")
        self.assertEqual(rule_ids(findings, "MUST"), set())

    def test_r2_cast_in_loop(self) -> None:
        findings = rules(FIXTURES / "hot_r2.cpp")
        names = {f.symbol for f in findings if f.rule == "R2"}
        self.assertIn("hot_r2", names)
        self.assertNotIn("hot_r2_hoisted", names)

    def test_r3_float_compare(self) -> None:
        findings = rules(FIXTURES / "hot_r3.cpp")
        names = {f.symbol for f in findings if f.rule == "R3"}
        self.assertIn("hot_r3", names)
        self.assertNotIn("hot_r3_int_pred", names)

    def test_r4_unsuffixed_and_double(self) -> None:
        findings = rules(FIXTURES / "hot_r4.cpp")
        names = {(f.symbol, f.rule) for f in findings if f.rule == "R4"}
        self.assertTrue(any(sym == "hot_r4_bad" for sym, _ in names))
        self.assertTrue(any(sym == "hot_r4_double" for sym, _ in names))
        self.assertFalse(any(sym == "hot_r4_ok" for sym, _ in names))

    def test_r5_float_induction_and_index(self) -> None:
        findings = rules(FIXTURES / "hot_r5.cpp")
        names = {f.symbol for f in findings if f.rule in {"R5", "R2"}}
        self.assertIn("hot_r5_for_float", names)
        self.assertIn("hot_r5_index", names)

    def test_r6_libm_vs_lut(self) -> None:
        findings = rules(FIXTURES / "hot_r6.cpp")
        names = {f.symbol for f in findings if f.rule == "R6"}
        self.assertIn("hot_r6_libm", names)
        self.assertNotIn("hot_r6_lut", names)

    def test_r7_inner_vs_outer(self) -> None:
        findings = rules(FIXTURES / "hot_r7.cpp")
        inner = [f for f in findings if f.rule == "R7" and f.symbol == "hot_r7_inner"]
        outer = [f for f in findings if f.rule == "R7" and f.symbol == "hot_r7_outer"]
        self.assertTrue(inner)
        self.assertTrue(all(f.severity == "SHOULD" for f in inner))
        self.assertTrue(outer)
        self.assertTrue(any("inner_loop=false" in f.detail for f in outer))

    def test_r8_fixed_point_mix(self) -> None:
        findings = rules(FIXTURES / "hot_r8.cpp")
        self.assertIn("R8", rule_ids(findings))

    def test_cold_double_zero_findings(self) -> None:
        findings = rules(FIXTURES / "cold_double.cpp", force_hot=False)
        self.assertEqual(findings, [])

    def test_inline_exempt_r3(self) -> None:
        findings = rules(FIXTURES / "exempt_r3.cpp")
        self.assertNotIn("R3", rule_ids(findings, "MUST"))

    def test_math_lut_standing_exemption(self) -> None:
        findings = rules(FIXTURES / "math_lut.h")
        must = [f for f in findings if f.severity == "MUST"]
        self.assertEqual(must, [], [asdictish(f) for f in must])


def asdictish(finding: Finding) -> str:
    return f"{finding.rule}:{finding.symbol}:{finding.detail}"


class RepoContractTests(unittest.TestCase):
    """Open Nectar: the real tree is R1/R10 clean (n3ds_port/AGENTS.md section 3)."""

    def setUp(self) -> None:
        from tools.runner import gates_vfp

        self.g = gates_vfp

    def test_r10_real_build_passes(self) -> None:
        self.assertEqual([asdictish(f) for f in gate_r10()], [])

    def test_r1_real_tree_is_clean(self) -> None:
        must = [f for f in gate_r1(self.g.REPO_ROOT) if f.rule == "R1" and f.severity == "MUST"]
        self.assertEqual([asdictish(f) for f in must], [])

    def test_r1_fpscr_comment_required(self) -> None:
        from tools.runner.gates_vfp import _fpscr_comment_ok

        bare = "static void h() { unsigned f; __asm__ volatile (\"vmrs %0, fpscr\" : \"=r\"(f)); }"
        self.assertFalse(_fpscr_comment_ok(bare))
        documented = "// VFPU (R1): denormals stall without FZ; per-thread FPSCR FZ|DN.\n" + bare
        self.assertTrue(_fpscr_comment_ok(documented))

    def test_run_audit_has_no_r1_r10(self) -> None:
        result = run_audit(self.g.REPO_ROOT, ingest_profile=False)
        self.assertFalse(any(f.rule == "R1" for f in result.findings))
        self.assertFalse(any(f.rule == "R10" for f in result.findings))


class R1WorkerEntryTests(unittest.TestCase):
    """R1 asks the WORKER, not the file it lives in: deleting a thread's n3ds_vfp_runfast() call must turn the gate red
    even though the file still mentions the helper (its rationale comment, an extern declaration)."""

    SUBS = ("n3ds_port/platform",)

    def setUp(self) -> None:
        from tools.runner import gates_vfp

        self.g = gates_vfp
        self._repo = gates_vfp.REPO_ROOT
        self.tmp = Path(tempfile.mkdtemp(prefix="nectar-r1-"))
        for sub in self.SUBS:
            shutil.copytree(self._repo / sub, self.tmp / sub)
        gates_vfp.REPO_ROOT = self.tmp

    def tearDown(self) -> None:
        self.g.REPO_ROOT = self._repo
        shutil.rmtree(self.tmp, ignore_errors=True)

    def r1(self) -> list[Finding]:
        return [f for f in self.g.gate_r1(self.tmp) if f.rule == "R1"]

    def cut(self, rel: str, needle: str) -> None:
        path = self.tmp / rel
        text = path.read_text()
        self.assertIn(needle, text, rel)
        path.write_text(text.replace(needle, "    // deleted", 1))

    def probe(self, body: str) -> None:
        (self.tmp / "n3ds_port" / "platform" / "probe_worker.cpp").write_text(body)

    def test_clean_tree_is_r1_clean(self) -> None:
        self.assertEqual([asdictish(f) for f in self.r1() if f.severity == "MUST"], [])

    def test_render_worker_helper_call_removed_is_must(self) -> None:
        self.cut("n3ds_port/platform/n3ds_worker.cpp", "    n3ds_vfp_runfast();\n")
        must = [f for f in self.r1() if f.severity == "MUST"]
        self.assertTrue(any(f.file.endswith("n3ds_worker.cpp") for f in must), [asdictish(f) for f in must])

    def test_mixer_helper_call_removed_is_must(self) -> None:
        self.cut("n3ds_port/platform/n3ds_sdl_audio.cpp", "    n3ds_vfp_runfast();\n")
        must = [f for f in self.r1() if f.severity == "MUST"]
        self.assertTrue(any(f.file.endswith("n3ds_sdl_audio.cpp") for f in must), [asdictish(f) for f in must])

    def test_new_worker_without_helper_is_must(self) -> None:
        self.probe(
            "void probeWorker(void*) {\n"
            "    volatile float x = 1.0f;\n"
            "    (void)x;\n"
            "}\n"
            "void start_probe() {\n"
            "    s_thread = threadCreate(probeWorker, nullptr, 8 * 1024, 0, -2, false);\n"
            "}\n"
        )
        findings = [f for f in self.r1() if f.file.endswith("probe_worker.cpp")]
        self.assertEqual([f.symbol for f in findings], ["threadCreate"])
        self.assertEqual(findings[0].severity, "MUST")

    def test_new_worker_that_calls_the_helper_is_clean(self) -> None:
        self.probe(
            "extern \"C\" void n3ds_vfp_runfast(void);\n"
            "void probeWorker(void*) {\n"
            "    n3ds_vfp_runfast();\n"
            "    volatile float x = 1.0f;\n"
            "    (void)x;\n"
            "}\n"
            "void start_probe() {\n"
            "    s_thread = threadCreate(probeWorker, nullptr, 8 * 1024, 0, -2, false);\n"
            "}\n"
        )
        self.assertEqual([asdictish(f) for f in self.r1() if f.file.endswith("probe_worker.cpp")], [])


class R1HelperTests(unittest.TestCase):
    """The three questions the old gate collapsed into one."""

    def setUp(self) -> None:
        from tools.runner import gates_vfp

        self.g = gates_vfp

    def test_definition_is_offered_not_established(self) -> None:
        defines = 'extern "C" void n3ds_vfp_runfast(void) { }\n'
        self.assertTrue(self.g.fpscr_ok(defines))
        self.assertFalse(self.g.fpscr_established(defines))

    def test_call_establishes(self) -> None:
        self.assertTrue(self.g.fpscr_established("void w(void*) { n3ds_vfp_runfast(); }"))

    def test_inline_asm_establishes(self) -> None:
        text = "unsigned f; __asm__ volatile(\"vmrs %0, FPSCR\"); f |= (1u << 24) | (1u << 25);"
        self.assertTrue(self.g.fpscr_established(text))

    def test_worker_name_extraction(self) -> None:
        code = "sWorker = threadCreate(workerMain, nullptr, 96 * 1024, prio, 2, false);"
        self.assertEqual(self.g.worker_name_at(code, code.index("threadCreate"), "threadCreate"), "workerMain")
        code2 = "s_thread.start(analyzerThreadMain, nullptr, 16 * 1024);"
        self.assertEqual(self.g.worker_name_at(code2, code2.index("s_thread"), "Thread::start"), "analyzerThreadMain")

    def test_verdict_is_none_for_an_unbound_pointer(self) -> None:
        # The shared wrapper's first argument is the parameter `fn`; there is no
        # routine to read, and that is reported as undecidable rather than as a
        # pass or a finding.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "w.cpp"
            path.write_text("void go() { threadCreate(fn, arg, 8192, 0, -2, false); }\n")
            self.assertIsNone(self.g.worker_helper_verdict("fn", [path]))


class IncBaselineTests(unittest.TestCase):
    def setUp(self) -> None:
        from tools.runner import gates_vfp

        self.g = gates_vfp

    def test_baseline_absorbs_known_and_blocks_new(self) -> None:
        f = Finding("R3", "MUST", "x/y.inc", 12, "fn", True, "seed", None, "float comparison")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "b.txt"
            path.write_text("")
            self.assertEqual(self.g.filter_inc_hot([f], path), ([f], 0))
            path.write_text(self.g.inc_hot_key(f) + "\n")
            self.assertEqual(self.g.filter_inc_hot([f], path), ([], 1))
            # A multiset: a second copy of a baselined key is one more than allowed.
            self.assertEqual(self.g.filter_inc_hot([f, f], path)[1], 1)


if __name__ == "__main__":
    unittest.main()
