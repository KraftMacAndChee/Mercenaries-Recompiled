from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCHER = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0000.c"
MANUAL = ROOT / "ports/mercenaries/src/recomp_manual.c"


def test_retail_vehicle_reward_path_is_observable_without_host_side_grants() -> None:
    patcher = PATCHER.read_text(encoding="utf-8")
    generated = GENERATED.read_text(encoding="utf-8")
    manual = MANUAL.read_text(encoding="utf-8")

    assert "retail vehicle reward damage attribution diagnostics" in patcher
    assert "retail vehicle reward CashValue diagnostics" in patcher
    assert "retail vehicle reward SetMoney diagnostics" in patcher
    assert "retail FindCulprit empty-result diagnostics" in patcher
    assert "retail FindCulprit winner diagnostics" in patcher
    assert generated.count("recomp_vehicle_reward_checkpoint(") >= 5
    assert generated.count("recomp_find_culprit_checkpoint(") == 2
    assert 'getenv("MERCENARIES_TRACE_VEHICLE_REWARD")' in manual
    assert "guest_u32(source + 0xAC4u)" in manual
    assert "source = pending_player;" in manual
    assert "if (pending_player == 0u)" in manual
    assert "recomp_trace_reward_vehicle_census" in manual
    assert "[REWARD-VEHICLE-CENSUS]" in manual
    assert "[FIND-CULPRIT]" in manual
    assert "money-before=%.9g money=%.9g" in manual
    assert '"money-bits=%08X delta=%.9g payout-matches=%u current=%08X\\n"' in manual
    assert "fabsf(delta.value - cash.value) <= 0.01f" in manual
    assert "const float authored_x = 1483.828f;" in manual
    assert "const float authored_z = 1173.829f;" in manual
    assert "SetMoney(" not in manual


if __name__ == "__main__":
    test_retail_vehicle_reward_path_is_observable_without_host_side_grants()
    print("Retail vehicle-reward trace checks passed")
