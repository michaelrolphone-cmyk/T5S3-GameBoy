#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "RiscDisplayOutputV1.h"
#include "RiscInputNavigationV1.h"
#define GB_PATH_MAX 512u
#define GB_CATALOG_MAX 96u
typedef struct {char name[128];bool directory;} gb_entry;
typedef struct {gb_entry entries[GB_CATALOG_MAX];size_t count;bool truncated;} gb_catalog;
bool gb_path(const char* path);
const char* gb_volume_path(const char* broker_path);
bool gb_rom_name(const char* name);
bool gb_join(const char* directory,const char* name,char* out,size_t capacity);
void gb_parent(char* path);
bool gb_catalog_add(gb_catalog*,const char*,bool);
uint8_t gb_buttons(uint32_t navigation);
bool gb_surface(const risc_display_surface_v1*,unsigned* width,unsigned* height);
void gb_pixel(risc_display_surface_v1*,int x,int y,bool black);
void gb_text(risc_display_surface_v1*,int x,int y,const char*,unsigned scale);
bool gb_blit(risc_display_surface_v1*,const uint8_t* source,size_t bytes);
