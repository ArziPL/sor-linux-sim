# SOR Simulator — Architecture & Technical Reference

## 1. Project Overview

Linux C++17 simulation of an Emergency Room (Szpitalny Oddział Ratunkowy).
Built with CMake, uses System V IPC (shared memory, semaphores, message queues),
POSIX signals, POSIX threads (pthreads), and fork()+exec() for process management.

**Source files:**
- `src/sor_common.hpp` — shared header (constants, enums, IPC helpers, structs)
- `src/main.cpp` → executable `dyrektor` — director process (orchestrator)
- `src/rejestracja.cpp` → executable `rejestracja` — registration process
- `src/lekarz.cpp` → executable `lekarz` — doctor process (7 instances)
- `src/pacjent.cpp` → executable `pacjent` — patient process (dynamic)

---

## 2. Process Tree

```
dyrektor (main.cpp)
├── rejestracja          — fork()+exec(), 1 process, 3 threads
│   ├── [main thread]    — okienko 1 (registration window 1)
│   ├── windowThread     — okienko 2 (registration window 2, activatable)
│   └── controllerThread — queue length monitor
├── lekarz 0 (POZ)       — fork()+exec(), triage doctor
├── lekarz 1 (kardiolog) — fork()+exec(), specialist
├── lekarz 2 (neurolog)  — fork()+exec(), specialist
├── lekarz 3 (okulista)  — fork()+exec(), specialist
├── lekarz 4 (laryngolog)— fork()+exec(), specialist
├── lekarz 5 (chirurg)   — fork()+exec(), specialist
├── lekarz 6 (pediatra)  — fork()+exec(), specialist
└── generator            — fork() (no exec), child of dyrektor
    ├── pacjent 1        — fork()+exec()
    ├── pacjent 2        — fork()+exec()
    ├── ...
    └── pacjent N        — fork()+exec()
```

