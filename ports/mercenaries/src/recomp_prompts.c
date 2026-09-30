/* Dynamic prompts resolve retail actions through the live input bindings.
 * Artwork is immutable; binding/device changes select another cached glyph. */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "recomp_controls.h"
#include "recomp_options.h"
#include "xinput_xbox.h"
#include "nv2a_pgraph_d3d11.h"
#include "prompt_assets.inc"
extern ptrdiff_t g_xbox_mem_offset;
extern float recomp_ui_prompt_aspect(void);
extern uint32_t recomp_title_heap_allocate(uint32_t size);
static void *guest(uint32_t address){return (void*)((uintptr_t)g_xbox_mem_offset+address);}
static uint32_t read32(uint32_t address){uint32_t v;memcpy(&v,guest(address),4);return v;}
static void write32(uint32_t address,uint32_t v){memcpy(guest(address),&v,4);}
static int keyboard_asset(unsigned key)
{
    static const unsigned letters[]={1343,1367,1365,1355,1345,1356,1357,1358,1350,1359,1360,1361,1362,1368,1351,1352,1353,1346,1354,1347,1349,1366,1363,1364,1348,1344};
    unsigned id=1411;
    if(key>='A'&&key<='Z')id=letters[key-'A'];
    else if(key>='1'&&key<='9')id=1331+key-'1';
    else if(key=='0')id=1340;
    else if(key>=VK_F1&&key<=VK_F12)id=1317+key-VK_F1+(key>=VK_F5);
    else switch(key){
    case VK_ESCAPE:id=1316;break;case VK_TAB:id=1372;break;case VK_CAPITAL:id=1373;break;
    case VK_SHIFT:case VK_LSHIFT:id=1375;break;case VK_RSHIFT:id=1386;break;
    case VK_CONTROL:case VK_LCONTROL:id=1376;break;case VK_RCONTROL:id=1387;break;
    case VK_MENU:case VK_LMENU:id=1378;break;case VK_RMENU:id=1380;break;
    case VK_SPACE:id=1379;break;case VK_RETURN:id=1389;break;case VK_BACK:id=3;break;
    case VK_SNAPSHOT:id=1394;break;case VK_SCROLL:id=1396;break;case VK_PAUSE:id=1399;break;
    case VK_INSERT:id=1401;break;case VK_DELETE:id=1406;break;case VK_END:id=1407;break;
    case VK_HOME:id=1408;break;case VK_PRIOR:id=1409;break;case VK_NEXT:id=1410;break;
    case VK_UP:id=1412;break;case VK_DOWN:id=1413;break;case VK_LEFT:id=1414;break;case VK_RIGHT:id=1415;break;
    case VK_NUMLOCK:id=1450;break;case VK_DIVIDE:id=1453;break;case VK_MULTIPLY:id=1455;break;case VK_SUBTRACT:id=1457;break;
    case VK_NUMPAD0:id=1487;break;case VK_NUMPAD1:id=1473;break;case VK_NUMPAD2:id=1476;break;
    case VK_NUMPAD3:id=1480;break;case VK_NUMPAD4:id=1467;break;case VK_NUMPAD5:id=1470;break;
    case VK_NUMPAD6:id=1471;break;case VK_NUMPAD7:id=1459;break;case VK_NUMPAD8:id=1461;break;
    case VK_NUMPAD9:id=1463;break;case VK_ADD:id=1481;break;case VK_DECIMAL:id=1485;break;
    case VK_LBUTTON:id=1583;break;case VK_RBUTTON:id=1588;break;case VK_MBUTTON:id=1592;break;
    case 0x100:id=1613;break;case 0x101:id=1624;break;
    case VK_XBUTTON1:id=1596;break;case VK_XBUTTON2:id=1600;break;
    case VK_LWIN:id=1626;break;case VK_RWIN:id=1628;break;case VK_APPS:id=1636;break;
    default:
        /* OEM key meanings depend on the user's active keyboard layout. */
        switch(MapVirtualKeyW(key,MAPVK_VK_TO_CHAR)&0xffffu){
        case '-':id=1341;break;case '+':id=1342;break;case '=':id=1543;break;
        case ',':id=1569;break;case '.':id=1527;break;case '/':id=1528;break;
        case '\\':id=1555;break;case ';':id=1570;break;case ':':id=1571;break;
        case '\'':id=1535;break;case '"':id=1534;break;case '[':id=1552;break;case ']':id=1558;break;
        case '`':id=1554;break;case '~':id=1545;break;case '<':id=1518;break;case '>':id=1517;break;
        case '%':id=1520;break;case '$':id=1523;break;case '?':id=1526;break;case '&':id=1532;break;
        case '(':id=1536;break;case ')':id=1542;break;case '_':id=1539;break;case '#':id=1546;break;
        case '{':id=1547;break;case '}':id=1559;break;case '|':id=1553;break;case '^':id=1556;break;
        case '@':id=1557;break;case '*':id=1568;break;case '!':id=1572;break;
        case 0x00b2:id=1330;break;case 0x00b5:id=1525;break;case 0x00a7:id=1531;break;
        case 0x00e9:id=1533;break;case 0x00e8:id=1538;break;case 0x00e7:id=1540;break;
        case 0x00e0:id=1541;break;case 0x00f9:id=1566;break;case 0x00a3:id=1573;break;
        case 0x00a4:id=1574;break;case 0x20ac:id=1575;break;case 0x00b0:id=1576;break;
        }break;
    }
    switch(id){
    case 1316:return PROMPT_KEY_1316;
    case 1317:return PROMPT_KEY_1317;
    case 1318:return PROMPT_KEY_1318;
    case 1319:return PROMPT_KEY_1319;
    case 1320:return PROMPT_KEY_1320;
    case 1322:return PROMPT_KEY_1322;
    case 1323:return PROMPT_KEY_1323;
    case 1324:return PROMPT_KEY_1324;
    case 1325:return PROMPT_KEY_1325;
    case 1326:return PROMPT_KEY_1326;
    case 1327:return PROMPT_KEY_1327;
    case 1328:return PROMPT_KEY_1328;
    case 1329:return PROMPT_KEY_1329;
    case 1330:return PROMPT_KEY_1330;
    case 1331:return PROMPT_KEY_1331;
    case 1332:return PROMPT_KEY_1332;
    case 1333:return PROMPT_KEY_1333;
    case 1334:return PROMPT_KEY_1334;
    case 1335:return PROMPT_KEY_1335;
    case 1336:return PROMPT_KEY_1336;
    case 1337:return PROMPT_KEY_1337;
    case 1338:return PROMPT_KEY_1338;
    case 1339:return PROMPT_KEY_1339;
    case 1340:return PROMPT_KEY_1340;
    case 1341:return PROMPT_KEY_1341;
    case 1342:return PROMPT_KEY_1342;
    case 1343:return PROMPT_KEY_1343;
    case 1344:return PROMPT_KEY_1344;
    case 1345:return PROMPT_KEY_1345;
    case 1346:return PROMPT_KEY_1346;
    case 1347:return PROMPT_KEY_1347;
    case 1348:return PROMPT_KEY_1348;
    case 1349:return PROMPT_KEY_1349;
    case 1350:return PROMPT_KEY_1350;
    case 1351:return PROMPT_KEY_1351;
    case 1352:return PROMPT_KEY_1352;
    case 1353:return PROMPT_KEY_1353;
    case 1354:return PROMPT_KEY_1354;
    case 1355:return PROMPT_KEY_1355;
    case 1356:return PROMPT_KEY_1356;
    case 1357:return PROMPT_KEY_1357;
    case 1358:return PROMPT_KEY_1358;
    case 1359:return PROMPT_KEY_1359;
    case 1360:return PROMPT_KEY_1360;
    case 1361:return PROMPT_KEY_1361;
    case 1362:return PROMPT_KEY_1362;
    case 1363:return PROMPT_KEY_1363;
    case 1364:return PROMPT_KEY_1364;
    case 1365:return PROMPT_KEY_1365;
    case 1366:return PROMPT_KEY_1366;
    case 1367:return PROMPT_KEY_1367;
    case 1368:return PROMPT_KEY_1368;
    case 1372:return PROMPT_KEY_1372;
    case 1373:return PROMPT_KEY_1373;
    case 1375:return PROMPT_KEY_1375;
    case 1376:return PROMPT_KEY_1376;
    case 1378:return PROMPT_KEY_1378;
    case 1379:return PROMPT_KEY_1379;
    case 1380:return PROMPT_KEY_1380;
    case 1386:return PROMPT_KEY_1386;
    case 1387:return PROMPT_KEY_1387;
    case 1389:return PROMPT_KEY_1389;
    case 1394:return PROMPT_KEY_1394;
    case 1396:return PROMPT_KEY_1396;
    case 1399:return PROMPT_KEY_1399;
    case 1401:return PROMPT_KEY_1401;
    case 1406:return PROMPT_KEY_1406;
    case 1407:return PROMPT_KEY_1407;
    case 1408:return PROMPT_KEY_1408;
    case 1409:return PROMPT_KEY_1409;
    case 1410:return PROMPT_KEY_1410;
    case 1411:return PROMPT_KEY_1411;
    case 1412:return PROMPT_KEY_1412;
    case 1413:return PROMPT_KEY_1413;
    case 1414:return PROMPT_KEY_1414;
    case 1415:return PROMPT_KEY_1415;
    case 1450:return PROMPT_KEY_1450;
    case 1453:return PROMPT_KEY_1453;
    case 1455:return PROMPT_KEY_1455;
    case 1457:return PROMPT_KEY_1457;
    case 1459:return PROMPT_KEY_1459;
    case 1461:return PROMPT_KEY_1461;
    case 1463:return PROMPT_KEY_1463;
    case 1467:return PROMPT_KEY_1467;
    case 1470:return PROMPT_KEY_1470;
    case 1471:return PROMPT_KEY_1471;
    case 1473:return PROMPT_KEY_1473;
    case 1476:return PROMPT_KEY_1476;
    case 1480:return PROMPT_KEY_1480;
    case 1481:return PROMPT_KEY_1481;
    case 1483:return PROMPT_KEY_1483;
    case 1485:return PROMPT_KEY_1485;
    case 1487:return PROMPT_KEY_1487;
    case 1489:return PROMPT_KEY_1489;
    case 1493:return PROMPT_KEY_1493;
    case 1500:return PROMPT_KEY_1500;
    case 1504:return PROMPT_KEY_1504;
    case 1507:return PROMPT_KEY_1507;
    case 1508:return PROMPT_KEY_1508;
    case 1513:return PROMPT_KEY_1513;
    case 1514:return PROMPT_KEY_1514;
    case 1515:return PROMPT_KEY_1515;
    case 1516:return PROMPT_KEY_1516;
    case 1517:return PROMPT_KEY_1517;
    case 1518:return PROMPT_KEY_1518;
    case 1519:return PROMPT_KEY_1519;
    case 1520:return PROMPT_KEY_1520;
    case 1523:return PROMPT_KEY_1523;
    case 1525:return PROMPT_KEY_1525;
    case 1526:return PROMPT_KEY_1526;
    case 1527:return PROMPT_KEY_1527;
    case 1528:return PROMPT_KEY_1528;
    case 1531:return PROMPT_KEY_1531;
    case 1532:return PROMPT_KEY_1532;
    case 1533:return PROMPT_KEY_1533;
    case 1534:return PROMPT_KEY_1534;
    case 1535:return PROMPT_KEY_1535;
    case 1536:return PROMPT_KEY_1536;
    case 1537:return PROMPT_KEY_1537;
    case 1538:return PROMPT_KEY_1538;
    case 1539:return PROMPT_KEY_1539;
    case 1540:return PROMPT_KEY_1540;
    case 1541:return PROMPT_KEY_1541;
    case 1542:return PROMPT_KEY_1542;
    case 1543:return PROMPT_KEY_1543;
    case 1545:return PROMPT_KEY_1545;
    case 1546:return PROMPT_KEY_1546;
    case 1547:return PROMPT_KEY_1547;
    case 1552:return PROMPT_KEY_1552;
    case 1553:return PROMPT_KEY_1553;
    case 1554:return PROMPT_KEY_1554;
    case 1555:return PROMPT_KEY_1555;
    case 1556:return PROMPT_KEY_1556;
    case 1557:return PROMPT_KEY_1557;
    case 1558:return PROMPT_KEY_1558;
    case 1559:return PROMPT_KEY_1559;
    case 1561:return PROMPT_KEY_1561;
    case 1566:return PROMPT_KEY_1566;
    case 1568:return PROMPT_KEY_1568;
    case 1569:return PROMPT_KEY_1569;
    case 1570:return PROMPT_KEY_1570;
    case 1571:return PROMPT_KEY_1571;
    case 1572:return PROMPT_KEY_1572;
    case 1573:return PROMPT_KEY_1573;
    case 1574:return PROMPT_KEY_1574;
    case 1575:return PROMPT_KEY_1575;
    case 1576:return PROMPT_KEY_1576;
    case 1583:return PROMPT_KEY_1583;
    case 1588:return PROMPT_KEY_1588;
    case 1592:return PROMPT_KEY_1592;
    case 1596:return PROMPT_KEY_1596;
    case 1600:return PROMPT_KEY_1600;
    case 1613:return PROMPT_KEY_1613;
    case 1624:return PROMPT_KEY_1624;
    case 1626:return PROMPT_KEY_1626;
    case 1628:return PROMPT_KEY_1628;
    case 1636:return PROMPT_KEY_1636;
    case 3:return PROMPT_KEY_3;
    default:return PROMPT_KEY_1411;
    }
}
static int pad_asset(unsigned button,int device)
{
    static const unsigned xbox[]={PROMPT_XBOX_DPAD_UP,PROMPT_XBOX_DPAD_DOWN,PROMPT_XBOX_DPAD_LEFT,PROMPT_XBOX_DPAD_RIGHT,
        PROMPT_XBOX_START,PROMPT_XBOX_BACK,PROMPT_XBOX_LS,PROMPT_XBOX_RS,
        PROMPT_XBOX_A,PROMPT_XBOX_B,PROMPT_XBOX_X,PROMPT_XBOX_Y,PROMPT_XBOX_RB,PROMPT_XBOX_LB,PROMPT_XBOX_LT,PROMPT_XBOX_RT,
        PROMPT_XBOX_LS_LEFT,PROMPT_XBOX_LS_RIGHT,PROMPT_XBOX_LS_DOWN,PROMPT_XBOX_LS_UP,PROMPT_XBOX_RS_LEFT,PROMPT_XBOX_RS_RIGHT,PROMPT_XBOX_RS_DOWN,PROMPT_XBOX_RS_UP};
    static const unsigned ps[]={PROMPT_PS_DPAD_UP,PROMPT_PS_DPAD_DOWN,PROMPT_PS_DPAD_LEFT,PROMPT_PS_DPAD_RIGHT,
        PROMPT_PS_START,PROMPT_PS_BACK,PROMPT_PS_LS,PROMPT_PS_RS,
        PROMPT_PS_A,PROMPT_PS_B,PROMPT_PS_X,PROMPT_PS_Y,PROMPT_PS_RB,PROMPT_PS_LB,PROMPT_PS_LT,PROMPT_PS_RT,
        PROMPT_PS_LS_LEFT,PROMPT_PS_LS_RIGHT,PROMPT_PS_LS_DOWN,PROMPT_PS_LS_UP,PROMPT_PS_RS_LEFT,PROMPT_PS_RS_RIGHT,PROMPT_PS_RS_DOWN,PROMPT_PS_RS_UP};
    return button<24?(device==XBOX_PROMPT_PLAYSTATION?ps[button]:xbox[button]):PROMPT_KEY_1411;
}
typedef struct PromptRegion {uint32_t region;unsigned action;int context;} PromptRegion;
static const PromptRegion regions[]={
    {0xFB0968D1u,11,-1},{0x45812F4Fu,10,-1},{0x3B811F91u,9,-1},{0x4D813BE7u,8,-1},
    {0xF4C15722u,14,-1},{0xF3C1558Fu,13,-1},{0xF880B618u,15,-1},{0xFB80BAD1u,12,-1},
    {0x1485E089u,5,-1},{0xBCFB2EC3u,4,-1},
    {0x89EEF07Bu,6,-1},{0x77EED425u,7,-1},
    {0x5E1743CAu,14,11},{0x5DC06317u,15,11},{0x35EDDC25u,8,11},{0x0AA45606u,11,11},
    {0xEA432160u,0,11},{0xD9543955u,1,11},{0xC7308EB0u,2,11},{0xFE626225u,3,11},{0x3A8A02B4u,5,11},
    {0x0CA0A8A1u,10,-1},{0xCCDC1675u,8,-1},{0xE5691987u,9,-1},{0xC1BED148u,4,-1}
};
typedef struct PromptTexture {uint32_t object;uint32_t *pixels;} PromptTexture;
#define PROMPT_TEXTURE_COUNT 1024
static PromptTexture textures[PROMPT_TEXTURE_COUNT];
static unsigned next_texture=PROMPT_ASSET_COUNT;
static int movement_scope;
static int support_scope;
static int support_fire_scope;
void recomp_prompts_support_fire_scope(int enabled){support_fire_scope=enabled;}
void recomp_prompts_support_scope(int enabled){support_scope=enabled;}
void recomp_prompts_movement_scope(int enabled){movement_scope=enabled;}
static uint32_t *decode_asset(unsigned index)
{
    uint32_t *pixels=(uint32_t*)calloc(4096,4);unsigned i,pos=0;
    if(!pixels || index>=PROMPT_ASSET_COUNT){free(pixels);return NULL;}
    for(i=prompt_asset_runs[index].start;i<prompt_asset_runs[index].start+prompt_asset_runs[index].count;++i){
        unsigned n=prompt_runs[i].count;
        if(pos+n>4096){free(pixels);return NULL;}
        while(n--)pixels[pos++]=prompt_runs[i].bgra;
    }
    if(pos!=4096){free(pixels);return NULL;}return pixels;
}
static unsigned group_asset(unsigned first,int device)
{
    static struct {unsigned glyph[4],index;} groups[PROMPT_TEXTURE_COUNT-PROMPT_ASSET_COUNT];
    static unsigned count;
    unsigned glyph[4],binding[4],i,j;int normal=1;
    for(i=0;i<4;++i){
        /* Omit directions the vehicle cannot use (e.g. winch D-pad left).
         * An available action explicitly left unbound still shows ???. */
        if(!recomp_controls_prompt_action_available(first+i,device)){
            binding[i]=0xffffu;glyph[i]=PROMPT_BLANK;continue;
        }
        binding[i]=recomp_controls_prompt_binding(first+i,device,-1);
        if(binding[i]!=first+i)normal=0;
        glyph[i]=device==XBOX_PROMPT_KEYBOARD?keyboard_asset(binding[i]):pad_asset(binding[i],device);
    }
    if(device!=XBOX_PROMPT_KEYBOARD && normal){
        if(first==0)return device==XBOX_PROMPT_PLAYSTATION?PROMPT_PS_DPAD:PROMPT_XBOX_DPAD;
        if(first==16)return device==XBOX_PROMPT_PLAYSTATION?PROMPT_PS_LS:PROMPT_XBOX_LS;
        return device==XBOX_PROMPT_PLAYSTATION?PROMPT_PS_RS:PROMPT_XBOX_RS;
    }
    for(i=0;i<count;++i)if(!memcmp(groups[i].glyph,glyph,sizeof(glyph)))return groups[i].index;
    if(next_texture>=PROMPT_TEXTURE_COUNT)return PROMPT_KEY_1411;
    uint32_t *pixels=(uint32_t*)calloc(4096,4);
    if(!pixels)return PROMPT_KEY_1411;
    for(i=0;i<4;++i){
        uint32_t *part=decode_asset(glyph[i]);
        /* D-pad order U,D,L,R; stick order L,R,D,U. */
        unsigned position=first==0?(unsigned[]){0,2,1,3}[i]:(unsigned[]){1,3,2,0}[i];
        unsigned ox=position==0?21:(position-1)*21,oy=position==0?10:32;
        if(!part){free(pixels);return PROMPT_KEY_1411;}
        for(j=0;j<21*21;++j)pixels[(oy+j/21)*64+ox+j%21]=part[(j/21*64/21)*64+(j%21*64/21)];
        free(part);
    }
    memcpy(groups[count].glyph,glyph,sizeof(glyph));
    groups[count++].index=next_texture;
    textures[next_texture].pixels=pixels;
    return next_texture++;
}
#define PROMPT_HASH_BASE 0xD9700000u
uint32_t recomp_prompts_find_texture(uint32_t hash,uint32_t original)
{
    unsigned index=hash-PROMPT_HASH_BASE;
    return !original&&index<PROMPT_TEXTURE_COUNT?textures[index].object:original;
}
/* Preserve the glyph's proportions in authored non-square PDA slots. */
static unsigned fit_region_asset(unsigned source,uint32_t region)
{
    static struct {unsigned source,shape,index;} fitted[512];static unsigned count;
    unsigned shape,i,x,y,w=64,h=64;
    float ratio=recomp_ui_prompt_aspect();
    if(region==0x5E1743CAu || region==0x5DC06317u)ratio*=64.0f/25.0f;
    else if(region==0xEA432160u || region==0xD9543955u)ratio*=53.0f/18.0f;
    else if(region==0xC7308EB0u || region==0xFE626225u)ratio*=42.0f/28.0f;
    if(!(ratio>0.1f && ratio<10.0f))ratio=1.0f;
    if(ratio>=1.0f)w=(unsigned)(64.0f/ratio+.5f);else h=(unsigned)(64.0f*ratio+.5f);
    if(w==64 && h==64)return source;
    shape=w+h*65;
    for(i=0;i<count;++i)if(fitted[i].source==source&&fitted[i].shape==shape)return fitted[i].index;
    if(count>=512||next_texture>=PROMPT_TEXTURE_COUNT)return source;
    const uint32_t *src=textures[source].pixels;uint32_t *temporary=NULL;
    if(!src)src=temporary=decode_asset(source);
    if(!src)return source;
    uint32_t *pixels=(uint32_t*)calloc(4096,4);
    if(!pixels){free(temporary);return source;}
    for(y=0;y<h;++y)for(x=0;x<w;++x){
        /* Area average premultiplied channels when reducing the glyph width. */
        double left=x*64.0/w,right=(x+1)*64.0/w,a=0,b=0,g=0,r=0;
        unsigned lo=(unsigned)left,hi=(unsigned)right;
        for(i=lo;i<=hi&&i<64;++i){
            double weight=(right<i+1?right:i+1)-(left>i?left:i);
            uint32_t p=src[(y*64/h)*64+i];double alpha=(p>>24)*weight;
            a+=alpha;b+=(p&255)*alpha;g+=((p>>8)&255)*alpha;r+=((p>>16)&255)*alpha;
        }
        uint32_t p=0;if(a>0)p=((unsigned)(a/(right-left)+.5)<<24)|((unsigned)(r/a+.5)<<16)|((unsigned)(g/a+.5)<<8)|(unsigned)(b/a+.5);
        pixels[(y+(64-h)/2)*64+(64-w)/2+x]=p;
    }
    free(temporary);fitted[count].source=source;fitted[count].shape=shape;fitted[count++].index=next_texture;
    textures[next_texture].pixels=pixels;return next_texture++;
}
/* The test override is private-build only; both glyph and glow use it. */
static int prompt_override_device=-1;
static int prompt_device(void)
{
    int device=xbox_InputPromptDevice();
    {
        static int checked;static const char *path;static ULONGLONG last;
        if(!checked){checked=1;if(getenv("MERCENARIES_TEST_ISOLATE_INPUT"))path=getenv("MERCENARIES_TEST_PROMPT_FILE");}
        if(path && GetTickCount64()-last>=200){
            FILE *file=fopen(path,"r");int value=-1;last=GetTickCount64();
            if(file){if(fscanf(file,"%d",&value)!=1)value=-1;fclose(file);}prompt_override_device=value;
        }
        if(prompt_override_device>=0 && prompt_override_device<=2)device=prompt_override_device;
    }
    return device;
}
static uint32_t binding_row_scope;
void recomp_prompts_binding_row(uint32_t hash){binding_row_scope=hash;}

