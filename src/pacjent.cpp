/**
 * @file pacjent.cpp
 * @brief Proces pacjenta SOR
 * 
 * Realizuje pełną ścieżkę pacjenta przez SOR:
 * A. Wejście do poczekalni (ograniczona pojemność N)
 * B. Rejestracja (kolejka VIP lub zwykła)
 * C. Triaż u lekarza POZ
 * D. Leczenie u specjalisty
 * E. Wyjście z SOR
 * 
 * Dla dzieci (<18 lat) używane są dwa wątki:
 * - Wątek Rodzica: realizuje rejestrację
 * - Wątek Dziecka: realizuje triaż i leczenie (po zakończeniu rejestracji)
 */

#include "sor_common.hpp"
#include <stdarg.h>

// ============================================================================
// STRUKTURA DANYCH PACJENTA
// ============================================================================

/**
 * @brief Dane pacjenta przekazywane między wątkami
 */
struct PatientData {
    int id;                     // ID pacjenta
    int age;                    // Wiek
    bool is_vip;                // Czy VIP
    bool is_child;              // Czy dziecko (<18)
    
    // Zasoby IPC
    int semid;
    int msgid;
    SharedState* state;
    
    // Synchronizacja wątków (tylko dla dzieci)
    pthread_mutex_t reg_mutex;          // Mutex rejestracji
    pthread_cond_t reg_done_cond;       // Warunek: rejestracja zakończona
    bool registration_complete;          // Flaga zakończenia rejestracji
    
    // Wynik ścieżki
    bool sent_home_from_triage;         // Odesłany z triażu
    TriageColor color;                   // Kolor triażu
    DoctorType assigned_doctor;          // Przypisany specjalista
};

// ============================================================================
// ZMIENNE GLOBALNE
// ============================================================================

static volatile sig_atomic_t g_shutdown = 0;

// ============================================================================
// DEKLARACJE FUNKCJI
// ============================================================================

void initIPC(PatientData* data);
void signalHandler(int sig);
void setupSignals();

// Etapy wizyty
void enterWaitingRoom(PatientData* data);
void doRegistration(PatientData* data);
void doTriage(PatientData* data);
void doSpecialist(PatientData* data);
void exitSOR(PatientData* data);

// Wątki dla dzieci
void* parentThread(void* arg);
void* childThread(void* arg);

// ============================================================================
// FUNKCJA GŁÓWNA
// ============================================================================

int main(int argc, char* argv[]) {
    // Sprawdź argumenty
    if (argc < 4) {
        fprintf(stderr, "Użycie: pacjent <id> <wiek> <vip>\n");
        return EXIT_FAILURE;
    }
    
    // Zainicjuj dane pacjenta
    PatientData data;
    memset(&data, 0, sizeof(data));
    
    data.id = atoi(argv[1]);
    data.age = atoi(argv[2]);
    data.is_vip = atoi(argv[3]) != 0;
    data.is_child = data.age < 18;
    data.registration_complete = false;
    data.sent_home_from_triage = false;
    
    // Ustaw handlery sygnałów
    setupSignals();
    
    // Inicjalizacja IPC
    initIPC(&data);
    
    if (data.is_child) {
        // ===== DZIECKO: dwa wątki =====
        
        // Inicjalizuj mutex i condition variable
        pthread_mutex_init(&data.reg_mutex, nullptr);
        pthread_cond_init(&data.reg_done_cond, nullptr);
        
        pthread_t parent_tid, child_tid;
        
        // Utwórz wątek Rodzica (rejestracja)
        if (pthread_create(&parent_tid, nullptr, parentThread, &data) != 0) {
            SOR_FATAL("pthread_create wątek rodzica pacjent %d", data.id);
        }
        
        // Utwórz wątek Dziecka (triaż + leczenie)
        if (pthread_create(&child_tid, nullptr, childThread, &data) != 0) {
            SOR_FATAL("pthread_create wątek dziecka pacjent %d", data.id);
        }
        
        // Czekaj na zakończenie obu wątków
        pthread_join(parent_tid, nullptr);
        pthread_join(child_tid, nullptr);
        
        // Sprzątanie
        pthread_mutex_destroy(&data.reg_mutex);
        pthread_cond_destroy(&data.reg_done_cond);
        
    } else {
        // ===== DOROSŁY: jeden wątek =====
        
        // Wejście do poczekalni
        enterWaitingRoom(&data);
        
        if (g_shutdown || data.state->shutdown) goto cleanup;
        
        // Rejestracja
        doRegistration(&data);
        
        if (g_shutdown || data.state->shutdown) goto cleanup;
        
        // Triaż
        doTriage(&data);
        
        if (g_shutdown || data.state->shutdown) goto cleanup;
        
        // Jeśli nie odesłany z triażu - leczenie u specjalisty
        if (!data.sent_home_from_triage) {
            doSpecialist(&data);
        }
    }
    
cleanup:
    // Wyjście z SOR (zwolnienie miejsca)
    exitSOR(&data);
    
    // Odłącz pamięć dzieloną
    if (data.state) {
        shmdt(data.state);
    }
    
    return 0;
}

