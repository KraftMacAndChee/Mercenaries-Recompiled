/* Contract return locations are distinct from manual-save respawn locations.
 * In retail, SaveGameLocation survives loading and can override the next map's
 * arrival point after mission_accepted is erased. Keep this correction limited
 * to Ace of Clubs completion and explicit province travel; never rewrite saves. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t pending_location;

static uint32_t location_hash(const char *s)
{
    uint32_t hash=2166136261u;
    while(*s){hash^=((unsigned char)*s++|0x20u);hash*=16777619u;}
    return hash;
}

void recomp_chapter_return_movie(uint32_t movie,uint32_t next_map)
{
    static const char *const endings[]={
        "CH1_C_UN","CH1_K_UN","CH1_C_CH","CH1_K_CH",
        "CH1_C_RM","CH1_K_RM","CH1_C_SK","CH1_K_SK"
    };
    pending_location=0;
    if(next_map!=0x4a5220afu)return; /* sw */
    for(unsigned i=0;i<sizeof(endings)/sizeof(endings[0]);++i){
        if(movie==location_hash(endings[i])){
            pending_location=location_hash("loc_hq-allies-respawn");
            fprintf(stderr,"[CHAPTER-RETURN] Ace of Clubs -> AN HQ (movie=%08X)\n",movie);
            break;
        }
    }
}

/* UINT32_MAX means retain the map's authored player start. The north/south
 * travel script resets the base layer after saving player inventory. Its old
 * SaveGameLocation must not override the new map's default Allied arrival. */
void recomp_chapter_return_province(uint32_t from_map, uint32_t to_map)
{
    const uint32_t sw=location_hash("sw"), nw=location_hash("nw");
    pending_location=0;
    if ((from_map==sw && to_map==nw) || (from_map==nw && to_map==sw)) {
        pending_location=UINT32_MAX;
        fprintf(stderr,"[PROVINCE-RETURN] %08X -> %08X: retain authored arrival\n",from_map,to_map);
    }
}

uint32_t recomp_chapter_return_consume(void)
{
    /* Isolated arrival replay: exercise the real map/spore placement code
     * without editing saves or fabricating mission completion. No effect in
     * ordinary runs; both diagnostic switches are required. */
    static int test_checked;
    if(!test_checked){
        test_checked=1;
        if(getenv("MERCENARIES_TEST_ISOLATE_INPUT") && getenv("MERCENARIES_TEST_ACE_RETURN"))
            recomp_chapter_return_movie(location_hash("CH1_C_UN"),0x4a5220afu);
    }
    uint32_t location=pending_location;
    pending_location=0;
    return location;
}
