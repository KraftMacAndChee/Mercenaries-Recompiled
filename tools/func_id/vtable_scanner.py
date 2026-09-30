"""
C++ vtable scanner for function classification.

Scans data sections (.rdata, .data, etc.) for arrays of consecutive
.text pointers that form C++ virtual function tables. Each vtable
corresponds to a class, and its entries are virtual methods.

Key feature: discovers THUNK FUNCTIONS that are only called via vtable
ICALLs. These are valid code entry points between `ret` instructions
that the function detector misses because there's no direct `call`.

Typical MSVC vtable layout in .rdata:
  [vfunc_0_ptr][vfunc_1_ptr][vfunc_2_ptr]...

Thunk pattern (after ret of previous function):
  mov ecx, [ecx+N]   ; adjust this pointer
  test ecx, ecx       ; null check
  jne target           ; jump to real method
  xor eax, eax
  ret N
"""

import struct
from collections import defaultdict

from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_MEM

from . import config


# Minimum number of consecutive code pointers to qualify as a vtable
MIN_VTABLE_ENTRIES = 3

# A destructor plus one virtual method is a valid (and common) small C++
# vtable.  Two arbitrary executable-looking words are too weak to classify on
# their own, so only admit this shorter form when code independently takes the
# table's address (normally a constructor installing the vptr).
MIN_REFERENCED_VTABLE_ENTRIES = 2

# Long compiler-generated initializer/registration tables are not C++
# vtables, but they have the same on-disk shape: a contiguous array of code
# pointers. Requiring every undiscovered entry in such a table to reach a
# branch or return inside a tiny decode window loses legitimate straight-line
# initializers. Only use this path for a large table whose entries are already
# overwhelmingly anchored by independently detected function starts.
MIN_ANCHORED_POINTER_TABLE_ENTRIES = 16
MIN_ANCHORED_POINTER_TABLE_KNOWN_RATIO = 0.75
# Large callback/registration tables can contain hundreds of valid entry
# points before iterative function discovery has named most of them. Treat a
# table as strongly anchored when nearly every entry is either independently
# known or decodes to bounded control flow. A few straight-line callbacks are
# allowed once the rest of the table establishes its callable nature.
MIN_ANCHORED_POINTER_TABLE_PLAUSIBLE_RATIO = 0.98


def scan_vtables(xbe_data, functions, imm_refs, verbose=False, sections=None):
    """
    Scan data sections for C++ vtables and classify virtual methods.

    Also discovers thunk functions: valid code entry points that are
    referenced by vtables but not in the known function list.

    Args:
        xbe_data: Raw XBE file bytes.
        functions: List of function dicts from disassembly.
        imm_refs: Dict of immediate reference data.
        verbose: Print progress info.
        sections: Optional list of section dicts with keys:
            name, va (int), size (int), raw (int), executable (bool)
            If None, falls back to config-based section detection.

    Returns:
        tuple: (vtable_results, vtables)
    """
    # Build set of known function starts
    func_starts = set()
    for f in functions:
        func_starts.add(int(f["start"], 16))

    # Determine code and data ranges from section info
    if sections:
        # Vtable methods can live in any executable XBE section. Titles often
        # place subsystem code in sections such as DSOUND, D3D, or XGRPH.
        text_ranges = [(s["va"], s["va"] + s["size"])
                       for s in sections if s.get("executable", False)]
        # Scan ALL sections with raw data for vtables.
        # Xbox vtables can be in ANY section (including .text itself!)
        data_sections = [{"name": s["name"], "va": s["va"],
                          "size": min(s["size"], s.get("raw_size", s["size"])),
                          "raw": s["raw"]}
                         for s in sections
                         if s["raw"] > 0 and s.get("raw_size", s["size"]) > 0]
    else:
        text_ranges = _get_code_ranges()
        data_sections = _get_data_sections()

    # Scan all data sections for vtable structures
    raw_vtables = _find_vtables(xbe_data, func_starts, text_ranges, data_sections)
    indexed_jump_tables = _find_indexed_jump_table_bases(xbe_data, sections)

    if verbose:
        print(f"  Raw vtable candidates: {len(raw_vtables)}")

    # Filter false positives
    vtables = _filter_vtables(
        raw_vtables, func_starts, xbe_data, sections,
        excluded_table_addresses=indexed_jump_tables,
        referenced_table_addresses=set(imm_refs))

    if verbose:
        total_entries = sum(len(vt["entries"]) for vt in vtables)
        print(f"  Validated vtables: {len(vtables)}")
        print(f"  Total virtual methods: {total_entries}")

    # Find functions referenced only through validated pointer tables.
    discovered_thunks = set()
    for vt in vtables:
        for entry in vt["entries"]:
            if entry not in func_starts:
                discovered_thunks.add(entry)

    if verbose and discovered_thunks:
        print(f"  Discovered vtable thunks (new functions): {len(discovered_thunks)}")
        for addr in sorted(discovered_thunks)[:10]:
            print(f"    0x{addr:08X}")
        if len(discovered_thunks) > 10:
            print(f"    ... and {len(discovered_thunks) - 10} more")

    # Find constructors
    constructors = _find_constructors(
        [vt for vt in vtables if vt.get("kind", "vtable") == "vtable"],
        xbe_data, functions)

    if verbose:
        print(f"  Constructors found: {len(constructors)}")

    # Build classification results
    results = {}
    for i, vt in enumerate(vtables):
        is_pointer_table = vt.get("kind") == "function_pointer_table"
        cls_id = f"{'tbl' if is_pointer_table else 'cls'}_{i:03d}"
        vt["class_id"] = cls_id

        for idx, entry_addr in enumerate(vt["entries"]):
            if entry_addr not in results:
                is_thunk = entry_addr in discovered_thunks
                results[entry_addr] = {
                    "category": ("game_engine" if is_pointer_table else
                                 "game_vtable"),
                    "subcategory": cls_id,
                    "confidence": 0.75 if is_thunk else 0.85,
                    "method": (
                        "pointer_table_entry" if is_pointer_table else
                        ("vtable_thunk" if is_thunk else "vtable_scan")
                    ),
                    "vtable_addr": vt["address"],
                    "vtable_index": idx,
                }

    # Add constructors
    for func_addr, vt_info in constructors.items():
        if func_addr not in results:
            results[func_addr] = {
                "category": "game_vtable",
                "subcategory": vt_info["class_id"],
                "confidence": 0.80,
                "method": "vtable_ctor",
                "vtable_addr": vt_info["vtable_addr"],
                "vtable_index": -1,
            }

    if verbose:
        print(f"  Functions classified by vtable: {len(results)}")

    return results, vtables


