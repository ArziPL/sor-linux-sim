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
 * - Wątek Rodzica: wejście + rejestracja
 * - Wątek Dziecka: triaż + leczenie (po zakończeniu rejestracji)
 */

#include "sor_common.hpp"

// ============================================================================
// STRUKTURA DANYCH PACJENTA
// ============================================================================

struct PatientData {
    int id;
    int age;
    bool is_vip;
    bool is_child;              // age < 18

    // Bilety wejścia do poczekalni (przydzielone przez generator)
    long gate_ticket1;          // Zawsze
    long gate_ticket2;          // Tylko dzieci (0 = brak)

    // Zasoby IPC
    int semid;
    int msgid;
    SharedState* state;

    // Synchronizacja wątków (tylko dzieci)
    pthread_mutex_t reg_mutex;
    pthread_cond_t reg_done_cond;
    bool registration_complete;

    // Wynik ścieżki
    bool sent_home_from_triage;
    TriageColor color;
    DoctorType assigned_doctor;

    // Token gate_log trzymany do msgsnd rejestracji
    GateToken held_order_token;
    bool holding_gate_token;

    // Bilety porządkujące (FIFO triaż i wyjście)
    int triage_ticket;
    int exit_ticket;
};

// ============================================================================
// ZMIENNE GLOBALNE
// ============================================================================

static volatile sig_atomic_t g_shutdown = 0;

// ============================================================================
// HELPERY
// ============================================================================

/// Bezpieczny msgsnd z obsługą EINTR/EIDRM/EINVAL
static bool safeMsgsnd(int qid, void* buf, size_t size, const char* ctx, int patient_id) {
    if (msgsnd(qid, buf, size, 0) == -1) {
        if (errno != EINTR && errno != EIDRM && errno != EINVAL)
            SOR_WARN("pacjent %d: msgsnd %s", patient_id, ctx);
        return false;
    }
    return true;
}

/// Bezpieczny msgrcv z retry na EINTR — zwraca true jeśli sukces
static bool safeMsgrcv(int qid, void* buf, size_t size, long mtype) {
    while (msgrcv(qid, buf, size, mtype, 0) == -1) {
        if (errno != EINTR) return false;
    }
    return true;
}

/// Czekaj na bilet w kolejce porządkującej (blokujące)
static bool orderQueueWait(int qid, long ticket) {
    if (ticket <= 0) return true;
    GateToken tok;
    return safeMsgrcv(qid, &tok, GATE_TOKEN_SIZE, ticket);
}

/// Oddaj bilet w kolejce porządkującej (odblokuj następnego)
static void orderQueueRelease(int qid, long ticket, int patient_id, const char* ctx) {
    if (ticket <= 0) return;
    GateToken rel;
    rel.mtype = ticket + 1;
    rel.data[0] = 0;
    safeMsgsnd(qid, &rel, GATE_TOKEN_SIZE, ctx, patient_id);
}

/// Czy powinniśmy przerwać (shutdown)
static inline bool shouldStop(PatientData* d) {
    return g_shutdown || d->state->shutdown;
}

// ============================================================================
// INICJALIZACJA IPC
// ============================================================================

static void initIPC(PatientData* data) {
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    int shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (shmid == -1) SOR_FATAL("pacjent %d: shmget", data->id);

    data->state = (SharedState*)shmat(shmid, nullptr, 0);
    if (data->state == (void*)-1) SOR_FATAL("pacjent %d: shmat", data->id);

    key_t sem_key = getIPCKey(SEM_KEY_ID);
    data->semid = semget(sem_key, SEM_COUNT, 0);
    if (data->semid == -1) SOR_FATAL("pacjent %d: semget", data->id);

    key_t msg_key = getIPCKey(MSG_KEY_ID);
    data->msgid = msgget(msg_key, 0);
    if (data->msgid == -1) SOR_FATAL("pacjent %d: msgget", data->id);
}

// ============================================================================
// OBSŁUGA SYGNAŁÓW
// ============================================================================

static void signalHandler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