**Total direct children of dyrektor:** 9 (1 rejestracja + 7 lekarzy + 1 generator).
Patients are grandchildren (forked by generator, exec'd as separate processes).

---

## 3. Startup Sequence

```
dyrektor main():
  1. setupSignals()          — register SIGINT, SIGTERM, SIGHUP handlers; atexit(cleanupIPC)
  2. initIPC()               — create all IPC resources (see §5)
  3. Write start time, director PID, log path to SharedState
  4. Clear log file
  5. startRegistration()     — fork()+exec("./rejestracja")
  6. startDoctors()          — fork()+exec("./lekarz", type) × 7
  7. setRawTerminal()        — disable canonical mode for keyboard input
  8. fork() generator child  — generator runs generatePatients()
  9. handleKeyboard()        — main loop: select() on stdin with 100ms timeout
```

Each child process (rejestracja, lekarz, pacjent) on startup:
1. Attaches to existing shared memory via `shmget(key, 0)` + `shmat()`
2. Attaches to existing semaphore set via `semget(key, 0)`
3. Attaches to existing message queue via `msgget(key, 0)`
4. Sets up its own signal handlers

---

## 4. Shutdown Sequence

Triggered by: key `q`, key `7` (evacuation/SIGUSR2), or SIGINT/SIGTERM.

```
dyrektor:
  1. Set g_state->shutdown = 1      — visible to all processes via shared memory
  2. Send SIGTERM to all 9 direct children
  3. usleep(500ms)                  — grace period for cleanup
  4. Send SIGKILL to all 9 direct children
  5. Blocking waitpid() for each child — no zombies
  6. cleanupIPC()                   — destroy all IPC resources
     (also called via atexit on abnormal exit)
```

Generator reaps its own patient children via `waitpid(-1, WNOHANG)` after each fork.

---

## 5. IPC Resources

### 5.1 Shared Memory (1 segment)

| Key | ID char | Content |
|-----|---------|---------|
| `ftok("/tmp", 'S')` | `'S'` | `SharedState` struct |

**SharedState fields:**

| Field | Type | Purpose |
|-------|------|---------|
| `start_time_sec`, `start_time_nsec` | `time_t`, `long` | CLOCK_MONOTONIC start for log timestamps |
| `shutdown` | `volatile sig_atomic_t` | Global shutdown flag (visible to all processes) |
| `director_pid` | `pid_t` | PID of dyrektor |
| `registration_pid` | `pid_t` | PID of rejestracja |
| `doctor_pids[7]` | `pid_t[]` | PIDs of all doctors (for sending signals) |
| `reg_window_2_open` | `int` | Whether registration window 2 is active |
| `reg_queue_count` | `int` | Current registration queue length |
| `total_patients` | `int` | Total generated patients counter |
| `patients_in_sor` | `int` | Current occupancy of the building |
| `doctor_on_break[7]` | `volatile sig_atomic_t[]` | Per-doctor break status |
| `specialist_msgids[7]` | `int[]` | Message queue IDs for specialist queues |
| `log_file[256]` | `char[]` | Log file path |

**Access:** Protected by `SEM_SHM_MUTEX` semaphore (semWait/semSignal around reads/writes).

### 5.2 Semaphores (1 set, 13 semaphores)

| Index | Name | Init | Purpose |
|-------|------|------|---------|
| 0 | `SEM_POCZEKALNIA` | N=20 | Counting semaphore — available seats in waiting room. Adult: semop(-1) to enter, semop(+1) to exit. Child+guardian: atomic semop(-2) / semop(+2). |
| 1 | `SEM_REG_QUEUE_MUTEX` | 1 | Binary mutex — protects `reg_queue_count` in SharedState. *(Currently used only via SEM_SHM_MUTEX — kept for future use.)* |
| 2 | `SEM_REG_WINDOW_1` | 1 | Registration window 1 availability. *(Not actively semWait'd — msgrcv serves as the blocking mechanism.)* |
| 3 | `SEM_REG_WINDOW_2` | 0 | Registration window 2 availability. *(Not actively semWait'd — pthread_cond controls activation.)* |
| 4 | `SEM_SPECIALIST_KARDIOLOG` | 1 | Binary — cardiologist availability (1=free, 0=busy) |
| 5 | `SEM_SPECIALIST_NEUROLOG` | 1 | Binary — neurologist availability |
| 6 | `SEM_SPECIALIST_OKULISTA` | 1 | Binary — ophthalmologist availability |
| 7 | `SEM_SPECIALIST_LARYNGOLOG` | 1 | Binary — ENT specialist availability |
| 8 | `SEM_SPECIALIST_CHIRURG` | 1 | Binary — surgeon availability |
| 9 | `SEM_SPECIALIST_PEDIATRA` | 1 | Binary — pediatrician availability |
| 10 | `SEM_SHM_MUTEX` | 1 | Binary mutex — protects all SharedState field modifications |
| 11 | `SEM_LOG_MUTEX` | 1 | Binary mutex — serializes log writes (file + stdout) |
| 12 | `SEM_REG_QUEUE_CHANGED` | 0 | Event semaphore — patient signals after joining/leaving registration queue; controller blocks on semWait until signaled. Zero CPU in idle. |

**Key:** `ftok("/tmp", 'E')`

### 5.3 Message Queues (7 queues)

#### Main Queue (1)

| Key | ID char | Purpose |
|-----|---------|---------|
| `ftok("/tmp", 'M')` | `'M'` | Registration + triage + all responses |

**Message types (mtype) on main queue:**

| mtype | Direction | Meaning |
|-------|-----------|---------|
| 1 (`MSG_PATIENT_TO_REGISTRATION_VIP`) | patient → rejestracja | VIP patient registration request |
| 2 (`MSG_PATIENT_TO_REGISTRATION`) | patient → rejestracja | Regular patient registration request |
| 3 (`MSG_PATIENT_TO_TRIAGE`) | patient → POZ doctor | Triage request (after registration) |
| 100 + patient_id | rejestracja → patient | Registration complete response |
| 150 + patient_id | POZ → patient | Triage result (color + assigned specialist) |
| 200 + patient_id | specialist → patient | Treatment result (outcome) |

**VIP priority mechanism:** Registration windows call `msgrcv(msgid, &msg, size, -2, 0)`.
Negative mtype means "receive message with mtype ≤ |2|, lowest first".
VIP (mtype=1) is always picked before regular (mtype=2).

#### Specialist Queues (6)

Each specialist (excluding POZ) has a **dedicated** message queue:

| Doctor | Key | mtype meaning |
|--------|-----|---------------|
| Kardiolog | `ftok("/tmp", 'a')` | 1=RED, 2=YELLOW, 3=GREEN |
| Neurolog | `ftok("/tmp", 'b')` | 1=RED, 2=YELLOW, 3=GREEN |
| Okulista | `ftok("/tmp", 'c')` | 1=RED, 2=YELLOW, 3=GREEN |
| Laryngolog | `ftok("/tmp", 'd')` | 1=RED, 2=YELLOW, 3=GREEN |
| Chirurg | `ftok("/tmp", 'e')` | 1=RED, 2=YELLOW, 3=GREEN |
| Pediatra | `ftok("/tmp", 'f')` | 1=RED, 2=YELLOW, 3=GREEN |

**Color priority mechanism:** Specialist calls `msgrcv(spec_msgid, &msg, size, -3, 0)`.
This picks the message with lowest mtype first: RED(1) before YELLOW(2) before GREEN(3).
Fully blocking — zero CPU while waiting.

Queue IDs are stored in `SharedState::specialist_msgids[]` so patient processes can look them up.

### 5.4 Message Struct

```cpp
struct SORMessage {
    long mtype;              // Required by System V
    int patient_id;
    int patient_pid;
    int age;
    int is_vip;
    TriageColor color;
    DoctorType assigned_doctor;
    int outcome;             // 0=home, 1=hospital ward, 2=other facility
};
```

---

## 6. Signal Handling

| Signal | Sender | Receiver | Effect |
|--------|--------|----------|--------|
| `SIGUSR1` | dyrektor (keyboard 1-6) | specialist doctor | Sets `g_go_to_ward=1`. After finishing current patient, doctor takes a break (randomSleep). If idle (blocked on msgrcv), EINTR fires → immediate break. |
| `SIGUSR1` | rejestracja controller thread | rejestracja window 2 thread | Interrupts blocked `msgrcv()` with EINTR → window 2 thread checks `g_window2_should_run` and deactivates. |
| `SIGUSR2` | dyrektor (keyboard 7) | all doctors, all children | Evacuation — sets `g_shutdown=1`, process exits. |
| `SIGTERM` | dyrektor (cleanup) | all direct children | Graceful shutdown signal. |
| `SIGKILL` | dyrektor (cleanup) | remaining children | Forced kill after 500ms grace period. |
| `SIGINT` | terminal (Ctrl+C) | dyrektor | Sets shutdown flag and begins cleanup. |

**All signal handlers use `sa_flags = 0`** (no SA_RESTART) — blocking `msgrcv()`, `semop()`, and `select()` are interrupted by signals, returning EINTR. This is intentional for responsive shutdown.

---

## 7. Patient Flow (Complete Path)

### 7.1 Adult Patient (single thread)

```
Generator: fork()+exec("./pacjent", id, age, vip)

pacjent main():
  1. enterWaitingRoom()
     - semWait(SEM_POCZEKALNIA)           — blocks if building full (20 seats)
     - SHM: patients_in_sor++
     - Log: "wchodzi do budynku (X/20)"

  2. doRegistration()
     - SHM: reg_queue_count++
     - semSignal(SEM_REG_QUEUE_CHANGED)   — wake controller thread
     - msgsnd(main_queue, mtype=1|2)      — VIP=1, regular=2
     - Log: "dołącza do kolejki rejestracji"
     - msgrcv(main_queue, mtype=100+id)   — block until registration done
     —— rejestracja processes, sends response ——

  3. doTriage()
     - msgsnd(main_queue, mtype=3)        — triage request
     - msgrcv(main_queue, mtype=150+id)   — block until triage done
     —— POZ doctor processes, sends color + specialist ——
     - If COLOR_SENT_HOME → skip to step 5

  4. doSpecialist()
     - msgsnd(specialist_queue[doctor], mtype=colorToMtype(color))
     - Log: "czeka na lekarza: X (kolor: Y)"
     - msgrcv(main_queue, mtype=200+id)   — block until treatment done
     —— specialist processes, sends outcome ——

  5. exitSOR()
     - SHM: patients_in_sor--
     - semSignal(SEM_POCZEKALNIA)         — free 1 seat
     - Log: "opuszcza SOR"
```

### 7.2 Child Patient (age < 18, two threads)

```
pacjent main():
  - pthread_create(parentThread)    — guardian handles registration
  - pthread_create(childThread)     — child handles triage + treatment

  parentThread (Opiekun):
    1. enterWaitingRoom()
       - atomic semop(-2, SEM_POCZEKALNIA) — occupy 2 seats at once (prevents deadlock)
       - SHM: patients_in_sor += 2
    2. doRegistration()              — same as adult
    3. pthread_cond_signal()         — notify childThread that registration is done

  childThread (Dziecko):
    1. pthread_cond_wait()           — wait for registration to complete
    2. doTriage()                    — same as adult
    3. doSpecialist()                — always assigned to pediatra
       (if not sent home from triage)

  main thread (after both join):
    exitSOR()
       - SHM: patients_in_sor -= 2
       - atomic semop(+2, SEM_POCZEKALNIA) — free 2 seats
```

**Why atomic semop(-2)?** If two child patients each grabbed 1 seat and both needed 2,
neither could proceed → classic deadlock. Atomic `-2` waits until ≥2 seats are free.

---

## 8. Registration Process Detail

```
rejestracja main():
  3 threads total:

  [main thread] — Window 1 (always active):
    loop:
      msgrcv(main_queue, mtype ≤ -2, blocking)   — VIP priority
      processPatient(1, msg):
        SHM: reg_queue_count--
        semSignal(SEM_REG_QUEUE_CHANGED)
        randomSleep(REGISTRATION_MIN_MS..MAX_MS)
        msgsnd(main_queue, mtype=100+patient_id)  — response

  [windowThread] — Window 2 (activatable):
    loop:
      pthread_cond_wait()                         — sleep until activated
      while active:
        msgrcv(main_queue, mtype ≤ -2, blocking)  — same VIP priority
        processPatient(2, msg)
      deactivated → back to cond_wait

  [controllerThread] — Queue monitor:
    loop:
      semWait(SEM_REG_QUEUE_CHANGED)              — zero-CPU blocking wait
      read SHM: reg_queue_count, reg_window_2_open
      if !open && count >= K_OPEN(10):
        SHM: reg_window_2_open = 1
        pthread_cond_signal() → activate window 2
      elif open && count < K_CLOSE(6):
        SHM: reg_window_2_open = 0
        g_window2_should_run = false
        pthread_kill(window2_thread, SIGUSR1)     — interrupt blocked msgrcv
```

---

## 9. Doctor Process Detail

### 9.1 POZ (Triage) Doctor

```
lekarz main(type=0):
  loop:
    msgrcv(main_queue, mtype=3, blocking)         — wait for triage patient
    randomSleep(TRIAGE_MIN_MS..MAX_MS)
    color = randomTriageColor()

    if COLOR_SENT_HOME (5%):
      msgsnd(main_queue, mtype=150+id, outcome=0)

    else (95%):
      specialist = randomSpecialist(age)
        - children (<18) → always PEDIATRA
        - adults → random among 5 specialists
      msgsnd(main_queue, mtype=150+id, color + specialist)

  POZ does NOT react to SIGUSR1 (no ward breaks).
```

### 9.2 Specialist Doctor

```
lekarz main(type=1..6):
  spec_msgid = SharedState::specialist_msgids[type]

  loop:
    msgrcv(spec_msgid, mtype ≤ -3, blocking)      — color priority
    if EINTR && g_go_to_ward && !g_treating:
      goToWard() → randomSleep(BREAK_MIN..MAX) → return
      continue

    semWait(SEM_SPECIALIST_X)                      — mark as busy
    g_treating = 1
    randomSleep(TREATMENT_MIN_MS..MAX_MS)
    outcome = randomOutcome()
      - 85% home, 14.5% hospital ward, 0.5% other facility
    msgsnd(main_queue, mtype=200+patient_id)       — response to patient
    g_treating = 0
    semSignal(SEM_SPECIALIST_X)                    — mark as free

    if g_go_to_ward:
      goToWard()
```

---

## 10. Timing Parameters

| Parameter | Min (ms) | Max (ms) | Where |
|-----------|----------|----------|-------|
| Patient generation interval | 500 | 1000 | Generator loop |
| Registration processing | 500 | 1000 | processPatient() |
| Triage assessment | 1000 | 2000 | POZ runPOZ() |
| Specialist treatment | 3000 | 5000 | runSpecialist() |
| Doctor ward break | 500 | 1000 | goToWard() |

## 11. Probability Distributions

| Event | Probabilities |
|-------|--------------|
| Triage color | 10% RED, 35% YELLOW, 50% GREEN, 5% SENT_HOME |
| Patient age | 20% child (1-17), 80% adult (18-90) |
| VIP status | 10% VIP, 90% regular |
| Specialist (adult) | Equal 20% each: kardiolog, neurolog, okulista, laryngolog, chirurg |
| Specialist (child) | 100% pediatra |
| Treatment outcome | 85% home, 14.5% hospital ward, 0.5% other facility |

## 12. Constants

| Name | Value | Meaning |
|------|-------|---------|
| `N` | 20 | Waiting room capacity (seats) |
| `K_OPEN` | 10 (N/2) | Queue length to open window 2 |
| `K_CLOSE` | 6 (N/3) | Queue length to close window 2 |
| `DOCTOR_COUNT` | 7 | Total doctor types (1 POZ + 6 specialists) |

---

## 13. Key Design Decisions

### No IPC_NOWAIT / No Polling
Every blocking operation uses fully blocking system calls (`msgrcv(..., 0)`, `semop(..., 0)`).
Zero busy-wait loops, zero usleep-based polling. CPU usage is near zero when idle.

### Negative mtype for Priority
System V `msgrcv(queue, &msg, size, -N, 0)` picks the message with the smallest `mtype ≤ N`.
This is used in two places:
1. **Registration:** `-2` picks VIP (mtype=1) before regular (mtype=2)
2. **Specialist:** `-3` picks RED (mtype=1) before YELLOW (mtype=2) before GREEN (mtype=3)

### Dedicated Queues per Specialist
Each specialist has its own message queue (6 total). Patient sends to the correct queue based on
`SharedState::specialist_msgids[assigned_doctor]`. Specialist reads only from its own queue.
Response goes back via the **main** queue with mtype=200+patient_id.

### Atomic semop for Children
Child + guardian occupy 2 seats. Using two separate `semop(-1)` calls could deadlock
(2 children each grab 1 seat, both need a 2nd). Solution: single `semop(-2)` — kernel
guarantees atomicity, waiting until ≥2 seats are available.

### Zombie Prevention
- Generator: `waitpid(-1, &status, WNOHANG)` loop after each fork to reap finished patients
- Dyrektor cleanup: blocking `waitpid(pid, &status, 0)` for each direct child after SIGKILL

### Window 2 Deactivation
When controller decides to close window 2:
1. Sets `g_window2_should_run = false`
2. Sends `pthread_kill(window2_thread, SIGUSR1)` to interrupt its blocked `msgrcv()`
3. `msgrcv()` returns `EINTR` → thread checks flag → exits inner loop → goes back to `pthread_cond_wait()`

---

## 14. Complete IPC Summary Table

| Resource | Type | Key | Count | Created by | Used by |
|----------|------|-----|-------|------------|---------|
| SharedState | Shared Memory | ftok("/tmp",'S') | 1 segment | dyrektor | all processes |
| Semaphore set | Semaphores | ftok("/tmp",'E') | 1 set, 13 sems | dyrektor | all processes |
| Main msg queue | Message Queue | ftok("/tmp",'M') | 1 queue | dyrektor | all processes |
| Kardiolog queue | Message Queue | ftok("/tmp",'a') | 1 queue | dyrektor | pacjent → kardiolog |
| Neurolog queue | Message Queue | ftok("/tmp",'b') | 1 queue | dyrektor | pacjent → neurolog |
| Okulista queue | Message Queue | ftok("/tmp",'c') | 1 queue | dyrektor | pacjent → okulista |
| Laryngolog queue | Message Queue | ftok("/tmp",'d') | 1 queue | dyrektor | pacjent → laryngolog |
| Chirurg queue | Message Queue | ftok("/tmp",'e') | 1 queue | dyrektor | pacjent → chirurg |
| Pediatra queue | Message Queue | ftok("/tmp",'f') | 1 queue | dyrektor | pacjent → pediatra |

**Totals:** 1 shared memory segment, 13 semaphores (1 set), 7 message queues.
All created by dyrektor in `initIPC()`, all destroyed in `cleanupIPC()`.
