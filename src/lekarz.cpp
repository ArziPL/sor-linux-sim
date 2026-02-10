/**
 * @file lekarz.cpp
 * @brief Proces lekarza SOR
 * 
 * Obsługuje dwa typy lekarzy:
 * 1. Lekarz POZ (triaż) - przeprowadza wstępną ocenę i przypisuje kolor/specjalistę
 * 2. Lekarze specjaliści - wykonują badania i podejmują decyzje o leczeniu
 * 
 * Sygnały:
 * - SIGUSR1: lekarz kończy obecnego pacjenta i idzie na oddział (przerwa)
 * - SIGUSR2: natychmiastowe zakończenie (ewakuacja)
 */

#include "sor_common.hpp"

// ============================================================================
// ZMIENNE GLOBALNE
// ============================================================================

static DoctorType g_doctor_type;           // Typ tego lekarza
static SharedState* g_state = nullptr;     // Pamięć dzielona
static int g_semid = -1;                   // ID semaforów
static int g_msgid = -1;                   // ID kolejki komunikatów

// Flagi sterujące zachowaniem lekarza
static volatile sig_atomic_t g_shutdown = 0;      // Zakończenie pracy
static volatile sig_atomic_t g_go_to_ward = 0;    // Idź na oddział po obecnym pacjencie
static volatile sig_atomic_t g_treating = 0;       // Czy aktualnie leczy pacjenta

// ============================================================================
// DEKLARACJE FUNKCJI
// ============================================================================

void initIPC();
void signalHandler(int sig);
void setupSignals();
void runPOZ();
void runSpecialist();
void goToWard();

// ============================================================================
// FUNKCJA GŁÓWNA
// ============================================================================

int main(int argc, char* argv[]) {
    // Sprawdź argumenty (typ lekarza)
    if (argc < 2) {
        fprintf(stderr, "Użycie: lekarz <typ>\n");
        return EXIT_FAILURE;
    }
    
    g_doctor_type = (DoctorType)atoi(argv[1]);
    
    if (g_doctor_type < 0 || g_doctor_type >= DOCTOR_COUNT) {
        fprintf(stderr, "Nieprawidłowy typ lekarza: %d\n", g_doctor_type);
        return EXIT_FAILURE;
    }
    
    // Inicjalizacja IPC
    initIPC();
    
    // Ustaw handlery sygnałów
    setupSignals();
    
    // Loguj start pracy
    logMessage(g_state, g_semid, "Lekarz %s rozpoczyna pracę", getDoctorName(g_doctor_type));
    
    // Uruchom odpowiednią funkcję lekarza
    if (g_doctor_type == DOCTOR_POZ) {
        runPOZ();
    } else {
        runSpecialist();
    }
    
    // Loguj zakończenie
    logMessage(g_state, g_semid, "Lekarz %s kończy pracę", getDoctorName(g_doctor_type));
    
    // Odłącz pamięć dzieloną
    if (g_state) {
        shmdt(g_state);
    }
    
    return 0;
}

// ============================================================================
// INICJALIZACJA IPC
// ============================================================================

/**
 * @brief Podłącza się do istniejących zasobów IPC
 */
void initIPC() {
    // Podłącz pamięć dzieloną
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    int shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (shmid == -1) {
        SOR_FATAL("lekarz %s: shmget", getDoctorName(g_doctor_type));
    }
    
    g_state = (SharedState*)shmat(shmid, nullptr, 0);
    if (g_state == (void*)-1) {
        SOR_FATAL("lekarz %s: shmat", getDoctorName(g_doctor_type));
    }
    
    // Podłącz semafory
    key_t sem_key = getIPCKey(SEM_KEY_ID);
    g_semid = semget(sem_key, SEM_COUNT, 0);
    if (g_semid == -1) {
        SOR_FATAL("lekarz %s: semget", getDoctorName(g_doctor_type));
    }
    
    // Podłącz kolejkę komunikatów
    key_t msg_key = getIPCKey(MSG_KEY_ID);
    g_msgid = msgget(msg_key, 0);
    if (g_msgid == -1) {
        SOR_FATAL("lekarz %s: msgget", getDoctorName(g_doctor_type));
    }
}

// ============================================================================
// OBSŁUGA SYGNAŁÓW
// ============================================================================

/**
 * @brief Handler sygnałów dla lekarza
 * SIGUSR1 - idź na oddział po zakończeniu obecnego pacjenta
 * SIGUSR2 - natychmiastowe zakończenie (ewakuacja)
 */
