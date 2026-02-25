/**
 * @file rejestracja.cpp
 * @brief Proces rejestracji SOR — jedno okienko = jeden wątek
 * 
 * Wątek okienka 1: zawsze aktywny
 * Wątek okienka 2: uruchamiany gdy kolejka >= K_OPEN, zatrzymywany gdy < K_CLOSE
 * Wątek kontrolera: monitoruje długość kolejki i steruje okienkiem 2
 * 
 * VIP obsługiwane priorytetowo (ujemny mtype w msgrcv).
 */

#include "sor_common.hpp"

// ============================================================================
// ZMIENNE GLOBALNE
// ============================================================================

static SharedState* g_state = nullptr;
static int g_semid = -1;
static int g_msgid = -1;

static volatile sig_atomic_t g_shutdown = 0;

// Kontrola wątku okienka 2
static pthread_t g_window2_thread;
static pthread_mutex_t g_window2_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_window2_cond = PTHREAD_COND_INITIALIZER;
static volatile bool g_window2_active = false;
static volatile bool g_window2_should_run = false;

// ============================================================================
// HELPERY
// ============================================================================

static inline bool shouldStop() { return g_shutdown || g_state->shutdown; }

// ============================================================================
// INICJALIZACJA IPC
// ============================================================================

static void initIPC() {
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    int shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (shmid == -1) SOR_FATAL("rejestracja: shmget");

    g_state = (SharedState*)shmat(shmid, nullptr, 0);
    if (g_state == (void*)-1) SOR_FATAL("rejestracja: shmat");

    key_t sem_key = getIPCKey(SEM_KEY_ID);
    g_semid = semget(sem_key, SEM_COUNT, 0);
    if (g_semid == -1) SOR_FATAL("rejestracja: semget");

    key_t msg_key = getIPCKey(MSG_KEY_ID);
    g_msgid = msgget(msg_key, 0);
    if (g_msgid == -1) SOR_FATAL("rejestracja: msgget");
}

// ============================================================================
// OBSŁUGA SYGNAŁÓW
// ============================================================================

static void signalHandler(int sig) {
    // SIGUSR1 — tylko przerywa msgrcv (EINTR) bez ustawiania shutdown
    if (sig == SIGUSR2 || sig == SIGTERM || sig == SIGINT)
        g_shutdown = 1;
}

