#ifndef MERCENARIES_DEV_MISSIONS_H
#define MERCENARIES_DEV_MISSIONS_H
#include <stdint.h>
#include <stddef.h>

/* Province indices: 0 south, 1 north; -1 outside a normal province. */
int recomp_dev_current_province(void);
const char *recomp_dev_mission_faction(unsigned faction);
unsigned recomp_dev_mission_count(int province, unsigned faction);
unsigned recomp_dev_mission_number(int province, unsigned faction, unsigned row);
int recomp_dev_request_mission(unsigned faction, unsigned number);
int recomp_dev_request_travel(unsigned province);
int recomp_dev_mission_script(unsigned command, char *output, size_t size);
void recomp_dev_missions_tick(void);
void recomp_dev_transition_result(int success, const char *message);
#endif
