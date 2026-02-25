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
    
    // Bilety wejścia do poczekalni (przydzielone przez generator)
    long gate_ticket1;                  // Pierwszy bilet (zawsze)
    long gate_ticket2;                  // Drugi bilet (tylko dzieci; 0 = brak)
    
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
    
    // Token gate_log trzymany do momentu msgsnd rejestracji
    GateToken held_order_token;
    bool holding_gate_token;
    
    // Bilety porządkujące (FIFO triaż i wyjście)
    int triage_ticket;               // Bilet triażowy (z rejestracji)
    int exit_ticket;                 // Bilet wyjściowy (od lekarza)
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
    if (argc < 5) {
        fprintf(stderr, "Użycie: pacjent <id> <wiek> <vip> <ticket1> [ticket2]\n");
        return EXIT_FAILURE;
    }
    
    // Zainicjuj dane pacjenta
    PatientData data;
    memset(&data, 0, sizeof(data));
    
    data.id = atoi(argv[1]);
    data.age = atoi(argv[2]);
    data.is_vip = atoi(argv[3]) != 0;
    data.is_child = data.age < 18;
    data.gate_ticket1 = atol(argv[4]);
    data.gate_ticket2 = (argc >= 6) ? atol(argv[5]) : 0;
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
 * Bilety przydzielone przez generator (argv) — ścisłe FIFO gwarantowane.
 * Pacjent czeka na msgrcv(mtype=swój_bilet), zero wyścigu.
 * Dziecko + opiekun zajmują 2 miejsca (2 bilety).
 */
void enterWaitingRoom(PatientData* data) {
    int gate = data->state->gate_msgid;
    GateToken token;

    if (data->is_child) {
        // Czekaj na oba bilety (przydzielone przez generator)
        for (long t : {data->gate_ticket1, data->gate_ticket2}) {
            while (msgrcv(gate, &token, GATE_TOKEN_SIZE, t, 0) == -1) {
                if (errno != EINTR) {
                    if (errno != EIDRM && errno != EINVAL) {
                        SOR_WARN("msgrcv gate dziecko %d (ticket %ld)", data->id, t);
                    }
                    return;
                }
            }
        }
        
        // Czekaj na swoją kolej w kolejce porządkującej (FIFO, blokujące — zero CPU)
        // Token trzymany aż do msgsnd rejestracji — gwarantuje FIFO od wejścia do kolejki
        {
            GateToken order_token;
            while (msgrcv(data->state->order_gate_log_msgid, &order_token, GATE_TOKEN_SIZE,
                          data->gate_ticket1, 0) == -1) {
                if (errno != EINTR) return;
            }
            semWait(data->semid, SEM_SHM_MUTEX);
            data->state->patients_in_sor += 2;
            int count = data->state->patients_in_sor;
            logMessage(data->state, data->semid, "Pacjent %d [Opiekun] wchodzi do budynku (%d/%d)",
                      data->id, count, N);
            semSignal(data->semid, SEM_SHM_MUTEX);
            // Token oddany w doRegistration() po msgsnd
            data->held_order_token = order_token;
            data->holding_gate_token = true;
        }
    } else {
        // Czekaj na swój bilet (przydzielony przez generator)
        while (msgrcv(gate, &token, GATE_TOKEN_SIZE, data->gate_ticket1, 0) == -1) {
            if (errno != EINTR) {
                if (errno != EIDRM && errno != EINVAL) {
                    SOR_WARN("msgrcv gate pacjent %d (ticket %ld)", data->id, data->gate_ticket1);
                }
                return;
            }
        }
        
        // Czekaj na swoją kolej w kolejce porządkującej (FIFO, blokujące — zero CPU)
        // Token trzymany aż do msgsnd rejestracji — gwarantuje FIFO od wejścia do kolejki
        {
            GateToken order_token;
            while (msgrcv(data->state->order_gate_log_msgid, &order_token, GATE_TOKEN_SIZE,
                          data->gate_ticket1, 0) == -1) {
                if (errno != EINTR) return;
            }
            semWait(data->semid, SEM_SHM_MUTEX);
            data->state->patients_in_sor++;
            int count = data->state->patients_in_sor;
            logMessage(data->state, data->semid, "Pacjent %d wchodzi do budynku (%d/%d)",
                      data->id, count, N);
            semSignal(data->semid, SEM_SHM_MUTEX);
            // Token oddany w doRegistration() po msgsnd
            data->held_order_token = order_token;
            data->holding_gate_token = true;
        }
    }
}

/**
 * @brief Rejestracja pacjenta
 * Wysyła wiadomość do procesu rejestracji i czeka na odpowiedź
 */
