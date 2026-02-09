/**
 * @file main.cpp
 * @brief Główny proces symulacji SOR - Dyrektor
 * 
 * Odpowiedzialności:
 * - Inicjalizacja zasobów IPC (pamięć dzielona, semafory, kolejki)
 * - Uruchamianie procesów lekarzy
 * - Generowanie pacjentów
 * - Obsługa klawiatury (sygnały do lekarzy)
 * - Kontrola okienek rejestracji
 * - Sprzątanie zasobów przy zakończeniu
 */

#include "sor_common.hpp"
#include <termios.h>
#include <sys/select.h>
#include <stdarg.h>
#include <getopt.h>

// ============================================================================
// ZMIENNE GLOBALNE
// ============================================================================

static int g_shmid = -1;          // ID pamięci dzielonej
static int g_semid = -1;          // ID semaforów
static int g_msgid = -1;          // ID kolejki komunikatów
static SharedState* g_state = nullptr;  // Wskaźnik do pamięci dzielonej
static volatile sig_atomic_t g_shutdown = 0;  // Flaga zakończenia

// Parametry z linii komend
static int g_max_time = 0;        // Maks czas symulacji w sekundach (0 = bez limitu)
static int g_max_patients = 0;    // Maks liczba pacjentów (0 = bez limitu)

// Lista PIDów procesów potomnych do sprzątania
static pid_t g_child_pids[1000];
static int g_child_count = 0;

// Oryginalne ustawienia terminala
static struct termios g_orig_termios;
static bool g_termios_set = false;

// ============================================================================
// DEKLARACJE FUNKCJI
// ============================================================================

void initIPC();
void cleanupIPC();
void startRegistration();
void startDoctors();
void generatePatients();
void handleKeyboard();
void signalHandler(int sig);
void setupSignals();
void restoreTerminal();
void setRawTerminal();

// ============================================================================
// FUNKCJA GŁÓWNA
// ============================================================================

/**
 * @brief Wyświetla sposób użycia programu i kończy z błędem
 */