def _get_code_ranges():
    """Get all code section VA ranges from config."""
    ranges = [(config.TEXT_VA_START, config.TEXT_VA_END)]

    # Add any other code sections defined in config
    if hasattr(config, 'SECTIONS'):
        skip_names = {".data", ".data1", "XIPS", "EnglishXlate", "JapaneseXlate",
                      "GermanXlate", "FrenchXlate", "SpanishXlate", "ItalianXlate"}
        for name, va, size, _ in config.SECTIONS:
            if name not in skip_names and name != ".text":
                ranges.append((va, va + size))

    return ranges


def _get_data_sections():
    """Get all data sections to scan for vtables."""
    sections = []

    # Primary scan range: .rdata or .data
    if hasattr(config, 'RDATA_RAW_ADDR'):
        sections.append({
            "name": "rdata",
            "va": config.RDATA_VA_START,
            "size": config.RDATA_VA_SIZE,
            "raw": config.RDATA_RAW_ADDR,
        })

    # Also scan all data sections from SECTIONS list
    if hasattr(config, 'SECTIONS'):
        data_names = {".data", ".data1", "DOLBY"}
        for name, va, size, raw in config.SECTIONS:
            if name in data_names:
                sections.append({
                    "name": name,
                    "va": va,
                    "size": size,
                    "raw": raw,
                })

    return sections


def _is_code_address(va, text_ranges):
    """Check if VA falls within the .text section and looks like a valid function addr."""
    # Reject obviously non-function addresses (misaligned or too small)
    if va < 0x10000:
        return False
    for lo, hi in text_ranges:
        if lo <= va < hi:
            return True
    return False


