/*
 * vehicle.c  –  POSIX thread function for each arriving vehicle.
 *
 * Workflow per vehicle thread:
 *   1. Record arrival, update stats.
 *   2. Add self to the visual wait queue.
 *   3. Acquire the appropriate semaphore (timed – gives up after
 *      WAIT_TIMEOUT_SEC if no slot becomes free).
 *   4. Claim a slot under the mutex.
 *   5. Remove self from the wait queue.
 *   6. Occupy the slot for park_duration seconds (simulated sleep).
 *   7. Release the slot; parking_release() posts the correct semaphore.
 *   8. Free the Vehicle struct allocated by main.
 *
 * Semaphore accounting:
 *   g_sem_vip     – one token per VIP-reserved slot (slots 0..VIP_SLOTS-1)
 *   g_sem_regular – one token per regular slot      (slots VIP_SLOTS..TOTAL-1)
 *
 *   Priority vehicles (VIP/EMERGENCY) try g_sem_vip first (non-blocking).
 *   If no VIP slot is free they fall back to g_sem_regular.
 *   The slot index returned by parking_assign() determines which semaphore
 *   parking_release() will post to, so the two always stay in sync.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#include "parking.h"

/* Maximum seconds a vehicle will wait for a free slot before giving up */
#define WAIT_TIMEOUT_SEC  8

/* Helper: remove vehicle with given id from the wait queue */
static void waitq_remove(int vid)
{
    pthread_mutex_lock(&g_wait_mutex);
    WaitNode *prev = NULL, *cur = g_wait_head;
    while (cur) {
        if (cur->vehicle.id == vid) {
            if (prev) prev->next = cur->next;
            else       g_wait_head = cur->next;
            if (cur == g_wait_tail) g_wait_tail = prev;
            g_wait_count--;
            free(cur);
            break;
        }
        prev = cur;
        cur  = cur->next;
    }
    pthread_mutex_unlock(&g_wait_mutex);
}

void *vehicle_thread(void *arg)
{
    Vehicle *v = (Vehicle *)arg;

    /* ── 1. Arrival ─────────────────────────────────────────────── */
    pthread_mutex_lock(&g_stats_mutex);
    g_stats.total_arrived++;
    pthread_mutex_unlock(&g_stats_mutex);

    log_event("ARRIVE : V%03d  type=%-9s  intended_stay=%ds",
              v->id, vehicle_type_str(v->type), v->park_duration);

    /* ── 2. Join wait queue ─────────────────────────────────────── */
    waitq_push(v);

    /* ── 3. Acquire semaphore (timed) ───────────────────────────── */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += WAIT_TIMEOUT_SEC;

    struct timespec arrive_ts;
    clock_gettime(CLOCK_REALTIME, &arrive_ts);

    bool is_priority = (v->type == VEHICLE_VIP || v->type == VEHICLE_EMERGENCY);

    /*
     * acquired_vip tracks which semaphore we decremented so we can
     * post the right one on the error path.  On the happy path,
     * parking_release() handles the post based on slot index.
     */
    bool acquired_vip = false;
    int  sem_rc       = -1;

    if (is_priority) {
        /* Non-blocking attempt on VIP semaphore */
        if (sem_trywait(&g_sem_vip) == 0) {
            acquired_vip = true;
            sem_rc       = 0;
        } else {
            /* Fall back: wait on regular semaphore */
            sem_rc = sem_timedwait(&g_sem_regular, &deadline);
        }
    } else {
        sem_rc = sem_timedwait(&g_sem_regular, &deadline);
    }

    if (sem_rc != 0) {
        /* Timed out – vehicle leaves without parking */
        waitq_remove(v->id);

        log_event("REJECT : V%03d  waited %ds, no slot available",
                  v->id, WAIT_TIMEOUT_SEC);

        pthread_mutex_lock(&g_stats_mutex);
        g_stats.total_rejected++;
        pthread_mutex_unlock(&g_stats_mutex);

        free(v);
        return NULL;
    }

    /* ── 4. Claim a slot under the mutex ────────────────────────── */
    struct timespec parked_ts;
    clock_gettime(CLOCK_REALTIME, &parked_ts);

    long wait_ms = (parked_ts.tv_sec  - arrive_ts.tv_sec)  * 1000L
                 + (parked_ts.tv_nsec - arrive_ts.tv_nsec) / 1000000L;

    int slot_id = -1;
    if (parking_assign(v, &slot_id) != 0) {
        /*
         * Defensive: semaphore said a slot is free but none was found.
         * Return the token to the semaphore we decremented.
         */
        log_event("ERROR  : V%03d  semaphore acquired but no slot found!", v->id);
        if (acquired_vip) sem_post(&g_sem_vip);
        else              sem_post(&g_sem_regular);
        waitq_remove(v->id);
        free(v);
        return NULL;
    }

    /*
     * If we grabbed g_sem_vip but parking_assign placed us in a regular
     * slot (fallback inside assign), we need to fix the semaphore counts:
     * post back g_sem_vip and decrement g_sem_regular instead.
     * The slot index tells us which pool the slot belongs to.
     */
    bool slot_is_vip = (slot_id - 1) < VIP_SLOTS;   /* slot_id is 1-based */
    if (acquired_vip && !slot_is_vip) {
        sem_post(&g_sem_vip);          /* return the VIP token */
        /* We already hold a regular slot; decrement regular semaphore */
        sem_trywait(&g_sem_regular);   /* should succeed immediately */
    }

    /* ── 5. Remove from wait queue ──────────────────────────────── */
    waitq_remove(v->id);

    /* ── Update stats ───────────────────────────────────────────── */
    pthread_mutex_lock(&g_stats_mutex);
    g_stats.total_parked++;
    g_stats.total_wait_ms += wait_ms;
    pthread_mutex_unlock(&g_stats_mutex);

    log_event("PARK   : V%03d  slot=%2d  type=%-9s  wait=%ldms  stay=%ds",
              v->id, slot_id, vehicle_type_str(v->type),
              wait_ms, v->park_duration);

    /* ── 6. Occupy slot ─────────────────────────────────────────── */
    sleep((unsigned int)v->park_duration);

    /* ── 7. Release slot ────────────────────────────────────────── */
    log_event("DEPART : V%03d  slot=%2d  stayed=%ds",
              v->id, slot_id, v->park_duration);

    /*
     * parking_release() clears the slot and posts the correct semaphore
     * based on whether slot_id is in the VIP range or not.
     */
    parking_release(slot_id);

    free(v);
    return NULL;
}
