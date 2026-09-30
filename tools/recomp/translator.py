"""
Function-level x86 → C translator.

For each function:
1. Read raw bytes from XBE
2. Disassemble with Capstone
3. Build basic blocks
4. Lift each block to C statements
5. Generate a complete C function

Produces compilable C code using recomp_types.h macros.
"""

import json
import os
from bisect import bisect_right
from copy import deepcopy

# Import the functions, not the VA constants: configure_from_xbe() rebinds those
# at startup, so a by-value import would freeze the fallback layout.
from .config import va_to_file_offset, is_code_address
from .disasm import Disassembler
from .lifter import (
    FLAG_SETTERS,
    _EFLAGS_PRESERVE,
    _EFLAGS_SETTERS,
    _FLAGS_UNDEFINED,
    Lifter,
    _make_condition,
    lift_basic_block,
    detect_seh_helpers,
)


def discover_jump_table_entry_splits(xbe_data, func_db):
    """Add translated entry points for jump-table targets inside functions.

    Function discovery commonly records a whole native control-flow region as
    one function even though an external switch table can jump past its first
    few instructions. Exact-address dispatch cannot map such an interior VA to
    the containing C function: entering at the parent's start would execute
    instructions that native x86 skipped. Split the containing region at each
    verified instruction-boundary target so every indirect tail jump has an
    exact translated entry and the normal adjacent-fallthrough bridge connects
    the preceding prefix.

    Only indexed memory jumps with at least two consecutive code targets are
    treated as jump tables. Targets inside a known function become exact
    splits. A table may also target a contiguous executable gap immediately
    following its dispatcher (optimized switch cases are often absent from
    symbol/function discovery); when a target from a verified multi-entry table
    covers the gap from its first byte and every target in that gap is a decoded
    instruction boundary, create exact entry functions for those cases as well.
    """
    disasm = Disassembler()
    table_reader = Lifter(func_db=func_db, xbe_data=xbe_data,
                          seh_prolog=0, seh_epilog=0)
    original = [(addr, info, int(info.get("end", addr)))
                for addr, info in sorted(func_db.items())]
    starts = [addr for addr, _, _ in original]
    boundaries = {}
    splits = {}
    gap_entries = {}

    def read_bytes(start, end):
        offset = va_to_file_offset(start)
        if offset is None or end <= start or offset + end - start > len(xbe_data):
            return None
        return xbe_data[offset:offset + end - start]

    # First collect every credible external switch-table target without
    # changing the ranges being scanned.
    for start, info, end in original:
        raw = read_bytes(start, end)
        if not raw:
            continue
        instructions = disasm.disassemble_function(raw, start, end)
        for insn in instructions:
            if insn.mnemonic != "jmp" or insn.jump_target or not insn.operands:
                continue
            op = insn.operands[0]
            if (op.type != "mem" or not op.mem_disp or
                    not (op.mem_index or op.mem_base)):
                continue
            targets = table_reader._read_jump_table(op.mem_disp)
            if len(targets) < 2:
                continue
            # A table wholly contained in this function is already lowered to
            # local gotos by Lifter._analyze_switch_table; splitting those case
            # blocks would only inflate the global dispatch table.
            if all(start <= target < end for target in targets):
                continue
            for target in set(targets):
                owner_index = bisect_right(starts, target) - 1
                if owner_index < 0:
                    continue
                owner_start, _, owner_end = original[owner_index]
                if not (owner_start < target < owner_end):
                    next_index = owner_index + 1
                    if (next_index < len(original) and
                            owner_end <= target < original[next_index][0]):
                        key = (owner_index, owner_end, original[next_index][0])
                        gap_entries.setdefault(key, set()).add(target)
                    continue
                if owner_start not in boundaries:
                    owner_raw = read_bytes(owner_start, owner_end)
                    owner_insns = (disasm.disassemble_function(
                        owner_raw, owner_start, owner_end) if owner_raw else [])
                    boundaries[owner_start] = {item.address for item in owner_insns}
                if target in boundaries[owner_start]:
                    splits.setdefault(owner_start, set()).add(target)

    # Materialize verified case entries in otherwise-unowned executable gaps.
    # The containing table has already been required to expose at least two
    # consecutive code targets. A particular gap may contain only one of those
    # cases, so require coverage from the first gap byte and exact instruction
    # boundaries rather than repeating the table-wide cardinality per gap.
    added = []
    for (owner_index, gap_start, gap_end), targets_set in gap_entries.items():
        targets = sorted(targets_set)
        if not targets or targets[0] != gap_start:
            continue
        raw = read_bytes(gap_start, gap_end)
        instructions = (disasm.disassemble_function(raw, gap_start, gap_end)
                        if raw else [])
        instruction_boundaries = {item.address for item in instructions}
        if not targets_set.issubset(instruction_boundaries):
            continue
        template = original[owner_index][1]
        for segment_start, segment_end in zip(targets, targets[1:] + [gap_end]):
            segment = deepcopy(template)
            segment["name"] = f"sub_{segment_start:08X}"
            segment["start"] = f"0x{segment_start:08X}"
            segment["_addr"] = segment_start
            segment["end"] = segment_end
            segment["size"] = segment_end - segment_start
            segment.pop("num_instructions", None)
            func_db[segment_start] = segment
            added.append(segment_start)

    # Re-segment each affected function. Metadata is inherited for comments;
    # classifications and ABI facts remain keyed to the original entry.
    for start, info, original_end in original:
        targets = sorted(splits.get(start, ()))
        if not targets:
            continue
        segment_starts = [start] + targets
        segment_ends = targets + [original_end]
        for index, (segment_start, segment_end) in enumerate(
                zip(segment_starts, segment_ends)):
            if index == 0:
                segment = info
            else:
                segment = deepcopy(info)
                segment["name"] = f"sub_{segment_start:08X}"
                segment["start"] = f"0x{segment_start:08X}"
                segment["_addr"] = segment_start
                segment.pop("num_instructions", None)
                func_db[segment_start] = segment
                added.append(segment_start)
            segment["end"] = segment_end
            segment["size"] = segment_end - segment_start

    return sorted(added)


