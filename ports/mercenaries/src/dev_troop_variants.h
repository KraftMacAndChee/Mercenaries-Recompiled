/* Some combat roles have retail models but missing or stale shared templates.
 * Apply their authored differences to a checked template of the same faction;
 * never load a mission layer merely to obtain one of its placed characters. */
typedef struct DevTroopVariant {
    const char *model, *display, *weapon, *secondary, *drop;
    const char *faction_value, *accuracy, *vision;
    const char *base_template, *base_model, *hit_points, *armor, *animation;
} DevTroopVariant;
static const DevTroopVariant dev_troop_variants[] = {
#include "dev_troop_variant_data.inc"
};
static const DevTroopVariant *dev_troop_variant(const DevVehicle *troop)
{
    for(unsigned i=0;i<sizeof(dev_troop_variants)/sizeof(dev_troop_variants[0]);++i)
        if(troop->template_hash==dev_hash(dev_troop_variants[i].base_template) &&
           troop->model_hash==dev_hash(dev_troop_variants[i].model))return &dev_troop_variants[i];
    return NULL;
}
static int dev_prepare_troop_variant(uint32_t stack,const DevTroopVariant *variant)
{
    if(!recomp_lookup(0x178020)||!recomp_lookup(0x1785D0)||!recomp_lookup(0x17F490))return 0;
    /* Models request their own textures through the counted managers. DoBigLoad
     * drains those dependencies too; texture is not an RsAssetHelper queue type. */
    uint32_t args[]={dev_hash(variant->model),0xB08B665Au};
    if(!(dev_call(stack,0x178020,0,2,args)&255u)){
        dev_call(stack,0x1785D0,0,2,args);
        dev_call(stack,0x17F490,0,0,NULL);
    }
    return dev_resident_model(args[0])!=0;
}

static void dev_apply_troop_variant(uint32_t stack,uint32_t scratch,uint32_t list,const DevTroopVariant *variant)
{
    dev_property(stack,scratch,list,"geometryfile",variant->model);
    dev_property(stack,scratch,list,"displayName",variant->display);
    dev_property(stack,scratch,list,"hitPoints",variant->hit_points);
    if(variant->armor)dev_property(stack,scratch,list,"ArmorType",variant->armor);
    if(variant->animation)dev_property(stack,scratch,list,"animationTable",variant->animation);
    dev_property(stack,scratch,list,"factionValue",variant->faction_value);
    dev_property(stack,scratch,list,"visionFactor",variant->vision);
    dev_property(stack,scratch,list,"ai_accuracy",variant->accuracy);
    dev_property(stack,scratch,list,"weapon_A_template",variant->weapon);
    dev_property(stack,scratch,list,"secondaryWeapon_A_template",variant->secondary);
    dev_property(stack,scratch,list,"spawnOnDeath",variant->drop);
    dev_property(stack,scratch,list,"ai_sentrySkillNormal","0");
    dev_property(stack,scratch,list,"ai_sentrySkillAlert","0");
}
