"""Compile the real primitive guard: isolated line segments must survive submission."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
start = source.index('static int draw_has_enough_vertices(')
end = source.index('\nstatic void submit_array_draw(void)', start)
helper = source[start:end]
fixture = r'''
#include <stdint.h>
#include <assert.h>
enum { D3DPT_LINELIST=2, D3DPT_LINESTRIP=3,
       D3DPT_TRIANGLELIST=4, D3DPT_TRIANGLESTRIP=5, D3DPT_TRIANGLEFAN=6 };
''' + helper + r'''
int main(void) {
    for (int type=D3DPT_LINELIST; type<=D3DPT_LINESTRIP; ++type) {
        assert(!draw_has_enough_vertices(0,type));
        assert(!draw_has_enough_vertices(1,type));
        assert(draw_has_enough_vertices(2,type));
        assert(draw_has_enough_vertices(3,type));
        assert(draw_has_enough_vertices(100,type));
    }
    for (int type=D3DPT_TRIANGLELIST; type<=D3DPT_TRIANGLEFAN; ++type) {
        assert(!draw_has_enough_vertices(0,type));
        assert(!draw_has_enough_vertices(1,type));
        assert(!draw_has_enough_vertices(2,type));
        assert(draw_has_enough_vertices(3,type));
        assert(draw_has_enough_vertices(100,type));
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nv2a-lines-') as temp:
    temp=Path(temp)
    (temp/'test.c').write_text(fixture,encoding='utf-8')
    compiler=shutil.which('gcc') or 'C:/msys64/mingw64/bin/gcc.exe'
    subprocess.run([compiler,'-std=c11',str(temp/'test.c'),'-o',str(temp/'test.exe')],check=True)
    subprocess.run([str(temp/'test.exe')],check=True)
# Both real entry points must use the tested guard, before reading vertices.
array=source[source.index('static void submit_array_draw(void)'):source.index('static void submit_draw(void)')]
immediate=source[source.index('static void submit_draw(void)'):]
assert '!draw_has_enough_vertices(count, g_pg.d3d_prim_type)' in array
assert '!draw_has_enough_vertices(num_verts, g_pg.d3d_prim_type)' in immediate
assert 'count < 3u' not in array.split('if (g_pg.inline_array_mode) {',2)[-1].split('vertex_bytes =',1)[0]
assert 'if (num_verts < 3)' not in immediate
print('NV2A two-vertex line regression passed')