void printUsage(const char* prog) {
    fprintf(stderr, "Użycie: %s [-t sekundy] [-p maks_pacjentów]\n", prog);
    fprintf(stderr, "  -t <s>  Czas trwania symulacji w sekundach (domyślnie: bez limitu)\n");
    fprintf(stderr, "  -p <n>  Maks jednoczesnych procesów pacjentów (domyślnie: bez limitu)\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char* argv[]) {
    // --- Parsowanie argumentów linii komend ---
    int opt;
    while ((opt = getopt(argc, argv, "t:p:")) != -1) {
        switch (opt) {
            case 't': {
                g_max_time = atoi(optarg);
                if (g_max_time <= 0) {
                    fprintf(stderr, "Błąd: czas symulacji musi być liczbą > 0 (podano: '%s')\n", optarg);
                    printUsage(argv[0]);
                }
                break;
            }
            case 'p': {
                g_max_patients = atoi(optarg);
                if (g_max_patients <= FIXED_PROCESS_COUNT) {
                    fprintf(stderr, "Błąd: limit procesów musi być > %d (stałe procesy: dyrektor+generator+rejestracja+%d lekarzy)\n",
                            FIXED_PROCESS_COUNT, DOCTOR_COUNT);
                    fprintf(stderr, "Podano: '%s', minimalnie: %d\n", optarg, FIXED_PROCESS_COUNT + 1);
                    printUsage(argv[0]);
                }
                break;
            }
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
    if (g_max_time > 0)
        printf("  Limit czasu: %d s\n", g_max_time);
    if (g_max_patients > 0)
        printf("  Limit procesów: %d (łącznie, w tym %d pacjentów)\n", 
               g_max_patients, g_max_patients - FIXED_PROCESS_COUNT);
    printf("=====================\n\n");
    
    // Ustaw handlery sygnałów
    setupSignals();
    
    // Inicjalizacja zasobów IPC
    initIPC();
    
    // Zapisz czas startu
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    g_state->start_time_sec = start.tv_sec;
    g_state->start_time_nsec = start.tv_nsec;
    g_state->director_pid = getpid();
    g_state->shutdown = 0;
    g_state->max_patients = g_max_patients;
    
    // Ustaw ścieżkę do logu
    snprintf(g_state->log_file, sizeof(g_state->log_file), 
             "/home/areczek/sor-linux-sim/sor_log.txt");
    
    // Wyczyść plik logu
    FILE* f = fopen(g_state->log_file, "w");
    if (f) {
        fprintf(f, "=== LOG SYMULACJI SOR ===\n");
        fclose(f);
    }
    
    // Uruchom rejestrację
    startRegistration();
    
    // Uruchom lekarzy
    startDoctors();
    
    // Ustaw terminal w tryb raw dla obsługi klawiszy
    setRawTerminal();
    
    // Fork dla generatora pacjentów
    pid_t gen_pid = fork();
    if (gen_pid == 0) {
        // Proces potomny - generator pacjentów
        generatePatients();
        exit(0);
    } else if (gen_pid > 0) {
        g_child_pids[g_child_count++] = gen_pid;
    }
    
    // Proces główny - obsługa klawiatury
    handleKeyboard();
    
    // Sprzątanie
    restoreTerminal();
    
    // Wyślij SIGUSR2 do wszystkich (zakończenie)
    if (g_state) {
        g_state->shutdown = 1;
    }
    
    // Wyślij SIGTERM do procesów potomnych i czekaj na zakończenie
    for (int i = 0; i < g_child_count; i++) {
        if (g_child_pids[i] > 0) {
            kill(g_child_pids[i], SIGTERM);
        }
    }
    
    // Daj czas na graceful shutdown
    usleep(500000);
    
    // Zabij pozostałe procesy i zbierz zombie (blokujący wait)
    for (int i = 0; i < g_child_count; i++) {
        if (g_child_pids[i] > 0) {
            kill(g_child_pids[i], SIGKILL);
            int status;
            waitpid(g_child_pids[i], &status, 0);
        }
    }
    
    // Sprzątanie IPC
    cleanupIPC();
    
    printf("\n=== Symulacja zakończona ===\n");
    return 0;
}

// ============================================================================
// INICJALIZACJA I SPRZĄTANIE IPC
// ============================================================================

/**
 * @brief Inicjalizuje wszystkie zasoby IPC
 * Tworzy pamięć dzieloną, semafory i kolejkę komunikatów
 */
void initIPC() {
    // --- PAMIĘĆ DZIELONA ---
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    
    // Usuń istniejącą pamięć dzieloną (jeśli istnieje)
    int old_shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (old_shmid != -1) {
        shmctl(old_shmid, IPC_RMID, nullptr);
    }
    
    // Utwórz nową pamięć dzieloną (prawa: właściciel r/w)
    g_shmid = shmget(shm_key, sizeof(SharedState), IPC_CREAT | IPC_EXCL | 0600);
    if (g_shmid == -1) {
        SOR_FATAL("shmget — nie można utworzyć pamięci dzielonej");
    }
    
    // Podłącz pamięć dzieloną
    g_state = (SharedState*)shmat(g_shmid, nullptr, 0);
    if (g_state == (void*)-1) {
        SOR_FATAL("shmat — nie można podłączyć pamięci dzielonej");
    }
    
    // Wyzeruj stan
    memset(g_state, 0, sizeof(SharedState));
    
    // --- SEMAFORY ---
    key_t sem_key = getIPCKey(SEM_KEY_ID);
    
    // Usuń istniejące semafory
    int old_semid = semget(sem_key, SEM_COUNT, 0);
    if (old_semid != -1) {
        semctl(old_semid, 0, IPC_RMID);
    }
    
    // Utwórz nowe semafory (prawa: właściciel r/w)
    g_semid = semget(sem_key, SEM_COUNT, IPC_CREAT | IPC_EXCL | 0600);
    if (g_semid == -1) {
        SOR_FATAL("semget — nie można utworzyć semaforów");
    }
    
    // Inicjalizacja wartości semaforów
    unsigned short sem_values[SEM_COUNT] = {0};
    sem_values[SEM_POCZEKALNIA] = N;           // N wolnych miejsc
    sem_values[SEM_REG_QUEUE_MUTEX] = 1;       // Mutex wolny
    sem_values[SEM_REG_WINDOW_1] = 1;          // Okienko 1 wolne
    sem_values[SEM_REG_WINDOW_2] = 0;          // Okienko 2 zamknięte
    // Semafory specjalistów - każdy zaczyna jako wolny (1)
    sem_values[SEM_SPECIALIST_KARDIOLOG] = 1;
    sem_values[SEM_SPECIALIST_NEUROLOG] = 1;
    sem_values[SEM_SPECIALIST_OKULISTA] = 1;
    sem_values[SEM_SPECIALIST_LARYNGOLOG] = 1;
    sem_values[SEM_SPECIALIST_CHIRURG] = 1;
    sem_values[SEM_SPECIALIST_PEDIATRA] = 1;
    sem_values[SEM_SHM_MUTEX] = 1;             // Mutex wolny
    sem_values[SEM_LOG_MUTEX] = 1;             // Mutex wolny
    sem_values[SEM_REG_QUEUE_CHANGED] = 0;     // Kontroler czeka na sygnał
    
    // Ustaw wszystkie wartości
    union semun {
        int val;
        struct semid_ds *buf;
        unsigned short *array;
    } arg;
    arg.array = sem_values;
    
    if (semctl(g_semid, 0, SETALL, arg) == -1) {
        SOR_FATAL("semctl SETALL — nie można ustawić wartości semaforów");
    }
    
    // --- KOLEJKA KOMUNIKATÓW ---
    key_t msg_key = getIPCKey(MSG_KEY_ID);
    
    // Usuń istniejącą kolejkę
    int old_msgid = msgget(msg_key, 0);
    if (old_msgid != -1) {
        msgctl(old_msgid, IPC_RMID, nullptr);
    }
    
    // Utwórz nową kolejkę (prawa: właściciel r/w)
    g_msgid = msgget(msg_key, IPC_CREAT | IPC_EXCL | 0600);
    if (g_msgid == -1) {
        SOR_FATAL("msgget — nie można utworzyć kolejki komunikatów");
    }
    
    // --- KOLEJKI SPECJALISTÓW (osobna per specjalista) ---
    for (int i = DOCTOR_KARDIOLOG; i <= DOCTOR_PEDIATRA; i++) {
        DoctorType dtype = (DoctorType)i;
        key_t spec_key = getSpecialistQueueKey(dtype);
        
        // Usuń istniejącą kolejkę
        int old_spec_msgid = msgget(spec_key, 0);
        if (old_spec_msgid != -1) {
            msgctl(old_spec_msgid, IPC_RMID, nullptr);
        }
        
        // Utwórz nową kolejkę
        int spec_msgid = msgget(spec_key, IPC_CREAT | IPC_EXCL | 0600);
        if (spec_msgid == -1) {
            SOR_FATAL("msgget — nie można utworzyć kolejki specjalisty %d", i);
        }
        
        // Zapisz ID w pamięci dzielonej (procesy potomne użyją tego)
        g_state->specialist_msgids[dtype] = spec_msgid;
    }
    
    printf("IPC zainicjalizowane: SHM=%d, SEM=%d, MSG=%d + 6 kolejek specjalistów\n", g_shmid, g_semid, g_msgid);
}

/**
 * @brief Sprząta wszystkie zasoby IPC
 * Usuwa pamięć dzieloną, semafory i kolejkę komunikatów
 */
void cleanupIPC() {
    printf("Sprzątanie zasobów IPC...\n");
    
    // Odłącz pamięć dzieloną
    if (g_state && g_state != (void*)-1) {
        shmdt(g_state);
        g_state = nullptr;
    }
    
    // Usuń pamięć dzieloną
    if (g_shmid != -1) {
        shmctl(g_shmid, IPC_RMID, nullptr);
        g_shmid = -1;
    }
    
    // Usuń semafory
    if (g_semid != -1) {
        semctl(g_semid, 0, IPC_RMID);
        g_semid = -1;
    }
    
    // Usuń kolejkę komunikatów
    if (g_msgid != -1) {
        msgctl(g_msgid, IPC_RMID, nullptr);
        g_msgid = -1;
    }
    
    // Usuń kolejki specjalistów
    for (int i = DOCTOR_KARDIOLOG; i <= DOCTOR_PEDIATRA; i++) {
        key_t spec_key = getSpecialistQueueKey((DoctorType)i);
        int spec_msgid = msgget(spec_key, 0);
        if (spec_msgid != -1) {
            msgctl(spec_msgid, IPC_RMID, nullptr);
        }
    }
    
    printf("Zasoby IPC usunięte\n");
}

// ============================================================================
// URUCHAMIANIE REJESTRACJI
// ============================================================================

/**
 * @brief Uruchamia proces rejestracji (jeden proces, dwa wątki okienek)
 */
void startRegistration() {
    pid_t pid = fork();
    
    if (pid == 0) {
        // Proces potomny - uruchom program rejestracji
        execl("./rejestracja", "rejestracja", nullptr);
        
        // Jeśli exec się nie udał
        SOR_FATAL("execl rejestracja");
        exit(EXIT_FAILURE);
        
    } else if (pid > 0) {
        // Proces rodzica - zapisz PID rejestracji
        g_state->registration_pid = pid;
        g_child_pids[g_child_count++] = pid;
        
    } else {
        // Błąd fork
        SOR_FATAL("fork rejestracja");
    }
}

// ============================================================================
// URUCHAMIANIE LEKARZY
// ============================================================================

/**
 * @brief Uruchamia procesy wszystkich lekarzy
 * Każdy lekarz to osobny proces uruchomiony przez exec
 */
void startDoctors() {
    for (int i = 0; i < DOCTOR_COUNT; i++) {
        pid_t pid = fork();
        
        if (pid == 0) {
            // Proces potomny - uruchom program lekarza
            char type_str[16];
            snprintf(type_str, sizeof(type_str), "%d", i);
            
            // Przekaż typ lekarza jako argument
            execl("./lekarz", "lekarz", type_str, nullptr);
            
            // Jeśli exec się nie udał
            SOR_FATAL("execl lekarz typ=%d", i);
            exit(EXIT_FAILURE);
            
        } else if (pid > 0) {
            // Proces rodzica - zapisz PID lekarza
            g_state->doctor_pids[i] = pid;
            g_child_pids[g_child_count++] = pid;
            
        } else {
            // Błąd fork
            SOR_FATAL("fork lekarz typ=%d", i);
        }
    }
}

// ============================================================================
// GENERATOR PACJENTÓW
// ============================================================================

/**
 * @brief Generuje pacjentów w losowych odstępach czasu
 * Uruchamia proces pacjenta przez exec dla każdego nowego pacjenta
 */
void generatePatients() {
    // Ponownie podłącz pamięć dzieloną w procesie potomnym
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    int shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (shmid == -1) {
        SOR_FATAL("Generator: shmget");
        return;
    }
    
    SharedState* state = (SharedState*)shmat(shmid, nullptr, 0);
    if (state == (void*)-1) {
        SOR_FATAL("Generator: shmat");
        return;
    }
    
    key_t sem_key = getIPCKey(SEM_KEY_ID);
    int semid = semget(sem_key, SEM_COUNT, 0);
    if (semid == -1) {
        SOR_FATAL("Generator: semget");
        return;
    }
    
    logMessage(state, semid, "[Generator] Generator pacjentów startuje");
    
    int patient_id = 0;
    
    while (!state->shutdown) {
        // Losowy czas do następnego pacjenta
        randomSleep(PATIENT_GEN_MIN_MS, PATIENT_GEN_MAX_MS);
        
        if (state->shutdown) break;
        
        // Czekaj jeśli osiągnięto limit jednoczesnych procesów pacjentów
        // max_patients to całkowity limit, odejmujemy stałe procesy
        int patient_limit = state->max_patients - FIXED_PROCESS_COUNT;
        if (state->max_patients > 0 && patient_limit > 0) {
            while (!state->shutdown) {
                semWait(semid, SEM_SHM_MUTEX);
                int active = state->active_patient_count;
                semSignal(semid, SEM_SHM_MUTEX);
                
                if (active < patient_limit) break;
                usleep(100000);  // 100ms polling
            }
            if (state->shutdown) break;
        }
        
        patient_id++;
        int age = randomAge();
        int is_vip = randomVIP() ? 1 : 0;
        
        // Loguj pojawienie się pacjenta
        if (age < 18) {
            logMessage(state, semid, "Pacjent %d pojawia się przed SOR (wiek %d, z opiekunem)", 
                      patient_id, age);
        } else {
            logMessage(state, semid, "Pacjent %d pojawia się przed SOR (wiek %d)%s", 
                      patient_id, age, is_vip ? " [VIP]" : "");
        }
        
        // Zapamiętaj ile pacjentów
        semWait(semid, SEM_SHM_MUTEX);
        state->total_patients = patient_id;
        semSignal(semid, SEM_SHM_MUTEX);
        
        // Inkrementuj licznik aktywnych procesów PRZED forkiem
        semWait(semid, SEM_SHM_MUTEX);
        state->active_patient_count++;
        semSignal(semid, SEM_SHM_MUTEX);
        
        // Fork dla nowego pacjenta
        pid_t pid = fork();
        
        if (pid == 0) {
            // Proces pacjenta
            char id_str[16], age_str[16], vip_str[16];
            snprintf(id_str, sizeof(id_str), "%d", patient_id);
            snprintf(age_str, sizeof(age_str), "%d", age);
            snprintf(vip_str, sizeof(vip_str), "%d", is_vip);
            
            execl("./pacjent", "pacjent", id_str, age_str, vip_str, nullptr);
            
            SOR_FATAL("execl pacjent id=%d", patient_id);
            exit(EXIT_FAILURE);
            
        } else if (pid < 0) {
            SOR_WARN("fork pacjenta %d", patient_id);
            // Cofnij licznik bo fork się nie udał
            semWait(semid, SEM_SHM_MUTEX);
            if (state->active_patient_count > 0) state->active_patient_count--;
            semSignal(semid, SEM_SHM_MUTEX);
        }
        
        // Zbierz zakończone procesy pacjentów (unikamy zombie)
        int status;
        pid_t finished;
        while ((finished = waitpid(-1, &status, WNOHANG)) > 0) {
            // Proces pacjenta zakończony - zombie usunięty
        }
    }
    
    // Zbierz pozostałe zombie
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
    
    shmdt(state);
}

// ============================================================================
// OBSŁUGA KLAWIATURY
// ============================================================================

/**
 * @brief Ustawia terminal w tryb raw (bez buforowania linii)
 * Pozwala na natychmiastowe odczytywanie pojedynczych klawiszy
 */
void setRawTerminal() {
    struct termios new_termios;
    
    // Zapisz oryginalne ustawienia
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) {
        return;  // Może być przekierowanie - ignoruj
    }
    g_termios_set = true;
    
    new_termios = g_orig_termios;
    
    // Wyłącz kanoniczny tryb (buforowanie linii) i echo
    new_termios.c_lflag &= ~(ICANON | ECHO);
    
    // Ustaw minimalne oczekiwanie
    new_termios.c_cc[VMIN] = 0;
    new_termios.c_cc[VTIME] = 1;  // 0.1s timeout
    
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
}

/**
 * @brief Przywraca oryginalne ustawienia terminala
 */
void restoreTerminal() {
    if (g_termios_set) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    }
}

/**
 * @brief Główna pętla obsługi klawiatury
 * Reaguje na naciśnięcia klawiszy 1-7 i q
 */
void handleKeyboard() {
    printf("\nCzekam na komendy (1-6: lekarz na oddział, 7: ewakuacja, q: wyjście)...\n\n");
    
    while (!g_shutdown && !g_state->shutdown) {
        // Użyj select do nieblokującego czekania na wejście
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  // 100ms timeout
        
        int ret = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                
                // Obsługa klawiszy 1-6 (lekarze specjaliści na oddział)
                if (c >= '1' && c <= '6') {
                    int doctor_idx = c - '1' + 1;  // 1->KARDIOLOG, 2->NEUROLOG, itd.
                    DoctorType dtype = (DoctorType)doctor_idx;
                    
                    pid_t doctor_pid = g_state->doctor_pids[dtype];
                    if (doctor_pid > 0) {
                        printf("Wysyłam SIGUSR1 do lekarza: %s (PID %d)\n", 
                               getDoctorName(dtype), doctor_pid);
                        logMessage(g_state, g_semid, "[SIGUSR1] Lekarz %s wysłany na oddział",
                                  getDoctorName(dtype));
                        
                        // Wyślij sygnał SIGUSR1 do lekarza
                        if (kill(doctor_pid, SIGUSR1) == -1) {
                            SOR_WARN("kill SIGUSR1 do lekarza PID=%d", doctor_pid);
                        }
                    }
                }
                // Klawisz 7 - ewakuacja (SIGUSR2 do wszystkich)
                else if (c == '7') {
                    printf("EWAKUACJA! Wysyłam SIGUSR2 do wszystkich...\n");
                    logMessage(g_state, g_semid, "[SIGUSR2] EWAKUACJA - zakończenie symulacji");
                    
                    g_state->shutdown = 1;
                    
                    // Wyślij SIGUSR2 do wszystkich lekarzy
                    for (int i = 0; i < DOCTOR_COUNT; i++) {
                        if (g_state->doctor_pids[i] > 0) {
                            kill(g_state->doctor_pids[i], SIGUSR2);
                        }
                    }
                    
                    // Wyślij SIGTERM do wszystkich procesów potomnych
                    for (int i = 0; i < g_child_count; i++) {
                        if (g_child_pids[i] > 0) {
                            kill(g_child_pids[i], SIGUSR2);
                        }
                    }
                    
                    g_shutdown = 1;
                    break;
                }
                // Klawisz q - wyjście
                else if (c == 'q' || c == 'Q') {
                    printf("Zamykanie symulacji...\n");
                    g_state->shutdown = 1;
                    g_shutdown = 1;
                    break;
                }
            }
        }
        
        // Sprawdź timeout symulacji
        if (g_max_time > 0 && getElapsedTime(g_state) >= g_max_time) {
            printf("\nCzas symulacji (%d s) upłynął — zamykanie...\n", g_max_time);
            logMessage(g_state, g_semid, "[Dyrektor] Timeout %d s — zamykanie symulacji", g_max_time);
            g_state->shutdown = 1;
            g_shutdown = 1;
            break;
        }
        
        // Sprawdź flagę shutdown
        if (g_state->shutdown) {
            g_shutdown = 1;
            break;
        }
    }
}

// ============================================================================
// OBSŁUGA SYGNAŁÓW
// ============================================================================

/**
 * @brief Handler sygnałów dla procesu głównego
 */
void signalHandler(int sig) {
    if (sig == SIGINT || sig == SIGTERM || sig == SIGHUP) {
        g_shutdown = 1;
        if (g_state) {
            g_state->shutdown = 1;
        }
    }
}

/**
 * @brief Konfiguruje handlery sygnałów
 */
void setupSignals() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    
    // Sprzątaj IPC nawet przy awaryjnym zakończeniu
    atexit(cleanupIPC);
}
