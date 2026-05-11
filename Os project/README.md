# Smart Parking Allocation System

A concurrent parking management simulator built with POSIX threads, semaphores,
and mutexes on Linux.

## Project Structure

```
.
├── include/
│   └── parking.h        # All types, constants, and prototypes
├── src/
│   ├── main.c           # Entry point, thread spawning, dashboard loop
│   ├── parking.c        # Slot table, semaphores, wait queue, stats, logging
│   ├── dashboard.c      # ANSI console live visualisation
│   ├── watchdog.c       # Background timeout-enforcement thread
│   └── vehicle.c        # Vehicle thread logic (arrive → park → depart)
├── Makefile
└── README.md
```

## Build & Run

```bash
make          # compile
make run      # compile and run
make clean    # remove build artefacts
make valgrind # run under valgrind for memory checks
```

Requires GCC and pthreads on Linux (Ubuntu 20.04+ recommended).

## Configuration (include/parking.h)

| Constant          | Default | Meaning                                      |
|-------------------|---------|----------------------------------------------|
| `TOTAL_SLOTS`     | 10      | Total parking slots                          |
| `VIP_SLOTS`       | 2       | Slots reserved for VIP / emergency vehicles  |
| `MAX_VEHICLES`    | 30      | Number of vehicles to simulate               |
| `SLOT_TIMEOUT_SEC`| 12      | Auto-release after this many seconds         |
| `WAIT_TIMEOUT_SEC`| 8       | Max seconds a vehicle waits before leaving   |

## Synchronisation Design

```
                    ┌─────────────────────────────────────┐
                    │         Parking Slot Table           │
                    │  protected by  g_slot_mutex (mutex)  │
                    └──────────────┬──────────────────────┘
                                   │
              ┌────────────────────┼────────────────────┐
              │                    │                    │
       g_sem_vip             g_sem_regular         watchdog
    (counts free VIP      (counts free regular    (scans slots
      slots 1-2)           slots 3-10)             every 1s)
              │                    │
    VIP/EMERGENCY           REGULAR vehicles
      vehicles
```

- **Semaphores** gate entry: a vehicle blocks until a slot of the right
  category is free, preventing busy-waiting.
- **Mutex** protects the slot table so only one thread modifies it at a time,
  eliminating double-allocation races.
- **Priority**: VIP/EMERGENCY vehicles attempt `sem_trywait` on the VIP
  semaphore first; if all VIP slots are taken they fall back to regular slots.
- **Timeout watchdog**: a dedicated thread scans the table every second and
  forcibly releases slots that have exceeded their duration.

## Activity Log

All events are written to `parking.log` in the working directory:

```
[14:32:01] ARRIVE : V001  type=REGULAR    intended_stay=7s
[14:32:01] PARK   : V001  slot= 3  type=REGULAR    wait=12ms  stay=7s
[14:32:05] ARRIVE : V002  type=VIP        intended_stay=5s
[14:32:05] PARK   : V002  slot= 1  type=VIP        wait=8ms   stay=5s
[14:32:08] DEPART : V002  slot= 1  stayed=5s
[14:32:08] Slot  1 released (vehicle V002)
```
