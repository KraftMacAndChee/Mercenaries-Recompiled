from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/kernel/xbox_memory_layout.c"


def test_guest_heap_free_coalesces_only_the_new_block() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index("static void xbox_HeapCoalesceFree(")
    end = source.index("\nstatic void xbox_HeapTrimTail", start)
    body = source[start:end]

    assert "xbox_heap_allocation *block" in body
    assert body.count("for (") == 1
    assert "xbox_HeapCoalesceFree(&g_heap_allocations[i]);" in source


def test_no_hole_bump_allocations_do_not_rescan_the_allocation_table() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index("static uint32_t xbox_HeapTryReuse(")
    end = source.index("static uint32_t xbox_HeapAllocClass(", start)
    body = source[start:end]

    assert "static int g_heap_free_count = 0;" in source
    assert "static uint32_t g_heap_page_next" in source
    assert "for (i = 0; g_heap_free_count != 0 &&" in body
    assert "--g_heap_free_count;" in body
    assert "++g_heap_free_count;" in source


def test_page_and_normal_free_extents_remain_segregated() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    assert "XBOX_HEAP_CLASS_NORMAL" in source
    assert "XBOX_HEAP_CLASS_PAGE_BACKED" in source
    coalesce_start = source.index("static void xbox_HeapCoalesceFree(")
    coalesce_end = source.index("static void xbox_HeapTrimTail", coalesce_start)
    coalesce = source[coalesce_start:coalesce_end]
    assert "candidate->allocation_class != block->allocation_class" not in coalesce
    assert "left->allocation_class = XBOX_HEAP_CLASS_PAGE_BACKED" in coalesce
    assert "block->allocation_class = XBOX_HEAP_CLASS_PAGE_BACKED" in coalesce
    assert "allocation->allocation_class != allocation_class" in source
    assert "allocation_class == XBOX_HEAP_CLASS_PAGE_BACKED" in source
    assert "g_heap_page_next - size" in source
    assert "result < g_heap_next" in source
    assert "exact_size_only && allocation->allocation_size - size >= alignment" in source


def test_guest_heap_oom_reporting_is_rate_limited_but_keeps_census() -> None:
    source = SOURCE.read_text(encoding="utf-8")

    assert "g_heap_oom_count <= 8u" in source
    assert "g_heap_oom_count & (g_heap_oom_count - 1u)" in source
    assert "xbox_HeapDumpCensus();" in source


if __name__ == "__main__":
    test_guest_heap_free_coalesces_only_the_new_block()
    test_no_hole_bump_allocations_do_not_rescan_the_allocation_table()
    test_page_and_normal_free_extents_remain_segregated()
    test_guest_heap_oom_reporting_is_rate_limited_but_keeps_census()
    print("Guest heap coalescing/performance checks passed")
