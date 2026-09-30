/* Optional assets are embedded in the executable. Retail banks and textures
 * remain owned by the game and are never modified or written back to disk. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "ps2_upgrades.h"
#include "ps2_retail_mix.h"
#include "recomp_options.h"
#include <stddef.h>
extern ptrdiff_t g_xbox_mem_offset;
#include "preview_log.h"
extern uint32_t recomp_title_heap_allocate(uint32_t size);
extern int pgraph_d3d11_register_world_texture(uint32_t, uint32_t, uint32_t, const uint32_t *);
static int valid(uint32_t p, uint32_t n) { return n <= 0x04000000u && p >= 0x10000u && p <= 0x04000000u - n; }
static uint8_t *guest(uint32_t p) { return (uint8_t *)((uintptr_t)p + g_xbox_mem_offset); }
static uint32_t read32(uint32_t p) { uint32_t v; memcpy(&v, guest(p), 4); return v; }
static uint16_t read16(uint32_t p) { uint16_t v; memcpy(&v, guest(p), 2); return v; }
static void write32(uint32_t p, uint32_t v) { memcpy(guest(p), &v, 4); }
static const uint8_t *resource(unsigned id, uint32_t *size) {
    HMODULE module = GetModuleHandleW(NULL);
    HRSRC r = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!r) return NULL;
    *size = SizeofResource(module, r);
    return LockResource(LoadResource(module, r));
}
typedef struct ReplacementWave { uint32_t data, bytes, rate; } ReplacementWave;
static ReplacementWave waves[4];
static int load_wave(unsigned index) {
    uint32_t size = 0, pos, bytes = 0, rate = 0;
    const uint8_t *data = NULL, *file;
    uint16_t format = 0, channels = 0, bits = 0;
    if (waves[index].data) return 1;
    file = resource(201 + index, &size);
    if (!file || size < 12 || memcmp(file, "RIFF", 4) || memcmp(file + 8, "WAVE", 4)) return 0;
    for (pos = 12; pos <= size - 8;) {
        uint32_t count; memcpy(&count, file + pos + 4, 4);
        if (count > size - pos - 8) return 0;
        if (!memcmp(file + pos, "fmt ", 4) && count >= 16) {
            memcpy(&format, file + pos + 8, 2); memcpy(&channels, file + pos + 10, 2);
            memcpy(&rate, file + pos + 12, 4); memcpy(&bits, file + pos + 22, 2);
        } else if (!memcmp(file + pos, "data", 4)) { data = file + pos + 8; bytes = count; }
        pos += 8 + count;
        if (pos < size && (count & 1)) ++pos;
    }
    if (!data || !bytes || bytes > 1048576 || (bytes & 1) || format != 1 || channels != 1 || bits != 16 || rate < 8000 || rate > 48000) return 0;
    uint32_t allocation = recomp_title_heap_allocate(bytes);
    if (!allocation) return 0;
    memcpy(guest(allocation), data, bytes);
    waves[index].data = allocation; waves[index].bytes = bytes; waves[index].rate = rate;
    return 1;
}
static int name_is(uint32_t address, const char *name) {
    size_t n = strlen(name) + 1;
    return valid(address, (uint32_t)n) && !memcmp(guest(address), name, n);
}
/* XACT v11 banks contain separate cue and sound tables followed by relative
 * offsets. Add a private cue instead of changing a live retail sound: voices
 * already playing keep their original definition when the option changes.
 * Only the two verified retail banks are extended. Modded banks pass through. */
typedef struct UpgradeBank {
    const char *name;
    uint32_t retail_size, fingerprint, data, size;
    uint16_t retail_cues, fire_cue;
} UpgradeBank;
static UpgradeBank upgrade_banks[2] = {
    {"w_autoca", 400, 0xe840eab6u, 0, 0, 3, 2},
    {"w_dragun", 608, 0x5aeecfaau, 0, 0, 4, 2}
};
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v,p,2); return v; }
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v,p,4); return v; }
static void put16(uint8_t *p, uint16_t v) { memcpy(p,&v,2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p,&v,4); }
static void put_float(uint8_t *p, float v) { memcpy(p,&v,4); }
static uint32_t fingerprint(const uint8_t *p, uint32_t size) {
    uint32_t hash=2166136261u;
    for (uint32_t i=0;i<size;++i) hash=(hash^p[i])*16777619u;
    return hash;
}
/* Event timestamps and track offsets use 24 bits. Pitch uses 4096 units per
 * octave; volume is in hundredths of a decibel. These records reproduce the
 * PS2 firing tracks, using the existing Xbox wave-bank indices. */
