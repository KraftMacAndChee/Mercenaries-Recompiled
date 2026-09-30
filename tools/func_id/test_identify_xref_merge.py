"""Regression checks for authoritative disassembler data-xref merging."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.func_id import config
from tools.func_id.identify import _merge_xref_data_reads


def main():
    saved = (config.RDATA_VA_START, config.RDATA_VA_END,
             config.DATA_VA_START, config.DATA_VA_END)
    try:
        config.RDATA_VA_START = 0x00300000
        config.RDATA_VA_END = 0x00310000
        config.DATA_VA_START = 0x00400000
        config.DATA_VA_END = 0x00410000
        functions = [{"start": "0x00200000"}, {"start": "0x00201000"}]
        xrefs = [
            {"from": "0x00200020", "to": "0x00300040", "type": "data_imm"},
            {"from": "0x00201020", "to": "0x00300044", "type": "data_read"},
            {"from": "0x00201024", "to": "0x00300048", "type": "jump"},
        ]
        refs = {}
        added = _merge_xref_data_reads(xrefs, functions, refs, False)
        assert added == 2
        assert refs[0x00300040] == [0x00200000]
        assert refs[0x00300044] == [0x00201000]
        assert 0x00300048 not in refs
    finally:
        (config.RDATA_VA_START, config.RDATA_VA_END,
         config.DATA_VA_START, config.DATA_VA_END) = saved
    print("OK: data_imm and data_read xrefs both feed data references")


if __name__ == "__main__":
    main()