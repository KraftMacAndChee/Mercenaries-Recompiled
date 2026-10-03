#ifndef MERCENARIES_MOD_LOADER_H
#define MERCENARIES_MOD_LOADER_H
#include <windows.h>
#ifdef __cplusplus
extern "C"
{
#endif
    BOOL mercenaries_mod_launch(HWND owner, const wchar_t *installation, const wchar_t *game);
    BOOL mercenaries_mod_preview(const wchar_t *output);
#ifdef __cplusplus
}
#include <filesystem>
#include <string>
#include <memory>
#include <vector>
namespace mercmods
{
namespace fs = std::filesystem;
struct Mod
{
    std::wstring name;
    fs::path root;
    bool enabled = false;
};
struct Selection
{
    std::vector<Mod> mods;
    bool separate_saves = true;
};
struct Launch
{
    fs::path executable, working, game, saves, cache, manifest, config;
    std::shared_ptr<void> lease;
};
Selection discover(const fs::path &installation);
void save_selection(const fs::path &installation, const Selection &selection);
Launch prepare(const fs::path &installation, const fs::path &game, const Selection &selection,
               bool vanilla);
void start(const Launch &launch);
BOOL selector(HWND owner, const fs::path &installation, const fs::path &game, Selection selection);
} // namespace mercmods
#endif
#endif
