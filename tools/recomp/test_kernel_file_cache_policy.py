"""Regression coverage for portable Xbox file-cache option translation."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class KernelFileCachePolicyTests(unittest.TestCase):
    def test_guest_no_buffering_does_not_bypass_host_cache(self):
        source = (ROOT / "src/kernel/kernel_file.c").read_text(encoding="utf-8")
        match = re.search(
            r"static DWORD xbox_create_options_to_win32\(ULONG CreateOptions\)"
            r"\s*\{(.*?)\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(match)
        body = match.group(1)
        self.assertIn("(void)CreateOptions", body)
        self.assertIn("FILE_ATTRIBUTE_NORMAL", body)
        self.assertNotIn("FILE_FLAG_NO_BUFFERING", body)

    def test_create_file_uses_translated_cache_policy(self):
        source = (ROOT / "src/kernel/kernel_file.c").read_text(encoding="utf-8")
        windows = source.split("/* ====================  POSIX backend", 1)[0]
        self.assertIn(
            "DWORD flags_and_attrs = xbox_create_options_to_win32(CreateOptions);",
            windows,
        )
        self.assertIn("CreateFileW(win_path", windows)
        self.assertIn("flags_and_attrs, NULL", windows)

    def test_guest_streaming_hint_uses_bounded_runtime_read_ahead(self):
        source = (ROOT / "src/kernel/kernel_file.c").read_text(encoding="utf-8")
        windows = source.split("/* ====================  POSIX backend", 1)[0]
        self.assertIn("#define XBOX_READ_CACHE_CONTEXTS 16", windows)
        self.assertIn("#define XBOX_READ_CACHE_BLOCKS 2", windows)
        self.assertIn(
            "#define XBOX_READ_CACHE_BLOCK_SIZE (8u * 1024u * 1024u)", windows
        )
        self.assertIn(
            "create_options & XBOX_FILE_NO_INTERMEDIATE_BUFFERING", windows
        )
        self.assertIn("read_file_through_guest_cache(", windows)
        self.assertIn("register_read_cache_handle(h, CreateOptions);", windows)

    def test_mutation_and_close_invalidate_cached_bytes(self):
        source = (ROOT / "src/kernel/kernel_file.c").read_text(encoding="utf-8")
        windows = source.split("/* ====================  POSIX backend", 1)[0]
        write_start = windows.index("NTSTATUS __stdcall xbox_NtWriteFile(")
        close_start = windows.index("NTSTATUS __stdcall xbox_NtClose(")
        write_body = windows[write_start:close_start]
        close_body = windows[close_start:]
        self.assertIn("invalidate_read_cache_for_handle(FileHandle);", write_body)
        self.assertIn("cleanup_read_cache_for_handle(Handle);", close_body)
        self.assertIn(
            "case XboxFileEndOfFileInformation:", close_body
        )
        eof_body = close_body.split("case XboxFileEndOfFileInformation:", 1)[1]
        self.assertIn("invalidate_read_cache_for_handle(FileHandle);", eof_body)


if __name__ == "__main__":
    unittest.main()
