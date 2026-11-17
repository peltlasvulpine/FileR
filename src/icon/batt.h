#ifndef batt_include_file
#define batt_include_file

#ifdef __cplusplus
extern "C" {
#endif

#define batt_width 12
#define batt_height 18
#define batt_size 126
#define batt ((gfx_rletsprite_t*)batt_data)
extern unsigned char batt_data[126];

#ifdef __cplusplus
}
#endif

#endif