static uint32_t envelope(uint8_t *out, unsigned type, uint32_t time,
                         int low, int high, uint32_t duration) {
    put32(out, type | (time<<8));
    out[4]=8;
    out[5]=(uint8_t)(low==high ? (type==4 ? 0x14 : 0x10) : (type==4 ? 0x14 : 0x30));
    put16(out+6, low==high ? 10 : (type==4 ? 10 : 30));
    put16(out+8,(uint16_t)low); put16(out+10,(uint16_t)high);
    put32(out+12,1 | (duration<<8));
    return 16;
}
static uint32_t play_wave(uint8_t *out, uint32_t time, unsigned wave, unsigned bank) {
    put32(out,time<<8); out[4]=4; out[5]=0; put16(out+6,0);
    put16(out+8,(uint16_t)wave); put16(out+10,(uint16_t)bank);
    return 12;
}
static void track(uint8_t *out, uint32_t table, unsigned index, unsigned count, uint32_t offset) {
    put32(out+table+index*4,count | (offset<<8));
}
static uint32_t extend_bank(uint8_t *out, const uint8_t *in, unsigned kind) {
    const UpgradeBank *bank=&upgrade_banks[kind];
    const uint32_t cues=bank->retail_cues, sounds=u16(in+28);
    const uint32_t old_sounds=56+cues*20, body=old_sounds+sounds*20;
    const uint32_t new_sounds=old_sounds+20, extra_sound=new_sounds+sounds*20;
    const uint32_t extra_cue=old_sounds;
    memcpy(out,in,old_sounds);
    memcpy(out+new_sounds,in+old_sounds,sounds*20);
    memcpy(out+body+40,in+body,bank->retail_size-body);
    put16(out+28,(uint16_t)(sounds+1)); put16(out+30,(uint16_t)(cues+1));
    for (unsigned i=8;i<=20;i+=4) put32(out+i,u32(in+i)+40);
    for (unsigned i=0;i<cues;++i) {
        uint8_t *cue=out+56+i*20;
        put32(cue+4,u32(cue+4)+40);
    }
    for (unsigned i=0;i<sounds;++i) {
        uint8_t *sound=out+new_sounds+i*20;
        if (sound[11]&0x18) continue; /* Direct wave: no track table. */
        uint32_t table=u32(sound)+40;
        put32(sound,table);
        for (unsigned j=0;j<sound[8];++j) {
            uint32_t rec=u32(out+table+j*4), event=(rec>>8)+40;
            put32(out+table+j*4,(rec&255)|(event<<8));
            for (unsigned k=0;k<(rec&255);++k) {
                if ((out[event]==0 || out[event]==1) && (out[event+5]&4))
                    put32(out+event+8,u32(out+event+8)+40);
                event+=8+out[event+4];
            }
        }
    }
    memcpy(out+extra_cue,out+56+bank->fire_cue*20,20);
    put16(out+extra_cue+2,(uint16_t)sounds);
    memcpy(out+extra_sound,out+new_sounds+u16(in+56+bank->fire_cue*20+2)*20,20);
    uint32_t end=bank->retail_size+40;
    static const char name[]="__ps2_fire";
    put32(out+extra_cue+4,end); memcpy(out+end,name,sizeof(name)); end+=sizeof(name);
    uint32_t hash=0;
    for (unsigned i=0;i<sizeof(name)-1;++i) hash=hash*3+(hash>>1)+(uint8_t)name[i];
    put16(out+extra_cue+14,(uint16_t)(hash%u16(in+26)));
    end=(end+3)&~3u;
    const unsigned tracks=kind==0 ? 2 : 4;
    const uint32_t table=end; end+=tracks*4;
    put32(out+extra_sound,table); out[extra_sound+8]=(uint8_t)tracks;
    /* The source PS2 sound attenuation is 25 m / rolloff 1 for Dragunov. */
    if (kind==1) {
        uint32_t spatial=u32(out+16)+u16(out+extra_sound+12)*40;
        memcpy(out+end,out+spatial,40);
        put_float(out+end+8,PS2_MIX_DRAG_MIN_DISTANCE); put_float(out+end+20,PS2_MIX_DRAG_ROLLOFF);
        /* Parameters are indexed in 40-byte records from the spatial table.
         * Append the private record at a matching stride. */
        uint32_t aligned=u32(out+16)+((end-u32(out+16)+39)/40)*40;
        memmove(out+aligned,out+end,40);
        put16(out+extra_sound+12,(uint16_t)((aligned-u32(out+16))/40)); end=aligned+40;
    }
    if (kind==0) {
        /* XACT stores sound gain in 0.16 dB steps (PS2 authoring: -2.50 dB). */
        put16(out+extra_sound+4,(u16(out+extra_sound+4)&0xfe00u)|PS2_MIX_AUTO_GAIN_CODE);
        track(out,table,0,2,end);
        end+=envelope(out+end,5,PS2_MIX_AUTO_OLD_VOLUME_TIME,PS2_MIX_AUTO_OLD_VOLUME_LOW,PS2_MIX_AUTO_OLD_VOLUME_HIGH,PS2_MIX_AUTO_OLD_VOLUME_DURATION);
        /* Original PS2 keeps the old loop sample as an attenuated one-shot. */
        uint32_t original_table=u32(out+new_sounds+u16(in+56+bank->fire_cue*20+2)*20);
        uint32_t original_event=u32(out+original_table)>>8;
        memcpy(out+end,out+original_event,24); put16(out+end+6,0);
        put16(out+end+14,PS2_MIX_AUTO_OLD_PLAY_PITCH_HIGH); end+=24;
        track(out,table,1,1,end);
        end+=play_wave(out+end,0,42,1);
    } else {
        uint32_t original_table=u32(out+new_sounds+2*20);
        uint32_t first=u32(out+original_table)>>8;
        track(out,table,0,3,end);
        /* Preserve the original randomized Dragunov/KSVK attack layer. */
        memcpy(out+end,out+first,44);
        put16(out+end+10,(uint16_t)PS2_MIX_DRAG_ATTACK_PITCH_LOW); end+=44;
        track(out,table,1,3,end);
        end+=envelope(out+end,4,PS2_MIX_DRAG_AK_PITCH_TIME,PS2_MIX_DRAG_AK_PITCH_LOW,PS2_MIX_DRAG_AK_PITCH_HIGH,PS2_MIX_DRAG_AK_PITCH_DURATION);
        end+=envelope(out+end,5,PS2_MIX_DRAG_AK_VOLUME_TIME,PS2_MIX_DRAG_AK_VOLUME_LOW,PS2_MIX_DRAG_AK_VOLUME_HIGH,PS2_MIX_DRAG_AK_VOLUME_DURATION);
        end+=play_wave(out+end,PS2_MIX_DRAG_AK_PLAY_TIME,3,0);
        track(out,table,2,3,end);
        end+=envelope(out+end,4,PS2_MIX_DRAG_CANNON_PITCH_TIME,PS2_MIX_DRAG_CANNON_PITCH_LOW,PS2_MIX_DRAG_CANNON_PITCH_HIGH,PS2_MIX_DRAG_CANNON_PITCH_DURATION);
        end+=envelope(out+end,5,PS2_MIX_DRAG_CANNON_VOLUME_TIME,PS2_MIX_DRAG_CANNON_VOLUME_LOW,PS2_MIX_DRAG_CANNON_VOLUME_HIGH,PS2_MIX_DRAG_CANNON_VOLUME_DURATION);
        end+=play_wave(out+end,PS2_MIX_DRAG_CANNON_PLAY_TIME,42,1);
        track(out,table,3,2,end);
        end+=envelope(out+end,4,PS2_MIX_DRAG_RIFLE_PITCH_TIME,PS2_MIX_DRAG_RIFLE_PITCH_LOW,PS2_MIX_DRAG_RIFLE_PITCH_HIGH,PS2_MIX_DRAG_RIFLE_PITCH_DURATION);
        /* Existing resident entry is a format placeholder; only this private
         * fourth track is bound to the embedded rifle_shot1 PCM. */
        end+=play_wave(out+end,PS2_MIX_DRAG_RIFLE_PLAY_TIME,0,0);
    }
    put32(out+20,end);
    return end;
}
uint32_t recomp_ps2_sound_bank(uint32_t original, uint32_t size_address) {
    if (!valid(size_address,4) || !valid(original,56)) return original;
    for (unsigned i=0;i<2;++i) {
        UpgradeBank *bank=&upgrade_banks[i];
        if (read32(size_address)!=bank->retail_size || !valid(original,bank->retail_size) ||
            !name_is(original+40,bank->name) || fingerprint(guest(original),bank->retail_size)!=bank->fingerprint) continue;
        if (!bank->data) {
            uint8_t buffer[2048]={0};
            uint32_t size=extend_bank(buffer,guest(original),i);
            uint32_t data=recomp_title_heap_allocate(size);
            if (!data) return original;
            memcpy(guest(data),buffer,size); bank->data=data; bank->size=size;
        }
        write32(size_address,bank->size);
        return bank->data;
    }
    return original;
}
uint32_t recomp_ps2_cue_index(uint32_t bank, uint32_t index) {
    if (recomp_options_ps2_upgrades() && valid(bank,8))
        for (unsigned i=0;i<2;++i)
            if (upgrade_banks[i].data && read32(bank+4)==upgrade_banks[i].data && index==upgrade_banks[i].fire_cue)
                return upgrade_banks[i].retail_cues;
    return index;
}
/* Initial track preparation, before DirectSound chooses its format and buffer.
 * Scope by both the high-detail sound bank and its referenced retail wave bank;
 * NPCs use the base bank and cannot match these player firing entries. */