def _fixup_icall_esp_save(lines):
    """
    Post-process generated C lines to insert _icall_esp save points.

    When RECOMP_ICALL_SAFE is used, we need to save g_esp BEFORE any
    args are pushed so the macro can restore it on lookup failure.

    Scans backwards from each RECOMP_ICALL_SAFE line to find consecutive
    PUSH32 lines (the arg pushes), then inserts a save before the first.
    """
    import re
    result = []
    # Find indices of all ICALL_SAFE lines
    icall_indices = []
    for i, line in enumerate(lines):
        if 'RECOMP_ICALL_SAFE(' in line:
            icall_indices.append(i)

    if not icall_indices:
        return lines  # nothing to do

    # Find the end of the entry-frame prologue. Optimizing x86 compilers may
    # schedule a callee-save push (especially EDI) after register loads,
    # immediately before the first virtual call. Those pushes belong to the
    # function frame, not to the call argument list. A failed indirect-call
    # lookup must never restore ESP to a point before them.
    first_icall_idx = icall_indices[0]
    popped_nonvolatile = {
        reg for reg in ("ebp", "ebx", "esi", "edi")
        if any(line.strip() == f"POP32(esp, {reg});" for line in lines)
    }
    entry_label_idx = next(
        (i for i, line in enumerate(lines[:first_icall_idx])
         if re.match(r'^loc_[0-9A-Fa-f]+:', line.strip())),
        0,
    )
    label_indices = {
        match.group(1): i
        for i, line in enumerate(lines)
        if (match := re.match(r'^(loc_[0-9A-Fa-f]+):', line.strip()))
    }
    prologue_scan_end = first_icall_idx
    for i in range(entry_label_idx + 1, first_icall_idx):
        stripped = lines[i].strip()
        # Disassembly labels also mark ordinary fall-through instruction
        # addresses. Do not treat those as basic-block boundaries: retail
        # code can delay a callee-save push across several such labels (for
        # example sub_000574B0 saves EDI immediately before its first virtual
        # call). A forward conditional branch that skips the first ICALL also
        # leaves its fall-through path dominant. A branch whose target reaches
        # the ICALL can bypass intervening pushes, so it ends the prologue scan.
        cond = re.match(r'^if \(.+\) goto (loc_[0-9A-Fa-f]+);', stripped)
        if cond:
            target_idx = label_indices.get(cond.group(1), -1)
            if target_idx <= first_icall_idx:
                prologue_scan_end = i
                break
            continue
        if (stripped.startswith('goto ') or 'return;' in stripped):
            prologue_scan_end = i
            break
    saved_push_indices = {
        next(
            (i for i in range(entry_label_idx + 1, prologue_scan_end)
             if lines[i].strip() == f"PUSH32(esp, {reg});"),
            -1,
        )
        for reg in popped_nonvolatile
    }
    prologue_end_idx = -1
    for i in range(entry_label_idx + 1, prologue_scan_end):
        line = lines[i]
        stripped = line.strip()
        if (stripped == "ebp = esp;" or
            re.match(r'^esp = esp [&-] ', stripped) or
            stripped == "MEM32(0) = esp;" or
            i in saved_push_indices):
            prologue_end_idx = i

    # For each ICALL, determine where to insert the save
    insert_before = set()  # map: line_index → True (insert save before this line)
    for icall_idx in icall_indices:
        # The ICALL line itself contains "PUSH32(esp, 0); RECOMP_ICALL_SAFE(...)"
        # Look backwards for consecutive lines containing PUSH32(esp,
        first_push_idx = icall_idx
        j = icall_idx - 1
        while j >= 0:
            stripped = lines[j].strip()
            # Skip blank lines
            if not stripped:
                j -= 1
                continue
            # Check if this is a PUSH32 line (arg push)
            if stripped.startswith('PUSH32(esp,'):
                first_push_idx = j
                j -= 1
                continue
            # Check if this is a non-push instruction that could be part of
            # arg evaluation (e.g., "eax = MEM32(...);") - these are interleaved
            # with pushes in the x86 code. We need to look past them.
            # Stop at labels, gotos, other control flow, or other ICALL lines.
            if (re.match(r'^loc_[0-9A-Fa-f]+:', stripped) or
                'goto ' in stripped or
                'RECOMP_ICALL' in stripped or
                'return;' in stripped or
                stripped.startswith('if (') or
                stripped.startswith('POP32(') or
                stripped.startswith('PUSH32(esp, 0); sub_')):
                break
            # It's an interleaved computation - skip past it
            j -= 1
            continue

        # Only the first call can share the entry basic block with the frame
        # prologue. Clamp its save point after that prologue even when the
        # backwards argument scan crossed scheduled callee-save pushes.
        if icall_idx == first_icall_idx and first_push_idx <= prologue_end_idx:
            first_push_idx = prologue_end_idx + 1
        insert_before.add(first_push_idx)

    # Build result with saves inserted
    for i, line in enumerate(lines):
        if i in insert_before:
            # Determine indentation from the current line
            indent = line[:len(line) - len(line.lstrip())]
            result.append(f"{indent}{{ uint32_t _icall_esp = g_esp;")
        result.append(line)
        if 'RECOMP_ICALL_SAFE(' in line:
            indent = line[:len(line) - len(line.lstrip())]
            result.append(f"{indent}}}")

    return result


