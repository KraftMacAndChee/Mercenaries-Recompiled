/*
 * kernel_path.c - Xbox device-path translation
 *
 * Translates Xbox device-style paths to host filesystem paths:
 *   \Device\CdRom0\  -> <game_dir>/
 *   D:\               -> <game_dir>/
 *   T:\               -> <save_dir>/TitleData/
 *   U:\               -> <save_dir>/UserData/
 *   Z:\               -> <save_dir>/Cache/
 *
 * The Win32 build emits UTF-16 paths (for CreateFileW); the Linux build
 * emits UTF-8 paths with '/' separators (for open()).
 */

#include "kernel.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/*
 * Helper: check if an ANSI string starts with a prefix (case-insensitive).
 * Returns the number of chars consumed from the prefix, or 0 if no match.
 * Platform-independent.
 */
static int match_prefix(const char* path, const char* prefix)
{
    int i = 0;
    while (prefix[i]) {
        /* NtCreateFile may strip a directory root's final separator before
         * translation. Accept the exact root, never a prefix lookalike such
         * as Partition50 for Partition5. Descendants still require '\\'. */
        if (path[i] == '\0' && prefix[i] == '\\' && prefix[i + 1] == '\0')
            return i;
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)prefix[i]))
            return 0;
        i++;
    }
    return i;
}

static int path_equals(const char* path, const char* expected)
{
    int matched = match_prefix(path, expected);
    return matched != 0 && path[matched] == '\0';
}

/* A device-path translation rule, shared by both backends. */
typedef struct {
    const char* prefix;     /* Xbox path prefix (backslash form)         */
    int         to_save;    /* 1 = under save_dir, 0 = under game_dir    */
    const char* sub_win;    /* sub-directory, Win32 backslash form       */
    const char* sub_posix;  /* sub-directory, POSIX slash form           */
} path_rule;

static const path_rule s_rules[] = {
    { "\\Device\\CdRom0\\",                   0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition1\\",    1, "",           ""            },
    { "\\Device\\Harddisk0\\Partition2\\",    1, "\\System\\Partition2", "/System/Partition2" },
    { "\\Device\\Harddisk0\\Partition3\\",    1, "\\Cache\\X",  "/Cache/X"    },
    { "\\Device\\Harddisk0\\Partition4\\",    1, "\\Cache\\Y",  "/Cache/Y"    },
    { "\\Device\\Harddisk0\\Partition5\\",    1, "\\Cache\\Z",  "/Cache/Z"    },
    { "D:\\",                                 0, NULL,         NULL          },
    { "d:\\",                                 0, NULL,         NULL          },
    /* Y: is the Xbox dashboard partition; the dashboard opens its assets
     * (e.g. "Y:\default.xip") from there. Map it to the game dir. */
    { "Y:\\",                                 0, NULL,         NULL          },
    { "y:\\",                                 0, NULL,         NULL          },
    { "T:\\",                                 1, "\\TitleData","/TitleData"  },
    { "U:\\",                                 1, "\\UserData", "/UserData"   },
    { "Z:\\",                                 1, "\\Cache",    "/Cache"      },
    { "\\??\\D:\\",                           0, NULL,         NULL          },
    { "\\??\\Y:\\",                           0, NULL,         NULL          },
    { "\\??\\y:\\",                           0, NULL,         NULL          },
    { "\\??\\T:\\",                           1, "\\TitleData","/TitleData"  },
};
#define PATH_RULE_COUNT ((int)(sizeof(s_rules) / sizeof(s_rules[0])))

/* ======================================================================== */
#if defined(_WIN32)
/* ======================================================================== */

#include <shlobj.h>
#include "mod_overlay.h"

static WCHAR s_game_dir[MAX_PATH];
static WCHAR s_save_dir[MAX_PATH];
static BOOL  s_initialized = FALSE;
static WCHAR s_partition0_path[MAX_PATH];

/* A newly created host directory already represents a formatted, empty cache
 * volume. Xapi's raw FATX formatter cannot operate on a directory handle.
 * Handle only exact, empty cache roots here; occupied volumes and other paths
 * retain the original guest path. Never erase saves or existing cache data. */
BOOL xbox_prepare_empty_cache_volume(const char *path)
{
    WCHAR host[MAX_PATH], search[MAX_PATH];
    WIN32_FIND_DATAW entry;
    HANDLE find;
    DWORD attributes, error;
    int partition;
    BOOL empty = TRUE;
    if (!path) return FALSE;
    for (partition = 3; partition <= 5; ++partition) {
        char expected[64];
        snprintf(expected, sizeof(expected), "\\Device\\Harddisk0\\Partition%d\\", partition);
        if (path_equals(path, expected)) break;
    }
    if (partition > 5 || !xbox_translate_path(path, host, MAX_PATH)) return FALSE;
    attributes = GetFileAttributesW(host);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return FALSE;
    if (wcslen(host) + 3 >= MAX_PATH) return FALSE;
    swprintf_s(search, MAX_PATH, L"%s\\*", host);
    find = FindFirstFileW(search, &entry);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (wcscmp(entry.cFileName, L".") && wcscmp(entry.cFileName, L"..")) {
            empty = FALSE;
            break;
        }
    } while (FindNextFileW(find, &entry));
    error = GetLastError();
    FindClose(find);
    return empty && error == ERROR_NO_MORE_FILES;
}