void recomp_ps2_wave(uint32_t sound, uint32_t event) {
    uint32_t cue, sound_bank, raw_sound_bank, wave_bank, header, index;
    int replacement = -1;
    if (!valid(sound, 0x50) || !valid(event, 0x88)) return;
    cue = read32(sound + 0x44);
    if (!valid(cue, 0x40)) return;
    sound_bank = read32(cue + 0x14);
    if (!valid(sound_bank, 8)) return;
    raw_sound_bank = read32(sound_bank + 4);
    wave_bank = read32(event + 0x74);
    if (!valid(wave_bank, 0x28)) return;
    header = read32(wave_bank + 0x1C);
    if (!valid(header, 40) || (read32(header) & 1)) return; /* No streamed-bank redirection. */
    index = read16(event + 0x56);
    if (raw_sound_bank == upgrade_banks[1].data && read32(sound + 0x10) ==
        raw_sound_bank + 56 + (upgrade_banks[1].retail_cues + 1 + 4) * 20) {
        uint32_t first = read32(sound + 0x34);
        if (event == first + 3 * 0x88 && name_is(header + 8, "w_dragun") && index == 0) replacement = 0;
        if (event == first + 2 * 0x88 && name_is(header + 8, "combat") && index == 42) replacement = 1;
    } else if (raw_sound_bank == upgrade_banks[0].data && read32(sound + 0x10) ==
        raw_sound_bank + 56 + (upgrade_banks[0].retail_cues + 1 + 3) * 20) {
        if (name_is(header + 8, "combat") && index == 42) replacement = 1;
    } else if (recomp_options_ps2_upgrades() && name_is(raw_sound_bank + 40, "w_20cann") && name_is(header + 8, "combat") && index == 42)
        replacement = 1;
    if (replacement < 0 || !load_wave((unsigned)replacement)) return;
    const ReplacementWave *w = &waves[replacement];
    write32(event + 0x60, 0x80000004u | (w->rate << 5)); /* PCM16 mono mini-format. */
    write32(event + 0x64, w->data);
    write32(event + 0x68, w->bytes);
    write32(event + 0x6C, 0);
    write32(event + 0x70, 0);
    xbox_preview_log_sample("ps2-wave", "sound=%08X event=%08X asset=%d bytes=%u rate=%u", sound, event, replacement, w->bytes, w->rate);
}
/* Only metadata produced above can bind a standalone PCM buffer. It stays
 * valid for active voices even after the option is switched off. */
