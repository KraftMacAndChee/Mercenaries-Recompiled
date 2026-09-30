#include "recomp_controls.h"
#include "recomp_options.h"
#include "recomp/recomp_types.h"
#include <stdint.h>
#include <string.h>
#define MENU_ITEM_VTABLE 0x002E9028u
#define MENU_MAIN_HASH 0xB5196F2Fu
#define MENU_PAUSE_HASH 0x18447439u
#define MENU_SHELL_OPTIONS_HASH 0x941EE5E8u
#define MENU_PAUSE_OPTIONS_HASH 0xA20CA488u
#define MENU_BACK_HASH 0x7954FE35u
#define MENU_OPTIONS_ID 14u
#define MENU_NONE_ID 0xFFFFFFFFu
#define OPTIONS_ITEM_COUNT 13u
#define SYNTHETIC_CAPACITY 16u
extern uint32_t recomp_title_heap_allocate(uint32_t size);
extern void recomp_title_heap_free(uint32_t address);
typedef struct options_menu_state {
    uint32_t menu, original_items[16], original_count, original_current, original_name;
    uint8_t original_has_back;
    uint32_t synthetic_items[SYNTHETIC_CAPACITY];
    int active, controls, acknowledgements;
} options_menu_state;
static options_menu_state g_menu;
int recomp_acknowledgements_active(void){return g_menu.active && g_menu.acknowledgements;}
static uint8_t *guest(uint32_t address) { return (uint8_t *)XBOX_PTR(address); }
static uint32_t u32(uint32_t address) { return *(volatile uint32_t *)guest(address); }
static uint8_t u8(uint32_t address) { return *(volatile uint8_t *)guest(address); }
static void w32(uint32_t address, uint32_t value) { *(volatile uint32_t *)guest(address) = value; }
static int valid_menu(uint32_t menu) { return menu >= 0x00010000u && menu <= 0x03FFFF80u && u32(menu + 0x44u) <= 16u; }
static uint32_t item_hash(uint32_t menu, uint32_t index) {
    uint32_t item;
    if (!valid_menu(menu) || index >= u32(menu + 0x44u)) return 0u;
    item = u32(menu + 4u + index * 4u);
    return item >= 0x00010000u && item <= 0x03FFFFECu ? u32(item + 4u) : 0u;
}
static uint32_t allocate_item(uint32_t hash, uint32_t child) {
    const uint32_t item = recomp_title_heap_allocate(0x14u);
    if (!item) return 0u;
    w32(item, MENU_ITEM_VTABLE); w32(item + 4u, hash); w32(item + 8u, child);
    w32(item + 0xCu, 0u); w32(item + 0x10u, 0x82A09C0Fu); return item;
}
static void insert_item_before(uint32_t menu, uint32_t hash, uint32_t before) {
    uint32_t count=u32(menu+0x44u), position=count;
    for(uint32_t i=0;i<count;++i) {
        if(item_hash(menu,i)==hash)return;
        if(item_hash(menu,i)==before)position=i;
    }
    if(count>=16u)return;
    uint32_t item=allocate_item(hash,MENU_OPTIONS_ID);
    if(!item)return;
    for(uint32_t i=count;i>position;--i)
        w32(menu+4u+i*4u,u32(menu+4u+(i-1u)*4u));
    w32(menu+4u+position*4u,item);w32(menu+0x44u,count+1u);
}
static void insert_entry(uint32_t menu) {
    if(!valid_menu(menu))return;
    /* Existing pause Controls and the new shell entry share the Options class. */
    for(uint32_t i=0;i<u32(menu+0x44u);++i)
        if(item_hash(menu,i)==0x8EAD97C8u)w32(u32(menu+4u+i*4u)+8u,MENU_OPTIONS_ID);
    uint32_t name=u32(menu+0x54u);
    if(name==MENU_MAIN_HASH) {
        insert_item_before(menu,0x8EAD97C8u,MENU_SHELL_OPTIONS_HASH);
        insert_item_before(menu,RECOMP_OPTIONS_MENU_HASH,MENU_SHELL_OPTIONS_HASH);
        insert_item_before(menu,RECOMP_ACK_MENU_HASH,0u);
        insert_item_before(menu,RECOMP_QUIT_HASH,0u);
    } else if(name==MENU_PAUSE_HASH) {
        insert_item_before(menu,RECOMP_OPTIONS_MENU_HASH,0x00DFD9ADu);
    }
}
static void restore_menu(void) {
    if (!g_menu.active || !valid_menu(g_menu.menu)) return;
    recomp_options_cancel_edit();
    if(g_menu.controls)recomp_controls_close();
    for (uint32_t i = 0; i < g_menu.original_count; ++i) w32(g_menu.menu + 4u + i * 4u, g_menu.original_items[i]);
    w32(g_menu.menu + 0x44u, g_menu.original_count); w32(g_menu.menu + 0x48u, g_menu.original_current);
    w32(g_menu.menu + 0x54u, g_menu.original_name); *(volatile uint8_t *)guest(g_menu.menu + 0x5Cu) = g_menu.original_has_back;
    for (uint32_t i = 0; i < SYNTHETIC_CAPACITY; ++i) if (g_menu.synthetic_items[i]) recomp_title_heap_free(g_menu.synthetic_items[i]);
    memset(&g_menu, 0, sizeof(g_menu));
}
static void activate_menu(uint32_t menu, int mode) {
    static const uint32_t hashes[OPTIONS_ITEM_COUNT] = { RECOMP_OPTIONS_FPS_HASH, RECOMP_OPTIONS_ASPECT_HASH,
        RECOMP_OPTIONS_WAKE_HASH, RECOMP_OPTIONS_NPC_LOD_HASH,
        RECOMP_OPTIONS_RESOLUTION_HASH, RECOMP_OPTIONS_AF_HASH,
        RECOMP_OPTIONS_DISPLAY_HASH, RECOMP_OPTIONS_HAZE_HASH,
        RECOMP_OPTIONS_FIXED_XBOX_HASH, RECOMP_OPTIONS_OG_BUGS_HASH, RECOMP_OPTIONS_PS2_UPGRADES_HASH, RECOMP_OPTIONS_APPLY_HASH, MENU_BACK_HASH };
    int controls=mode==1,acknowledgements=mode==2;
    uint32_t count;
    if (g_menu.active || !valid_menu(menu)) return;
    count = u32(menu + 0x44u); g_menu.menu = menu; g_menu.original_count = count;
    g_menu.original_current = u32(menu + 0x48u); g_menu.original_name = u32(menu + 0x54u); g_menu.original_has_back = u8(menu + 0x5Cu);
    for (uint32_t i = 0; i < count; ++i) g_menu.original_items[i] = u32(menu + 4u + i * 4u);
    const uint32_t item_count = acknowledgements ? RECOMP_ACK_ROWS : controls ? RECOMP_CONTROLS_ROWS : OPTIONS_ITEM_COUNT;
    for (uint32_t i = 0; i < item_count; ++i) {
        g_menu.synthetic_items[i] = allocate_item(acknowledgements ? RECOMP_ACK_ROW+i : controls ? RECOMP_CONTROLS_ROW+i : hashes[i], MENU_NONE_ID);
        if (!g_menu.synthetic_items[i]) { for (uint32_t j = 0; j < i; ++j) recomp_title_heap_free(g_menu.synthetic_items[j]); memset(&g_menu, 0, sizeof(g_menu)); return; }
    }
    for (uint32_t i = 0; i < item_count; ++i) w32(menu + 4u + i * 4u, g_menu.synthetic_items[i]);
    w32(menu + 0x44u, item_count); w32(menu + 0x48u, acknowledgements ? RECOMP_ACK_ROWS-1u : 0u); w32(menu + 0x54u, acknowledgements ? RECOMP_ACK_MENU_HASH : controls ? RECOMP_CONTROLS_TITLE : RECOMP_OPTIONS_MENU_HASH);
    *(volatile uint8_t *)guest(menu + 0x5Cu) = 1u; recomp_options_begin_edit(); g_menu.active = 1;g_menu.controls=controls;g_menu.acknowledgements=acknowledgements;
    if(controls){recomp_controls_open();w32(menu+0x44u,recomp_controls_count());}
}
void recomp_options_menu_transition(uint32_t owner, uint32_t next) {
    const uint32_t previous = u32(owner + 0x3EB8u);
    const uint32_t selected = valid_menu(previous) ? item_hash(previous, u32(previous + 0x48u)) : 0u;
    if (g_menu.active && previous == g_menu.menu && next != previous) restore_menu();
    if ((selected == RECOMP_OPTIONS_MENU_HASH || selected == RECOMP_ACK_MENU_HASH || selected == 0x8EAD97C8u) && valid_menu(next)) {
        const uint32_t name = u32(next + 0x54u);
        if (name == MENU_SHELL_OPTIONS_HASH || name == MENU_PAUSE_OPTIONS_HASH) activate_menu(next,selected==RECOMP_ACK_MENU_HASH ? 2 : selected==0x8EAD97C8u);
    }
    insert_entry(next);
}
uint32_t recomp_options_input_checkpoint(uint32_t menu, uint32_t input, uint32_t event) {
    uint32_t selected;
    if(valid_menu(menu) && u32(menu+0x54u)==MENU_MAIN_HASH &&
       item_hash(menu,u32(menu+0x48u))==RECOMP_QUIT_HASH && input==5u && event==1u){
        /* Same orderly host exit route as closing the game window. */
        PostQuitMessage(0);return 1u;
    }
    if (!g_menu.active || menu != g_menu.menu || !valid_menu(menu)) return 0u;
    if(g_menu.acknowledgements){
        w32(menu+0x48u,RECOMP_ACK_ROWS-1u);
        if(input==5u && event==1u)return 2u;
        if(input==4u)return 0u; /* authored Back handling and custom binding */
        return 1u; /* static credits, only Back is interactive */
    }
    if(g_menu.controls){
        unsigned row=u32(menu+0x48u),before=recomp_controls_count();
        uint32_t result=recomp_controls_input(row,input,event);
        unsigned after=recomp_controls_count();
        if(before!=after){w32(menu+0x44u,after);w32(menu+0x48u,0u);}
        return result;
    }
    selected = item_hash(menu, u32(menu + 0x48u));
    /* Match the retail option handlers: mutate once on the pressed event.
     * Event 4 is the later repeat/release notification for the same physical
     * pulse and accepting both advances every selector twice. */
    if ((input == 2u || input == 3u) && event == 1u) return recomp_options_adjust(selected, input == 2u ? -1 : 1) ? 1u : 0u;
    if (input == 5u && event == 1u) {
        /* Ask the generated checkpoint to translate A on the synthetic Back
         * row into retail input 4 (B/cancel).  The original handler then runs
         * its complete close/transition path instead of us manufacturing one
         * of its internal state bytes here. */
        if (selected == MENU_BACK_HASH) return 2u;
        if (selected == RECOMP_OPTIONS_APPLY_HASH) {
            recomp_options_apply();
            return 1u;
        }
        return recomp_options_adjust(selected, 1) ? 1u : 0u;
    }
    return 0u;
}
void recomp_options_prepare_menu_paint(uint32_t menu, uint32_t brush) {
    if (!g_menu.active || menu != g_menu.menu || !valid_menu(menu) ||
        brush < 0x00010000u || brush > 0x03FFFF20u) return;
    /* Retail BrushMenu initializes fade offsets only while opening, for
     * _nItems + 1 entries. Controls -> Rebind grows 5 rows to 9 in place;
     * rows 6 onward can retain FLT_MAX and never paint. Recompute the same
     * retail fade schedule for this custom menu, including its end sentinel.
     * Keep the opening/closing animation and its timer untouched. */
    if (u32(brush + 0x34u) != 3u) return; /* eStateUpdate */
    const uint32_t count = u32(brush + 0x3Cu);
    if (count != u32(menu + 0x44u) || count > SYNTHETIC_CAPACITY) return;
    float per;
    memcpy(&per, guest(brush + 0xC8u), sizeof(per));
    for (uint32_t i = 0; i <= count; ++i) {
        float offset = (float)i * per;
        memcpy(guest(brush + 0x80u + i * 4u), &offset, sizeof(offset));
    }
}
void recomp_options_replace_label(uint32_t hash, uint32_t buffer) {
    const char *label = recomp_controls_label(hash); size_t length;
    if(!label)label=recomp_options_label(hash);
    if (!label || buffer < 0x00010000u || buffer >= 0x03FFFF80u) return;
    length = strlen(label); if (length > 120u) length = 120u;
    memcpy(guest(buffer), label, length); guest(buffer)[length] = 0u;
}

/* BeginText has already applied canvas scale. Shrink only the static credits
 * body; the next BeginText resets these globals for every subsequent draw. */
void recomp_ack_text_scale(uint32_t brush,uint32_t row,int foreground){
    if(!recomp_acknowledgements_active() || row>=RECOMP_ACK_ROWS-1u ||
       brush<0x10000u || brush>0x03FFFF20u || MEM32(brush+0x40u+row*4u)!=RECOMP_ACK_ROW+row)return;
    MEMF(0x7AB600u)*=.60f;MEMF(0x7AB5FCu)*=.60f;
    if(foreground){MEM32(0x7AB5F0u)=0x80FFFFFFu;uint32_t primitive=MEM32(0x7AC818u);
        if(primitive>=0x10000u && primitive<0x03FFFFE4u)MEM32(primitive+0xCu)=0x80FFFFFFu;}

}
