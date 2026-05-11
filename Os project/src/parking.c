/*
 * parking.c  –  Core parking logic, globals, slot management,
 *               wait-queue, statistics, and logging.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "parking.h"

/* ═══════════════════════════════════════════════════════════════
 *  Globals
 * ═══════════════════════════════════════════════════════════════ */
ParkingSlot     g_slots[TOTAL_SLOTS];
pthread_mutex_t g_slot_mutex  = PTHREAD_MUTEX_INITIALIZER;
sem_t           g_sem_regular;
sem_t           g_sem_vip;

WaitNode       *g_wait_head   = NULL;
WaitNode       *g_wait_tail   = NULL;
int             g_wait_count  = 0;
pthread_mutex_t g_wait_mutex  = PTHREAD_MUTEX_INITIALIZER;

Stats           g_stats       = {0, 0, 0, 0, 0};
pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

bool            g_running     = true;

static FILE    *g_log_fp      = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ═══════════════════════════════════════════════════════════════
 *  Initialisation / teardown
 * ═══════════════════════════════════════════════════════════════ */
void parking_init(void)
{
    /* Initialise slot table */
    for (int i = 0; i < TOTAL_SLOTS; i++) {
        g_slots[i].id             = i + 1;
        g_slots[i].state          = SLOT_FREE;
        g_slots[i].vehicle_id     = -1;
        g_slots[i].vehicle_type   = VEHICLE_REGULAR;
        g_slots[i].occupied_since = 0;
        g_slots[i].park_duration  = 0;
    }

    /*
     * Semaphore layout:
     *   g_sem_vip     – counts free VIP slots  (slots 0 .. VIP_SLOTS-1)
     *   g_sem_regular – counts free regular slots (slots VIP_SLOTS .. TOTAL_SLOTS-1)
     */
    sem_init(&g_sem_vip,     0, VIP_SLOTS);
    sem_init(&g_sem_regular, 0, TOTAL_SLOTS - VIP_SLOTS);

    /* Open log file */
    g_log_fp = fopen(LOG_FILE, "w");
    if (!g_log_fp) {
        perror("fopen log");
    }

    log_event("=== Parking system started. Slots: %d (VIP: %d, Regular: %d) ===",
              TOTAL_SLOTS, VIP_SLOTS, TOTAL_SLOTS - VIP_SLOTS);
}

void parking_destroy(void)
{
    /* Write the final log entry BEFORE destroying the log mutex */
    log_event("=== Parking system stopped. ===");

    sem_destroy(&g_sem_vip);
    sem_destroy(&g_sem_regular);
    pthread_mutex_destroy(&g_slot_mutex);
    pthread_mutex_destroy(&g_wait_mutex);
    pthread_mutex_destroy(&g_stats_mutex);

    /* Close the log file, then destroy its mutex */
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    pthread_mutex_unlock(&g_log_mutex);
    pthread_mutex_destroy(&g_log_mutex);
}

/* ═══════════════════════════════════════════════════════════════
 *  Slot assignment
 *
 *  Priority rules:
 *    VIP / EMERGENCY  → try VIP slots first, fall back to regular
 *    REGULAR          → regular slots only
 *
 *  Returns 0 on success, -1 if no slot available right now.
 *  (Callers that want to block should wait on the semaphore first.)
 * ═══════════════════════════════════════════════════════════════ */
