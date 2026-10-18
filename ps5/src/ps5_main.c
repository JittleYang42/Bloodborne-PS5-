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
#include <signal.h>

int bb_probe_main(int argc, char **argv);
int sceSystemServiceHideSplashScreen(void);

#define ROOT "/app0"
#define PORT ROOT "/bbport"
const char *bb_ps5_app_root(void) { return ROOT; }

/* ---- the log ----
 * stdout and stderr are the log file itself (no pipe or reader thread that could stop and block
 * the game), line-buffered and unbuffered; every line reaches the file as it is printed. The
 * first test build's pipe left last_run.log empty. A one-line marker file, written and synced
 * with plain write(), shows whether the title could write to its folder at all. */
static int log_fd = -1;
void bb_ps5_log_line(const char *text) {
    if (log_fd >= 0) { ssize_t w = write(log_fd, text, strlen(text)); (void)w; }
    sceKernelDebugOutText(0, text);
}
static void log_start(void) {
    mkdir(PORT, 0777);
    int marker = open(PORT "/started.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (marker >= 0) {
        static const char text[] = "bbport PS5 test build " __DATE__ " " __TIME__ " started\n";
        ssize_t w = write(marker, text, sizeof(text) - 1); (void)w;
        fsync(marker);
        close(marker);
    }
    rename(PORT "/last_run.log", PORT "/previous_run.log");
    log_fd = open(PORT "/last_run.log", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
    if (log_fd < 0) { sceKernelDebugOutText(0, "[bbport] cannot open bbport/last_run.log\n"); return; }
    dup2(log_fd, 1);
    dup2(log_fd, 2);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("PS5: log open\n");
    fflush(stdout);
    /* If the console's stdio does not write through descriptor 1, reopen the streams by name. */
    struct stat st;
    if (fstat(log_fd, &st) == 0 && st.st_size == 0) {
        freopen(PORT "/last_run.log", "a", stdout);
        freopen(PORT "/last_run.log", "a", stderr);
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        printf("PS5: log reopened through stdio\n");
        fflush(stdout);
    }
}

/* ---- heartbeat: what the game is doing when it seems stuck ----
 * A line every 10 s; at 30 s, 60 s, 120 s and every 2 minutes after, the runtime's counters and
 * a stack dump of every guest thread (SIGUSR2: guest offsets for the code the game runs). */
void runtime_report(void);
void runtime_thread_signal_all(int sig);
static void *heartbeat_main(void *unused) {
    (void)unused;
    for (unsigned seconds = 10;; seconds += 10) {
        sleep(10);
        printf("PS5: heartbeat %u s\n", seconds);
        if (seconds == 30 || seconds == 60 || seconds % 120 == 0) {
            printf("PS5: ---- runtime state at %u s ----\n", seconds);
            runtime_report();
            printf("PS5: ---- guest thread stacks at %u s ----\n", seconds);
            runtime_thread_signal_all(SIGUSR2);
            sleep(2);
            printf("PS5: ---- end of state ----\n");
        }
    }
    return NULL;
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
    pthread_t heartbeat;
    if (pthread_create(&heartbeat, NULL, heartbeat_main, NULL) == 0) pthread_detach(heartbeat);
    int status = bb_probe_main((int)(sizeof(argv) / sizeof(*argv)) - 1, argv);
    printf("PS5: the game ended with status %d\n", status);
    sleep(1);
    return status;
}
