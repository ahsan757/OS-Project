#ifndef PARKING_H
#define PARKING_H

#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <stdbool.h>

/* ── Configuration ─────────────────────────────────────────────── */
#define TOTAL_SLOTS       10
#define MAX_VEHICLES      30
#define VIP_SLOTS          2   /* first N slots reserved for VIP/emergency */
#define SLOT_TIMEOUT_SEC  12   /* auto-release after this many seconds idle */
#define LOG_FILE          "parking.log"

/* ── Vehicle types ─────────────────────────────────────────────── */
typedef enum {
    VEHICLE_REGULAR   = 0,
    VEHICLE_VIP       = 1,
    VEHICLE_EMERGENCY = 2
} VehicleType;

static inline const char *vehicle_type_str(VehicleType t) {
    switch (t) {
        case VEHICLE_VIP:       return "VIP      ";
        case VEHICLE_EMERGENCY: return "EMERGENCY";
        default:                return "REGULAR  ";
    }
}

/* ── Slot states ───────────────────────────────────────────────── */
typedef enum {
    SLOT_FREE     = 0,
    SLOT_OCCUPIED = 1
} SlotState;

/* ── Parking slot ──────────────────────────────────────────────── */
typedef struct {
    int         id;
    SlotState   state;
    int         vehicle_id;
    VehicleType vehicle_type;
    time_t      occupied_since;   /* wall-clock time when slot was taken */
    int         park_duration;    /* seconds the vehicle intends to stay */
} ParkingSlot;

/* ── Vehicle descriptor (thread argument) ──────────────────────── */
typedef struct {
    int         id;
    VehicleType type;
    int         park_duration;   /* seconds to stay (simulated) */
} Vehicle;

/* ── Wait-queue node ───────────────────────────────────────────── */
typedef struct WaitNode {
    Vehicle          vehicle;
    struct WaitNode *next;
} WaitNode;

/* ── Statistics ────────────────────────────────────────────────── */
typedef struct {
    int  total_arrived;
    int  total_parked;
    int  total_rejected;   /* timed-out while waiting */
    int  total_timeouts;   /* auto-released due to inactivity */
    long total_wait_ms;    /* cumulative wait time for parked vehicles */
} Stats;

/* ── Globals (defined in parking.c) ───────────────────────────── */
extern ParkingSlot     g_slots[TOTAL_SLOTS];
extern pthread_mutex_t g_slot_mutex;
extern sem_t           g_sem_regular;   /* counts free regular slots */
extern sem_t           g_sem_vip;       /* counts free VIP slots     */

extern WaitNode       *g_wait_head;
extern WaitNode       *g_wait_tail;
extern int             g_wait_count;
extern pthread_mutex_t g_wait_mutex;

extern Stats           g_stats;
extern pthread_mutex_t g_stats_mutex;

extern bool            g_running;       /* set false to stop watchdog */

/* ── Function prototypes ───────────────────────────────────────── */

/* core */
void    parking_init(void);
void    parking_destroy(void);
int     parking_assign(Vehicle *v, int *slot_out);   /* 0 = success */
void    parking_release(int slot_id);

/* wait queue */
void    waitq_push(Vehicle *v);
Vehicle waitq_pop(void);
void    waitq_display(void);

/* dashboard */
void    dashboard_render(void);

/* log */
void    log_event(const char *fmt, ...);

/* stats */
void    stats_display(void);

/* watchdog (timeout thread) */
void   *watchdog_thread(void *arg);

/* vehicle thread */
void   *vehicle_thread(void *arg);

#endif /* PARKING_H */