uint32_t recomp_ps2_wave_data(uint32_t entry) {
    if (!valid(entry, 24)) return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (waves[i].data && read32(entry + 8) == waves[i].data && read32(entry + 12) == waves[i].bytes &&
            read32(entry + 4) == (0x80000004u | (waves[i].rate << 5))) return waves[i].data;
    return 0;
}
uint32_t recomp_ps2_texture(uint32_t original) {
    static uint32_t replacement;
    static int registration_failed;
    /* RedTexture name hash (FNV-1a with each byte ORed with 0x20). */
    if (!recomp_options_ps2_upgrades() || !valid(original, 0x64) || read32(original + 0x24) != 0xa9a66d3fu) return original;
    if (replacement) return replacement;
    if (registration_failed) return original;
    uint32_t size = 0, header = read32(original + 0x44);
    const uint8_t *pixels = resource(205, &size);
    if (!valid(header, 20) || !pixels || size != 256 * 256 * 4) return original;
    uint32_t allocation = recomp_title_heap_allocate(512);
    if (!allocation) return original;
    uint32_t offset = (allocation + 0x17Fu) & ~127u;
    memcpy(guest(allocation), guest(original), 0x64);
    memcpy(guest(allocation + 0x70), guest(header), 20);
    write32(allocation + 0x44, allocation + 0x70);
    write32(allocation + 0x74, offset);
    if (!pgraph_d3d11_register_world_texture(offset, 256, 256, (const uint32_t *)pixels)) {
        registration_failed = 1;
        return original;
    }
    replacement = allocation;
    xbox_preview_log_event("ps2-texture", "M1 replacement=%08X original=%08X", replacement, original);
    return replacement;
}