// ============================================================================
// INICJALIZACJA IPC
// ============================================================================

/**
 * @brief Podłącza się do istniejących zasobów IPC
 */
void initIPC(PatientData* data) {
    // Podłącz pamięć dzieloną
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    int shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (shmid == -1) {
        SOR_FATAL("pacjent %d: shmget", data->id);
    }
    
    data->state = (SharedState*)shmat(shmid, nullptr, 0);
    if (data->state == (void*)-1) {
        SOR_FATAL("pacjent %d: shmat", data->id);
    }
    
    // Podłącz semafory
    key_t sem_key = getIPCKey(SEM_KEY_ID);
    data->semid = semget(sem_key, SEM_COUNT, 0);
    if (data->semid == -1) {
        SOR_FATAL("pacjent %d: semget", data->id);
    }
    
    // Podłącz kolejkę komunikatów
    key_t msg_key = getIPCKey(MSG_KEY_ID);
    data->msgid = msgget(msg_key, 0);
    if (data->msgid == -1) {
        SOR_FATAL("pacjent %d: msgget", data->id);
    }
}

// ============================================================================
// OBSŁUGA SYGNAŁÓW
// ============================================================================

/**
 * @brief Handler sygnałów dla pacjenta
 */
void signalHandler(int sig) {
    if (sig == SIGUSR2 || sig == SIGTERM || sig == SIGINT) {
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
    sa.sa_flags = 0;
    
    sigaction(SIGUSR2, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

// ============================================================================
// ETAPY WIZYTY
// ============================================================================

/**
 * @brief Wejście do poczekalni (blokuje jeśli pełna)
 * Semafor SEM_POCZEKALNIA kontroluje liczbę miejsc (inicjalnie N)
 * Dziecko + opiekun zajmują 2 miejsca
 */
void enterWaitingRoom(PatientData* data) {
    if (data->is_child) {
        // Dziecko + opiekun = 2 miejsca (atomowo, unika deadlocku)
        struct sembuf op;
        op.sem_num = SEM_POCZEKALNIA;
        op.sem_op = -2;      // Czekaj aż >=2 i zajmij oba naraz
        op.sem_flg = 0;
        while (semop(data->semid, &op, 1) == -1) {
            if (errno != EINTR) {
                if (errno != EIDRM && errno != EINVAL) {
                    SOR_WARN("semop enterWaitingRoom dziecko %d", data->id);
                }
                return;
            }
        }
        
        semWait(data->semid, SEM_SHM_MUTEX);
        data->state->patients_in_sor += 2;
        int count = data->state->patients_in_sor;
        semSignal(data->semid, SEM_SHM_MUTEX);
        
        logMessage(data->state, data->semid, "Pacjent %d [Opiekun] wchodzi do budynku (%d/%d)",
                  data->id, count, N);
    } else {
        // Dorosły = 1 miejsce
        semWait(data->semid, SEM_POCZEKALNIA);
        
        semWait(data->semid, SEM_SHM_MUTEX);
        data->state->patients_in_sor++;
        int count = data->state->patients_in_sor;
        semSignal(data->semid, SEM_SHM_MUTEX);
        
        logMessage(data->state, data->semid, "Pacjent %d wchodzi do budynku (%d/%d)",
                  data->id, count, N);
    }
}

/**
 * @brief Rejestracja pacjenta
 * Wysyła wiadomość do procesu rejestracji i czeka na odpowiedź
 */
void doRegistration(PatientData* data) {
    // Dla dzieci: [Opiekun] (rodzic rejestruje)
    if (data->is_child) {
        logMessage(data->state, data->semid, "Pacjent %d [Opiekun] dołącza do kolejki rejestracji",
                  data->id);
    } else {
        logMessage(data->state, data->semid, "Pacjent %d dołącza do kolejki rejestracji%s",
                  data->id, data->is_vip ? " [VIP]" : "");
    }
    
    // Zaktualizuj licznik kolejki
    semWait(data->semid, SEM_SHM_MUTEX);
    data->state->reg_queue_count++;
    semSignal(data->semid, SEM_SHM_MUTEX);
    
    // Powiadom kontroler kolejki o zmianie (budzi z blokującego semWait)
    semSignal(data->semid, SEM_REG_QUEUE_CHANGED);
    
    // Przygotuj wiadomość do rejestracji
    SORMessage msg;
    memset(&msg, 0, sizeof(msg));
    // VIP używa osobnego typu wiadomości dla priorytetu
    msg.mtype = data->is_vip ? MSG_PATIENT_TO_REGISTRATION_VIP : MSG_PATIENT_TO_REGISTRATION;
    msg.patient_id = data->id;
    msg.patient_pid = getpid();
    msg.age = data->age;
    msg.is_vip = data->is_vip ? 1 : 0;
    
    // Wyślij do kolejki rejestracji
    if (msgsnd(data->msgid, &msg, sizeof(SORMessage) - sizeof(long), 0) == -1) {
        if (errno != EINTR) {
            SOR_WARN("pacjent %d: msgsnd rejestracja", data->id);
        }
        return;
    }
    
    // Czekaj na odpowiedź od rejestracji (blokująco)
    SORMessage response;
    long expected_type = MSG_REGISTRATION_RESPONSE + data->id;
    
    ssize_t ret = msgrcv(data->msgid, &response, sizeof(SORMessage) - sizeof(long),
                         expected_type, 0);
    
    if (ret == -1) {
        if (errno != EINTR && errno != EIDRM) {
            SOR_WARN("pacjent %d: msgrcv odpowiedź rejestracji", data->id);
        }
        return;
    }
    
    // Rejestracja zakończona - kontynuuj do triażu
}

/**
 * @brief Triaż u lekarza POZ
 * Wysyła wiadomość do kolejki i czeka na odpowiedź
 */
void doTriage(PatientData* data) {
    // Przygotuj wiadomość
    SORMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = MSG_PATIENT_TO_TRIAGE;
    msg.patient_id = data->id;
    msg.patient_pid = getpid();
    msg.age = data->age;
    msg.is_vip = data->is_vip ? 1 : 0;
    
    // Wyślij do kolejki triażu
    if (msgsnd(data->msgid, &msg, sizeof(SORMessage) - sizeof(long), 0) == -1) {
        if (errno != EINTR) {
            SOR_WARN("pacjent %d: msgsnd triaż", data->id);
        }
        return;
    }
    
    // Czekaj na odpowiedź od POZ (MSG_TRIAGE_RESPONSE)
    SORMessage response;
    long expected_type = MSG_TRIAGE_RESPONSE + data->id;
    
    ssize_t ret = msgrcv(data->msgid, &response, sizeof(SORMessage) - sizeof(long),
                         expected_type, 0);
    
    if (ret == -1) {
        if (errno != EINTR && errno != EIDRM) {
            SOR_WARN("pacjent %d: msgrcv odpowiedź triażu", data->id);
        }
        return;
    }
    
    // Zapisz wynik triażu
    data->color = response.color;
    data->assigned_doctor = response.assigned_doctor;
    
    if (response.color == COLOR_SENT_HOME || response.assigned_doctor == DOCTOR_POZ) {
        // Odesłany do domu z triażu
        data->sent_home_from_triage = true;
    }
    // Jeśli nie odesłany - doSpecialist() zajmie się resztą
}

/**
 * @brief Leczenie u specjalisty
 * Wysyła wiadomość do kolejki specjalisty i czeka na odpowiedź
 */
void doSpecialist(PatientData* data) {
    // Loguj czekanie na specjalistę - [Dziecko] dla dzieci
    if (data->is_child) {
        logMessage(data->state, data->semid, "Pacjent %d [Dziecko] czeka na lekarza: %s (kolor: %s)",
                  data->id, getDoctorName(data->assigned_doctor), 
                  getColorName(data->color));
    } else {
        logMessage(data->state, data->semid, "Pacjent %d czeka na lekarza: %s (kolor: %s)",
                  data->id, getDoctorName(data->assigned_doctor), 
                  getColorName(data->color));
    }
    
    // Przygotuj wiadomość do specjalisty (mtype = kolor triażu = priorytet)
    SORMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = colorToMtype(data->color);  // RED=1, YELLOW=2, GREEN=3
    msg.patient_id = data->id;
    msg.patient_pid = getpid();
    msg.age = data->age;
    msg.is_vip = data->is_vip ? 1 : 0;
    msg.color = data->color;
    msg.assigned_doctor = data->assigned_doctor;
    
    // Wyślij do dedykowanej kolejki specjalisty
    int spec_msgid = data->state->specialist_msgids[data->assigned_doctor];
    if (msgsnd(spec_msgid, &msg, sizeof(SORMessage) - sizeof(long), 0) == -1) {
        if (errno != EINTR && errno != EIDRM) {
            SOR_WARN("pacjent %d: msgsnd specjalista %s", data->id, getDoctorName(data->assigned_doctor));
        }
        return;
    }
    
    // Czekaj na odpowiedź od specjalisty (blokująco)
    SORMessage response;
    long expected_type = MSG_SPECIALIST_RESPONSE + data->id;
    
    ssize_t ret = msgrcv(data->msgid, &response, sizeof(SORMessage) - sizeof(long),
                         expected_type, 0);
    
    if (ret == -1) {
        if (errno != EINTR && errno != EIDRM) {
            SOR_WARN("pacjent %d: msgrcv odpowiedź specjalisty", data->id);
        }
        return;
    }
    
    // Wynik już został zalogowany przez lekarza
}

/**
 * @brief Wyjście z SOR (zwolnienie miejsca w poczekalni)
 * Dziecko + opiekun zwalniają 2 miejsca
 */
void exitSOR(PatientData* data) {
    if (data->is_child) {
        // Dziecko + opiekun = 2 miejsca
        logMessage(data->state, data->semid, "Pacjent %d [Dziecko] opuszcza SOR", data->id);
        
        semWait(data->semid, SEM_SHM_MUTEX);
        if (data->state->patients_in_sor >= 2) {
            data->state->patients_in_sor -= 2;
        } else {
            data->state->patients_in_sor = 0;
        }
        if (data->state->active_patient_count > 0) data->state->active_patient_count--;
        semSignal(data->semid, SEM_SHM_MUTEX);
        
        // Zwolnij 2 miejsca (atomowo)
        struct sembuf op;
        op.sem_num = SEM_POCZEKALNIA;
        op.sem_op = 2;       // Zwolnij oba naraz
        op.sem_flg = 0;
        while (semop(data->semid, &op, 1) == -1) {
            if (errno != EINTR) {
                if (errno != EIDRM && errno != EINVAL) {
                    SOR_WARN("semop exitSOR dziecko %d", data->id);
                }
                return;
            }
        }
    } else {
        // Dorosły = 1 miejsce
        logMessage(data->state, data->semid, "Pacjent %d opuszcza SOR", data->id);
        
        semWait(data->semid, SEM_SHM_MUTEX);
        if (data->state->patients_in_sor > 0) {
            data->state->patients_in_sor--;
        }
        if (data->state->active_patient_count > 0) data->state->active_patient_count--;
        semSignal(data->semid, SEM_SHM_MUTEX);
        
        semSignal(data->semid, SEM_POCZEKALNIA);
    }
}

// ============================================================================
// WĄTKI DLA DZIECI
// ============================================================================

/**
 * @brief Wątek Rodzica - realizuje rejestrację dla dziecka
 */
void* parentThread(void* arg) {
    PatientData* data = (PatientData*)arg;
    
    // Wejście do poczekalni (rodzic prowadzi dziecko)
    enterWaitingRoom(data);
    
    if (g_shutdown || data->state->shutdown) {
        // Sygnalizuj dziecku że kończymy
        pthread_mutex_lock(&data->reg_mutex);
        data->registration_complete = true;
        pthread_cond_signal(&data->reg_done_cond);
        pthread_mutex_unlock(&data->reg_mutex);
        return nullptr;
    }
    
    // Rodzic rejestruje dziecko
    logMessage(data->state, data->semid, "Pacjent %d [Opiekun] rozpoczyna rejestrację",
              data->id);
    
    doRegistration(data);
    
    // Sygnalizuj dziecku że rejestracja zakończona
    pthread_mutex_lock(&data->reg_mutex);
    data->registration_complete = true;
    pthread_cond_signal(&data->reg_done_cond);
    pthread_mutex_unlock(&data->reg_mutex);
    
    logMessage(data->state, data->semid, "Pacjent %d [Opiekun] zakończył rejestrację",
              data->id);
    
    return nullptr;
}

/**
 * @brief Wątek Dziecka - realizuje triaż i leczenie po zakończeniu rejestracji
 */
void* childThread(void* arg) {
    PatientData* data = (PatientData*)arg;
    
    // Czekaj na zakończenie rejestracji przez rodzica
    pthread_mutex_lock(&data->reg_mutex);
    while (!data->registration_complete && !g_shutdown && !data->state->shutdown) {
        pthread_cond_wait(&data->reg_done_cond, &data->reg_mutex);
    }
    pthread_mutex_unlock(&data->reg_mutex);
    
    if (g_shutdown || data->state->shutdown) {
        return nullptr;
    }
    
    // Dziecko idzie na triaż
    doTriage(data);
    
    if (g_shutdown || data->state->shutdown) {
        return nullptr;
    }
    
    // Jeśli nie odesłane - leczenie u specjalisty
    if (!data->sent_home_from_triage) {
        doSpecialist(data);
    }
    
    return nullptr;
}