void xbox_path_init(const char* game_dir, const char* save_dir)
{
    WCHAR save_base[MAX_PATH];

    if (game_dir) {
        MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, s_game_dir, MAX_PATH);
    } else {
        GetCurrentDirectoryW(MAX_PATH, s_game_dir);
        wcscat_s(s_game_dir, MAX_PATH, L"\\Burnout 3 Takedown");
    }

    if (save_dir) {
        MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, s_save_dir, MAX_PATH);
    } else {
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, save_base))) {
            swprintf_s(s_save_dir, MAX_PATH, L"%s\\Burnout3", save_base);
        } else {
            GetCurrentDirectoryW(MAX_PATH, s_save_dir);
            wcscat_s(s_save_dir, MAX_PATH, L"\\SaveData");
        }
    }

    size_t len = wcslen(s_game_dir);
    if (len > 0 && s_game_dir[len - 1] == L'\\')
        s_game_dir[len - 1] = L'\0';

    len = wcslen(s_save_dir);
    if (len > 0 && s_save_dir[len - 1] == L'\\')
        s_save_dir[len - 1] = L'\0';

    {
        WCHAR absolute_save[MAX_PATH];
        DWORD absolute_length = GetFullPathNameW(
            s_save_dir, MAX_PATH, absolute_save, NULL);
        if (absolute_length > 0 && absolute_length < MAX_PATH)
            wcscpy_s(s_save_dir, MAX_PATH, absolute_save);
    }

    SHCreateDirectoryExW(NULL, s_save_dir, NULL);

    {
        WCHAR system_dir[MAX_PATH];
        HANDLE partition;
        LARGE_INTEGER size;
        LARGE_INTEGER minimum_size;

        swprintf_s(system_dir, MAX_PATH, L"%s\\System", xbox_mod_cache_root() ? xbox_mod_cache_root() : s_save_dir);
        SHCreateDirectoryExW(NULL, system_dir, NULL);
        swprintf_s(s_partition0_path, MAX_PATH,
                   L"%s\\partition0.bin", system_dir);

        partition = CreateFileW(s_partition0_path,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
        if (partition != INVALID_HANDLE_VALUE) {
            minimum_size.QuadPart = 0xA00;
            if (GetFileSizeEx(partition, &size) &&
                size.QuadPart < minimum_size.QuadPart &&
                SetFilePointerEx(partition, minimum_size, NULL, FILE_BEGIN)) {
                SetEndOfFile(partition);
            }
            CloseHandle(partition);
        } else {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                     "Could not create partition 0 backing file: %S", s_partition0_path);
        }
    }
    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%S, save=%S", s_game_dir, s_save_dir);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char*  remainder = NULL;
    const WCHAR* base_dir  = NULL;
    const char*  sub_dir   = NULL;
    int          skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    if (path_equals(xbox_path, "\\Device\\Harddisk0\\Partition0")) {
        wcscpy_s(host_path_buf, buf_size, s_partition0_path);
        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %S", xbox_path, host_path_buf);
        return TRUE;
    }
    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_win;
            if (xbox_mod_cache_root() && sub_dir &&
                (!strncmp(sub_dir, "\\Cache", 6) || !strncmp(sub_dir, "\\System", 7)))
                base_dir = xbox_mod_cache_root();
            goto translate;
        }
    }

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    MultiByteToWideChar(CP_ACP, 0, xbox_path, -1, host_path_buf, buf_size);
    return TRUE;

