/*
 * dashboard.c  –  ANSI console dashboard for real-time slot visualisation.
 *
 * Uses ANSI escape codes to redraw the parking grid in-place so the
 * terminal doesn't scroll.  Works on any Linux terminal that supports
 * ANSI sequences (xterm, gnome-terminal, etc.).
 */

#include <stdio.h>
#include <time.h>
#include <string.h>

#include "parking.h"

/* ANSI colour helpers */
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_WHITE   "\033[37m"
#define ANSI_BG_RED  "\033[41m"
#define ANSI_BG_GRN  "\033[42m"
#define ANSI_BG_YEL  "\033[43m"
#define ANSI_BG_BLU  "\033[44m"

/* Move cursor to top-left and clear screen on first call, then just home */
static int first_render = 1;

/* Number of lines the dashboard occupies (used to move cursor back up) */
#define DASHBOARD_LINES  28

static void cursor_home(void)
{
    if (first_render) {
        printf("\033[2J");   /* clear entire screen */
        first_render = 0;
    }
    printf("\033[H");        /* move cursor to row 1, col 1 */
    fflush(stdout);
}

/* ─── slot colour based on type ─────────────────────────────────── */
static const char *slot_colour(VehicleType t)
{
    switch (t) {
        case VEHICLE_VIP:       return ANSI_BG_BLU ANSI_WHITE ANSI_BOLD;
        case VEHICLE_EMERGENCY: return ANSI_BG_RED ANSI_WHITE ANSI_BOLD;
        default:                return ANSI_BG_GRN ANSI_WHITE ANSI_BOLD;
    }
}

/* ─── elapsed seconds since a slot was occupied ─────────────────── */
static int elapsed(time_t since)
{
    return (int)(time(NULL) - since);
}

/* ═══════════════════════════════════════════════════════════════
 *  Main render function – called periodically from main thread
 * ═══════════════════════════════════════════════════════════════ */
void dashboard_render(void)
{
    cursor_home();

    /* ── Header ─────────────────────────────────────────────────── */
    time_t    now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);   /* thread-safe */
    char      ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d  %H:%M:%S", tm);

    printf(ANSI_BOLD ANSI_CYAN
           "╔══════════════════════════════════════════════════════════╗\n"
           "║       SMART PARKING ALLOCATION SYSTEM  –  LIVE VIEW     ║\n"
           "║  %-56s  ║\n"
           "╚══════════════════════════════════════════════════════════╝\n"
           ANSI_RESET, ts);

    /* ── Legend ─────────────────────────────────────────────────── */
    printf("  Legend: "
           ANSI_BG_GRN ANSI_WHITE " REGULAR " ANSI_RESET "  "
           ANSI_BG_BLU ANSI_WHITE " VIP     " ANSI_RESET "  "
           ANSI_BG_RED ANSI_WHITE " EMERG   " ANSI_RESET "  "
           ANSI_BG_YEL ANSI_WHITE " FREE    " ANSI_RESET
           "\n\n");

    /* ── Slot grid ──────────────────────────────────────────────── */
    printf("  %-4s  %-9s  %-10s  %-9s  %-8s  %-6s\n",
           "SLOT", "STATUS", "VEHICLE", "TYPE", "DURATION", "ELAPSED");
    printf("  ────  ─────────  ──────────  ─────────  ────────  ──────\n");

    pthread_mutex_lock(&g_slot_mutex);

    for (int i = 0; i < TOTAL_SLOTS; i++) {
        ParkingSlot *s = &g_slots[i];

        /* VIP-reserved marker */
        const char *vip_mark = (i < VIP_SLOTS) ? "*" : " ";

        if (s->state == SLOT_FREE) {
            printf("  %s%2d%s  "
                   ANSI_BG_YEL ANSI_WHITE " FREE    " ANSI_RESET
                   "  %-10s  %-9s  %-8s  %-6s\n",
                   ANSI_BOLD, s->id, ANSI_RESET,
                   "---", "---", "---", vip_mark);
        } else {
            int   secs = elapsed(s->occupied_since);
            int   rem  = s->park_duration - secs;
            char  dur_buf[16], ela_buf[16];
            snprintf(dur_buf, sizeof(dur_buf), "%ds", s->park_duration);
            snprintf(ela_buf, sizeof(ela_buf), "%ds", secs);

            /* Warn if close to timeout */
            const char *time_col = (rem <= 3 && rem >= 0)
                                   ? ANSI_RED ANSI_BOLD : ANSI_RESET;

            char vid_buf[16];
            snprintf(vid_buf, sizeof(vid_buf), "V%03d", s->vehicle_id);

            printf("  %s%2d%s  %s%-9s%s  %-10s  %-9s  %s%-8s%s  %-6s\n",
                   ANSI_BOLD, s->id, ANSI_RESET,
                   slot_colour(s->vehicle_type), "OCCUPIED", ANSI_RESET,
                   vid_buf,
                   vehicle_type_str(s->vehicle_type),
                   time_col, dur_buf, ANSI_RESET,
                   vip_mark);
            (void)ela_buf; /* suppress unused warning */
        }
    }

    pthread_mutex_unlock(&g_slot_mutex);

    printf("  ────  ─────────  ──────────  ─────────  ────────  ──────\n");
    printf("  (* = VIP-reserved slot)\n\n");

    /* ── Wait queue ─────────────────────────────────────────────── */
    waitq_display();

    /* ── Stats summary ──────────────────────────────────────────── */
    pthread_mutex_lock(&g_stats_mutex);
    Stats s = g_stats;
    pthread_mutex_unlock(&g_stats_mutex);

    pthread_mutex_lock(&g_slot_mutex);
    int occupied = 0;
    for (int i = 0; i < TOTAL_SLOTS; i++) {
        if (g_slots[i].state == SLOT_OCCUPIED) occupied++;
    }
    pthread_mutex_unlock(&g_slot_mutex);

    /* Utilisation bar (20 chars wide) */
    int bar_filled = (TOTAL_SLOTS > 0) ? (occupied * 20 / TOTAL_SLOTS) : 0;
    printf("  Utilisation [");
    for (int i = 0; i < 20; i++) {
        if (i < bar_filled)
            printf(ANSI_BG_GRN " " ANSI_RESET);
        else
            printf(ANSI_BG_YEL " " ANSI_RESET);
    }
    printf("] %d/%d  (arrived:%d parked:%d rejected:%d timeouts:%d)\n",
           occupied, TOTAL_SLOTS,
           s.total_arrived, s.total_parked,
           s.total_rejected, s.total_timeouts);

    fflush(stdout);
}
