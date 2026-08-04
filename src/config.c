/* config.c — flat key=value config file. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <pwd.h>
#endif

Config cfg;
static char cfg_path[CFG_PATH_MAX + 64];

void config_defaults(void)
{
    const char *home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (!home) {
#ifndef _WIN32
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/";
#else
        home = "C:\\";
#endif
    }
    snprintf(cfg.music_dir, sizeof cfg.music_dir, "%s/Music", home);
    snprintf(cfg.last_dir, sizeof cfg.last_dir, "%s", cfg.music_dir);
    cfg.volume = 80;
    cfg.shuffle = 0;
    cfg.repeat = 0;
    cfg.refresh = 5;
    cfg.lf_key[0] = cfg.lf_secret[0] = cfg.lf_session[0] = cfg.lf_user[0] = cfg.lb_token[0] = 0;
}

static void set_str(char *dst, size_t n, const char *v)
{
    snprintf(dst, n, "%s", v ? v : "");
}

void config_load(const char *path)
{
    FILE *fp;
    char line[2048];
    snprintf(cfg_path, sizeof cfg_path, "%s", path);
    config_defaults();
    fp = fopen(path, "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        char *eq = strchr(line, '=');
        char *val;
        if (!eq) continue;
        *eq = 0;
        val = eq + 1;
        while (*val && (*val == ' ' || *val == '\t')) val++;
        {
            size_t l = strlen(val);
            while (l && (val[l-1] == '\n' || val[l-1] == '\r' || val[l-1] == ' ' || val[l-1] == '\t')) val[--l] = 0;
        }
        if (!strcmp(line, "music_dir")) set_str(cfg.music_dir, sizeof cfg.music_dir, val);
        else if (!strcmp(line, "last_dir")) set_str(cfg.last_dir, sizeof cfg.last_dir, val);
        else if (!strcmp(line, "volume")) cfg.volume = atoi(val);
        else if (!strcmp(line, "shuffle")) cfg.shuffle = atoi(val);
        else if (!strcmp(line, "repeat")) cfg.repeat = atoi(val);
        else if (!strcmp(line, "refresh")) cfg.refresh = atoi(val);
        else if (!strcmp(line, "lf_key")) set_str(cfg.lf_key, sizeof cfg.lf_key, val);
        else if (!strcmp(line, "lf_secret")) set_str(cfg.lf_secret, sizeof cfg.lf_secret, val);
        else if (!strcmp(line, "lf_session")) set_str(cfg.lf_session, sizeof cfg.lf_session, val);
        else if (!strcmp(line, "lf_user")) set_str(cfg.lf_user, sizeof cfg.lf_user, val);
        else if (!strcmp(line, "lb_token")) set_str(cfg.lb_token, sizeof cfg.lb_token, val);
    }
    fclose(fp);
}

void config_save(void)
{
    FILE *fp;
    if (!cfg_path[0]) return;
    fp = fopen(cfg_path, "w");
    if (!fp) return;
    fprintf(fp, "# marimo config\n");
    fprintf(fp, "music_dir=%s\n", cfg.music_dir);
    fprintf(fp, "last_dir=%s\n", cfg.last_dir);
    fprintf(fp, "volume=%d\n", cfg.volume);
    fprintf(fp, "shuffle=%d\n", cfg.shuffle);
    fprintf(fp, "repeat=%d\n", cfg.repeat);
    fprintf(fp, "refresh=%d\n", cfg.refresh);
    fprintf(fp, "lf_key=%s\n", cfg.lf_key);
    fprintf(fp, "lf_secret=%s\n", cfg.lf_secret);
    fprintf(fp, "lf_session=%s\n", cfg.lf_session);
    fprintf(fp, "lf_user=%s\n", cfg.lf_user);
    fprintf(fp, "lb_token=%s\n", cfg.lb_token);
    fclose(fp);
}
