/**
 * @file main.cpp
 * @brief Proces dyrektora SOR — IPC init, spawn procesów, klawiatura, cleanup
 */

#include "sor_common.hpp"
#include <termios.h>
#include <sys/select.h>

// ============================================================================
// ZMIENNE GLOBALNE
// ============================================================================

static int g_shmid = -1;
static int g_semid = -1;
static int g_msgid = -1;
static SharedState* g_state = nullptr;
static volatile sig_atomic_t g_shutdown = 0;

// Parametry z linii komend
static int g_max_time = 0;        // 0 = bez limitu
static int g_max_patients = 0;    // 0 = bez limitu
static int g_gen_min_ms = 0;      // 0 = domyślny z sor_common.hpp
static int g_gen_max_ms = 0;

static std::vector<pid_t> g_child_pids;
static pid_t g_generator_pid = -1;

static struct termios g_orig_termios;
static bool g_termios_set = false;

static inline bool shouldStop() { return g_shutdown || (g_state && g_state->shutdown); }

// ============================================================================
// OBSŁUGA BŁĘDÓW UŻYCIA
// ============================================================================

static void printUsage(const char* prog) {
    fprintf(stderr, "Użycie: %s [-t sekundy] [-p maks_procesów] [-g min_ms max_ms]\n", prog);
    fprintf(stderr, "  -t <s>        Czas trwania symulacji w sekundach (domyślnie: bez limitu)\n");
    fprintf(stderr, "  -p <n>        Maks jednoczesnych procesów łącznie (domyślnie: bez limitu)\n");
    fprintf(stderr, "  -g <min> <max> Czas między generowaniem pacjentów w ms (domyślnie: %d-%d)\n",
            PATIENT_GEN_MIN_MS, PATIENT_GEN_MAX_MS);
    exit(EXIT_FAILURE);
}

// ============================================================================
// INICJALIZACJA I SPRZĄTANIE IPC
// ============================================================================

/// Usuwa starą kolejkę (jeśli istnieje) i tworzy nową z prawami 0600
static int createQueue(key_t key, const char* name) {
    int old = msgget(key, 0);
    if (old != -1) msgctl(old, IPC_RMID, nullptr);
    int qid = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    if (qid == -1) SOR_FATAL("msgget — %s", name);
    return qid;
}

/// Tworzy kolejkę porządkującą z jednym seed tokenem mtype=1
static int createOrderQueue(key_t key, const char* name) {
    int qid = createQueue(key, name);
    GateToken seed{};
    seed.mtype = 1;
    if (msgsnd(qid, &seed, GATE_TOKEN_SIZE, 0) == -1)
        SOR_FATAL("msgsnd seed kolejki %s", name);
    return qid;
}