class FunctionTranslator:
    """Translates individual x86 functions to C source code."""

    def __init__(self, xbe_data, func_db, label_db=None, classification_db=None,
                 abi_db=None, seh_prolog=None, seh_epilog=None):
        """
        xbe_data: bytes - raw XBE file contents
        func_db: dict - addr → function info from functions.json
        label_db: dict - addr → name from labels.json
        classification_db: dict - addr → classification from identified_functions.json
        abi_db: dict - addr → ABI info from abi_functions.json
        seh_prolog/seh_epilog: override the detected SEH helper addresses
        """
        self.xbe_data = xbe_data
        self.func_db = func_db
        self.label_db = label_db or {}
        self.classification_db = classification_db or {}
        self.abi_db = abi_db or {}
        self.disasm = Disassembler()
        self.lifter = Lifter(func_db=func_db, label_db=label_db, abi_db=abi_db,
                             xbe_data=xbe_data, seh_prolog=seh_prolog,
                             seh_epilog=seh_epilog)

    def _read_func_bytes(self, start_va, end_va):
        """Read raw bytes for a function from the XBE."""
        offset = va_to_file_offset(start_va)
        if offset is None:
            return None
        size = end_va - start_va
        if offset + size > len(self.xbe_data):
            return None
        return self.xbe_data[offset:offset + size]

    def _determine_calling_convention(self, func_info):
        """Guess calling convention from function properties."""
        name = func_info.get("name", "")
        # thiscall methods have ecx = this
        if "thiscall" in name or func_info.get("calling_convention") == "thiscall":
            return "thiscall"
        return "cdecl"

    def _func_has_prologue(self, instructions):
        """Check if function starts with push ebp; mov ebp, esp."""
        if len(instructions) < 2:
            return False
        return (instructions[0].mnemonic == "push" and
                instructions[0].op_str == "ebp" and
                instructions[1].mnemonic == "mov" and
                instructions[1].op_str == "ebp, esp")

    def translate_function(self, func_addr, func_info):
        """
        Translate a single function to C code.
        Returns a string of C source code, or None on failure.
        """
        start = func_addr
        end = func_info.get("end")
        if not end:
            end = start + func_info.get("size", 0)
        if end <= start:
            return None

        name = func_info.get("name", f"sub_{start:08X}")
        size = end - start

        # Read bytes from XBE
        raw_bytes = self._read_func_bytes(start, end)
        if not raw_bytes:
            return None

        # Set function bounds for the lifter
        self.lifter.func_start = start
        self.lifter.func_end = end
        self.lifter._direction_step = 1

        # Disassemble
        instructions = self.disasm.disassemble_function(raw_bytes, start, end)
        if not instructions:
            return None

        # Collect switch table targets as extra block leaders
        switch_leaders = set()
        for insn in instructions:
            if insn.mnemonic == "jmp" and not insn.jump_target and insn.operands:
                targets = self.lifter._analyze_switch_table(insn.operands)
                for t in targets:
                    if start <= t < end:
                        switch_leaders.add(t)

        # Build basic blocks
        blocks = self.disasm.build_basic_blocks(
            instructions, start, end,
            extra_leaders=switch_leaders if switch_leaders else None)
        if not blocks:
            return None

        # The normal block pass carries reconstructed EFLAGS through adjacent
        # fallthrough blocks. A join can also consume flags produced on two
        # non-adjacent incoming edges, however. C source order cannot model
        # that by itself: whichever block happened to be emitted last wins.
        #
        # Materialize the condition on simple incoming edges (each predecessor
        # has this join as its sole successor), then make the join consume that
        # snapshot. Restricting this to single-successor edges avoids encoding
        # two different successor conditions in the one fallback flag slot.
        predecessors = {bb.start: [] for bb in blocks}
        for predecessor in blocks:
            for successor in predecessor.successors:
                if successor in predecessors:
                    predecessors[successor].append(predecessor)

        def first_cross_block_flag_consumer(bb):
            for insn in bb.instructions:
                mnemonic = insn.mnemonic
                if (insn.is_cond_jump and
                        mnemonic not in ("jecxz", "jcxz")):
                    return insn
                if (mnemonic in FLAG_SETTERS or
                        mnemonic in _EFLAGS_SETTERS or
                        mnemonic in _FLAGS_UNDEFINED):
                    return None
                if mnemonic not in _EFLAGS_PRESERVE:
                    return None
            return None

        preliminary_out_flags = {}
        reconstruct_join_flags = (
            func_info.get("section") == "XMV" or
            func_info.get("_reviewed_range", False))
        if reconstruct_join_flags:
            for bb in blocks:
                _, preliminary_out_flags[bb.start] = lift_basic_block(
                    self.lifter, bb, flag_state=None)

        edge_flag_snapshots = {}
        join_flag_snapshots = {}
        for join in blocks:
            # Cross-edge reconstruction is enabled for the runtime-validated
            # retail XMV decoder and for explicitly reviewed native ranges.
            # The latter are authoritative whole-frame regions whose shared
            # labels cannot safely fall back to C source-order flag state.
            if not reconstruct_join_flags:
                continue
            incoming = predecessors.get(join.start, [])
            consumer = first_cross_block_flag_consumer(join)
            if len(incoming) < 2 or consumer is None:
                continue
            if any(len(predecessor.successors) != 1 or
                   predecessor.successors[0] != join.start
                   for predecessor in incoming):
                continue
            snapshots = []
            for predecessor in incoming:
                state = preliminary_out_flags.get(predecessor.start)
                if not state:
                    snapshots = []
                    break
                producer_index = next((
                    index for index in range(len(predecessor.instructions) - 1,
                                             -1, -1)
                    if predecessor.instructions[index].mnemonic == state[0]
                ), None)
                if producer_index is None:
                    snapshots = []
                    break
                producer_ops = state[1]
                dependency_regs = {
                    register
                    for operand in producer_ops
                    for register in (
                        getattr(operand, "reg", None),
                        getattr(operand, "mem_base", None),
                        getattr(operand, "mem_index", None),
                    )
                    if register
                }
                producer_reads_memory = any(
                    getattr(operand, "type", None) == "mem"
                    for operand in producer_ops)
                unsafe = False
                for middle in predecessor.instructions[producer_index + 1:]:
                    if middle.mnemonic == "call":
                        unsafe = True
                        break
                    if not middle.operands:
                        continue
                    destination = middle.operands[0]
                    if (getattr(destination, "type", None) == "reg" and
                            getattr(destination, "reg", None) in
                            dependency_regs):
                        unsafe = True
                        break
                    if (producer_reads_memory and
                            getattr(destination, "type", None) == "mem"):
                        unsafe = True
                        break
                if unsafe:
                    snapshots = []
                    break
                result = _make_condition(consumer.mnemonic, state[0], state[1])
                if result is None:
                    snapshots = []
                    break
                snapshots.append((predecessor.start, result[0], result[1]))
            # A join only needs a snapshot phi when incoming conditions differ.
            # This includes different producer mnemonics (TEST versus CMP) and
            # the equally important case of the same mnemonic comparing
            # different operands on separate edges.
            if (not snapshots or
                    len({expression for _, expression, _ in snapshots}) < 2):
                continue
            for predecessor_start, expression, description in snapshots:
                edge_flag_snapshots[predecessor_start] = (
                    join.start, expression, description, consumer.mnemonic)
            join_flag_snapshots[join.start] = (
                snapshots[0][2], consumer.mnemonic)

        # Get classification and ABI info
        cls_info = self.classification_db.get(start, {})
        category = cls_info.get("category", "unknown")
        module = cls_info.get("module", "")
        source_file = cls_info.get("source_file", "")
        abi_info = self.abi_db.get(start, {})

        # ABI-derived info (kept for comments)
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)
        return_hint = abi_info.get("return_hint", "int_or_void")
        frame_type = abi_info.get("frame_type", "fpo_leaf")
        stack_frame_size = abi_info.get("stack_frame_size", 0)

        # Determine which registers are used
        used_regs = self._find_used_registers(instructions)
        used_xmm = self._find_used_xmm(instructions)
        has_prologue = self._func_has_prologue(instructions)
        has_fpu = any(insn.mnemonic.startswith("f") for insn in instructions)

        # Volatile registers (eax, ecx, edx, esp) are globals - don't declare
        # them as locals. The RECOMP_GENERATED_CODE #define maps register names
        # to the global variables via preprocessor macros.
        volatile_regs = {"eax", "ecx", "edx", "esp"}

        # Ensure ebp tracked if function uses 'leave' (implicit ebp)
        if any(insn.mnemonic == "leave" for insn in instructions):
            used_regs.add("ebp")

        # Ensure ebp tracked if function has tail jumps (lifter emits
        # g_seh_ebp = ebp before external jmp and indirect jmp).
        has_tail_jump = any(
            insn.mnemonic == "jmp" and (
                (insn.jump_target and not (start <= insn.jump_target < end))
                or not insn.jump_target  # indirect jmp
            )
            for insn in instructions
        )
        if has_tail_jump:
            used_regs.add("ebp")

        # Ensure ebp tracked if function calls __SEH_prolog or __SEH_epilog
        # (lifter emits ebp = g_seh_ebp readback after these calls).
        SEH_FUNCS = {
            addr for addr in (self.lifter.SEH_PROLOG, self.lifter.SEH_EPILOG)
            if addr is not None
        }
        if any(insn.call_target in SEH_FUNCS for insn in instructions):
            used_regs.add("ebp")

        # Build call targets list
        call_targets = set()
        for insn in instructions:
            if insn.call_target and is_code_address(insn.call_target):
                call_targets.add(insn.call_target)

        # All translated functions are void(void).
        # Arguments pass via the global simulated stack (push instructions).
        # Return values pass via g_eax (the global eax register).
        ret_type = "void"
        param_str = "void"

        # Generate C code
        lines = []

        # Header comment
        lines.append(f"/**")
        lines.append(f" * {name}")
        lines.append(f" * Original: 0x{start:08X} - 0x{end:08X} ({size} bytes, {len(instructions)} insns)")
        if category != "unknown":
            lines.append(f" * Category: {category}")
        if source_file:
            lines.append(f" * Source: {source_file}")
        lines.append(f" * CC: {cc}, {num_params} params, returns {return_hint}")
        if frame_type == "ebp_frame":
            lines.append(f" * Frame: EBP-based ({stack_frame_size} bytes locals)")
        else:
            lines.append(f" * Frame: {frame_type}")
        lines.append(f" */")

        # Function signature
        lines.append(f"{ret_type} {name}({param_str})")
        lines.append(f"{{")

        # ebp is the only callee-saved register declared as a local.
        # ebx, esi, edi are global via #define macros (g_ebx, g_esi, g_edi)
        # and must NOT be declared locally, otherwise the local shadows
        # the global and cross-function register passing breaks.
        # Volatile registers (eax, ecx, edx, esp) are also global via macros.
        reg_decls = []
        if "ebp" in used_regs:
            reg_decls.append("ebp")
        if reg_decls:
            lines.append(f"    uint32_t {', '.join(reg_decls)};")

        # Add _flags when a consumer or an instruction with an explicitly
        # materialized condition (currently CMPXCHG) needs it.
        has_conditionals = any(
            insn.is_cond_jump or insn.mnemonic.startswith("set")
            or insn.mnemonic.startswith("cmov")
            or insn.mnemonic in ("cmpxchg", "lock cmpxchg")
            for insn in instructions)
        if has_conditionals:
            lines.append(f"    int _flags = 0; /* fallback flag var */")

        # NEG and rotate-through-carry instructions produce carry; SBB and ADC consume it.
        has_carry = any(insn.mnemonic in ("neg", "sbb", "adc", "rcl", "rcr")
                        for insn in instructions)
        self.lifter.track_carry = has_carry
        if has_carry:
            lines.append(f"    int _cf = 0; /* carry flag */")

        # Add _fpu_cmp for FPU compare instructions (both old and new style)
        has_fpu_cmp = any(insn.mnemonic in ("fcompi", "fcomip", "fucomi",
                                             "fucompi", "fucomip", "fcomi",
                                             "fcom", "fcomp", "fcompp",
                                             "fucom", "fucomp", "fucompp")
                          for insn in instructions)
        if has_fpu_cmp:
            lines.append(f"    int _fpu_cmp = 0; /* FPU compare result: -1/0/1, 2=unordered */")

        # SSE/MMX registers are shared runtime architectural state, just like
        # the integer registers and x87 stack. Do not shadow them with locals.

        # x87 state is architectural state, so it must survive translated C
        # calls (notably floating-point returns in ST(0)). Emit per-function
        # convenience macros over the shared runtime stack.
        if has_fpu:
            lines.append(f"    #define fp_push(v) (g_fp_stack[--g_fp_top & 7u] = (v))")
            lines.append(f"    #define fp_pop() (g_fp_top++)")
            lines.append(f"    #define fp_popp() (fp_pop())")
            lines.append(f"    #define fp_top() g_fp_stack[g_fp_top & 7u]")
            lines.append(f"    #define fp_st(i) g_fp_stack[(g_fp_top + (uint32_t)(i)) & 7u]")
            lines.append(f"    #define fp_st1() g_fp_stack[(g_fp_top + 1u) & 7u]")

        # Every translated function inherits the guest EBP value. Prologue
        # functions read it immediately in push ebp before assigning their own
        # frame, while FPO functions may use it as either a caller frame or
        # scratch register. Initializing both cases preserves the callee-save
        # chain and prevents undefined native C values from entering the guest
        # stack. g_seh_ebp bridges EBP across translated C calls.
        if "ebp" in used_regs:
            lines.append(
                f"    ebp = g_seh_ebp; /* inherit guest caller frame */")

        lines.append(f"    RECOMP_TRACE_FUNC(0x{start:08X}u);")

        lines.append(f"")

        # Generate code for each basic block
        # Create a set of addresses that need labels
        label_addrs = set()
        for bb in blocks:
            for succ in bb.successors:
                label_addrs.add(succ)
        # Also add any jump targets within the function
        for insn in instructions:
            if insn.jump_target and start <= insn.jump_target < end:
                label_addrs.add(insn.jump_target)
        # Add switch table targets (indirect jmp with intra-function table)
        for insn in instructions:
            if insn.mnemonic == "jmp" and not insn.jump_target and insn.operands:
                switch_targets = self.lifter._analyze_switch_table(insn.operands)
                for t in switch_targets:
                    label_addrs.add(t)

        flag_state = None
        generated_out_flags = {}
        for bb in blocks:
            # Emit label if this block is a branch target
            if bb.start in label_addrs or bb.start == start:
                # The trailing ';' is load-bearing: C requires a statement after
                # a label, and a block whose instructions all emit comments only
                # (a lone `cmp`, which just sets flags for the next jcc) would
                # otherwise produce `loc_X:` immediately before `}` and fail to
                # compile. The null statement costs nothing and is always valid.
                lines.append(f"loc_{bb.start:08X}: ;")

            # Propagate flag state from previous block (fallthrough path).
            # This handles patterns like: test eax,eax / ja X / jb Y
            # where jb uses the same flags as ja from the preceding block.
            block_flag_state = flag_state
            # A branch target with one already-generated predecessor inherits
            # that predecessor's flags, not the flags of the block that merely
            # precedes it in source order. This is not XMV-specific: retail
            # inventory insertion also uses CMP/JNE ... RET ... JGE.
            incoming = predecessors.get(bb.start, [])
            if len(incoming) == 1:
                incoming_state = generated_out_flags.get(incoming[0].start)
                if incoming_state:
                    block_flag_state = incoming_state
            if bb.start in join_flag_snapshots:
                description, consumer = join_flag_snapshots[bb.start]
                block_flag_state = (
                    "snapshot_condition", [description, consumer])
            stmts, flag_state = lift_basic_block(
                self.lifter, bb, flag_state=block_flag_state)
            generated_out_flags[bb.start] = flag_state
            snapshot = edge_flag_snapshots.get(bb.start)
            if snapshot is not None:
                target, expression, _, consumer = snapshot
                statement = (
                    f"_flags = ({expression}); /* preserve flags for "
                    f"{consumer} at CFG join 0x{target:08X} */")
                insert_at = len(stmts)
                if (stmts and
                        stmts[-1].strip() == f"goto loc_{target:08X};"):
                    insert_at -= 1
                stmts.insert(insert_at, statement)
            for stmt in stmts:
                lines.append(f"    {stmt}")

            lines.append(f"")

        # Insert _icall_esp save points before RECOMP_ICALL_SAFE arg pushes.
        # The pattern is: optional PUSH32 args, then PUSH32(esp, 0); RECOMP_ICALL_SAFE(...).
        # We insert "uint32_t _icall_esp = g_esp;" before the first arg push.
        lines = _fixup_icall_esp_save(lines)

        # A target inside the nominal function range can still be absent from
        # the decoded basic blocks when an embedded jump table disrupts linear
        # disassembly. Preserve the branch as an indirect tail dispatch. A
        # title-specific manual override can then supply the interior entry.
        import re
        defined_labels = set()
        goto_lines = []
        for idx, line in enumerate(lines):
            lbl_match = re.match(r'^(loc_[0-9A-Fa-f]+):', line)
            if lbl_match:
                defined_labels.add(lbl_match.group(1))
            goto_match = re.search(r'goto (loc_[0-9A-Fa-f]+);', line)
            if goto_match:
                goto_lines.append((idx, goto_match.group(1)))
        for idx, target in goto_lines:
            if target not in defined_labels:
                frame_bridge = (
                    "g_seh_ebp = ebp; " if "ebp" in used_regs else "")
                target_va = int(target[4:], 16)
                lines[idx] = lines[idx].replace(
                    f"goto {target};",
                    (f"{{ {frame_bridge}RECOMP_ITAIL(0x{target_va:08X}u); "
                     f"return; }} /* missing local label {target} */"))

        # Function discovery can split one native control-flow region at a
        # reviewed interior entry (for example, a jump-table or shared-tail
        # target). If the final decoded instruction is not terminal, x86
        # execution naturally continues at the adjacent function start. Keep
        # that edge explicit in C instead of silently returning from the host
        # function with the guest frame still live.
        last_insn = instructions[-1]
        if (last_insn.end_address == end and
                not last_insn.is_terminator and end in self.func_db):
            frame_bridge = "g_seh_ebp = ebp; " if "ebp" in used_regs else ""
            next_name = self.func_db[end].get("name", f"sub_{end:08X}")
            lines.append(
                f"    {frame_bridge}{next_name}(); return; "
                f"/* fallthrough 0x{end:08X} */")
            lines.append("")

        # Ensure labels at end of function have a statement after them.
        # In C, a label must be followed by a statement; a comment alone is not
        # enough.  Walk backwards from the end and if the last real content is a
        # label (with only blank lines / comments after it), insert "(void)0;".
        _last_label_idx = None
        _has_stmt_after = False
        for _ri in range(len(lines) - 1, -1, -1):
            _s = lines[_ri].strip()
            if not _s:
                continue
            if _s.startswith("/*") and _s.endswith("*/"):
                continue
            if re.match(r'^loc_[0-9A-Fa-f]+:', _s):
                _last_label_idx = _ri
                break
            _has_stmt_after = True
            break
        if _last_label_idx is not None and not _has_stmt_after:
            lines.insert(_last_label_idx + 1, "    (void)0;")

        # Undefine FPU macros
        if has_fpu:
            lines.append(f"    #undef fp_push")
            lines.append(f"    #undef fp_pop")
            lines.append(f"    #undef fp_popp")
            lines.append(f"    #undef fp_top")
            lines.append(f"    #undef fp_st")
            lines.append(f"    #undef fp_st1")

        lines.append(f"}}")
        lines.append(f"")

        return "\n".join(lines)

    def _find_used_registers(self, instructions):
        """Find which 32-bit registers are referenced by any instruction."""
        regs = set()
        reg_map = {
            "eax": "eax", "ax": "eax", "al": "eax", "ah": "eax",
            "ebx": "ebx", "bx": "ebx", "bl": "ebx", "bh": "ebx",
            "ecx": "ecx", "cx": "ecx", "cl": "ecx", "ch": "ecx",
            "edx": "edx", "dx": "edx", "dl": "edx", "dh": "edx",
            "esi": "esi", "si": "esi",
            "edi": "edi", "di": "edi",
            "ebp": "ebp", "bp": "ebp",
            "esp": "esp", "sp": "esp",
        }
        for insn in instructions:
            for op in insn.operands:
                if op.type == "reg" and op.reg in reg_map:
                    regs.add(reg_map[op.reg])
                elif op.type == "mem":
                    if op.mem_base and op.mem_base in reg_map:
                        regs.add(reg_map[op.mem_base])
                    if op.mem_index and op.mem_index in reg_map:
                        regs.add(reg_map[op.mem_index])
        return regs

    def _find_used_xmm(self, instructions):
        """Find which XMM and MMX registers are used."""
        regs = set()
        for insn in instructions:
            for op in insn.operands:
                if op.type == "reg" and op.reg:
                    if op.reg.startswith("xmm") or op.reg.startswith("mm"):
                        regs.add(op.reg)
        return regs