/* Measure actual artwork, including its transparent padding, after aspect fit.
 * Retail FeChooseButtonSymbol fills its 20x20 region; its glow is 28x28.
 * Keep that ratio, centre and animation rather than scaling by device name. */
void recomp_prompts_menu_glow(uint32_t args)
{
    if(recomp_options_fixed_xbox_prompts())return;
    int device=prompt_device();
    unsigned binding=recomp_controls_prompt_binding(8,device,-1);
    unsigned index=fit_region_asset(device==XBOX_PROMPT_KEYBOARD?keyboard_asset(binding):pad_asset(binding,device),0xCCDC1675u);
    static struct { unsigned valid; float x,y,w,h; } bounds[PROMPT_TEXTURE_COUNT];
    if(!bounds[index].valid){
        uint32_t *temporary=NULL;
        const uint32_t *p=textures[index].pixels;
        if(!p)p=temporary=decode_asset(index);
        if(!p)return;
        unsigned peak=0,x0=64,y0=64,x1=0,y1=0;
        for(unsigned i=0;i<4096;++i)if((p[i]>>24)>peak)peak=p[i]>>24;
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
            if(peak && (p[y*64+x]>>24)>=((peak+1)/2)){
                if(x<x0)x0=x;if(y<y0)y0=y;if(x+1>x1)x1=x+1;if(y+1>y1)y1=y+1;
            }
        free(temporary);if(!peak)return;
        bounds[index].x=((float)(x0+x1)*.5f-32.f)/64.f*20.f;
        bounds[index].y=((float)(y0+y1)*.5f-32.f)/64.f*20.f;
        bounds[index].w=(x1-x0)/64.f;bounds[index].h=(y1-y0)/64.f;
        bounds[index].valid=1;
    }
    float box[4];memcpy(box,guest(args),sizeof(box));
    float w=box[2]*bounds[index].w,h=box[3]*bounds[index].h;
    box[0]+=(box[2]-w)*.5f+bounds[index].x;
    box[1]+=(box[3]-h)*.5f+bounds[index].y;
    box[2]=w;box[3]=h;memcpy(guest(args),box,sizeof(box));
}