void signalHandler(int sig) {
    if (sig == SIGUSR1) {
        // Lekarz ma iść na oddział po zakończeniu obecnego pacjenta
        g_go_to_ward = 1;
        
    } else if (sig == SIGUSR2) {
        // Ewakuacja - natychmiastowe zakończenie
        g_shutdown = 1;
        
    } else if (sig == SIGTERM || sig == SIGINT) {
        g_shutdown = 1;
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
    sa.sa_flags = 0;  // Nie używamy SA_RESTART - chcemy przerwać blokujące wywołania
    
    sigaction(SIGUSR1, &sa, nullptr);
    sigaction(SIGUSR2, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

// ============================================================================
// LEKARZ POZ (TRIAŻ)
// ============================================================================

/**
 * @brief Główna pętla lekarza POZ
 * - Odbiera pacjentów po rejestracji
 * - Przeprowadza triaż (przypisuje kolor)
 * - Kieruje do odpowiedniego specjalisty lub odsyła do domu
 * 
 * Lekarz POZ nie reaguje na SIGUSR1 (nie idzie na oddział)
 */
void runPOZ() {
    while (!g_shutdown && !g_state->shutdown) {
        // Czekaj na wiadomość od pacjenta (blokująco)
        SORMessage msg;
        
        // Odbierz wiadomość typu MSG_PATIENT_TO_TRIAGE
        ssize_t ret = msgrcv(g_msgid, &msg, sizeof(SORMessage) - sizeof(long), 
                             MSG_PATIENT_TO_TRIAGE, 0);
        
        if (ret == -1) {
            if (errno == EINTR) {
                // Przerwane przez sygnał - sprawdź czy kończymy
                continue;
            }
            if (errno == EIDRM || errno == EINVAL) {
                // Kolejka usunięta - kończymy
                break;
            }
            continue;
        }
        
        // Mamy pacjenta do triażu
        
        // [Dziecko] dla pacjentów < 18 lat
        if (msg.age < 18) {
            logMessage(g_state, g_semid, "Pacjent %d [Dziecko] jest weryfikowany przez lekarza POZ", 
                      msg.patient_id);
        } else {
            logMessage(g_state, g_semid, "Pacjent %d jest weryfikowany przez lekarza POZ", 
                      msg.patient_id);
        }
        
        // Symulacja czasu triażu
        randomSleep(TRIAGE_MIN_MS, TRIAGE_MAX_MS);
        
        // Losuj kolor triażu
        TriageColor color = randomTriageColor();
        msg.color = color;
        
        if (color == COLOR_SENT_HOME) {
            // Pacjent odsyłany do domu bezpośrednio z triażu
            if (msg.age < 18) {
                logMessage(g_state, g_semid, "Pacjent %d [Dziecko] odesłany do domu z triażu", 
                          msg.patient_id);
            } else {
                logMessage(g_state, g_semid, "Pacjent %d odesłany do domu z triażu", 
                          msg.patient_id);
            }
            
            // Wyślij odpowiedź do pacjenta - odesłany z triażu
            msg.mtype = MSG_TRIAGE_RESPONSE + msg.patient_id;
            msg.assigned_doctor = DOCTOR_POZ;  // Oznacza: brak specjalisty
            msg.outcome = 0;  // Do domu
            
            if (msgsnd(g_msgid, &msg, sizeof(SORMessage) - sizeof(long), 0) == -1) {
                if (errno != EINTR && errno != EIDRM) {
                    SOR_WARN("POZ msgsnd odpowiedź pacjent %d", msg.patient_id);
                }
            }
            
        } else {
            // Przypisz specjalistę
            DoctorType specialist = randomSpecialist(msg.age);
            msg.assigned_doctor = specialist;
            
            // [Dziecko] dla pacjentów < 18 lat
            if (msg.age < 18) {
                logMessage(g_state, g_semid, 
                          "Pacjent %d [Dziecko] uzyskuje status [%s] — kierowany do lekarza: %s",
                          msg.patient_id, getColorName(color), getDoctorName(specialist));
            } else {
                logMessage(g_state, g_semid, 
                          "Pacjent %d uzyskuje status [%s] — kierowany do lekarza: %s",
                          msg.patient_id, getColorName(color), getDoctorName(specialist));
            }
            
            // Wyślij odpowiedź do pacjenta - idzie do specjalisty
            // Pacjent sam wyśle się do kolejki specjalisty
            msg.mtype = MSG_TRIAGE_RESPONSE + msg.patient_id;
            
            if (msgsnd(g_msgid, &msg, sizeof(SORMessage) - sizeof(long), 0) == -1) {
                if (errno != EINTR && errno != EIDRM) {
                    SOR_WARN("POZ msgsnd triaż pacjent %d", msg.patient_id);
                }
            }
        }
        
        // POZ nie reaguje na SIGUSR1, więc nie sprawdzamy g_go_to_ward
    }
}

// ============================================================================
// LEKARZ SPECJALISTA
// ============================================================================

/**
 * @brief Obsługuje przerwę lekarza (wyjście na oddział)
 */
void goToWard() {
    logMessage(g_state, g_semid, "Lekarz %s idzie na oddział (przerwa)", 
              getDoctorName(g_doctor_type));
    
    // Zaznacz w stanie że lekarz jest na przerwie
    semWait(g_semid, SEM_SHM_MUTEX);
    g_state->doctor_on_break[g_doctor_type] = 1;
    semSignal(g_semid, SEM_SHM_MUTEX);
    
    // Losowy czas przerwy
    randomSleep(DOCTOR_BREAK_MIN_MS, DOCTOR_BREAK_MAX_MS);
    
    // Wróć z przerwy
    semWait(g_semid, SEM_SHM_MUTEX);
    g_state->doctor_on_break[g_doctor_type] = 0;
    semSignal(g_semid, SEM_SHM_MUTEX);
    
    logMessage(g_state, g_semid, "Lekarz %s wraca z oddziału", getDoctorName(g_doctor_type));
    
    g_go_to_ward = 0;
}

/**
 * @brief Główna pętla lekarza specjalisty
 * - Odbiera pacjentów po triażu
 * - Wykonuje badania i leczenie
 * - Podejmuje decyzję o dalszym postępowaniu
 * - Reaguje na SIGUSR1 (przerwa po zakończeniu pacjenta)
 */
void runSpecialist() {
    int sem_idx = getSpecialistSemIndex(g_doctor_type);
    
    // Otwórz dedykowaną kolejkę tego specjalisty
    int spec_msgid = g_state->specialist_msgids[g_doctor_type];
    
    while (!g_shutdown && !g_state->shutdown) {
        SORMessage msg;
        size_t msg_size = sizeof(SORMessage) - sizeof(long);
        
        // Blokujący odbiór z priorytetem koloru: ujemny mtype (-3) oznacza
        // "odbierz wiadomość z mtype <= 3, najniższy mtype pierwszy"
        // RED(1) przed YELLOW(2) przed GREEN(3)
        ssize_t ret = msgrcv(spec_msgid, &msg, msg_size, -SPECIALIST_MTYPE_GREEN, 0);
        
        if (ret == -1) {
            if (errno == EINTR) {
                // Przerwane przez sygnał — sprawdź czy kończymy lub idziemy na oddział
                if (g_go_to_ward && !g_treating) {
                    goToWard();
                }
                continue;
            }
            if (errno == EIDRM || errno == EINVAL) {
                break;
            }
            continue;
        }
        
        // Zajmij semafor specjalisty (czekaj na wolne miejsce u lekarza)
        semWait(g_semid, sem_idx);
        
        g_treating = 1;
        
        // [Dziecko] dla pacjentów < 18 lat
        if (msg.age < 18) {
            logMessage(g_state, g_semid, "Pacjent %d [Dziecko] jest badany przez lekarza %s (kolor: %s)",
                      msg.patient_id, getDoctorName(g_doctor_type), getColorName(msg.color));
        } else {
            logMessage(g_state, g_semid, "Pacjent %d jest badany przez lekarza %s (kolor: %s)",
                      msg.patient_id, getDoctorName(g_doctor_type), getColorName(msg.color));
        }
        
        // Symulacja czasu leczenia
        randomSleep(TREATMENT_MIN_MS, TREATMENT_MAX_MS);
        
        // Losuj wynik leczenia
        int outcome = randomOutcome();
        msg.outcome = outcome;
        
        const char* outcome_str;
        switch (outcome) {
            case 0: outcome_str = "wypisany do domu"; break;
            case 1: outcome_str = "skierowany na oddział szpitalny"; break;
            case 2: outcome_str = "skierowany do innej placówki"; break;
            default: outcome_str = "nieznany"; break;
        }
        
        // [Dziecko] dla pacjentów < 18 lat
        if (msg.age < 18) {
            logMessage(g_state, g_semid, "Pacjent %d [Dziecko] — %s", msg.patient_id, outcome_str);
        } else {
            logMessage(g_state, g_semid, "Pacjent %d — %s", msg.patient_id, outcome_str);
        }
        
        // Wyślij odpowiedź do pacjenta
        msg.mtype = MSG_SPECIALIST_RESPONSE + msg.patient_id;
        
        if (msgsnd(g_msgid, &msg, sizeof(SORMessage) - sizeof(long), 0) == -1) {
            if (errno != EINTR && errno != EIDRM) {
                SOR_WARN("specjalista %s msgsnd pacjent %d", getDoctorName(g_doctor_type), msg.patient_id);
            }
        }
        
        g_treating = 0;
        
        // Zwolnij semafor specjalisty
        semSignal(g_semid, sem_idx);
        
        // Sprawdź czy mamy iść na oddział
        if (g_go_to_ward) {
            goToWard();
        }
    }
}