class BatchTranslator:
    """Translates multiple functions and writes C source files."""

    def __init__(self, xbe_path, func_json_path, labels_json_path=None,
                 identified_json_path=None, abi_json_path=None,
                 output_dir=None, seh_prolog=None, seh_epilog=None,
                 discover_entry_splits=True, function_ranges=None):
        self.xbe_path = xbe_path
        self.output_dir = output_dir or os.path.join(
            os.path.dirname(__file__), "output")

        # Load XBE
        with open(xbe_path, "rb") as f:
            self.xbe_data = f.read()

        # Load function database
        with open(func_json_path, "r") as f:
            func_list = json.load(f)

        self.func_db = {}
        for func in func_list:
            addr = int(func["start"], 16)
            func["_addr"] = addr
            if "end" in func:
                func["end"] = int(func["end"], 16)
            self.func_db[addr] = func

        # Some optimized native routines expose their internal switch cases in
        # global pointer tables. Function discovery must keep those addresses
        # available as possible entry points in the general case, but a port
        # with authoritative symbols can state that the cases belong to one
        # control-flow region. Coalesce those reviewed ranges before jump-table
        # analysis so the lifter emits local gotos and preserves the native
        # frame across every case transition.
        self.function_ranges = []
        for start, end in function_ranges or ():
            if start not in self.func_db:
                raise ValueError(
                    f"function range start 0x{start:08X} is not detected")
            if end <= start:
                raise ValueError(
                    f"invalid function range 0x{start:08X}:0x{end:08X}")
            for interior in [
                    addr for addr in self.func_db if start < addr < end]:
                del self.func_db[interior]
            owner = self.func_db[start]
            owner["end"] = end
            owner["size"] = end - start
            owner["_reviewed_range"] = True
            self.function_ranges.append((start, end))

        self.jump_table_entry_splits = (
            discover_jump_table_entry_splits(self.xbe_data, self.func_db)
            if discover_entry_splits else [])

        # Global jump-table discovery may find an external table that targets a
        # reviewed owner's shared epilogue.  That is normally a legitimate
        # exact-entry split, but an authoritative --function-range explicitly
        # says the target shares the owner's live frame and must remain a local
        # label.  Re-apply the ranges after discovery so it cannot undo that
        # decision, and do not report protected entries as generated splits.
        if self.function_ranges:
            for start, end in self.function_ranges:
                for interior in [
                        addr for addr in self.func_db if start < addr < end]:
                    del self.func_db[interior]
                owner = self.func_db[start]
                owner["end"] = end
                owner["size"] = end - start
                owner["_reviewed_range"] = True
            self.jump_table_entry_splits = [
                addr for addr in self.jump_table_entry_splits
                if not any(start < addr < end
                           for start, end in self.function_ranges)]

        # Load labels
        self.label_db = {}
        if labels_json_path and os.path.exists(labels_json_path):
            with open(labels_json_path, "r") as f:
                labels = json.load(f)
            for lbl in labels:
                addr = int(lbl["address"], 16)
                self.label_db[addr] = lbl["name"]

        # Load classifications
        self.classification_db = {}
        if identified_json_path and os.path.exists(identified_json_path):
            with open(identified_json_path, "r") as f:
                identified = json.load(f)
            for entry in identified:
                addr = int(entry["start"], 16)
                self.classification_db[addr] = entry

        # Load ABI data
        self.abi_db = {}
        if abi_json_path and os.path.exists(abi_json_path):
            with open(abi_json_path, "r") as f:
                abi_list = json.load(f)
            for entry in abi_list:
                addr = int(entry["address"], 16)
                self.abi_db[addr] = entry

        # Detect the SEH helpers once here rather than per-Lifter, so the
        # result can be reported and overridden from the command line.
        if seh_prolog is None or seh_epilog is None:
            found_prolog, found_epilog = detect_seh_helpers(
                self.func_db, self.xbe_data, verbose=True)
            seh_prolog = seh_prolog if seh_prolog is not None else found_prolog
            seh_epilog = seh_epilog if seh_epilog is not None else found_epilog
        self.seh_prolog = seh_prolog
        self.seh_epilog = seh_epilog

        # Create translator
        self.translator = FunctionTranslator(
            self.xbe_data, self.func_db, self.label_db,
            self.classification_db, self.abi_db,
            seh_prolog=seh_prolog, seh_epilog=seh_epilog)

    def get_functions_by_category(self, categories=None, exclude_categories=None):
        """
        Get function addresses filtered by category.
        Returns list of (addr, func_info) tuples.
        """
        result = []
        for addr, func_info in sorted(self.func_db.items()):
            cls_info = self.classification_db.get(addr, {})
            cat = cls_info.get("category", "unknown")

            if categories and cat not in categories:
                continue
            if exclude_categories and cat in exclude_categories:
                continue

            result.append((addr, func_info))
        return result

    def _make_declaration(self, addr, name):
        """Generate a function declaration string.
        All translated functions are void(void) - args pass via stack,
        return values via g_eax."""
        return f"void {name}(void)"

    def translate_single(self, addr):
        """Translate a single function by address. Returns C code string."""
        func_info = self.func_db.get(addr)
        if not func_info:
            return None
        return self.translator.translate_function(addr, func_info)

    def translate_batch(self, func_list, output_file=None, max_funcs=None,
                        verbose=False):
        """
        Translate a batch of functions.

        func_list: list of (addr, func_info) tuples
        output_file: path to write combined C output
        max_funcs: limit number of functions
        verbose: print progress

        Returns dict with statistics.
        """
        os.makedirs(self.output_dir, exist_ok=True)

        if max_funcs:
            func_list = func_list[:max_funcs]

        stats = {
            "total": len(func_list),
            "translated": 0,
            "failed": 0,
            "total_lines": 0,
            "total_insns": 0,
        }

        c_chunks = []
        c_chunks.append("/**")
        c_chunks.append(" * Xbox - Mechanically Translated Game Code")
        c_chunks.append(f" * Generated by tools/recomp from original Xbox x86 code.")
        c_chunks.append(f" * Functions: {len(func_list)}")
        c_chunks.append(" */")
        c_chunks.append("")
        c_chunks.append('#define RECOMP_GENERATED_CODE')
        c_chunks.append('#include "recomp_types.h"')
        c_chunks.append('#include <math.h>')
        c_chunks.append("")
        c_chunks.append("/* Forward declarations */")

        # Forward declarations
        for addr, func_info in func_list:
            name = func_info.get("name", f"sub_{addr:08X}")
            decl = self._make_declaration(addr, name)
            c_chunks.append(f"{decl};")
        c_chunks.append("")
        c_chunks.append("/* ═══════════════════════════════════════════════════ */")
        c_chunks.append("")

        # Translate each function
        for i, (addr, func_info) in enumerate(func_list):
            name = func_info.get("name", f"sub_{addr:08X}")
            if verbose and (i % 100 == 0 or i == len(func_list) - 1):
                print(f"  [{i+1}/{len(func_list)}] Translating {name} at 0x{addr:08X}...")

            code = self.translator.translate_function(addr, func_info)
            if code:
                c_chunks.append(code)
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")

                # Count instructions
                num_insns = func_info.get("num_instructions", 0)
                stats["total_insns"] += num_insns
            else:
                c_chunks.append(f"/* FAILED to translate {name} at 0x{addr:08X} */")
                c_chunks.append(f"void {name}(void) {{ /* translation failed */ }}")
                c_chunks.append("")
                stats["failed"] += 1

        # Write output
        if output_file is None:
            output_file = os.path.join(self.output_dir, "recompiled.c")

        output_text = "\n".join(c_chunks)
        with open(output_file, "w", encoding="utf-8") as f:
            f.write(output_text)

        stats["output_file"] = output_file
        stats["output_size"] = len(output_text)

        return stats

    def translate_by_category(self, categories, output_prefix=None,
                              max_per_file=500, verbose=False):
        """
        Translate functions grouped by category, one file per category.
        Returns dict with per-category stats.
        """
        os.makedirs(self.output_dir, exist_ok=True)
        all_stats = {}

        for cat in categories:
            funcs = self.get_functions_by_category(categories={cat})
            if not funcs:
                continue

            prefix = output_prefix or cat
            out_file = os.path.join(self.output_dir, f"{prefix}.c")

            if verbose:
                print(f"\nCategory: {cat} ({len(funcs)} functions)")

            stats = self.translate_batch(
                funcs, output_file=out_file,
                max_funcs=max_per_file, verbose=verbose)
            all_stats[cat] = stats

        return all_stats

    def translate_batch_split(self, func_list, output_dir, chunk_size=1000,
                              header_name="recomp_funcs.h",
                              prefix="recomp", manual_functions=None,
                              verbose=False):
        """
        Translate functions into multiple .c files + a shared header.

        Generates:
          output_dir/recomp_funcs.h       - forward declarations for all functions
          output_dir/recomp_0000.c        - chunk 0
          output_dir/recomp_0001.c        - chunk 1
          ...
          output_dir/recomp_dispatch.c    - address -> function pointer table

        Returns dict with stats and list of generated files.
        """
        import sys

        os.makedirs(output_dir, exist_ok=True)

        # Translate all functions first, collecting results. A manual function
        # remains in the declarations and dispatch table, but its body is
        # supplied by the port instead of being emitted here. This also makes
        # direct generated calls resolve to the replacement at link time.
        manual_functions = set(manual_functions or ())
        translations = []
        stats = {
            "total": len(func_list),
            "translated": 0,
            "manual_overrides": 0,
            "failed": 0,
            "total_lines": 0,
        }

        for i, (addr, func_info) in enumerate(func_list):
            name = func_info.get("name", f"sub_{addr:08X}")
            if verbose and (i % 500 == 0 or i == len(func_list) - 1):
                print(f"  [{i+1}/{len(func_list)}] Translating {name}...",
                      file=sys.stderr)


            if addr in manual_functions:
                translations.append((addr, name, None))
                stats["translated"] += 1
                stats["manual_overrides"] += 1
                continue
            code = self.translator.translate_function(addr, func_info)
            if code:
                translations.append((addr, name, code))
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")
            else:
                # Stub for failed translations
                stub = f"/* FAILED: {name} at 0x{addr:08X} */\n"
                stub += f"void {name}(void) {{ /* translation failed */ }}\n"
                translations.append((addr, name, stub))
                stats["failed"] += 1

        # Any address called but never defined needs a stub, or the link fails.
        # These are almost all mid-function entry points the function detector
        # did not split out: a call lands a few bytes inside (or just past) a
        # function it already found. Emitting a minimal guest return keeps the
        # build linking without leaking the synthetic return address pushed by
        # every translated direct call. The target's behavior is still
        # unresolved, so these are reported and written to their own file
        # rather than hidden among the translated chunks.
        defined = {name for _, name, _ in translations}
        unresolved = {
            addr: name
            for addr, name in self.translator.lifter.referenced_calls.items()
            if name not in defined
        }
        stats["unresolved_stubs"] = len(unresolved)

        # Generate header with all forward declarations
        header_path = os.path.join(output_dir, header_name)
        header_lines = [
            "/**",
            " * Xbox - Recompiled Function Declarations",
            f" * {stats['translated']} functions, auto-generated by tools/recomp",
            " */",
            "",
            "#ifndef RECOMP_FUNCS_H",
            "#define RECOMP_FUNCS_H",
            "",
            '#include "recomp_types.h"',
            "",
        ]
        for addr, name, _ in translations:
            decl = self._make_declaration(addr, name)
            header_lines.append(f"{decl};")

        if unresolved:
            header_lines.append("")
            header_lines.append("/* Unresolved call targets (stubbed) */")
            for addr in sorted(unresolved):
                header_lines.append(f"void {unresolved[addr]}(void);")

        header_lines.extend(["", "#endif /* RECOMP_FUNCS_H */", ""])

        with open(header_path, "w", encoding="utf-8") as f:
            f.write("\n".join(header_lines))

        # Split translations into chunks and write .c files
        generated_files = [header_path]
        emitted_translations = [entry for entry in translations
                                if entry[2] is not None]
        chunks = [emitted_translations[i:i+chunk_size]
                  for i in range(0, len(emitted_translations), chunk_size)]

        for ci, chunk in enumerate(chunks):
            c_path = os.path.join(output_dir, f"{prefix}_{ci:04d}.c")
            c_lines = [
                "/**",
                f" * Xbox - Recompiled code chunk {ci}",
                f" * Functions: {len(chunk)} "
                f"(0x{chunk[0][0]:08X} - 0x{chunk[-1][0]:08X})",
                " */",
                "",
                "#define RECOMP_GENERATED_CODE",
                f'#include "{header_name}"',
                '#include <math.h>',
                "",
            ]
            for addr, name, code in chunk:
                c_lines.append(code)

            with open(c_path, "w", encoding="utf-8") as f:
                f.write("\n".join(c_lines))
            generated_files.append(c_path)

            if verbose:
                print(f"  Wrote {c_path} ({len(chunk)} functions)",
                      file=sys.stderr)

        # Emit the stub bodies for call targets with no definition.
        if unresolved:
            stub_path = os.path.join(output_dir, f"{prefix}_stubs_unresolved.c")
            stub_lines = [
                "/**",
                " * Unresolved call target stubs",
                f" * {len(unresolved)} addresses called by translated code but not",
                " * detected as functions - typically mid-function entry points.",
                " * Auto-generated by tools/recomp.",
                " */",
                "",
                "#define RECOMP_GENERATED_CODE",
                f'#include "{header_name}"',
                "",
            ]
            for addr in sorted(unresolved):
                stub_lines.append(
                    f"void {unresolved[addr]}(void) {{ esp += 4; /* 0x{addr:08X}: not detected; minimal guest ret */ }}"
                )
            stub_lines.append("")

            with open(stub_path, "w", encoding="utf-8") as f:
                f.write("\n".join(stub_lines))
            generated_files.append(stub_path)

            if verbose:
                print(f"  Wrote {stub_path} ({len(unresolved)} stubs)",
                      file=sys.stderr)

        # Generate dispatch table
        dispatch_path = os.path.join(output_dir, f"{prefix}_dispatch.c")
        self._write_dispatch_table(translations, dispatch_path, header_name)
        generated_files.append(dispatch_path)

        stats["files"] = generated_files
        stats["num_chunks"] = len(chunks)
        stats["chunk_size"] = chunk_size
        return stats

    def _write_dispatch_table(self, translations, output_path, header_name):
        """
        Generate a dispatch table mapping Xbox VA -> function pointer.

        Uses a sorted array + binary search for O(log n) lookup.
        """
        lines = [
            "/**",
            " * Xbox - Recompiled Function Dispatch Table",
            f" * Maps {len(translations)} Xbox VAs to translated function pointers.",
            " * Auto-generated by tools/recomp",
            " */",
            "",
            "#define RECOMP_DISPATCH_H",
            f'#include "{header_name}"',
            '#include <stddef.h>',
            "",
            "/* Generic function pointer type */",
            "typedef void (*recomp_func_t)(void);",
            "",
            "typedef struct {",
            "    uint32_t xbox_va;",
            "    recomp_func_t func;",
            "} recomp_entry_t;",
            "",
            f"static const recomp_entry_t g_recomp_table[] = {{",
        ]

        for addr, name, _ in translations:
            lines.append(f"    {{ 0x{addr:08X}u, (recomp_func_t){name} }},")

        lines.extend([
            "};",
            "",
            f"static const size_t g_recomp_table_size = "
            f"{len(translations)};",
            "",
            "/* Binary search for a function by Xbox VA */",
            "recomp_func_t recomp_lookup(uint32_t xbox_va)",
            "{",
            "    size_t lo = 0, hi = g_recomp_table_size;",
            "    while (lo < hi) {",
            "        size_t mid = lo + (hi - lo) / 2;",
            "        if (g_recomp_table[mid].xbox_va < xbox_va)",
            "            lo = mid + 1;",
            "        else if (g_recomp_table[mid].xbox_va > xbox_va)",
            "            hi = mid;",
            "        else",
            "            return g_recomp_table[mid].func;",
            "    }",
            "    return NULL;",
            "}",
            "",
            "/* Get the number of registered functions */",
            "size_t recomp_get_count(void)",
            "{",
            "    return g_recomp_table_size;",
            "}",
            "",
            "/* Call all registered functions (for bulk testing) */",
            "size_t recomp_call_all(void)",
            "{",
            "    size_t i;",
            "    for (i = 0; i < g_recomp_table_size; i++) {",
            "        g_recomp_table[i].func();",
            "    }",
            "    return g_recomp_table_size;",
            "}",
            "",
        ])

        with open(output_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