void doRegistration(PatientData* data) {
    // Przygotuj wiadomość do rejestracji
    SORMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = data->is_vip ? MSG_PATIENT_TO_REGISTRATION_VIP : MSG_PATIENT_TO_REGISTRATION;
    msg.patient_id = data->id;
    msg.patient_pid = getpid();
    msg.age = data->age;
    msg.is_vip = data->is_vip ? 1 : 0;
    
    // Dołącz do kolejki rejestracji — pod ochroną tokenu gate (FIFO gwarantowane)
    semWait(data->semid, SEM_SHM_MUTEX);
    if (data->is_child) {
        logMessage(data->state, data->semid, "Pacjent %d [Opiekun] dołącza do kolejki rejestracji",
                  data->id);
    } else {
        logMessage(data->state, data->semid, "Pacjent %d dołącza do kolejki rejestracji%s",
                  data->id, data->is_vip ? " [VIP]" : "");
    }
    data->state->reg_queue_count++;
    semSignal(data->semid, SEM_SHM_MUTEX);
    // msgsnd do kolejki rejestracji — wciąż trzymamy token gate_log
    if (msgsnd(data->msgid, &msg, sizeof(SORMessage) - sizeof(long), 0) == -1) {
        if (errno != EINTR && errno != EIDRM && errno != EINVAL)
            SOR_WARN("pacjent %d: msgsnd do kolejki rejestracji", data->id);
    }
    // Teraz oddaj token gate — następny pacjent może wejść
    if (data->holding_gate_token) {
        int step = data->is_child ? 2 : 1;
        data->held_order_token.mtype = data->gate_ticket1 + step;
        if (msgsnd(data->state->order_gate_log_msgid, &data->held_order_token, GATE_TOKEN_SIZE, 0) == -1) {
            if (errno != EINTR && errno != EIDRM && errno != EINVAL)
                SOR_WARN("pacjent %d: msgsnd order_gate_log", data->id);
        }
        data->holding_gate_token = false;
    }
    // Powiadom kontroler kolejki o zmianie
    semSignal(data->semid, SEM_REG_QUEUE_CHANGED);
    
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
    
    // Rejestracja zakończona - zapisz bilet triażowy
    data->triage_ticket = response.triage_ticket;
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
    
    // Czekaj na swoją kolej w triażu (bilet z rejestracji — FIFO gwarantowane)
    if (data->triage_ticket > 0) {
        GateToken triage_order;
        while (msgrcv(data->state->order_triage_msgid, &triage_order, GATE_TOKEN_SIZE,
                      data->triage_ticket, 0) == -1) {
            if (errno != EINTR) break;
        }
    }
    
    // Wyślij wiadomość do triażu
    if (msgsnd(data->msgid, &msg, sizeof(SORMessage) - sizeof(long), 0) == -1) {
        if (errno != EINTR && errno != EIDRM && errno != EINVAL)
            SOR_WARN("pacjent %d: msgsnd do triażu", data->id);
    }
    
    // Oddaj token triażu — następny pacjent może wysłać
    if (data->triage_ticket > 0) {
        GateToken rel;
        rel.mtype = data->triage_ticket + 1;
        rel.data[0] = 0;
        if (msgsnd(data->state->order_triage_msgid, &rel, GATE_TOKEN_SIZE, 0) == -1) {
            if (errno != EINTR && errno != EIDRM && errno != EINVAL)
                SOR_WARN("pacjent %d: msgsnd order_triage", data->id);
        }
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
        data->exit_ticket = response.exit_ticket;
    }
    // Jeśli nie odesłany - doSpecialist() zajmie się resztą
}

/**
 * @brief Leczenie u specjalisty
 * Wysyła wiadomość do kolejki specjalisty i czeka na odpowiedź
 */
void doSpecialist(PatientData* data) {
    // Log + msgsnd do kolejki specjalisty wykonał już lekarz POZ (gwarantuje FIFO).
    // Pacjent czeka tylko na wynik od specjalisty.

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
    
    // Wynik już został zalogowany przez lekarza — zapisz bilet wyjściowy
    data->exit_ticket = response.exit_ticket;
}

/**
 * @brief Wyjście z SOR (zwolnienie miejsca w poczekalni)
 * Dziecko + opiekun zwalniają 2 miejsca
 */
void exitSOR(PatientData* data) {
    // Czekaj na swoją kolej wyjścia (FIFO)
    if (data->exit_ticket > 0) {
        GateToken exit_order;
        while (msgrcv(data->state->order_exit_msgid, &exit_order, GATE_TOKEN_SIZE,
                      data->exit_ticket, 0) == -1) {
            if (errno != EINTR) break;
        }
    }
    
    int gate = data->state->gate_msgid;
    GateToken token;
    token.data[0] = 0;

    int step = data->is_child ? 2 : 1;

    semWait(data->semid, SEM_SHM_MUTEX);
    // Log + dekrementacja + wysłanie tokenów — wszystko pod mutexem
    if (data->is_child) {
        logMessage(data->state, data->semid, "Pacjent %d [Dziecko] opuszcza SOR", data->id);
        if (data->state->patients_in_sor >= 2)
            data->state->patients_in_sor -= 2;
        else
            data->state->patients_in_sor = 0;
    } else {
        logMessage(data->state, data->semid, "Pacjent %d opuszcza SOR", data->id);
        if (data->state->patients_in_sor > 0)
            data->state->patients_in_sor--;
    }
    if (data->state->active_patient_count > 0) data->state->active_patient_count--;

    // Wyślij tokeny gate (obudź następnych)
    for (int i = 0; i < step; i++) {
        long w = data->state->gate_now_serving++;
        token.mtype = w;
        if (msgsnd(gate, &token, GATE_TOKEN_SIZE, 0) == -1) {
            if (errno != EINTR && errno != EIDRM && errno != EINVAL)
                SOR_WARN("pacjent %d: msgsnd gate token (mtype %ld)", data->id, w);
        }
    }
    semSignal(data->semid, SEM_SHM_MUTEX);
    
    // Oddaj token wyjścia — następny pacjent może opuścić SOR
    if (data->exit_ticket > 0) {
        GateToken rel;
        rel.mtype = data->exit_ticket + 1;
        rel.data[0] = 0;
        if (msgsnd(data->state->order_exit_msgid, &rel, GATE_TOKEN_SIZE, 0) == -1) {
            if (errno != EINTR && errno != EIDRM && errno != EINVAL)
                SOR_WARN("pacjent %d: msgsnd order_exit", data->id);
        }
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
