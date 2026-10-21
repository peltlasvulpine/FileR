#ifndef text_include_file
#define text_include_file

#ifdef __cplusplus
extern "C" {
#endif

#define text_width 15
#define text_height 18
#define text_size 80
#define text ((gfx_rletsprite_t*)text_data)
extern unsigned char text_data[80];

#ifdef __cplusplus
}
#endif

#endif
