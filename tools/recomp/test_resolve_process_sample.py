#!/usr/bin/env python3

import pathlib
import tempfile

import resolve_process_sample as resolver


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        map_path = pathlib.Path(temporary) / "sample.map"
        map_path.write_text(
            " Preferred load address is 0000000140000000\n"
            " 0001:00000000       first 0000000140001000 f   first.obj\n"
            " 0001:00000100       second 0000000140001100 f   second.obj\n",
            encoding="utf-8",
        )
        preferred, symbols = resolver.load_map(map_path)
        assert preferred == 0x140000000
        assert symbols == [(0x1000, "first"), (0x1100, "second")]

        text = "0x7FF600001000 <- 0x7FF60000112A <- 0x7FF700000000"
        actual = resolver.resolve_text(text, 0x7FF600000000, symbols)
        assert actual == "first <- second+0x2A <- 0x7FF700000000", actual

        external = resolver.resolve_text("0x7FF500000010", 0x7FF600000000, symbols)
        assert external == "0x7FF500000010"

    print("resolve_process_sample regression checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())