int parking_assign(Vehicle *v, int *slot_out)
{
    pthread_mutex_lock(&g_slot_mutex);

    int assigned = -1;

    if (v->type == VEHICLE_VIP || v->type == VEHICLE_EMERGENCY) {
        /* Try VIP-reserved slots first */
        for (int i = 0; i < VIP_SLOTS && assigned == -1; i++) {
            if (g_slots[i].state == SLOT_FREE) {
                assigned = i;
            }
        }
        /* Fall back to regular slots */
        for (int i = VIP_SLOTS; i < TOTAL_SLOTS && assigned == -1; i++) {
            if (g_slots[i].state == SLOT_FREE) {
                assigned = i;
            }
        }
    } else {
        /* Regular vehicles only get regular slots */
        for (int i = VIP_SLOTS; i < TOTAL_SLOTS && assigned == -1; i++) {
            if (g_slots[i].state == SLOT_FREE) {
                assigned = i;
            }
        }
    }

    if (assigned != -1) {
        g_slots[assigned].state          = SLOT_OCCUPIED;
        g_slots[assigned].vehicle_id     = v->id;
        g_slots[assigned].vehicle_type   = v->type;
        g_slots[assigned].occupied_since = time(NULL);
        g_slots[assigned].park_duration  = v->park_duration;
        *slot_out = g_slots[assigned].id;
    }

    pthread_mutex_unlock(&g_slot_mutex);
    return (assigned == -1) ? -1 : 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  Slot release
 * ═══════════════════════════════════════════════════════════════ */
void parking_release(int slot_id)
{
    int idx = slot_id - 1;   /* slot IDs are 1-based */
    if (idx < 0 || idx >= TOTAL_SLOTS) return;

    pthread_mutex_lock(&g_slot_mutex);

    bool was_vip_slot = (idx < VIP_SLOTS);
    int  vid          = g_slots[idx].vehicle_id;

    g_slots[idx].state          = SLOT_FREE;
    g_slots[idx].vehicle_id     = -1;
    g_slots[idx].vehicle_type   = VEHICLE_REGULAR;
    g_slots[idx].occupied_since = 0;
    g_slots[idx].park_duration  = 0;

    pthread_mutex_unlock(&g_slot_mutex);

    /* Post to the correct semaphore */
    if (was_vip_slot) {
        sem_post(&g_sem_vip);
    } else {
        sem_post(&g_sem_regular);
    }

    log_event("Slot %2d released (vehicle V%03d)", slot_id, vid);
}

/* ═══════════════════════════════════════════════════════════════
 *  Wait queue
 * ═══════════════════════════════════════════════════════════════ */
void waitq_push(Vehicle *v)
{
    WaitNode *node = malloc(sizeof(WaitNode));
    if (!node) return;
    node->vehicle = *v;
    node->next    = NULL;

    pthread_mutex_lock(&g_wait_mutex);
    if (g_wait_tail) {
        g_wait_tail->next = node;
    } else {
        g_wait_head = node;
    }
    g_wait_tail = node;
    g_wait_count++;
    pthread_mutex_unlock(&g_wait_mutex);
}

Vehicle waitq_pop(void)
{
    Vehicle v = {-1, VEHICLE_REGULAR, 0};

    pthread_mutex_lock(&g_wait_mutex);
    if (g_wait_head) {
        WaitNode *node = g_wait_head;
        v              = node->vehicle;
        g_wait_head    = node->next;
        if (!g_wait_head) g_wait_tail = NULL;
        g_wait_count--;
        free(node);
    }
    pthread_mutex_unlock(&g_wait_mutex);

    return v;
}

void waitq_display(void)
{
    pthread_mutex_lock(&g_wait_mutex);
    printf("  Waiting queue (%d): ", g_wait_count);
    WaitNode *cur = g_wait_head;
    while (cur) {
        printf("V%03d(%s) ", cur->vehicle.id,
               vehicle_type_str(cur->vehicle.type));
        cur = cur->next;
    }
    if (!g_wait_head) printf("(empty)");
    printf("\n");
    pthread_mutex_unlock(&g_wait_mutex);
}

/* ═══════════════════════════════════════════════════════════════
 *  Logging
 * ═══════════════════════════════════════════════════════════════ */
void log_event(const char *fmt, ...)
{
    time_t    now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);   /* thread-safe */
    char      ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm);

    va_list ap;
    va_start(ap, fmt);

    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fp) {
        fprintf(g_log_fp, "[%s] ", ts);
        vfprintf(g_log_fp, fmt, ap);
        fprintf(g_log_fp, "\n");
        fflush(g_log_fp);
    }
    pthread_mutex_unlock(&g_log_mutex);

    va_end(ap);
}

/* ═══════════════════════════════════════════════════════════════
 *  Statistics display
 * ═══════════════════════════════════════════════════════════════ */
void stats_display(void)
{
    pthread_mutex_lock(&g_stats_mutex);
    Stats s = g_stats;
    pthread_mutex_unlock(&g_stats_mutex);

    /* Count currently occupied slots */
    pthread_mutex_lock(&g_slot_mutex);
    int occupied = 0;
    for (int i = 0; i < TOTAL_SLOTS; i++) {
        if (g_slots[i].state == SLOT_OCCUPIED) occupied++;
    }
    pthread_mutex_unlock(&g_slot_mutex);

    double avg_wait = (s.total_parked > 0)
                      ? (double)s.total_wait_ms / s.total_parked
                      : 0.0;

    printf("\n  ┌─────────────────────────────────────────┐\n");
    printf("  │           PARKING STATISTICS             │\n");
    printf("  ├─────────────────────────────────────────┤\n");
    printf("  │  Vehicles arrived   : %-5d              │\n", s.total_arrived);
    printf("  │  Successfully parked: %-5d              │\n", s.total_parked);
    printf("  │  Rejected (timeout) : %-5d              │\n", s.total_rejected);
    printf("  │  Auto-released      : %-5d              │\n", s.total_timeouts);
    printf("  │  Currently occupied : %-5d / %-5d      │\n", occupied, TOTAL_SLOTS);
    printf("  │  Avg wait time      : %-6.0f ms          │\n", avg_wait);
    printf("  │  Utilisation        : %-5.1f %%            │\n",
           TOTAL_SLOTS ? (100.0 * occupied / TOTAL_SLOTS) : 0.0);
    printf("  └─────────────────────────────────────────┘\n");
}
