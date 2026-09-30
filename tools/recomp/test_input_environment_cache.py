from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'src/input/xinput_device.c').read_text(encoding='utf-8')

def main() -> None:
    assert 'INPUT_ENV_CACHE_CAPACITY 64' in SOURCE
    assert 'static const char *input_cached_getenv' in SOURCE
    assert '__declspec(thread) static InputEnvCacheEntry' in SOURCE
    assert SOURCE.count('getenv(') - SOURCE.count('input_cached_getenv(') == 1, 'only the cache miss may call getenv'
    assert SOURCE.count('input_cached_getenv(') >= 40
    assert 'input_cached_getenv(name)' in SOURCE
    assert re.search(r'for \(i = 0; i < g_input_env_cache_count; \+\+i\).*?strcmp\(g_input_env_cache\[i\]\.name, name\)', SOURCE, re.S)
    print('input environment cache regression passed')

if __name__ == '__main__':
    main()
