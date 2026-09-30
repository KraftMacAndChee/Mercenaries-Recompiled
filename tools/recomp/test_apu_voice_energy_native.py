"""Exercise production optional energy tracing with simultaneous Windows workers."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VoiceEnergyTests(unittest.TestCase):
    def test_concurrent_accumulation_and_frame_only_flush(self):
        source = (ROOT / "src/apu/apu_vp.c").read_text(encoding="utf-8")
        begin = source.index("static int g_trace_voice_energy = -1;")
        end = source.index("static void voice_process(", begin)
        functions = source[begin:end]
        frame = source[source.index("void mcpx_apu_vp_frame("):]
        self.assertLess(frame.index('getenv("MERCENARIES_TRACE_APU_VOICE_ENERGY")'),
                        frame.index("voice_work_dispatch(d, mixbins);"))
        self.assertLess(frame.index("voice_work_dispatch(d, mixbins);"),
                        frame.index("trace_voice_energy_flush();"))
        process = source[end:source.index("static void get_voice_bin_src_dst", end)]
        self.assertNotIn("trace_voice_energy_flush(", process)
        self.assertRegex(process, r"if \(g_trace_voice_energy\) \{\s+for \(int i = 0;")

        prelude = r'''
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
/* This older MinGW SDK declares SRW locks but omits their zero-init macro. */
#ifndef SRWLOCK_INIT
#define SRWLOCK_INIT {0}
#endif
#define MCPX_HW_MAX_VOICES 256
#define NUM_SAMPLES_PER_FRAME 32
#define QEMU_CLOCK_REALTIME 0
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failed line %d\n",__LINE__); exit(2); } } while(0)
static DWORD frame_thread;
static int64_t fake_now;
static int64_t qemu_clock_get_us(int clock) {
 CHECK(GetCurrentThreadId() == frame_thread); return fake_now;
}
static void trace_voice_energy_flush(void);
'''
        harness = r'''
static DWORD WINAPI record_worker(void *arg) {
 float samples[32][2];
 for (int i=0;i<32;++i) { samples[i][0]=.5f; samples[i][1]=-.25f; }
 for (int i=0;i<2000;++i) trace_voice_energy_record(83,samples,8.0,.25f);
 return 0;
}
int main(void) {
 frame_thread=GetCurrentThreadId(); g_trace_voice_energy=1;
 g_trace_voice_energy_start_us=1000000; fake_now=1500000;
 HANDLE workers[8];
 for(int i=0;i<8;++i) { workers[i]=CreateThread(NULL,0,record_worker,NULL,0,NULL); CHECK(workers[i]); }
 CHECK(WaitForMultipleObjects(8,workers,TRUE,10000)==WAIT_OBJECT_0);
 for(int i=0;i<8;++i) CloseHandle(workers[i]);
 CHECK(g_trace_voice_energy_samples[83]==16000*64);
 CHECK(g_trace_voice_energy_pre[83]==16000*8.0);
 CHECK(g_trace_voice_energy_post[83]==16000*10.0);
 CHECK(g_trace_voice_energy_gain[83]==16000*.25);
 trace_voice_energy_flush();
 CHECK(g_trace_voice_energy_samples[83]==16000*64);
 fake_now=2000000; trace_voice_energy_flush();
 CHECK(g_trace_voice_energy_samples[83]==0);
 CHECK(g_trace_voice_energy_pre[83]==0 && g_trace_voice_energy_post[83]==0);
 CHECK(g_trace_voice_energy_gain[83]==0);
 g_trace_voice_energy=0; record_worker(NULL); trace_voice_energy_flush();
 CHECK(g_trace_voice_energy_samples[83]==0);
 puts("16000 simultaneous contributions retained; frame-only flush and disabled path passed");
 return 0;
}
'''
        # Deterministically reject the original ownership error: a worker
        # attempting a time-based shared flush, irrespective of race timing.
        mutant = functions.replace(
            "ReleaseSRWLockExclusive(&g_trace_voice_energy_lock);",
            "ReleaseSRWLockExclusive(&g_trace_voice_energy_lock);\n    trace_voice_energy_flush();")
        self.assertNotEqual(functions, mutant)
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        for label, body in (("fixed", functions), ("worker-flush", mutant)):
            with self.subTest(label=label), tempfile.TemporaryDirectory(prefix="merc-energy-") as directory:
                src, exe = Path(directory)/"check.c", Path(directory)/"check.exe"
                src.write_text(prelude+body+harness, encoding="utf-8")
                build = subprocess.run([compiler,"-std=c11","-O1",str(src),"-o",str(exe)],
                                       capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                run = subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
                if label == "fixed":
                    self.assertEqual(run.returncode,0,run.stderr)
                    self.assertIn("samples=1024000",run.stderr)
                    self.assertIn("pre=0.35355339 post=0.39528471 avg_gain=0.25000000",run.stderr)
                    print(run.stdout.strip())
                else:
                    self.assertNotEqual(run.returncode,0)


if __name__ == "__main__":
    unittest.main()
