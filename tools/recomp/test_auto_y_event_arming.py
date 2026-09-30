#!/usr/bin/env python3
"""Regression coverage for event-armed unattended aircraft input."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "input" / "xinput_device.c").read_text(encoding="utf-8")
MANUAL = (ROOT / "ports" / "mercenaries" / "src" / "recomp_manual.c").read_text(encoding="utf-8")

assert 'getenv("MERCENARIES_TEST_AUTO_Y_AFTER_USE_ONLY") != NULL' in SOURCE
assert 'getenv("MERCENARIES_TEST_AUTO_Y_AFTER_COLLISION_DISABLED") != NULL' in SOURCE
assert "after_movie || after_use || after_collision" in SOURCE
assert "after_armed_event ? 2500u : 100000u" in SOURCE
assert "if (after_armed_event && g_test_auto_y_armed_ms != 0u)" in SOURCE
assert "(!after_armed_event || g_test_auto_y_armed_ms != 0u)" in SOURCE
assert "after-movie=%u after-use=%u after-collision=%u" in SOURCE
assert '"MERCENARIES_TEST_AUTO_MOVE_TURN_MS"' in SOURCE
assert '"MERCENARIES_TEST_AUTO_MOVE_USE_DELAY_MS"' in SOURCE
assert '"MERCENARIES_TEST_AUTO_MOVE_USE_PULSE_MS"' in SOURCE
assert '"MERCENARIES_TEST_AUTO_MOVE_NO_STICK"' in SOURCE
assert "if (!no_stick && move_offset < turn_duration)" in SOURCE
assert "state->Gamepad.sThumbRX = stick_max" in SOURCE
assert "move_offset >= turn_duration + use_delay" in SOURCE
assert "use_pulse" in SOURCE
assert 'getenv("MERCENARIES_TRACE_SCRIPT_USE_AFTER_MOVIE") != NULL ||' in MANUAL
assert 'getenv("MERCENARIES_TEST_AUTO_Y_AFTER_USE_ONLY") != NULL)' in MANUAL
movie_close = MANUAL.index('"[XACT-CUE] trace enabled after movie close')
standup_trace = MANUAL.index('"[SCRIPT-USE-TRACE] enabled after movie close', movie_close)
movie_close_block = MANUAL[movie_close:standup_trace]
assert 'getenv("MERCENARIES_TEST_AUTO_Y_AFTER_USE_ONLY") != NULL' in movie_close_block

print("auto-Y event arming regression passed")