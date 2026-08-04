/* config.h — simple key=value config (~/.config/mikaplay/config.ini) */
#ifndef MIKA_CONFIG_H
#define MIKA_CONFIG_H

#define CFG_PATH_MAX 1024

typedef struct {
    char music_dir[CFG_PATH_MAX];
    char last_dir[CFG_PATH_MAX];
    int  volume;
    int  shuffle;
    int  repeat;   /* 0 off, 1 all, 2 one */
    char lf_key[512];
    char lf_secret[512];
    char lf_session[128];
    char lf_user[128];
    char lb_token[512];
} Config;

extern Config cfg;

void config_defaults(void);
void config_load(const char *path);
void config_save(void);   /* saves to the path config_load saw */

#endif