def _find_vtables(xbe_data, func_starts, text_ranges, data_sections):
    """
    Scan data sections for sequences of consecutive code pointers.

    Accepts any VA in a code section range as a potential vtable entry,
    not just known func_starts. This discovers thunk functions.
    """
    vtables = []

    for section in data_sections:
        sec_raw = section["raw"]
        sec_va = section["va"]
        sec_size = section["size"]

        if sec_raw + sec_size > len(xbe_data):
            sec_size = len(xbe_data) - sec_raw
        if sec_size <= 0:
            continue

        sec_bytes = xbe_data[sec_raw:sec_raw + sec_size]

        i = 0
        while i < len(sec_bytes) - 4:
            # Align to 4 bytes
            if i % 4 != 0:
                i += 4 - (i % 4)
                continue

            val = struct.unpack_from('<I', sec_bytes, i)[0]

            # Check if this looks like a code pointer
            if not _is_code_address(val, text_ranges):
                i += 4
                continue

            # Found a code pointer - scan forward for more
            entries = []
            j = i
            while j < len(sec_bytes) - 4:
                val = struct.unpack_from('<I', sec_bytes, j)[0]
                if _is_code_address(val, text_ranges):
                    entries.append(val)
                    j += 4
                else:
                    break

            if len(entries) >= MIN_REFERENCED_VTABLE_ENTRIES:
                vtables.append({
                    "address": sec_va + i,
                    "entries": entries,
                    "section": section["name"],
                })
                i = j
            else:
                i += 4

    return vtables


def _code_bytes_at(xbe_data, va, sections, limit=48):
    """Return raw bytes for an executable VA from parsed XBE sections."""
    if not sections:
        return b""
    for section in sections:
        if not section.get("executable", False):
            continue
        start = section["va"]
        raw_size = section.get("raw_size", section["size"])
        span = min(section["size"], raw_size)
        if not start <= va < start + span:
            continue
        raw = section["raw"] + va - start
        available = min(limit, start + span - va, len(xbe_data) - raw)
        if raw < 0 or available <= 0:
            return b""
        return xbe_data[raw:raw + available]
    return b""


def _find_indexed_jump_table_bases(xbe_data, sections):
    """Return absolute tables referenced by indexed indirect JMPs.

    A switch table and a callable pointer table have the same on-disk shape,
    but their entries have different meaning. Switch entries are internal
    basic-block labels and must not be fed back as independent function seeds;
    doing so splits a live stack frame at every case label. The reference
    instruction is authoritative: ``jmp [index * scale + displacement]`` marks
    the displacement as a jump-table base.
    """
    if not sections:
        return set()

    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    decoder.detail = True
    bases = set()
    for section in sections:
        if not section.get("executable", False):
            continue
        raw = section.get("raw", 0)
        raw_size = section.get("raw_size", section.get("size", 0))
        if raw < 0 or raw_size <= 0 or raw >= len(xbe_data):
            continue
        code = xbe_data[raw:min(raw + raw_size, len(xbe_data))]
        for insn in decoder.disasm(code, section["va"]):
            if insn.mnemonic != "jmp" or not insn.operands:
                continue
            operand = insn.operands[0]
            if operand.type != CS_OP_MEM or operand.mem.index == 0:
                continue
            displacement = operand.mem.disp & 0xFFFFFFFF
            if displacement:
                bases.add(displacement)
    return bases

def _looks_like_code_entry(xbe_data, va, sections, limit=49):
    """Require an undiscovered vtable entry to show bounded code structure.

    Most virtual methods should already be function starts. A method reachable
    only through a vtable can nevertheless be a full function rather than a
    short compiler thunk, so RET/JMP alone is too strict. Accept an entry when
    its first bounded instruction window reaches control flow (CALL/JMP/Jcc or
    RET). The byte window must include a complete instruction that begins near
    its end: an MSVC method with a large stack-frame prologue can place a
    five-byte CALL at byte 44, so a 49-byte window is sufficient without
    admitting unrelated control flow farther into packed executable-section
    data. Reject straight-line streams with no control
    flow; this is the key distinction for packed data words which otherwise
    decode indefinitely as harmless arithmetic instructions.
    """
    code = _code_bytes_at(xbe_data, va, sections, limit)
    if not code:
        return False
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    decoded = 0
    instructions = 0
    for insn in decoder.disasm(code, va):
        decoded += insn.size
        instructions += 1
        if (insn.mnemonic.startswith("ret") or
                insn.mnemonic.startswith("call") or
                insn.mnemonic.startswith("j")):
            return True
        if decoded >= limit or instructions >= 16:
            break
    return False


