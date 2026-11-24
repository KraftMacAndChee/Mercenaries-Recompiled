"""The USA and EU-FR retail revisions are accepted consistently everywhere.

The EU-FR default.xbe carries the same code, section layout and entry point as
the USA one; only its certificate differs. Every gate that pins the retail XBE
must therefore accept exactly the same two hashes, and the virtual console must
report the language whose DATAxbox/<language>.dsk the disc actually ships.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "ports" / "mercenaries"
USA = "AA08EA21D952AC35F49C02C7E2ED08AA25AD7535FBBBCCC95636775F37BE99D7"
EU_FR = "9DD26D6CE030F467462A4A8132AC82217ECEA69ECFEF5FAB9238392642FDC356"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def c_byte_arrays_as_hex(text: str) -> list[str]:
    """Return every 32-byte brace initializer in text as uppercase hex."""
    arrays = []
    for body in re.findall(r"\{([^{}]*)\}", text):
        values = re.findall(r"0x([0-9A-Fa-f]{2})\b", body)
        if len(values) == 32:
            arrays.append("".join(values).upper())
    return arrays


def test_powershell_gates_accept_both_revisions() -> None:
    for script in ("Extract-Game.ps1", "Recompile.ps1"):
        text = read(PORT / "scripts" / script)
        assert f'"{USA}" = "USA"' in text
        assert f'"{EU_FR}" = "EU-FR"' in text
        assert "$supportedXbeHashes.ContainsKey($actualHash)" in text
        assert "$expectedXbeHash" not in text


def test_runtime_and_launcher_pin_the_same_two_hashes() -> None:
    main = read(PORT / "src" / "main.c")
    table = main[main.index("g_supported_retail_xbe_sha256[][32]") :]
    table = table[: table.index("};") + 2]
    assert c_byte_arrays_as_hex(table) == [USA, EU_FR]

    launcher = read(PORT / "src" / "setup_launcher.c")
    profiles = launcher[launcher.index("k_retail_profiles[]") :]
    profiles = profiles[: profiles.index("\n};") + 3]
    assert c_byte_arrays_as_hex(profiles) == [USA, EU_FR]
    # Each profile's textual hash (written into the install marker) sits in the
    # same order as its binary digest.
    assert profiles.index(USA) < profiles.index(EU_FR)
    assert 'L"DATAxbox\\\\english.dsk", 434ULL' in profiles
    assert 'L"DATAxbox\\\\french.dsk", 405ULL' in profiles


def test_console_language_follows_the_installed_language_pack() -> None:
    header = read(ROOT / "src" / "kernel" / "kernel.h")
    # Retail XC_LANGUAGE values used by XAPI's XGetLanguage.
    for name, value in (
        ("ENGLISH", 1), ("JAPANESE", 2), ("GERMAN", 3), ("FRENCH", 4),
        ("SPANISH", 5), ("ITALIAN", 6), ("KOREAN", 7), ("CHINESE", 8),
        ("PORTUGUESE", 9),
    ):
        assert re.search(rf"#define XC_LANGUAGE_{name}\s+{value}\b", header)

    kernel = read(ROOT / "src" / "kernel" / "kernel_xbox.c")
    assert "static ULONG xbox_console_language = XC_LANGUAGE_ENGLISH;" in kernel
    assert "settings[12] = xbox_console_language;" in kernel
    query = kernel[kernel.index("case XC_LANGUAGE:") :]
    query = query[: query.index("break;")]
    assert "*(PULONG)Value = xbox_console_language;" in query

    main = read(PORT / "src" / "main.c")
    for pack, language in (
        ("english.dsk", "ENGLISH"), ("french.dsk", "FRENCH"),
        ("german.dsk", "GERMAN"), ("italian.dsk", "ITALIAN"),
        ("spanish.dsk", "SPANISH"),
    ):
        assert re.search(rf'\{{ "{re.escape(pack)}",\s+XC_LANGUAGE_{language},', main)
    # English is probed first so USA discs keep their original behavior, and
    # the language is chosen only after the XBE revision has been verified.
    assert main.index('"english.dsk"') < main.index('"french.dsk"')
    verified = main.index('printf("Retail XBE SHA-256 verified.\\n");')
    assert verified < main.index("select_console_language_for_disc();", verified)


if __name__ == "__main__":
    test_powershell_gates_accept_both_revisions()
    test_runtime_and_launcher_pin_the_same_two_hashes()
    test_console_language_follows_the_installed_language_pack()
    print("ok mercenaries EU-FR revision")
