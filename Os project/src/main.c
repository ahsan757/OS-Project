/*
 * main.c  –  Entry point for the Smart Parking Allocation System.
 *
 * Spawns MAX_VEHICLES vehicle threads with random arrival gaps,
 * random park durations, and a mix of vehicle types.
 * A watchdog thread enforces slot timeouts.
 * The main thread refreshes the dashboard every second.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

#include "parking.h"

/* ── Signal handler for clean shutdown ─────────────────────────── */
static volatile sig_atomic_t g_quit = 0;

static void handle_sigint(int sig)
{
    (void)sig;
    g_quit   = 1;
    g_running = false;
}

/* ── Random helpers ─────────────────────────────────────────────── */
static int rand_range(int lo, int hi)
{
    return lo + rand() % (hi - lo + 1);
}

static VehicleType rand_type(void)
{
    int r = rand() % 10;
    if (r == 0) return VEHICLE_EMERGENCY;   /* 10 % */
    if (r <= 2) return VEHICLE_VIP;         /* 20 % */
    return VEHICLE_REGULAR;                 /* 70 % */
}

/* ═══════════════════════════════════════════════════════════════
 *  main
 * ═══════════════════════════════════════════════════════════════ */
int main(void)
{
    srand((unsigned)time(NULL));

    /* Install signal handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Initialise parking system */
    parking_init();

    /* Start watchdog thread */
    pthread_t watchdog_tid;
    pthread_create(&watchdog_tid, NULL, watchdog_thread, NULL);

    /* Spawn vehicle threads */
    pthread_t tids[MAX_VEHICLES];
    bool      joined[MAX_VEHICLES];   /* track which threads are already joined */
    int       spawned = 0;

    for (int i = 0; i < MAX_VEHICLES; i++) joined[i] = false;

    printf("\033[2J\033[H");   /* clear screen before first render */
    fflush(stdout);

    for (int i = 0; i < MAX_VEHICLES && !g_quit; i++) {
        Vehicle *v = malloc(sizeof(Vehicle));
        if (!v) break;

        v->id           = i + 1;
        v->type         = rand_type();
        v->park_duration = rand_range(4, 15);   /* 4–15 seconds */

        if (pthread_create(&tids[spawned], NULL, vehicle_thread, v) != 0) {
            free(v);
            continue;
        }
        spawned++;

        /* Render dashboard after each arrival */
        dashboard_render();

        /* Random inter-arrival gap: 0.3 – 1.5 seconds */
        usleep(rand_range(300000, 1500000));
    }

    /* Keep refreshing the dashboard while vehicles are still active */
    while (!g_quit) {
        dashboard_render();
        sleep(1);

        /* Check if all threads have finished */
        bool any_alive = false;
        for (int i = 0; i < spawned; i++) {
            if (joined[i]) continue;
            if (pthread_tryjoin_np(tids[i], NULL) == 0) {
                joined[i] = true;
            } else {
                any_alive = true;
            }
        }
        if (!any_alive) break;
    }

    /* Final render */
    dashboard_render();

    /* Stop watchdog */
    g_running = false;
    pthread_join(watchdog_tid, NULL);

    /* Join any remaining vehicle threads */
    for (int i = 0; i < spawned; i++) {
        if (!joined[i]) {
            pthread_join(tids[i], NULL);
        }
    }

    /* Final stats */
    printf("\n");
    stats_display();
    printf("\n  Activity log written to: %s\n\n", LOG_FILE);

    parking_destroy();
    return 0;
}
