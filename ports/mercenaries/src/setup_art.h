#ifndef MERCENARIES_SETUP_ART_H
#define MERCENARIES_SETUP_ART_H
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SETUP_DESIGN_WIDTH 2962
#define SETUP_DESIGN_HEIGHT 1796
#define SETUP_HEADER_HEIGHT 118
typedef struct setup_view {
    const wchar_t *headline;
    const wchar_t *button;
    unsigned progress;
    BOOL busy, completed, cancelling;
} setup_view;
BOOL setup_art_init(HINSTANCE instance);
void setup_art_shutdown(void);
void setup_art_paint(HDC dc, int width, int height, const setup_view *view);
void setup_art_button(HDC dc, int width, int height, const setup_view *view,
                      BOOL close_button, UINT state);
RECT setup_art_rect(int width, int height, BOOL close_button);
BOOL setup_art_export(const wchar_t *path, int width, int height, const setup_view *view);
#ifdef __cplusplus
}
#endif
#endif