static void initIPC() {
    // --- PAMIĘĆ DZIELONA ---
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    int old_shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (old_shmid != -1) shmctl(old_shmid, IPC_RMID, nullptr);

    g_shmid = shmget(shm_key, sizeof(SharedState), IPC_CREAT | IPC_EXCL | 0600);
    if (g_shmid == -1) SOR_FATAL("shmget");

    g_state = (SharedState*)shmat(g_shmid, nullptr, 0);
    if (g_state == (void*)-1) SOR_FATAL("shmat");

    *g_state = SharedState{};

    // --- SEMAFORY ---
    key_t sem_key = getIPCKey(SEM_KEY_ID);
    int old_semid = semget(sem_key, SEM_COUNT, 0);
    if (old_semid != -1) semctl(old_semid, 0, IPC_RMID);

    g_semid = semget(sem_key, SEM_COUNT, IPC_CREAT | IPC_EXCL | 0600);
    if (g_semid == -1) SOR_FATAL("semget");

    unsigned short sem_values[SEM_COUNT] = {0};
    for (int i = SEM_SPECIALIST_KARDIOLOG; i <= SEM_SPECIALIST_PEDIATRA; i++)
        sem_values[i] = 1;
    sem_values[SEM_SHM_MUTEX] = 1;
    sem_values[SEM_LOG_MUTEX] = 1;

    union semun { int val; struct semid_ds *buf; unsigned short *array; } arg;
    arg.array = sem_values;
    if (semctl(g_semid, 0, SETALL, arg) == -1) SOR_FATAL("semctl SETALL");

    // --- KOLEJKA TOKENÓW POCZEKALNI ---
    int gate_msgid = createQueue(getGateQueueKey(), "gate tokens");
    g_state->gate_msgid = gate_msgid;

    GateToken token{};
    for (int i = 1; i <= N; i++) {
        token.mtype = i;
        if (msgsnd(gate_msgid, &token, GATE_TOKEN_SIZE, 0) == -1)
            SOR_FATAL("msgsnd gate token %d", i);
    }
    g_state->gate_next_ticket = 1;
    g_state->gate_now_serving = N + 1;
    g_state->triage_next_ticket = 1;
    g_state->exit_next_ticket = 1;

    // --- KOLEJKI PORZĄDKUJĄCE ---
    g_state->order_gate_log_msgid = createOrderQueue(getOrderGateLogKey(), "gate_log");
    g_state->order_triage_msgid   = createOrderQueue(getOrderTriageKey(), "triage");
    g_state->order_exit_msgid     = createOrderQueue(getOrderExitKey(), "exit");

    // --- KOLEJKA KOMUNIKATÓW ---
    g_msgid = createQueue(getIPCKey(MSG_KEY_ID), "komunikaty");

    // --- KOLEJKI SPECJALISTÓW ---
    g_state->specialist_msgids[DOCTOR_POZ] = -1;
    for (int i = DOCTOR_KARDIOLOG; i <= DOCTOR_PEDIATRA; i++) {
        DoctorType dtype = (DoctorType)i;
        g_state->specialist_msgids[dtype] = createQueue(
            getSpecialistQueueKey(dtype), getDoctorName(dtype));
    }

    printf("IPC zainicjalizowane: SHM=%d, SEM=%d, MSG=%d + 6 kolejek specjalistów + 4 kolejki porządkujące\n",
           g_shmid, g_semid, g_msgid);
}

static void cleanupIPC() {
    printf("Sprzątanie zasobów IPC...\n");

    if (g_state) { shmdt(g_state); g_state = nullptr; }
    if (g_shmid != -1) { shmctl(g_shmid, IPC_RMID, nullptr); g_shmid = -1; }
    if (g_semid != -1) { semctl(g_semid, 0, IPC_RMID); g_semid = -1; }
    if (g_msgid != -1) { msgctl(g_msgid, IPC_RMID, nullptr); g_msgid = -1; }

    auto removeQueue = [](key_t k) {
        int qid = msgget(k, 0);
        if (qid != -1) msgctl(qid, IPC_RMID, nullptr);
    };

    removeQueue(getGateQueueKey());
    for (int i = DOCTOR_KARDIOLOG; i <= DOCTOR_PEDIATRA; i++)
        removeQueue(getSpecialistQueueKey((DoctorType)i));
    removeQueue(getOrderGateLogKey());
    removeQueue(getOrderTriageKey());
    removeQueue(getOrderExitKey());

    printf("Zasoby IPC usunięte\n");
}

// ============================================================================
// URUCHAMIANIE PROCESÓW
// ============================================================================

static void startRegistration() {
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        execl("./rejestracja", "rejestracja", nullptr);
        SOR_FATAL("execl rejestracja");
    } else if (pid > 0) {
        g_state->registration_pid = pid;
        g_child_pids.push_back(pid);
    } else {
        SOR_FATAL("fork rejestracja");
    }
}

static void startDoctors() {
    for (int i = 0; i < DOCTOR_COUNT; i++) {
        if (!DOCTOR_ENABLED[i]) {
            g_state->doctor_pids[i] = 0;
            logMessage(g_state, g_semid, "[Dyrektor] Lekarz %s WYŁĄCZONY — pomijam",
                      getDoctorName((DoctorType)i));
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            char type_str[16];
            snprintf(type_str, sizeof(type_str), "%d", i);
            execl("./lekarz", "lekarz", type_str, nullptr);
            SOR_FATAL("execl lekarz typ=%d", i);
        } else if (pid > 0) {
            g_state->doctor_pids[i] = pid;
            g_child_pids.push_back(pid);
        } else {
            SOR_FATAL("fork lekarz typ=%d", i);
        }
    }
}

