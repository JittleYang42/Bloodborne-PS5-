/* bbport PS5: the title's entry. Everything the game prints goes to /app0/bbport/last_run.log
 * and to klog; settings come from /app0/bbport/bbport-ps5.env (KEY=VALUE lines, the environment
 * run.py gives the game on a PC) and /app0/bbport/bbport.ini; then the PC loader's main runs on
 * the prepared image (src/probe.c, bb_probe_main).
 *
 * The title folder (/data/homebrew/<TITLE_ID>/ on the console, /app0 to the title):
 *   eboot.bin, sce_sys/, sce_module/libc.prx     the title (ps5/tools/link-title.sh)
 *   CUSA03173/                                   your game dump: eboot.bin, sce_sys, dvdroot_ps4...
 *   bbport/boot-linked.bin, content.bin, patches.bin   prepared on the PC (ps5/tools/prepare-game.sh)
 *   bbport/bbport-ps5.env, bbport.ini, input.ini       settings
 *   bbport/user/                                 saves, shader and pipeline caches
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "compat_ps5.h"
#include "../console/platform.h"

#include <ps5platform/klog.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int bb_probe_main(int argc, char **argv);
int sceSystemServiceHideSplashScreen(void);

#define ROOT "/app0"
#define PORT ROOT "/bbport"
const char *bb_ps5_app_root(void) { return ROOT; }

/* ---- the log: stdout and stderr through one pipe, each line to the file and to klog ---- */
static FILE *log_file;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
void bb_ps5_log_line(const char *text) {
    pthread_mutex_lock(&log_lock);
    if (log_file) { fputs(text, log_file); fflush(log_file); }
    pthread_mutex_unlock(&log_lock);
    sceKernelDebugOutText(0, text);
}
static int log_pipe[2] = {-1, -1};
static void *log_main(void *unused) {
    (void)unused;
    char buffer[4096], line[4096 + 32];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(log_pipe[0], buffer + used, sizeof(buffer) - 1 - used);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        used += (size_t)n;
        size_t start = 0;
        for (size_t i = 0; i < used; ++i) {
            if (buffer[i] != '\n' && i - start < sizeof(buffer) - 2) continue;
            snprintf(line, sizeof(line), "[bbport] %.*s\n", (int)(i - start), buffer + start);
            bb_ps5_log_line(line);
            start = i + 1;
        }
        memmove(buffer, buffer + start, used - start);
        used -= start;
        if (used == sizeof(buffer) - 1) { buffer[used] = 0; bb_ps5_log_line(buffer); used = 0; }
    }
    return NULL;
}
static void log_start(void) {
    mkdir(PORT, 0777);
    rename(PORT "/last_run.log", PORT "/previous_run.log");
    log_file = fopen(PORT "/last_run.log", "w");
    if (pipe(log_pipe) == 0) {
        dup2(log_pipe[1], 1);
        dup2(log_pipe[1], 2);
        pthread_t thread;
        pthread_create(&thread, NULL, log_main, NULL);
        pthread_detach(thread);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

/* ---- settings ---- */
static void load_environment(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("PS5: no %s, defaults apply\n", path); return; }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || !*p) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *value = eq + 1, *end = value + strlen(value);
        while (end > value && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        setenv(p, value, 1);
        printf("PS5: %s=%s\n", p, value);
    }
    fclose(f);
}
static void defaults(void) {
    /* What run.py sets on a PC, for the console (each one can be overridden in the .env file). */
    setenv("BB_CONFIG", PORT "/bbport.ini", 0);
    setenv("BB_INPUT_CONFIG", PORT "/input.ini", 0);
    setenv("BB_GPU_USER_DIR", PORT "/user", 0);
    setenv("BB_UPSCALER", "fsr3", 0);
    /* The PC preparation's patches render the scene at 1916x1078; FSR 3.1 brings it to 4K. */
    setenv("BB_OUTPUT_RES", "3840x2160", 0);
    setenv("BB_RENDER_RES", "1916x1078", 0);
    setenv("BB_AUTO_RENDER_RES", "1", 0);
    /* Uncapped frame rate: the display at 120 Hz where the TV takes it (else 60). */
    setenv("BB_VBLANK_HZ", getenv("BB_FPS") && !strcmp(getenv("BB_FPS"), "uncap") ? "120" : "60", 0);
    setenv("BB_FULLSCREEN", "1", 0);
    setenv("BB_QUIT_TO_DESKTOP", "0", 0);
    setenv("BB_ONLINE", "0", 0);
    setenv("HOME", PORT "/user/home", 0);
}
static int exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

int main(void) {
    log_start();
    printf("PS5: Bloodborne (bbport) for PS5, test build " __DATE__ " " __TIME__ "\n");
    /* The shell's splash covers the title until it is dismissed. */
    printf("PS5: splash dismissed: %d\n", sceSystemServiceHideSplashScreen());
    load_environment(PORT "/bbport-ps5.env");
    defaults();
    mkdir(PORT "/user", 0777);
    mkdir(PORT "/user/home", 0777);
    const char *game = getenv("BB_GAME_DIR") ? getenv("BB_GAME_DIR") : ROOT "/CUSA03173";
    char eboot[512];
    snprintf(eboot, sizeof(eboot), "%s/eboot.bin", game);
    /* BB_FPS picks the game patches made on the PC (ps5/tools/prepare-game.sh): 60 or uncap. */
    static char patches[256];
    const char *fps = getenv("BB_FPS") ? getenv("BB_FPS") : "60";
    snprintf(patches, sizeof(patches), PORT "/patches-%s.bin", fps);
    if (!exists(patches)) snprintf(patches, sizeof(patches), PORT "/patches.bin");
    printf("PS5: game %s, patches %s\n", game, patches);
    const char *missing = !exists(eboot) ? eboot
                        : !exists(PORT "/boot-linked.bin") ? PORT "/boot-linked.bin"
                        : !exists(PORT "/content.bin") ? PORT "/content.bin"
                        : !exists(patches) ? patches : NULL;
    if (missing) {
        printf("STOP: %s is missing. Copy your game dump and the prepared files into the title "
               "folder (README-PS5.txt).\n", missing);
        sleep(1);
        return 2;
    }
    char *argv[] = {"bb-probe", PORT "/boot-linked.bin", "--content-profile", PORT "/content.bin",
                    "--patches", patches, "--app0", (char *)game, "--user", PORT "/user",
                    "--timeout", getenv("BB_TIMEOUT") ? getenv("BB_TIMEOUT") : "0", NULL};
    int status = bb_probe_main((int)(sizeof(argv) / sizeof(*argv)) - 1, argv);
    printf("PS5: the game ended with status %d\n", status);
    sleep(1);
    return status;
}
