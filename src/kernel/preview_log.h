#ifndef XBOX_PREVIEW_LOG_H
#define XBOX_PREVIEW_LOG_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
/* Optional runtime diagnostics. Producers never wait for disk or queue space. */
int xbox_preview_log_init(const wchar_t *directory);
int xbox_preview_log_enabled(void);
void xbox_preview_log_event(const char *category, const char *format, ...);
/* Distinct timing samples retain the shared 64/sec and queue limits, but do
 * not coalesce different operations that use the same format string. */
void xbox_preview_log_sample(const char *category, const char *format, ...);
void xbox_preview_log_eventv(const char *category, const char *format, va_list args);
/* Main guest thread: bounded RAM context, appended by the crash writer. */
void xbox_preview_log_set_crash_context(const char *text);
void xbox_preview_log_crash(uint32_t code, uintptr_t pc, uint32_t guest_pc,
                            uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t esp);
void xbox_preview_log_shutdown(void);
#endif
