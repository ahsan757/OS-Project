/*
 * watchdog.c  –  Background thread that enforces parking timeouts.
 *
 * Every second it scans all occupied slots.  If a slot has been
 * occupied longer than SLOT_TIMEOUT_SEC it is forcibly released.
 */

#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "parking.h"

void *watchdog_thread(void *arg)
{
    (void)arg;

    while (g_running) {
        sleep(1);

        time_t now = time(NULL);

        pthread_mutex_lock(&g_slot_mutex);

        for (int i = 0; i < TOTAL_SLOTS; i++) {
            if (g_slots[i].state != SLOT_OCCUPIED) continue;

            int age = (int)(now - g_slots[i].occupied_since);

            /* Has the vehicle exceeded its intended stay? */
            if (age >= g_slots[i].park_duration) {
                int vid    = g_slots[i].vehicle_id;
                int slotid = g_slots[i].id;
                bool is_vip = (i < VIP_SLOTS);

                /* Release the slot while still holding the mutex */
                g_slots[i].state          = SLOT_FREE;
                g_slots[i].vehicle_id     = -1;
                g_slots[i].vehicle_type   = VEHICLE_REGULAR;
                g_slots[i].occupied_since = 0;
                g_slots[i].park_duration  = 0;

                pthread_mutex_unlock(&g_slot_mutex);

                /* Post to the correct semaphore */
                if (is_vip) {
                    sem_post(&g_sem_vip);
                } else {
                    sem_post(&g_sem_regular);
                }

                log_event("TIMEOUT: Slot %2d auto-released (vehicle V%03d, age %ds)",
                          slotid, vid, age);

                pthread_mutex_lock(&g_stats_mutex);
                g_stats.total_timeouts++;
                pthread_mutex_unlock(&g_stats_mutex);

                /* Re-acquire mutex to continue the scan */
                pthread_mutex_lock(&g_slot_mutex);
            }
        }

        pthread_mutex_unlock(&g_slot_mutex);
    }

    return NULL;
}