def _filter_vtables(vtables, func_starts=None, xbe_data=None, sections=None,
                    excluded_table_addresses=None,
                    referenced_table_addresses=None):
    """Filter false-positive vtable candidates."""
    filtered = []
    func_starts = set(func_starts or ())
    excluded_table_addresses = set(excluded_table_addresses or ())
    referenced_table_addresses = set(referenced_table_addresses or ())

    for vt in vtables:
        if vt["address"] in excluded_table_addresses:
            continue
        entries = vt["entries"]

        # Filter: all entries identical
        if len(set(entries)) == 1:
            continue

        # Filter: arithmetic progression (likely data table)
        if len(entries) >= 4:
            diffs = [entries[j+1] - entries[j] for j in range(len(entries) - 1)]
            if len(set(diffs)) == 1 and diffs[0] <= 16:
                continue

        # Filter: mostly sequential small-increment (data table)
        if len(entries) >= 6:
            small_diffs = sum(1 for j in range(len(entries) - 1)
                            if abs(entries[j+1] - entries[j]) <= 8)
            if small_diffs > len(entries) * 0.8:
                continue

        if xbe_data is not None and sections:
            known_count = sum(entry in func_starts for entry in entries)
            known_ratio = known_count / len(entries)
            unknown = [entry for entry in entries if entry not in func_starts]
            code_like = {
                entry: _looks_like_code_entry(xbe_data, entry, sections)
                for entry in unknown
            }
            code_like_unknown = sum(code_like.values())
            plausible_ratio = (
                known_count + code_like_unknown
            ) / len(entries)

            # MSVC/XDK startup code uses long .data arrays of initializer
            # callbacks. These tables are authoritative entry-point evidence
            # when a large majority of their entries were independently found.
            # Treat them separately from vtables so constructor propagation is
            # not applied to the table address itself.
            if (len(entries) >= MIN_ANCHORED_POINTER_TABLE_ENTRIES and
                    (known_ratio >= MIN_ANCHORED_POINTER_TABLE_KNOWN_RATIO or
                     plausible_ratio >=
                     MIN_ANCHORED_POINTER_TABLE_PLAUSIBLE_RATIO)):
                retained = dict(vt)
                retained["kind"] = "function_pointer_table"
                filtered.append(retained)
                continue

            # Section execute flags are coarse on retail Xbox images: .rdata
            # may be executable even though most of it is ordinary data. A
            # field immediately after a vtable can therefore contain a pointer
            # into .rdata and look like one more code address to the raw scan.
            # A native vtable cannot continue through a non-callable entry, so
            # terminate at the first unknown pointer without bounded control
            # flow. Retain the callable prefix only when it is independently
            # large enough to qualify as a table.
            first_noncallable = next(
                (index for index, entry in enumerate(entries)
                 if entry not in func_starts and not code_like[entry]),
                len(entries),
            )
            if first_noncallable < len(entries):
                # Trimming is deliberately narrower than ordinary vtable
                # acceptance. Long runs are commonly packed subsystem data,
                # while a real short vtable is installed by code that takes
                # the table address (normally a constructor). Require that
                # independent reference before treating its callable prefix as
                # a table; otherwise a random executable-data run can create
                # global dispatch entries and split live functions.
                if (len(entries) >= MIN_ANCHORED_POINTER_TABLE_ENTRIES or
                        vt["address"] not in referenced_table_addresses):
                    continue
                entries = entries[:first_noncallable]
            if len(entries) < MIN_VTABLE_ENTRIES:
                if (len(entries) < MIN_REFERENCED_VTABLE_ENTRIES or
                        vt["address"] not in referenced_table_addresses or
                        any(entry not in func_starts and not code_like[entry]
                            for entry in entries)):
                    continue

        # Without section bytes we cannot validate an undiscovered short
        # method.  Preserve the historical three-entry threshold in that
        # fallback mode.
        elif len(entries) < MIN_VTABLE_ENTRIES:
            continue

        retained = dict(vt)
        retained["entries"] = entries
        retained.setdefault("kind", "vtable")
        filtered.append(retained)

    return filtered
def _find_constructors(vtables, xbe_data, functions):
    """
    Find constructor functions that embed vtable addresses as immediates.
    """
    vtable_addrs = {}
    vtable_methods = set()
    for vt in vtables:
        addr_bytes = struct.pack('<I', vt["address"])
        vtable_addrs[addr_bytes] = vt
        vtable_methods.update(vt["entries"])

    if not vtable_addrs:
        return {}

    constructors = {}

    for f in functions:
        func_addr = int(f["start"], 16)
        func_size = f.get("size", 0)
        if func_size < 8 or func_size > 8192:
            continue

        file_offset = config.va_to_file_offset(func_addr)
        if file_offset is None or file_offset + func_size > len(xbe_data):
            continue

        func_bytes = xbe_data[file_offset:file_offset + func_size]

        for addr_bytes, vt in vtable_addrs.items():
            pos = func_bytes.find(addr_bytes)
            if pos >= 0:
                if func_addr not in vtable_methods:
                    constructors[func_addr] = {
                        "class_id": vt.get("class_id", "cls_???"),
                        "vtable_addr": vt["address"],
                    }
                break

    return constructors
