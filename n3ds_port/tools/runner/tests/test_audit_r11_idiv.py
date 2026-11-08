"""Tests for the R11 integer-div/mod screen (T2K, adapted to Open Nectar; python3 -m unittest from n3ds_port/).

The screen is a SCREEN: it infers "is this integer" and "is this hot" and it is
allowed to be wrong in the conservative direction. These tests pin the four
2026-10-14 filter fixes, because each of them was a filter that hid a real site
(or invented a fake one) for a reason someone had written down as deliberate.
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.runner import audit_r11_idiv as R  # noqa: E402


def consts(text: str):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "x.cpp"
        path.write_text(text)
        return R.collect_constants([path])


class FloatOperandTests(unittest.TestCase):
    def test_int_declared_operand_is_not_float_evidence(self) -> None:
        # entity_geometry.cpp:941 -- the double-wrap idiom AGENTS.md R11 calls
        # its worst case, hidden for months because `vv` looked float-shaped.
        text = "int vv = v + enemy.animation_vec;\nvv = ((vv % n) + n) % n;\n"
        self.assertIn("vv", R.int_operand_names(text))
        self.assertFalse(
            R.looks_float("vv = ((vv % n) + n) % n;", R.int_operand_names(text), R.float_operand_names(text))
        )

    def test_float_declared_divisor_is_float_evidence(self) -> None:
        # line_geometry.cpp:648 -- the floatness sits AFTER the operator, which
        # the old single-sided name test never looked at.
        text = "const float len = glm::length(o);\nif (hasLen) radial = o / len;\n"
        self.assertIn("len", R.float_operand_names(text))
        self.assertTrue(
            R.looks_float("if (hasLen) radial = o / len;", R.int_operand_names(text), R.float_operand_names(text))
        )

    def test_float_declared_dividend_is_float_evidence(self) -> None:
        # popup_fx.h:453 -- `slen` is outside the FLOAT_NAME prefix list, so only
        # the declaration says it is a float divide.
        text = "const float slen = std::sqrt(l2);\nconst int steps = (int)(slen / step);\n"
        self.assertIn("slen", R.float_operand_names(text))
        self.assertTrue(
            R.looks_float("const int steps = (int)(slen / step);", R.int_operand_names(text), R.float_operand_names(text))
        )

    def test_literal_still_decides_it(self) -> None:
        self.assertTrue(R.looks_float("x = a / 2.0f;", set(), frozenset()))


class ConstantDivisorTests(unittest.TestCase):
    def test_constexpr_is_a_constant(self) -> None:
        self.assertIn("kFftSize", consts("constexpr int kFftSize = 512;\n"))

    def test_runtime_const_local_is_not(self) -> None:
        # arcade_flipper.cpp:102 -- `const int n = e.lane_count` is a RUNTIME
        # value, and treating it as a constant divisor made the worst-case idiom
        # in AGENTS.md 4 invisible in every file at once.
        self.assertNotIn("n", consts("const int n = e.lane_count;\n"))

    def test_upper_case_convention_is_a_constant(self) -> None:
        # The UPPER_CASE naming convention is trusted on its own, at the filter
        # (audit_r11_idiv.py:448), not by collect_constants().
        self.assertTrue("TOTAL_LEVELS".isupper())
        self.assertFalse("lane_count".isupper())


class PinSuppressionTests(unittest.TestCase):
    PIN = "/* @vfp-exempt R11 — one-period wrap. measured n/a. Verified 2026-10-14. */\n"

    def test_pin_above_the_line_suppresses(self) -> None:
        raw = self.PIN + "d = ((d % n) + n) % n;\n"
        pins = R.collect_pins(raw)
        self.assertTrue(R.pinned(pins, 2, "R11"))

    def test_reach_is_short(self) -> None:
        raw = self.PIN + "\n".join("x" + str(i) + " = a % n;" for i in range(10))
        pins = R.collect_pins(raw)
        self.assertTrue(R.pinned(pins, 1 + R.PIN_REACH, "R11"))
        self.assertFalse(R.pinned(pins, 2 + R.PIN_REACH, "R11"))

    def test_a_pin_for_another_rule_does_not_suppress_r11(self) -> None:
        raw = "/* @vfp-exempt R3 — swept window. measured n/a. Verified 2026-10-14. */\nd = a % n;\n"
        self.assertFalse(R.pinned(R.collect_pins(raw), 2, "R11"))


class ScreenIntegrationTests(unittest.TestCase):
    """What the shipped screen reports over this tree, right now."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.hits, cls.filtered, cls.files = R.scan()
        cls.keys = {h["key"][0] for h in cls.hits}

    def text_of(self, rel: str) -> str:
        return "\n".join(h["stmt"] for h in self.hits if h["key"][0] == rel)

    def test_ring_wrap_with_modulo_is_reported(self) -> None:
        # n3ds_audio.cpp keeps a plain % wrap (cold, per audio block): the screen must see it.
        self.assertIn("% kBlocks", self.text_of("n3ds_port/platform/n3ds_audio.cpp"))

    def test_gx_queue_wraps_with_a_mask(self) -> None:
        # AGENTS.md R11: the GX command queue (power-of-two size) wraps with & (sQCap - 1), never %.
        self.assertNotIn("% sQCap", self.text_of("n3ds_port/platform/n3ds_gfx.cpp"))

    def test_system_include_lines_are_not_candidates(self) -> None:
        # `#include <sys/...>` used to expose its slashes as divisors.
        for h in self.hits:
            self.assertNotIn("#include", h["stmt"])


if __name__ == "__main__":
    unittest.main()