static void setupSignals() {
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGUSR1, &sa, nullptr);  // Budzenie wątku okienka 2 (EINTR)
    sigaction(SIGUSR2, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

// ============================================================================
// OBSŁUGA PACJENTA
// ============================================================================

static void processPatient(int window_id, SORMessage& msg) {
    logMessage(g_state, g_semid, "Pacjent %d podchodzi do okienka rejestracji %d%s",
              msg.patient_id, window_id, msg.is_vip ? " [VIP]" : "");

    semWait(g_semid, SEM_SHM_MUTEX);
    if (g_state->reg_queue_count > 0) g_state->reg_queue_count--;
    semSignal(g_semid, SEM_SHM_MUTEX);

    semSignal(g_semid, SEM_REG_QUEUE_CHANGED);

    randomSleep(REGISTRATION_MIN_MS, REGISTRATION_MAX_MS);

    // Przydziel bilet triażowy (pod mutexem — gwarantuje FIFO)
    semWait(g_semid, SEM_SHM_MUTEX);
    int triage_ticket = g_state->triage_next_ticket++;
    semSignal(g_semid, SEM_SHM_MUTEX);

    SORMessage response = msg;
    response.mtype = MSG_REGISTRATION_RESPONSE + msg.patient_id;
    response.triage_ticket = triage_ticket;

    if (msgsnd(g_msgid, &response, sizeof(SORMessage) - sizeof(long), 0) == -1) {
        if (errno != EINTR && errno != EIDRM)
            SOR_WARN("rejestracja: msgsnd odpowiedź pacjent %d", msg.patient_id);
    }

    logMessage(g_state, g_semid, "Pacjent %d przekazany do triażu, czeka na lekarza POZ",
              msg.patient_id);
}

// ============================================================================
// WĄTEK OKIENKA 2
// ============================================================================

static void* windowThread(void* arg) {
    int window_id = (int)(intptr_t)arg;

    while (!shouldStop()) {
        // Czekaj na aktywację
        pthread_mutex_lock(&g_window2_mutex);
        while (!g_window2_should_run && !shouldStop())
            pthread_cond_wait(&g_window2_cond, &g_window2_mutex);

        if (shouldStop()) { pthread_mutex_unlock(&g_window2_mutex); break; }

        g_window2_active = true;
        pthread_mutex_unlock(&g_window2_mutex);

        logMessage(g_state, g_semid, "Okienko rejestracji %d rozpoczyna pracę", window_id);

        // Obsługuj pacjentów dopóki okienko jest aktywne
        while (g_window2_should_run && !shouldStop()) {
            SORMessage msg;
            ssize_t ret = msgrcv(g_msgid, &msg, sizeof(SORMessage) - sizeof(long),
                                 -MSG_PATIENT_TO_REGISTRATION, 0);
            if (ret == -1) {
                if (errno == EINTR) continue;
                if (errno == EIDRM || errno == EINVAL) break;
                SOR_WARN("rejestracja okienko %d: msgrcv", window_id);
                continue;
            }
            processPatient(window_id, msg);
        }

        pthread_mutex_lock(&g_window2_mutex);
        g_window2_active = false;
        pthread_mutex_unlock(&g_window2_mutex);

        logMessage(g_state, g_semid, "Okienko rejestracji %d kończy pracę", window_id);
    }
    return nullptr;
}

// ============================================================================
// KONTROLER KOLEJKI
// ============================================================================

static void* queueControllerThread(void*) {
    logMessage(g_state, g_semid, "[RegCtrl] Kontroler rejestracji startuje (K_OPEN=%d, K_CLOSE=%d)",
              K_OPEN, K_CLOSE);

    while (!shouldStop()) {
        // Blokujące czekanie na zmianę kolejki (zero CPU w idle)
        semWait(g_semid, SEM_REG_QUEUE_CHANGED);
        if (shouldStop()) break;

        semWait(g_semid, SEM_SHM_MUTEX);
        int queue_count = g_state->reg_queue_count;
        bool window2_open = g_state->reg_window_2_open;
        semSignal(g_semid, SEM_SHM_MUTEX);

        if (!window2_open && queue_count >= K_OPEN) {
            // Otwórz okienko 2
            semWait(g_semid, SEM_SHM_MUTEX);
            g_state->reg_window_2_open = 1;
            semSignal(g_semid, SEM_SHM_MUTEX);

            logMessage(g_state, g_semid, "[RegCtrl] Otwieram okienko 2 (kolejka: %d >= %d)",
                      queue_count, K_OPEN);

            pthread_mutex_lock(&g_window2_mutex);
            g_window2_should_run = true;
            pthread_cond_signal(&g_window2_cond);
            pthread_mutex_unlock(&g_window2_mutex);

        } else if (window2_open && queue_count < K_CLOSE) {
            // Zamknij okienko 2
            semWait(g_semid, SEM_SHM_MUTEX);
            g_state->reg_window_2_open = 0;
            semSignal(g_semid, SEM_SHM_MUTEX);

            logMessage(g_state, g_semid, "[RegCtrl] Zamykam okienko 2 (kolejka: %d < %d)",
                      queue_count, K_CLOSE);

            pthread_mutex_lock(&g_window2_mutex);
            g_window2_should_run = false;
            pthread_mutex_unlock(&g_window2_mutex);

            // Powtarzaj SIGUSR1 aż wątek potwierdzi wyjście z pętli msgrcv
            while (g_window2_active && !shouldStop()) {
                pthread_kill(g_window2_thread, SIGUSR1);
                usleep(50000);
            }
        }
    }
    return nullptr;
}

// ============================================================================
// EMERGENCY IPC CLEANUP — gdy dyrektor nie posprzątał (kill -9)
// ============================================================================

static void emergencyIPCCleanup() {
    pid_t director_pid = g_state->director_pid;
    shmdt(g_state);

    if (director_pid > 0 && kill(director_pid, 0) == 0)
        return;  // Dyrektor żyje — on posprząta

    fprintf(stderr, "[Rejestracja] Dyrektor martwy — sprzątam IPC\n");

    auto removeQueue = [](key_t k) {
        int qid = msgget(k, 0);
        if (qid != -1) msgctl(qid, IPC_RMID, nullptr);
    };

    // Pamięć dzielona + semafory
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    int shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (shmid != -1) shmctl(shmid, IPC_RMID, nullptr);

    key_t sem_key = getIPCKey(SEM_KEY_ID);
    int semid = semget(sem_key, SEM_COUNT, 0);
    if (semid != -1) semctl(semid, 0, IPC_RMID);

    // Kolejki komunikatów
    removeQueue(getIPCKey(MSG_KEY_ID));
    removeQueue(getGateQueueKey());
    removeQueue(getOrderGateLogKey());
    removeQueue(getOrderTriageKey());
    removeQueue(getOrderExitKey());

    for (int i = DOCTOR_KARDIOLOG; i <= DOCTOR_PEDIATRA; i++)
        removeQueue(getSpecialistQueueKey((DoctorType)i));
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    if constexpr (STARTUP_DELAY_REJESTRACJA_MS > 0)
        msleep(STARTUP_DELAY_REJESTRACJA_MS);

    initIPC();
    setupSignals();

    logMessage(g_state, g_semid, "Okienko rejestracji 1 rozpoczyna pracę");

    // Kontroler kolejki (decyduje o otwarciu/zamknięciu okienka 2)
    pthread_t controller_thread;
    if (pthread_create(&controller_thread, nullptr, queueControllerThread, nullptr) != 0)
        SOR_FATAL("pthread_create kontroler kolejki");

    // Okienko 2 (początkowo nieaktywne, czeka na sygnał od kontrolera)
    if (pthread_create(&g_window2_thread, nullptr, windowThread, (void*)(intptr_t)2) != 0)
        SOR_FATAL("pthread_create okienko 2");

    // Wątek główny = okienko 1
    while (!shouldStop()) {
        SORMessage msg;
        ssize_t ret = msgrcv(g_msgid, &msg, sizeof(SORMessage) - sizeof(long),
                             -MSG_PATIENT_TO_REGISTRATION, 0);
        if (ret == -1) {
            if (errno == EINTR) continue;
            if (errno == EIDRM || errno == EINVAL) break;
            SOR_WARN("rejestracja okienko 1: msgrcv");
            continue;
        }
        processPatient(1, msg);
    }

    // Zakończenie — obudź wątki pomocnicze
    g_shutdown = 1;

    pthread_mutex_lock(&g_window2_mutex);
    g_window2_should_run = false;
    pthread_cond_signal(&g_window2_cond);
    pthread_mutex_unlock(&g_window2_mutex);
    pthread_kill(g_window2_thread, SIGUSR1);

    semSignal(g_semid, SEM_REG_QUEUE_CHANGED);

    pthread_join(g_window2_thread, nullptr);
    pthread_join(controller_thread, nullptr);

    logMessage(g_state, g_semid, "Rejestracja kończy pracę");

    emergencyIPCCleanup();
    return 0;
}
