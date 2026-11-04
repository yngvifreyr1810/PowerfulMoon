// MIT. Simulates a stock update rewriting powermoon.ini; no game data is used.
#include "../runtime/settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static char mapping[32]="east";
bool bind_settings_load(const char *key,const char *value) {
    if (strcmp(key,"bind_pad_a")) return false;
    snprintf(mapping,sizeof mapping,"%s",value); return true;
}
void bind_settings_save(FILE *f) { fprintf(f,"bind_pad_a = %s\n",mapping); }
int main(int argc,char **argv) {
    assert(argc==2);
    FILE *f=fopen(getenv("ODYSSEY_SETTINGS"),"w"); assert(f);
    if (!strcmp(argv[1],"seed"))
        fprintf(f,"motion = 0\nrumble = 1\nhd_rumble = 0\nrumble_strength = 1.65\nbind_pad_a = west\nframe_interp = 1\n");
    else
        fprintf(f,"fullscreen = 1\nresolution = 50%%\nframe_interp = 1\n");
    fclose(f); settings_load();
    assert(!g_settings.motion && g_settings.rumble && !g_settings.hd_rumble);
    assert(g_settings.rumble_strength>1.649f && g_settings.rumble_strength<1.651f);
    assert(!strcmp(mapping,"west")); assert(g_settings.frame_interp);
    if (strcmp(argv[1],"seed")) assert(g_settings.res_mode==RES_Q50 && g_settings.fullscreen);
    settings_save(); puts("Controller profile migration / stock settings rewrite recovery: PASS");
    return 0;
}