static void setupSignals() {
    struct sigaction sa{};
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
 * Bilety przydzielone przez generator (argv) — ścisłe FIFO.
 * Dziecko + opiekun zajmują 2 miejsca (2 bilety).
 */
static void enterWaitingRoom(PatientData* data) {
    int gate = data->state->gate_msgid;
    GateToken token;
    int step = data->is_child ? 2 : 1;

    // Czekaj na bilet(y) gate
    if (!safeMsgrcv(gate, &token, GATE_TOKEN_SIZE, data->gate_ticket1)) return;
    if (data->is_child) {
        if (!safeMsgrcv(gate, &token, GATE_TOKEN_SIZE, data->gate_ticket2)) return;
    }

    // Czekaj na kolej w kolejce porządkującej (FIFO logowania wejścia)
    GateToken order_token;
    if (!safeMsgrcv(data->state->order_gate_log_msgid, &order_token, GATE_TOKEN_SIZE,
                    data->gate_ticket1))
        return;

    semWait(data->semid, SEM_SHM_MUTEX);
    data->state->patients_in_sor += step;
    int count = data->state->patients_in_sor;
    if (data->is_child) {
        logMessage(data->state, data->semid, "Pacjent %d [Opiekun] wchodzi do budynku (%d/%d)",
                  data->id, count, N);
    } else {
        logMessage(data->state, data->semid, "Pacjent %d wchodzi do budynku (%d/%d)",
                  data->id, count, N);
    }
    semSignal(data->semid, SEM_SHM_MUTEX);

    // Token trzymany aż do msgsnd rejestracji — gwarantuje FIFO od wejścia do kolejki
    data->held_order_token = order_token;
    data->holding_gate_token = true;
}

/**
 * @brief Rejestracja pacjenta — wysyła do okienka i czeka na odpowiedź
 */
static void doRegistration(PatientData* data) {
    SORMessage msg{};
    msg.mtype = data->is_vip ? MSG_PATIENT_TO_REGISTRATION_VIP : MSG_PATIENT_TO_REGISTRATION;
    msg.patient_id = data->id;
    msg.patient_pid = getpid();
    msg.age = data->age;
    msg.is_vip = data->is_vip ? 1 : 0;

    // Dołącz do kolejki rejestracji (pod ochroną tokenu gate — FIFO)
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

    safeMsgsnd(data->msgid, &msg, sizeof(SORMessage) - sizeof(long), "kolejka rejestracji", data->id);

    // Oddaj token gate — następny pacjent może wejść
    if (data->holding_gate_token) {
        int step = data->is_child ? 2 : 1;
        data->held_order_token.mtype = data->gate_ticket1 + step;
        safeMsgsnd(data->state->order_gate_log_msgid, &data->held_order_token,
                   GATE_TOKEN_SIZE, "order_gate_log", data->id);
        data->holding_gate_token = false;
    }

    semSignal(data->semid, SEM_REG_QUEUE_CHANGED);

    // Czekaj na odpowiedź od rejestracji
    SORMessage response;
    if (!safeMsgrcv(data->msgid, &response, sizeof(SORMessage) - sizeof(long),
                    MSG_REGISTRATION_RESPONSE + data->id))
        return;

    data->triage_ticket = response.triage_ticket;
}

/**
 * @brief Triaż u lekarza POZ — bilet porządkujący gwarantuje FIFO
 */
static void doTriage(PatientData* data) {
    SORMessage msg{};
    msg.mtype = MSG_PATIENT_TO_TRIAGE;
    msg.patient_id = data->id;
    msg.patient_pid = getpid();
    msg.age = data->age;
    msg.is_vip = data->is_vip ? 1 : 0;

    // Czekaj na swoją kolej w triażu
    orderQueueWait(data->state->order_triage_msgid, data->triage_ticket);

    safeMsgsnd(data->msgid, &msg, sizeof(SORMessage) - sizeof(long), "triaż", data->id);

    // Oddaj token triażu
    orderQueueRelease(data->state->order_triage_msgid, data->triage_ticket,
                      data->id, "order_triage");

    // Czekaj na odpowiedź od POZ
    SORMessage response;
    if (!safeMsgrcv(data->msgid, &response, sizeof(SORMessage) - sizeof(long),
                    MSG_TRIAGE_RESPONSE + data->id))
        return;

    data->color = response.color;
    data->assigned_doctor = response.assigned_doctor;

    if (response.color == COLOR_SENT_HOME || response.assigned_doctor == DOCTOR_POZ) {
        data->sent_home_from_triage = true;
        data->exit_ticket = response.exit_ticket;
    }
}

/**
 * @brief Leczenie u specjalisty — POZ już wstawił do kolejki specjalisty,
 *        pacjent czeka tylko na wynik.
 */
static void doSpecialist(PatientData* data) {
    SORMessage response;
    if (!safeMsgrcv(data->msgid, &response, sizeof(SORMessage) - sizeof(long),
                    MSG_SPECIALIST_RESPONSE + data->id))
        return;

    data->exit_ticket = response.exit_ticket;
}

/**
 * @brief Wyjście z SOR — zwolnienie miejsca, oddanie tokenów gate
 */
static void exitSOR(PatientData* data) {
    // Czekaj na swoją kolej wyjścia (FIFO)
    orderQueueWait(data->state->order_exit_msgid, data->exit_ticket);

    int gate = data->state->gate_msgid;
    int step = data->is_child ? 2 : 1;

    semWait(data->semid, SEM_SHM_MUTEX);

    if (data->is_child) {
        logMessage(data->state, data->semid, "Pacjent %d [Dziecko] opuszcza SOR", data->id);
        data->state->patients_in_sor = (data->state->patients_in_sor >= 2)
            ? data->state->patients_in_sor - 2 : 0;
    } else {
        logMessage(data->state, data->semid, "Pacjent %d opuszcza SOR", data->id);
        if (data->state->patients_in_sor > 0) data->state->patients_in_sor--;
    }
    if (data->state->active_patient_count > 0) data->state->active_patient_count--;

    // Wyślij tokeny gate (obudź następnych czekających)
    for (int i = 0; i < step; i++) {
        GateToken token;
        token.data[0] = 0;
        token.mtype = data->state->gate_now_serving++;
        safeMsgsnd(gate, &token, GATE_TOKEN_SIZE, "gate token", data->id);
    }
    semSignal(data->semid, SEM_SHM_MUTEX);

    // Oddaj token wyjścia
    orderQueueRelease(data->state->order_exit_msgid, data->exit_ticket,
                      data->id, "order_exit");
}

// ============================================================================
// WĄTKI DLA DZIECI
// ============================================================================

static void* parentThread(void* arg) {
    PatientData* data = (PatientData*)arg;

    enterWaitingRoom(data);

    if (!shouldStop(data)) {
        logMessage(data->state, data->semid, "Pacjent %d [Opiekun] rozpoczyna rejestrację", data->id);
        doRegistration(data);
        logMessage(data->state, data->semid, "Pacjent %d [Opiekun] zakończył rejestrację", data->id);
    }

    // Sygnalizuj dziecku że rejestracja zakończona (lub shutdown)
    pthread_mutex_lock(&data->reg_mutex);
    data->registration_complete = true;
    pthread_cond_signal(&data->reg_done_cond);
    pthread_mutex_unlock(&data->reg_mutex);

    return nullptr;
}

static void* childThread(void* arg) {
    PatientData* data = (PatientData*)arg;

    // Czekaj na zakończenie rejestracji przez rodzica
    pthread_mutex_lock(&data->reg_mutex);
    while (!data->registration_complete && !shouldStop(data))
        pthread_cond_wait(&data->reg_done_cond, &data->reg_mutex);
    pthread_mutex_unlock(&data->reg_mutex);

    if (shouldStop(data)) return nullptr;

    doTriage(data);
    if (shouldStop(data)) return nullptr;

    if (!data->sent_home_from_triage)
        doSpecialist(data);

    return nullptr;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Użycie: pacjent <id> <wiek> <vip> <ticket1> [ticket2]\n");
        return EXIT_FAILURE;
    }

    PatientData data{};
    data.id = atoi(argv[1]);
    data.age = atoi(argv[2]);
    data.is_vip = atoi(argv[3]) != 0;
    data.is_child = data.age < 18;
    data.gate_ticket1 = atol(argv[4]);
    data.gate_ticket2 = (argc >= 6) ? atol(argv[5]) : 0;

    setupSignals();
    initIPC(&data);

    if (data.is_child) {
        // Dziecko: dwa wątki (rodzic=rejestracja, dziecko=triaż+leczenie)
        pthread_mutex_init(&data.reg_mutex, nullptr);
        pthread_cond_init(&data.reg_done_cond, nullptr);

        pthread_t parent_tid, child_tid;
        if (pthread_create(&parent_tid, nullptr, parentThread, &data) != 0)
            SOR_FATAL("pthread_create rodzic pacjent %d", data.id);
        if (pthread_create(&child_tid, nullptr, childThread, &data) != 0)
            SOR_FATAL("pthread_create dziecko pacjent %d", data.id);

        pthread_join(parent_tid, nullptr);
        pthread_join(child_tid, nullptr);

        pthread_mutex_destroy(&data.reg_mutex);
        pthread_cond_destroy(&data.reg_done_cond);
    } else {
        // Dorosły: liniowa ścieżka
        enterWaitingRoom(&data);
        if (!shouldStop(&data)) doRegistration(&data);
        if (!shouldStop(&data)) doTriage(&data);
        if (!shouldStop(&data) && !data.sent_home_from_triage) doSpecialist(&data);
    }

    exitSOR(&data);
    shmdt(data.state);
    return 0;
}
