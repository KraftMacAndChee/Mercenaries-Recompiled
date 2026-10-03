#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "mod_loader.h"
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace mercmods
{
namespace
{
using Files = std::map<std::wstring, fs::path>;
std::wstring lower(std::wstring s)
{
    for (auto &c : s)
        c = (wchar_t)towlower(c);
    return s;
}
[[noreturn]] void fail(const char *s) { throw std::runtime_error(s); }
void bounded(const fs::path &p)
{
    if (p.wstring().size() >= MAX_PATH - 1)
        fail("A mod path is too long. Move the installation to a shorter path.");
}
bool regular(const fs::path &p) { return fs::is_regular_file(p); }
Files files(const fs::path &root)
{
    Files result;
    if (!fs::exists(root))
        return result;
    if (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
        fail("Mod folders must be ordinary folders, not links or junctions.");
    for (const auto &e : fs::recursive_directory_iterator(root))
    {
        bounded(e.path());
        if (GetFileAttributesW(e.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
            fail("Links and junctions inside mods are not supported.");
        if (e.is_regular_file())
        {
            auto key = lower(e.path().lexically_relative(root).wstring());
            if (key.find_first_of(L"\t\r\n") != std::wstring::npos)
                fail("Unsupported control character in a mod filename.");
            if (!result.emplace(key, e.path()).second)
                fail("Duplicate filenames differ only by case.");
        }
    }
    return result;
}
struct Hash
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE value = nullptr;
    Hash()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptCreateHash(algorithm, &value, nullptr, 0, nullptr, 0, 0) < 0)
            fail("Cannot initialize mod fingerprint.");
    }
    ~Hash()
    {
        if (value)
            BCryptDestroyHash(value);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    void add(const void *data, size_t n)
    {
        while (n)
        {
            ULONG count = (ULONG)std::min<size_t>(n, 1 << 20);
            if (BCryptHashData(value, (PUCHAR)data, count, 0) < 0)
                fail("Cannot fingerprint mod data.");
            data = (const unsigned char *)data + count;
            n -= count;
        }
    }
    void name(const std::wstring &s)
    {
        uint64_t n = s.size();
        add(&n, sizeof(n));
        add(s.data(), s.size() * sizeof(wchar_t));
    }
    void file(const fs::path &p)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f)
            fail("Cannot read a mod or game file.");
        uint64_t n = fs::file_size(p);
        add(&n, sizeof(n));
        std::array<char, 65536> buf;
        while (f)
        {
            f.read(buf.data(), buf.size());
            add(buf.data(), (size_t)f.gcount());
        }
        if (!f.eof())
            fail("Error reading a mod or game file.");
    }
    std::wstring finish()
    {
        unsigned char digest[32];
        if (BCryptFinishHash(value, digest, 32, 0) < 0)
            fail("Cannot finish mod fingerprint.");
        std::wostringstream s;
        for (auto c : digest)
            s << std::hex << std::setfill(L'0') << std::setw(2) << (unsigned)c;
        return s.str();
    }
};
struct Record
{
    uint32_t size, key, kind;
    uint64_t offset;
    fs::path source;
};
struct Archive
{
    uint32_t tag;
    bool trailer;
    std::vector<Record> records;
};
uint32_t read32(std::istream &f)
{
    uint32_t v = 0;
    if (!f.read((char *)&v, 4))
        fail("Truncated DSK archive.");
    return v;
}
Archive read_dsk(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        fail("Cannot open DSK archive.");
    uint64_t bytes = fs::file_size(p);
    uint32_t count = read32(f);
    Archive a{read32(f), false, {}};
    if (bytes < 8 || count > (bytes - 8) / 12 || count > 1000000)
        fail("Invalid DSK directory.");
    uint64_t offset = 8ull + count * 12ull;
    std::map<uint64_t, bool> unique;
    for (uint32_t i = 0; i < count; ++i)
    {
        Record r{read32(f), read32(f), read32(f), offset, p};
        offset += r.size;
        if (offset > bytes || !unique.emplace((uint64_t(r.key) << 32) | r.kind, true).second)
            fail("Invalid DSK bounds or duplicate asset identifiers.");
        a.records.push_back(r);
    }
    if (bytes - offset != 0 && bytes - offset != 2048)
        fail("Unsupported DSK trailer.");
    a.trailer = bytes != offset;
    // Archive builders may place a signature in the sector after the indexed payload.
    // That sector is not an asset; single archives remain byte-for-byte unchanged.
    return a;
}
bool same_record(const Record &a, const Record &b)
{
    if (a.size != b.size)
        return false;
    std::ifstream x(a.source, std::ios::binary), y(b.source, std::ios::binary);
    x.seekg(a.offset);
    y.seekg(b.offset);
    std::array<char, 65536> bx, by;
    uint32_t left = a.size;
    while (left)
    {
        auto n = std::min<size_t>(left, bx.size());
        if (!x.read(bx.data(), n) || !y.read(by.data(), n))
            fail("Cannot compare DSK assets.");
        if (memcmp(bx.data(), by.data(), n))
            return false;
        left -= (uint32_t)n;
    }
    return true;
}
void merge_dsk(const fs::path &base, const std::vector<fs::path> &layers, const fs::path &out)
{
    Archive original = read_dsk(base);
    std::map<uint64_t, Record> baseline, current;
    std::vector<uint64_t> order;
    for (auto &r : original.records)
    {
        auto id = (uint64_t(r.key) << 32) | r.kind;
        baseline.emplace(id, r);
        current.emplace(id, r);
        order.push_back(id);
    }
    for (auto &path : layers)
    {
        Archive mod = read_dsk(path);
        if (mod.tag != original.tag)
            fail("DSK versions do not match.");
        std::map<uint64_t, Record> entries;
        for (auto &r : mod.records)
            entries.emplace((uint64_t(r.key) << 32) | r.kind, r);
        for (auto &r : baseline)
            if (!entries.count(r.first))
                current.erase(r.first);
        for (auto &r : mod.records)
        {
            auto id = (uint64_t(r.key) << 32) | r.kind;
            auto old = baseline.find(id);
            if (old != baseline.end() && same_record(old->second, r))
                continue;
            if (!current.count(id) && std::find(order.begin(), order.end(), id) == order.end())
                order.push_back(id);
            current[id] = r;
        }
    }
    fs::create_directories(out.parent_path());
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    auto put = [&](uint32_t v) { f.write((char *)&v, 4); };
    put((uint32_t)current.size());
    put(original.tag);
    for (auto id : order)
    {
        auto it = current.find(id);
        if (it != current.end())
        {
            put(it->second.size);
            put(it->second.key);
            put(it->second.kind);
        }
    }
    std::array<char, 65536> buf;
    for (auto id : order)
    {
        auto it = current.find(id);
        if (it == current.end())
            continue;
        auto &r = it->second;
        std::ifstream in(r.source, std::ios::binary);
        in.seekg(r.offset);
        uint32_t left = r.size;
        while (left)
        {
            auto n = std::min<size_t>(left, buf.size());
            if (!in.read(buf.data(), n))
                fail("Cannot copy DSK asset.");
            f.write(buf.data(), n);
            left -= (uint32_t)n;
        }
    }
    if (original.trailer)
    {
        std::array<char, 2048> zero{};
        f.write(zero.data(), zero.size());
    }
    f.close();
    if (!f)
        fail("Cannot write merged DSK archive (check free disk space).");
}
std::wstring ini(const fs::path &p, const wchar_t *section, const wchar_t *key,
                 const wchar_t *fallback = L"")
{
    wchar_t value[32768];
    GetPrivateProfileStringW(section, key, fallback, value, 32768, p.c_str());
    return value;
}
void copy_file_into(const fs::path &from, const fs::path &to)
{
    bounded(to);
    fs::create_directories(to.parent_path());
    if (!fs::exists(to))
    {
        auto temporary = to;
        temporary += L".preparing";
        bounded(temporary);
        fs::copy_file(from, temporary, fs::copy_options::overwrite_existing);
        fs::rename(temporary, to);
    }
}
// Native mods choose their own settings paths. Preserve configuration files;
// executable payloads and generated logs remain disposable runtime files.
bool setting_file(const fs::path &p)
{
    auto ext = lower(p.extension().wstring());
    for (auto suffix : {L".ini", L".json", L".cfg", L".conf", L".config",
                        L".toml", L".yaml", L".yml", L".xml"})
        if (ext == suffix) return true;
    return false;
}
std::wstring digest(const fs::path &p)
{
    Hash h;
    h.file(p);
    return h.finish();
}
bool profile_name(const std::wstring &name)
{
    return name.size() == 64 && std::all_of(name.begin(), name.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    });
}
void checked_tree(const fs::path &root)
{
    // Validate the entire tree before copying settings or recursively removing it.
    files(root);
}
void preserve_settings(const fs::path &installation, const fs::path &profile)
{
    std::wifstream identity(profile / L"settings-profile");
    std::wstring key;
    std::getline(identity, key);
    bool legacy = !profile_name(key);
    auto destination = installation / L"mod-settings" /
                       (legacy ? L"legacy" : key) / (legacy ? profile.filename() : fs::path());
    checked_tree(installation / L"mod-settings");
    for (const auto &e : files(profile / L"runtime"))
    {
        if (!setting_file(e.second)) continue;
        auto target = destination / e.first;
        bounded(target);
        fs::create_directories(target.parent_path());
        if (!fs::exists(target) || !fs::equivalent(e.second, target))
            fs::copy_file(e.second, target, fs::copy_options::overwrite_existing);
    }
}
void retire_profiles(const fs::path &installation, const fs::path &keep)
{
    auto root = installation / L"mod-cache";
    checked_tree(root);
    if (!fs::exists(root)) return;
    std::vector<fs::path> obsolete;
    std::vector<std::shared_ptr<void>> guards;
    for (const auto &entry : fs::directory_iterator(root))
    {
        if (!entry.is_directory() || entry.path() == keep ||
            !profile_name(entry.path().filename().wstring())) continue;
        // Also respect launchers from before the installation-wide lease existed.
        HANDLE h = CreateFileW((entry.path() / L"build.lock").c_str(), GENERIC_WRITE,
                              0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (h == INVALID_HANDLE_VALUE) fail("An older mod profile is in use. Close the other launcher.");
        guards.emplace_back(h, [](void *v) { CloseHandle(v); });
        auto executable = entry.path() / L"runtime/mercenaries_recomp.exe";
        if (regular(executable))
        {
            h = CreateFileW(executable.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) fail("Close the running modded game before changing profiles.");
            guards.emplace_back(h, [](void *v) { CloseHandle(v); });
        }
        obsolete.push_back(entry.path());
    }
    for (const auto &p : obsolete) preserve_settings(installation, p);
    guards.clear();
    for (const auto &p : obsolete) fs::remove_all(p);
}
std::shared_ptr<void> acquire_lease(const fs::path &installation)
{
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE h = CreateFileW((installation / L"mod-loader.lock").c_str(), GENERIC_READ,
                          0, &attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        fail("Close the running game or other launcher before preparing another launch.");
    return std::shared_ptr<void>(h, [](void *v) { CloseHandle(v); });
}
std::wstring quoted(const fs::path &p) { return L"\"" + p.wstring() + L"\""; }
} // namespace
Selection discover(const fs::path &installation)
{
    Selection s;
    fs::path folder = installation / L"mods", prefs = installation / L"mod-loader.ini";
    std::map<std::wstring, fs::path> found;
    if (fs::exists(folder))
        for (auto &e : fs::directory_iterator(folder))
            if (e.is_directory())
                found.emplace(e.path().filename().wstring(), e.path());
    s.separate_saves = GetPrivateProfileIntW(L"Loader", L"SeparateSaves", 1, prefs.c_str()) != 0;
    unsigned count = GetPrivateProfileIntW(L"Loader", L"Count", 0, prefs.c_str());
    if (count > 100000)
        fail("Invalid mod list in mod-loader.ini.");
    for (unsigned i = 0; i < count; ++i)
    {
        auto section = L"Mod." + std::to_wstring(i);
        auto name = ini(prefs, section.c_str(), L"Folder");
        auto it = found.find(name);
        if (it != found.end())
        {
            s.mods.push_back(
                {name, it->second,
                 GetPrivateProfileIntW(section.c_str(), L"Enabled", 0, prefs.c_str()) != 0});
            found.erase(it);
        }
    }
    for (auto &p : found)
        s.mods.push_back({p.first, p.second, false});
    return s;
}
void save_selection(const fs::path &installation, const Selection &s)
{
    const auto path = installation / L"mod-loader.ini";
    const auto temporary = installation / L"mod-loader.ini.preparing";
    std::wstring text = L"\xfeff[Loader]\r\nCount=" + std::to_wstring(s.mods.size()) +
                        L"\r\nSeparateSaves=" + (s.separate_saves ? L"1" : L"0") + L"\r\n";
    for (size_t i = 0; i < s.mods.size(); ++i)
    {
        if (s.mods[i].name.find_first_of(L"\r\n") != std::wstring::npos)
            fail("Mod folder names cannot contain newlines.");
        text += L"\r\n[Mod." + std::to_wstring(i) + L"]\r\nFolder=\"" + s.mods[i].name +
                L"\"\r\nEnabled=" + (s.mods[i].enabled ? L"1" : L"0") + L"\r\n";
    }
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(text.data()), text.size() * sizeof(wchar_t));
    output.close();
    if (!output || !MoveFileExW(temporary.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fail("Cannot save mod selection.");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}
Launch prepare(const fs::path &installation, const fs::path &game, const Selection &s, bool vanilla)
{
    Launch launch{installation / L"mercenaries_recomp.exe",
                  installation,
                  game,
                  installation / L"saves/mercenaries",
                  {},
                  {},
                  installation};
    launch.lease = acquire_lease(installation);
    if (vanilla ||
        std::none_of(s.mods.begin(), s.mods.end(), [](const Mod &m) { return m.enabled; }))
    {
        retire_profiles(installation, {});
        return launch;
    }
    Files retail = files(game), merged = retail, host;
    std::map<std::wstring, std::vector<fs::path>> archives;
    Hash settings_identity;
    settings_identity.name(L"native-settings-v1");
    Hash fingerprint;
    fingerprint.name(L"mercenaries-mod-overlay-v5");
    fingerprint.name(fs::absolute(game).wstring());
    fingerprint.file(launch.executable);
    Files runtime_inputs;
    for (auto &e : fs::directory_iterator(installation))
        if (e.is_regular_file() &&
            (lower(e.path().extension().wstring()) == L".dll" ||
             lower(e.path().filename().wstring()) == L"modcompatibility.ini"))
        {
            runtime_inputs.emplace(lower(e.path().filename().wstring()), e.path());
        }
    for (auto &e : runtime_inputs)
    {
        fingerprint.name(e.first);
        fingerprint.file(e.second);
    }
    for (auto &e : files(installation / L"compat"))
    {
        fingerprint.name(L"compat\\" + e.first);
        fingerprint.file(e.second);
    }
    for (auto &r : retail)
    {
        fingerprint.name(r.first);
        fingerprint.file(r.second);
    }
    for (auto &m : s.mods)
        if (m.enabled)
        {
            fingerprint.name(m.name);
            auto entries = files(m.root);
            bool native_mod = false;
            for (auto &e : entries)
            {
                fingerprint.name(e.first);
                fingerprint.file(e.second);
                auto key = e.first;
                bool game_file = false;
                for (auto prefix : {L"game_files\\mercenaries-retail\\", L"mercenaries-retail\\",
                                    L"game_files\\"})
                    if (key.rfind(prefix, 0) == 0)
                    {
                        key.erase(0, wcslen(prefix));
                        game_file = true;
                        break;
                    }
                if (key.rfind(L"dataxbox\\", 0) == 0 || key.rfind(L"movies\\", 0) == 0 ||
                    retail.count(key))
                    game_file = true;
                if (fs::path(key).extension() == L".dsk" && key.find(L'\\') == std::wstring::npos)
                {
                    key = L"dataxbox\\" + key;
                    game_file = true;
                }
                if (game_file)
                {
                    if (key == L"default.xbe")
                        fail("Replacing default.xbe requires recompilation and is not "
                             "supported by the mod loader.");
                    merged[key] = e.second;
                    if (fs::path(key).extension() == L".dsk")
                    {
                        read_dsk(e.second);
                        if (retail.count(key))
                            archives[key].push_back(e.second);
                    }
                }
                else
                {
                    if (key == L"mercenaries_recomp.exe" || key == L"mercenaries recompiled.exe" ||
                        key.rfind(L"saves\\", 0) == 0 || key.rfind(L"mod-saves\\", 0) == 0)
                        fail("Mods may not replace the game executable or include player "
                             "saves.");
                    native_mod = true;
                    host[key] = e.second;
                }
            }
            if (native_mod) settings_identity.name(m.name);
        }
    auto settings_key = settings_identity.finish();
    auto cache = installation / L"mod-cache" / fingerprint.finish();
    bounded(cache / L"namespace");
    retire_profiles(installation, cache);
    fs::create_directories(cache);
    // A profile lock prevents two launchers from rebuilding the same derived
    // files concurrently.
    HANDLE lock = CreateFileW((cache / L"build.lock").c_str(), GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (lock == INVALID_HANDLE_VALUE)
        fail("This mod selection is already being prepared. Try again when that "
             "launch completes.");
    struct Close
    {
        HANDLE h;
        ~Close() { CloseHandle(h); }
    } close{lock};
    if (!regular(cache / L"ready"))
    {
        for (auto &a : archives)
        {
            // One archive needs no transformation. Multiple archives are merged as
            // changes from vanilla.
            if (a.second.size() > 1)
            {
                auto output = cache / L"merged" / a.first;
                merge_dsk(retail.at(a.first), a.second, output);
                merged[a.first] = output;
            }
        }
        auto names = cache / L"namespace";
        fs::create_directories(names);
        std::ofstream manifest(cache / L"overlay.bin", std::ios::binary | std::ios::trunc);
        uint32_t magic = 0x31444f4d, count = (uint32_t)merged.size();
        manifest.write((char *)&magic, 4);
        manifest.write((char *)&count, 4);
        for (auto &e : merged)
        {
            auto stub = names / e.first;
            bounded(stub);
            fs::create_directories(stub.parent_path());
            std::ofstream placeholder(stub, std::ios::binary);
            auto target = fs::absolute(e.second).wstring();
            uint32_t a = (uint32_t)e.first.size(), b = (uint32_t)target.size();
            manifest.write((char *)&a, 4);
            manifest.write((char *)&b, 4);
            manifest.write((char *)e.first.data(), a * 2);
            manifest.write((char *)target.data(), b * 2);
            if (!placeholder)
                fail("Cannot create mod namespace.");
        }
        manifest.close();
        if (!manifest)
            fail("Cannot write mod overlay index.");
        std::ofstream ready(cache / L"ready");
        ready << "1\n";
        ready.close();
        if (!ready)
            fail("Cannot complete mod profile.");
    }
    // Native libraries live in the selected profile so Windows resolves proxy
    // imports before WinMain.
    auto runtime = cache / L"runtime";
    fs::create_directories(runtime);
    Files native;
    for (auto &e : fs::directory_iterator(installation))
        if (e.is_regular_file())
        {
            auto ext = lower(e.path().extension().wstring());
            auto name = lower(e.path().filename().wstring());
            if (name == L"mercenaries_recomp.exe" || ext == L".dll" || ext == L".ini")
                native[name] = e.path();
        }
    for (auto &e : files(installation / L"compat"))
        native[L"compat\\" + e.first] = e.second;
    for (auto &e : host)
        native[e.first] = e.second;
    for (auto &e : native)
        copy_file_into(e.second, runtime / e.first);
    // DSK packs can exceed the retail pools. Keep these opt-ins inside the
    // selected profile; a mod-supplied compatibility file may override them.
    if (!archives.empty() && !host.count(L"modcompatibility.ini") &&
        !regular(cache / L"settings-profile"))
    {
        const auto settings = runtime / L"modcompatibility.ini";
        if (!WritePrivateProfileStringW(L"Mods", L"extended_graphics_memory", L"1",
                                        settings.c_str()) ||
            !WritePrivateProfileStringW(L"Mods", L"expanded_world_properties", L"1",
                                        settings.c_str()))
            fail("Cannot configure memory capacity for the selected DSK mods.");
    }
    auto marker = cache / L"settings-profile";
    checked_tree(installation / L"mod-settings");
    auto settings_root = installation / L"mod-settings" / settings_key;
    auto defaults_root = installation / L"mod-settings/.defaults" / settings_key;
    if (!regular(marker))
    {
        for (const auto &e : files(runtime))
        {
            if (!setting_file(e.second)) continue;
            auto saved = settings_root / e.first;
            auto previous_default = defaults_root / e.first;
            bool unchanged = regular(saved) && regular(previous_default) &&
                             digest(saved) == digest(previous_default);
            fs::create_directories(saved.parent_path());
            if (!regular(saved) || unchanged)
                fs::copy_file(e.second, saved, fs::copy_options::overwrite_existing);
            fs::create_directories(previous_default.parent_path());
            fs::copy_file(e.second, previous_default, fs::copy_options::overwrite_existing);
        }
        std::wofstream output(marker);
        output << settings_key << L"\n";
        output.close();
        if (!output) fail("Cannot record the persistent mod settings profile.");
    }
    else
        preserve_settings(installation, cache);
    for (const auto &e : files(settings_root))
    {
        auto target = runtime / e.first;
        bounded(target);
        fs::create_directories(target.parent_path());
        if (fs::exists(target) && fs::equivalent(target, e.second)) continue;
        fs::remove(target);
        std::error_code error;
        fs::create_hard_link(e.second, target, error);
        // Some filesystems cannot link files. Also resynchronize on preparation
        // and retirement for mods that save by replacing the file atomically.
        if (error) fs::copy_file(e.second, target);
    }
    launch.executable = runtime / L"mercenaries_recomp.exe";
    launch.working = runtime;
    launch.cache = cache / L"storage";
    launch.manifest = cache / L"overlay.bin";
    if (s.separate_saves)
        launch.saves = installation / L"mod-saves/mercenaries";
    return launch;
}
void start(const Launch &l)
{
    // Build an explicit child environment; vanilla must not inherit a previous
    // overlay.
    std::map<std::wstring, std::wstring> env;
    wchar_t *block = GetEnvironmentStringsW();
    if (!block)
        fail("Cannot read launch environment.");
    for (auto p = block; *p; p += wcslen(p) + 1)
    {
        auto eq = wcschr(p + 1, L'=');
        if (eq)
            env[lower(std::wstring(p, eq))] = eq + 1;
    }
    FreeEnvironmentStringsW(block);
    env[L"mercenaries_game_dir"] = l.game.wstring();
    env[L"mercenaries_save_dir"] = l.saves.wstring();
    env[L"mercenaries_config_root"] = l.config.wstring();
    env[L"mercenaries_mod_overlay"] = l.manifest.wstring();
    env[L"mercenaries_mod_cache"] = l.cache.wstring();
    env[L"mercenaries_mod_bootstrapped"] = L"1";
    std::vector<wchar_t> bytes;
    for (auto &v : env)
    {
        auto line = v.first + L"=" + v.second;
        bytes.insert(bytes.end(), line.begin(), line.end());
        bytes.push_back(0);
    }
    bytes.push_back(0);
    auto command = quoted(l.executable) + L" " + quoted(l.game);
    std::vector<wchar_t> cmd(command.begin(), command.end());
    cmd.push_back(0);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<unsigned char> attribute_storage(attribute_size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attribute_size))
        fail("Cannot initialize game launch attributes.");
    struct Attributes {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~Attributes() { DeleteProcThreadAttributeList(value); }
    } attributes{si.lpAttributeList};
    HANDLE lease = l.lease.get();
    if (!lease || !UpdateProcThreadAttribute(si.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &lease, sizeof(lease), nullptr, nullptr))
        fail("Cannot protect the active mod cache.");
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(l.executable.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, bytes.data(),
                        l.working.c_str(), &si.StartupInfo, &pi))
        fail("The game could not start. Check the selected DLL mods and their "
             "dependencies.");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}
} // namespace mercmods
extern "C" BOOL mercenaries_mod_launch(HWND owner, const wchar_t *installation, const wchar_t *game)
{
    try
    {
        auto s = mercmods::discover(installation);
        if (!s.mods.empty())
            return mercmods::selector(owner, installation, game, std::move(s));
        mercmods::start(mercmods::prepare(installation, game, s, true));
        return TRUE;
    }
    catch (const std::exception &e)
    {
        std::string text = e.what();
        MessageBoxA(owner, text.c_str(), "Mercenaries Mod Loader", MB_OK | MB_ICONERROR);
        return FALSE;
    }
}
