"""Regression coverage for Xbox FileAllocationInformation (class 19)."""
from pathlib import Path
import ctypes
import os
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class KernelFileAllocationInformationTests(unittest.TestCase):
    def test_runtime_implements_both_host_backends(self):
        header = (ROOT / "src/kernel/kernel.h").read_text(encoding="utf-8")
        source = (ROOT / "src/kernel/kernel_file.c").read_text(encoding="utf-8")
        self.assertIn("typedef struct _XBOX_FILE_ALLOCATION_INFORMATION", header)
        self.assertIn("LARGE_INTEGER AllocationSize;", header)
        self.assertEqual(source.count("case XboxFileAllocationInformation:"), 2)
        windows, posix = source.split("/* ====================  POSIX backend", 1)
        self.assertIn("SetFileInformationByHandle(FileHandle, FileAllocationInfo", windows)
        self.assertIn("Length < sizeof(*info)", windows)
        self.assertIn("info->AllocationSize.QuadPart < 0", windows)
        self.assertIn("fstat(fd, &st)", posix)
        self.assertIn("ftruncate(fd, (off_t)info->AllocationSize.QuadPart)", posix)

    @unittest.skipUnless(os.name == "nt", "Windows allocation contract")
    def test_windows_allocation_does_not_extend_logical_eof(self):
        class LARGE_INTEGER(ctypes.Structure):
            _fields_ = [("QuadPart", ctypes.c_longlong)]

        class FILE_ALLOCATION_INFO(ctypes.Structure):
            _fields_ = [("AllocationSize", LARGE_INTEGER)]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileW.restype = ctypes.c_void_p
        kernel32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32,
                                         ctypes.c_uint32, ctypes.c_void_p,
                                         ctypes.c_uint32, ctypes.c_uint32,
                                         ctypes.c_void_p]
        kernel32.SetFileInformationByHandle.restype = ctypes.c_int
        kernel32.SetFileInformationByHandle.argtypes = [ctypes.c_void_p,
                                                         ctypes.c_int,
                                                         ctypes.c_void_p,
                                                         ctypes.c_uint32]
        kernel32.GetFileSizeEx.restype = ctypes.c_int
        kernel32.GetFileSizeEx.argtypes = [ctypes.c_void_p,
                                            ctypes.POINTER(LARGE_INTEGER)]
        kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        with tempfile.TemporaryDirectory(prefix="mercs-file-allocation-") as td:
            target = Path(td) / "save.bin"
            target.write_bytes(b"S" * 4096)
            handle = kernel32.CreateFileW(str(target), 0xC0000000, 0x7, None,
                                          3, 0x80, None)
            self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
            try:
                allocation = FILE_ALLOCATION_INFO(LARGE_INTEGER(1024 * 1024))
                ok = kernel32.SetFileInformationByHandle(
                    handle, 5, ctypes.byref(allocation), ctypes.sizeof(allocation)
                )
                self.assertTrue(ok, ctypes.get_last_error())
                logical_size = LARGE_INTEGER()
                self.assertTrue(kernel32.GetFileSizeEx(handle,
                                                       ctypes.byref(logical_size)))
                self.assertEqual(logical_size.QuadPart, 4096)
            finally:
                kernel32.CloseHandle(handle)


if __name__ == "__main__":
    unittest.main()