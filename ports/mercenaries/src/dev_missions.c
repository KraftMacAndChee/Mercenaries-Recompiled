/* Developer commands execute on the guest thread, never in a window callback. */
#include "dev_missions.h"
#include "dev_menu.h"
#include <stdio.h>
#include <stdlib.h>

static volatile LONG pending_command;
static volatile LONG current_province = -1;

/* Asset numbers from the original mission arcs, not sequence numbers. */
static const unsigned allied_missions[2][6] = {
    {1, 3, 4, 5, 6, 8}, {1, 3, 4, 2, 8, 0}
};
const char *recomp_dev_mission_faction(unsigned faction)
{
    static const char *names[] = {"allies", "china", "mafia", "sk"};
    return faction < 4 ? names[faction] : NULL;
}
unsigned recomp_dev_mission_count(int province, unsigned faction)
{
    if (province < 0 || province > 1 || faction >= 4) return 0;
    return faction == 0 && province == 1 ? 5 : 6;
}
unsigned recomp_dev_mission_number(int province, unsigned faction, unsigned row)
{
    if (row >= recomp_dev_mission_count(province, faction)) return 0;
    return faction == 0 ? allied_missions[province][row] : row + 1;
}
int recomp_dev_current_province(void)
{
    return (int)InterlockedCompareExchange(&current_province, 0, 0);
}
void recomp_dev_missions_set_province(int province)
{
    InterlockedExchange(&current_province, province);
}
unsigned recomp_dev_missions_take(void)
{
    /* Private regression input is disabled in ordinary interactive runs. */
    static int checked;
    static const char *path;
    static unsigned sequence;
    static ULONGLONG last_poll;
    if (!checked) {
        checked = 1;
        if (getenv("MERCENARIES_TEST_ISOLATE_INPUT"))
            path = getenv("MERCENARIES_TEST_TRANSITION_FILE");
    }
    if (path && recomp_dev_menu_allowed() && GetTickCount64() - last_poll >= 250) {
        last_poll = GetTickCount64();
        FILE *file = fopen(path, "r");
        if (file) {
            unsigned next, command;
            if (fscanf(file, "%u %x", &next, &command) == 2 && next > sequence) {
                sequence = next;
                InterlockedCompareExchange(&pending_command, (LONG)command, 0);
            }
            fclose(file);
        }
    }
    return (unsigned)InterlockedExchange(&pending_command, 0);
}
static int queue(unsigned command)
{
    return recomp_dev_menu_allowed() &&
        InterlockedCompareExchange(&pending_command, (LONG)command, 0) == 0;
}
int recomp_dev_request_mission(unsigned faction, unsigned number)
{
    int province = recomp_dev_current_province();
    for (unsigned i = 0; i < recomp_dev_mission_count(province, faction); ++i)
        if (number == recomp_dev_mission_number(province, faction, i))
            return queue(((unsigned)(province + 1) << 8) | (faction << 4) | number);
    return 0;
}
int recomp_dev_request_travel(unsigned province)
{
    int current = recomp_dev_current_province();
    if (province > 1 || current < 0 || province == (unsigned)current) return 0;
    return queue(0x400u | province);
}
int recomp_dev_mission_script(unsigned command, char *output, size_t size)
{
    int length;
    if (command == 0x400u || command == 0x401u) {
        const char *from = command == 0x400u ? "nw" : "sw";
        length = snprintf(output, size,
            "assert(Utility_ReadStringFromScribbleMemory('SkipTo_Map')=='%s',"
            "'Province changed; choose travel again') "
            "assert(not Utility_ReadNumberFromScribbleMemory('mission_accepted'),"
            "'Finish or cancel the current contract before province travel') "
            "assert(type(TeleportBetweenQuadrants)=='function','Province travel unavailable') "
            "TeleportBetweenQuadrants()", from);
    } else {
        unsigned province = (command >> 8) - 1u;
        unsigned faction = (command >> 4) & 15u, number = command & 15u;
        int valid = 0;
        if (command < 0x100u || command >= 0x300u) return 0;
        for (unsigned i = 0; i < recomp_dev_mission_count((int)province, faction); ++i)
            if (number == recomp_dev_mission_number((int)province, faction, i)) valid = 1;
        if (!valid) return 0;
        length = snprintf(output, size,
            "assert(Utility_ReadStringFromScribbleMemory('SkipTo_Map')=='%s',"
            "'Province changed; choose the mission again') "
            "assert(type(DebugSkipToMission)=='function','Mission selection unavailable') "
            "DebugSkipToMission('%s',%u)",
            province == 0 ? "sw" : "nw", recomp_dev_mission_faction(faction), number);
    }
    return length >= 0 && (size_t)length < size;
}
