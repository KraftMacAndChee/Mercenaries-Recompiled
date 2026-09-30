/* Carry pickup is entered only after the retail Use action has accepted the
 * target. Observe the lower-body blend before Enter replaces its animation. */
int recomp_crouch_pickup_transition(uint32_t carrier,uint32_t passenger)
{
    if(carrier<0x10000u || carrier>0x03FFF800u ||
       passenger<0x10000u || passenger>0x03FFF800u)return 0;
    if(guest_u32(carrier+4u)!=0x660E4490u || /* player0 */
       guest_u32(carrier+0x79Cu)!=0u || !guest_u8(passenger+0x6B9u))return 0;
    uint32_t anim=guest_u32(carrier+0x6B0u);
    if(anim<0x10000u || anim>0x03FFD000u)return 0;
    /* RsAnimHuman::_GetAnimSingle(LOWER): partitioned animations use Lower;
     * full-body animations use Upper. Single's trailing word is its handle. */
    uint32_t lower=anim+(guest_u32(anim+0x28E8u)==1u?0x1964u:0xE10u);
    uint32_t handle=guest_u32(lower+0xB50u);
    float weight=guest_f32(lower+0xB38u);
    return (handle==0xD0993B47u || handle==0xDA4F821Bu) &&
        weight>=0.f && weight<1.f && guest_u32(carrier+0x6B4u)==0u;
}