/* Called only after the original region and atlas have resolved successfully. */
uint32_t recomp_prompts_region(uint32_t region,uint32_t original,uint32_t *hash)
{
    unsigned i,index;int device;uint32_t base,header,offset;PromptTexture *texture;
    /* Preserve the retail atlas, labels and decorative layers verbatim.
     * This affects presentation only; physical bindings remain active. */
    if(!binding_row_scope && recomp_options_fixed_xbox_prompts())return 0;
    for(i=0;i<sizeof(regions)/sizeof(regions[0]);++i)if(regions[i].region==region)break;
    int decoration=(movement_scope && (region==0x64471311u || region==0x2D41C7D8u)) || (support_scope && region==0x55034E83u);
    int support=region==0xA7045200u;
    if((i==sizeof(regions)/sizeof(regions[0]) && !decoration && !support) || !original || original>0x03ffff9cu)return 0;
    device=prompt_device();
    unsigned binding=0xffffu;
    if(binding_row_scope && region==0xCCDC1675u){
        int keyboard;unsigned short assigned;
        if(!recomp_controls_row_binding(binding_row_scope,&keyboard,&assigned))return 0;
        device=keyboard?XBOX_PROMPT_KEYBOARD:xbox_InputPromptController();
        /* Allow isolated visual tests to select either connected pad family. */
        if(!keyboard && prompt_override_device>=0 && prompt_override_device<2)device=prompt_override_device;
        binding=assigned;
        index=keyboard?keyboard_asset(binding):pad_asset(binding,device);
    }
    else if(decoration)index=PROMPT_BLANK;
    else if(support)index=group_asset(0,device);
    else if(movement_scope && (region==0x89EEF07Bu || region==0x77EED425u))
        index=group_asset(region==0x89EEF07Bu?16:20,device);
    else {
        /* Keep the dedicated support selection glyph through the closing
         * fade; changing input state must not switch it to equipped Fire. */
        binding=support_fire_scope && region==0xF880B618u
            ? recomp_controls_prompt_support_binding(device)
            : recomp_controls_prompt_binding(regions[i].action,device,regions[i].context);
        /* The PDA's four original d-pad pieces already show each direction.
         * Keep them when that direction is still bound to the same pad input. */
        if(device!=XBOX_PROMPT_KEYBOARD && regions[i].context==11 && regions[i].action<4 && binding==regions[i].action)return 0;
        index=device==XBOX_PROMPT_KEYBOARD?keyboard_asset(binding):pad_asset(binding,device);
    }
    index=fit_region_asset(index,region);
    texture=&textures[index];
    if(!texture->object){
        header=read32(original+0x44);
        if(header<0x10000u || header>0x03ffffecu)return 0;
        uint32_t *pixels=texture->pixels?texture->pixels:decode_asset(index);
        if(!pixels)return 0;
        base=recomp_title_heap_allocate(512);
        if(!base){if(!texture->pixels)free(pixels);return 0;}
        memcpy(guest(base),guest(original),0x64);
        memcpy(guest(base+0x70),guest(header),0x14);
        offset=(base+0x17f)&~0x7fu;
        write32(base+0x44,base+0x70);
        write32(base+0x74,offset);
        if(!pgraph_d3d11_register_ui_texture(offset,64,64,pixels)){
            void recomp_title_heap_free(uint32_t);recomp_title_heap_free(base);
            if(!texture->pixels)free(pixels);return 0;
        }
        texture->pixels=pixels;texture->object=base;
        fprintf(stderr,"[PROMPTS] asset=%u region=%08X object=%08X offset=%08X device=%d binding=%u\n",index,region,base,offset,device,binding);
    }
    *hash=PROMPT_HASH_BASE+index;
    return texture->object;
}