static void startGenerator() {
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (g_gen_min_ms > 0) {
            char min_str[16], max_str[16];
            snprintf(min_str, sizeof(min_str), "%d", g_gen_min_ms);
            snprintf(max_str, sizeof(max_str), "%d", g_gen_max_ms);
            execl("./generator", "generator", min_str, max_str, nullptr);
        } else {
            execl("./generator", "generator", nullptr);
        }
        SOR_FATAL("execl generator");
    } else if (pid > 0) {
        g_child_pids.push_back(pid);
        g_generator_pid = pid;
    } else {
        SOR_FATAL("fork generator");
    }
}

// ============================================================================
// TERMINAL
// ============================================================================

static void setRawTerminal() {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1)
        return;  // Może być przekierowanie — ignoruj
    g_termios_set = true;

    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void restoreTerminal() {
    if (g_termios_set)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}

// ============================================================================
// OBSŁUGA KLAWIATURY
// ============================================================================

static void handleKeyboard() {
    printf("\nCzekam na komendy (1-6: lekarz na oddział, 7: ewakuacja, q: wyjście)...\n\n");

    while (!shouldStop()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);

        struct timeval tv = {0, 100000};  // 100ms
        int ret = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);

        if (ret > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) != 1) continue;

            if (c >= '1' && c <= '6') {
                DoctorType dtype = (DoctorType)(c - '0');
                pid_t doctor_pid = g_state->doctor_pids[dtype];
                if (doctor_pid > 0) {
                    printf("Wysyłam SIGUSR1 do lekarza: %s (PID %d)\n",
                           getDoctorName(dtype), doctor_pid);
                    logMessage(g_state, g_semid, "[SIGUSR1] Lekarz %s wysłany na oddział",
                              getDoctorName(dtype));
                    if (kill(doctor_pid, SIGUSR1) == -1)
                        SOR_WARN("kill SIGUSR1 do lekarza PID=%d", doctor_pid);
                }
            } else if (c == '7') {
                printf("EWAKUACJA! Wysyłam SIGUSR2 do wszystkich...\n");
                logMessage(g_state, g_semid, "[SIGUSR2] EWAKUACJA - zakończenie symulacji");
                g_state->shutdown = 1;
                for (pid_t pid : g_child_pids)
                    if (pid > 0) kill(pid, SIGUSR2);
                g_shutdown = 1;
                break;
            } else if (c == 'q' || c == 'Q') {
                printf("Zamykanie symulacji...\n");
                g_state->shutdown = 1;
                g_shutdown = 1;
                break;
            }
        }

        // Timeout symulacji
        if (g_max_time > 0 && getElapsedTime(g_state) >= g_max_time) {
            printf("\nCzas symulacji (%d s) upłynął — zamykanie...\n", g_max_time);
            logMessage(g_state, g_semid, "[Dyrektor] Timeout %d s — zamykanie symulacji", g_max_time);
            g_state->shutdown = 1;
            g_shutdown = 1;
            break;
        }
    }
}

// ============================================================================
// OBSŁUGA SYGNAŁÓW
// ============================================================================

static void signalHandler(int /*sig*/) {
    g_shutdown = 1;
    if (g_state) g_state->shutdown = 1;
}

/// Automatycznie reapuje martwe procesy potomne (zapobiega zombie).
static void sigchldHandler(int /*sig*/) {
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
}

static void setupSignals() {
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);

    struct sigaction sa_chld{};
    sa_chld.sa_handler = sigchldHandler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, nullptr);

    atexit(cleanupIPC);
}

// ============================================================================
// ZAMKNIĘCIE PROCESÓW
// ============================================================================

