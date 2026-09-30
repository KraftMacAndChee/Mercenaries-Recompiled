/* Included by recomp_manual.c after the guest CPU save/restore helpers. */
extern void sub_0020BA60(void); /* Brush2D::GetStringWidth */
float recomp_custom_menu_width(uint32_t brush)
{
    const float retail_width=245.f;
    if(brush<0x10000u || brush>0x03FFFF20u || g_esp<0x20000u || g_esp>=0x4000000u)return retail_width;
    uint32_t first=guest_u32(brush+0x40u),count=guest_u32(brush+0x3Cu);
    if((first!=RECOMP_CONTROLS_ROW && first!=RECOMP_OPTIONS_FPS_HASH) || count>16u)return retail_width;
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    uint32_t scratch=(g_esp-160u)&~15u;
    float width=retail_width;
    for(uint32_t row=0;row<count;++row){
        uint32_t hash=guest_u32(brush+0x40u+row*4u);
        const char *label=recomp_controls_label(hash);
        if(!label)label=recomp_options_label(hash);
        if(!label)continue;
        size_t length=strlen(label);
        if(length>120u)length=120u;
        memcpy(guest_ptr(scratch),label,length);
        *(char*)guest_ptr(scratch+(uint32_t)length)=0;
        g_esp=scratch;
        recomp_guest_push_u32(0x3F800000u); /* Menu's authored 1.0 font scale. */
        recomp_guest_push_u32(0xA9512042u); /* Agency */
        recomp_guest_push_u32(scratch);
        recomp_guest_push_u32(0);
        g_ecx=brush;
        sub_0020BA60();
        float text_width=(float)g_fp_stack[g_fp_top&7u];
        /* Left edge -4, text at 61, and 12 units beyond the drop shadow.
         * Binding rows reserve another 22 units for their input glyph. */
        float required=77.f+text_width+(recomp_controls_row_binding(hash,NULL,NULL)?22.f:0.f);
        if(isfinite(required) && required>width)width=required;
        recomp_restore_guest_cpu_context(&saved);
    }
    recomp_restore_guest_cpu_context(&saved);
    return width;
}
