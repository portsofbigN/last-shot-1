// The game settings (splatoon.ini), shared by the in-game menu (nx2_settings.c) and Launcher.exe (launcher_ui.c).
#pragma once

typedef struct {
    const char *key;            // ini key
    const char *env;            // environment variable it sets
    const char *def;            // default value
    const char *label[2];       // label: Portuguese, English
    const char *const *values;  // choices
    const char *const *names[2];   // their labels: Portuguese, English
    int nvalues;
    int cat;                    // category (CAT_*)
    const char *desc[2];        // what it does
    char value[512];   // long enough for a game folder path (64 cut ".../updates/65536")
} Setting;

enum { CAT_DISPLAY, CAT_IMAGE, CAT_PERF, CAT_PAD, CAT_GENERAL, NCAT };

int nx2_cfg_count(void);
Setting *nx2_cfg_at(int i);
Setting *nx2_cfg_find(const char *key);
void nx2_cfg_load(void);                 // splatoon.ini next to the program (defaults for what it lacks)
void nx2_cfg_save(void);
void nx2_cfg_set_lang(const char *v);    // "auto", "pt" or "en"
int nx2_cfg_lang(void);                  // 0 Portuguese, 1 English
const char *nx2_cfg_cat_name(int cat);   // in the current language