/// Czeka na generator (do 5s), potem SIGKILL. Zeruje jego slot w g_child_pids.
static void shutdownGenerator() {
    if (g_generator_pid <= 0) return;

    kill(g_generator_pid, SIGTERM);

    bool exited = false;
    for (int attempt = 0; attempt < 50 && !exited; attempt++) {
        if (waitpid(g_generator_pid, nullptr, WNOHANG) > 0)
            exited = true;
        else
            usleep(100000);  // 100ms
    }
    if (!exited) {
        kill(g_generator_pid, SIGKILL);
        waitpid(g_generator_pid, nullptr, 0);
    }

    for (auto& pid : g_child_pids)
        if (pid == g_generator_pid) { pid = 0; break; }
}

/// SIGTERM → 500ms grace → SIGKILL + waitpid dla pozostałych procesów
static void shutdownRemaining() {
    for (pid_t pid : g_child_pids)
        if (pid > 0) kill(pid, SIGTERM);

    usleep(500000);

    for (pid_t pid : g_child_pids) {
        if (pid > 0) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "t:p:g:")) != -1) {
        switch (opt) {
            case 't':
                g_max_time = atoi(optarg);
                if (g_max_time <= 0) {
                    fprintf(stderr, "Błąd: czas symulacji musi być liczbą > 0 (podano: '%s')\n", optarg);
                    printUsage(argv[0]);
                }
                break;
            case 'p':
                g_max_patients = atoi(optarg);
                if (g_max_patients <= FIXED_PROCESS_COUNT) {
                    fprintf(stderr, "Błąd: limit procesów musi być > %d (stałe: dyrektor+generator+rejestracja+%d lekarzy)\n",
                            FIXED_PROCESS_COUNT, ENABLED_DOCTOR_COUNT);
                    printUsage(argv[0]);
                }
                break;
            case 'g':
                g_gen_min_ms = atoi(optarg);
                if (optind >= argc || argv[optind][0] == '-') {
                    fprintf(stderr, "Błąd: -g wymaga dwóch argumentów: min_ms max_ms\n");
                    printUsage(argv[0]);
                }
                g_gen_max_ms = atoi(argv[optind++]);
                if (g_gen_min_ms <= 0 || g_gen_max_ms <= 0 || g_gen_max_ms < g_gen_min_ms) {
                    fprintf(stderr, "Błąd: -g wartości muszą być > 0 i max >= min (podano: %d %d)\n",
                            g_gen_min_ms, g_gen_max_ms);
                    printUsage(argv[0]);
                }
                break;
            default:
                printUsage(argv[0]);
        }
    }

    printf("=== SYMULATOR SOR ===\n");
    printf("Sterowanie:\n");
    printf("  1-6: Wyślij lekarza na oddział (1=kardiolog, 2=neurolog, 3=okulista,\n");
    printf("       4=laryngolog, 5=chirurg, 6=pediatra)\n");
    printf("  7:   Ewakuacja - zakończ symulację (SIGUSR2)\n");
    printf("  q:   Wyjście\n");
    if (g_max_time > 0)     printf("  Limit czasu: %d s\n", g_max_time);
    if (g_max_patients > 0) printf("  Limit procesów: %d (w tym %d pacjentów)\n",
                                   g_max_patients, g_max_patients - FIXED_PROCESS_COUNT);
    if (g_gen_min_ms > 0)   printf("  Generowanie pacjentów: %d-%d ms\n", g_gen_min_ms, g_gen_max_ms);
    printf("=====================\n\n");

    setupSignals();
    initIPC();

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    g_state->start_time_sec = start.tv_sec;
    g_state->start_time_nsec = start.tv_nsec;
    g_state->director_pid = getpid();
    g_state->max_patients = g_max_patients;

    snprintf(g_state->log_file, sizeof(g_state->log_file), "sor_log.txt");
    FILE* f = fopen(g_state->log_file, "w");
    if (f) { fprintf(f, "=== LOG SYMULACJI SOR ===\n"); fclose(f); }

    startRegistration();
    startDoctors();
    setRawTerminal();
    startGenerator();

    handleKeyboard();

    // Zakończenie
    restoreTerminal();
    if (g_state) g_state->shutdown = 1;

    shutdownGenerator();
    shutdownRemaining();

    printf("\n=== Symulacja zakończona ===\n");
    return 0;
}