translate:
    {
        WCHAR remainder_wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, remainder, -1, remainder_wide, MAX_PATH);

        for (WCHAR* p = remainder_wide; *p; p++) {
            if (*p == L'/') *p = L'\\';
        }

        if (base_dir == s_game_dir && xbox_mod_overlay_path(remainder_wide, host_path_buf, buf_size))
            return TRUE;
        if (sub_dir) {
            WCHAR sub_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, sub_dir, -1, sub_wide, MAX_PATH);
            swprintf_s(host_path_buf, buf_size, L"%s%s\\%s", base_dir, sub_wide, remainder_wide);

            WCHAR dir_path[MAX_PATH];
            swprintf_s(dir_path, MAX_PATH, L"%s%s", base_dir, sub_wide);
            SHCreateDirectoryExW(NULL, dir_path, NULL);
        } else {
            swprintf_s(host_path_buf, buf_size, L"%s\\%s", base_dir, remainder_wide);
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %S", xbox_path, host_path_buf);
        return TRUE;
    }
}

/* ======================================================================== */
#else /* !_WIN32  -- POSIX / Linux */
/* ======================================================================== */

#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];
static BOOL s_initialized = FALSE;

static char s_partition0_path[MAX_PATH];
/* Strip a single trailing '/' (but never the root '/'). */
static void strip_trailing_slash(char* s)
{
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == '/')
        s[len - 1] = '\0';
}

/* Recursively create a directory and all missing parents. */
static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    if (game_dir) {
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", game_dir);
    } else {
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(s_game_dir, sizeof(s_game_dir), "%s/Burnout 3 Takedown", cwd);
    }

    if (save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    } else {
        /* XDG base-directory spec: $XDG_DATA_HOME or ~/.local/share */
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/burnout3", xdg);
        } else {
            const char* home = getenv("HOME");
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/.local/share/burnout3",
                     (home && home[0]) ? home : ".");
        }
    }

    strip_trailing_slash(s_game_dir);
    strip_trailing_slash(s_save_dir);

    mkdir_p(s_save_dir);
    s_initialized = TRUE;
    {
        char system_dir[MAX_PATH];
        int partition;
        struct stat st;

        snprintf(system_dir, sizeof(system_dir), "%s/System", s_save_dir);
        mkdir_p(system_dir);
        snprintf(s_partition0_path, sizeof(s_partition0_path),
                 "%s/partition0.bin", system_dir);

        partition = open(s_partition0_path, O_RDWR | O_CREAT, 0644);
        if (partition >= 0) {
            if (fstat(partition, &st) == 0 && st.st_size < 0xA00)
                ftruncate(partition, 0xA00);
            close(partition);
        } else {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH,
                     "Could not create partition 0 backing file %s: %s",
                     s_partition0_path, strerror(errno));
        }
    }
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s",
             s_game_dir, s_save_dir);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char* remainder = NULL;
    const char* base_dir  = NULL;
    const char* sub_dir   = NULL;
    int         skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    if (path_equals(xbox_path, "\\Device\\Harddisk0\\Partition0")) {
        snprintf(host_path_buf, buf_size, "%s", s_partition0_path);
        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        return TRUE;
    }
    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_posix;
            goto translate;
        }
    }

    /* Unrecognized path: pass through, just normalize separators. */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    snprintf(host_path_buf, buf_size, "%s", xbox_path);
    for (char* p = host_path_buf; *p; p++)
        if (*p == '\\') *p = '/';
    return TRUE;

translate:
    {
        char remainder_posix[MAX_PATH];
        snprintf(remainder_posix, sizeof(remainder_posix), "%s", remainder);

        /* Xbox paths use backslashes -> POSIX slashes. */
        for (char* p = remainder_posix; *p; p++)
            if (*p == '\\') *p = '/';

        if (sub_dir) {
            snprintf(host_path_buf, buf_size, "%s%s/%s",
                     base_dir, sub_dir, remainder_posix);

            /* Ensure the save directory tree exists. */
            char dir_path[MAX_PATH];
            snprintf(dir_path, sizeof(dir_path), "%s%s", base_dir, sub_dir);
            mkdir_p(dir_path);
        } else {
            snprintf(host_path_buf, buf_size, "%s/%s", base_dir, remainder_posix);
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        return TRUE;
    }
}

#endif /* _WIN32 */
