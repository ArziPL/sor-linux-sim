/**
 * @file rejestracja.cpp
 * @brief Proces rejestracji SOR - jedno okienko = jeden wątek
 * 
 * Proces zawiera dwa wątki:
 * - Wątek okienka 1: zawsze aktywny
 * - Wątek okienka 2: uruchamiany gdy kolejka >= K_OPEN, zatrzymywany gdy < K_CLOSE
 * 
 * Obsługa kolejek:
 * - Pacjenci VIP są obsługiwani priorytetowo (osobna kolejka)
 * - Pacjenci zwykli czekają w kolejce FIFO
 */

#include "sor_common.hpp"
#include <stdarg.h>

// ============================================================================
// ZMIENNE GLOBALNE
// ============================================================================

static SharedState* g_state = nullptr;     // Pamięć dzielona
static int g_semid = -1;                   // ID semaforów
static int g_msgid = -1;                   // ID kolejki komunikatów
static int g_shmid = -1;                   // ID pamięci dzielonej

static volatile sig_atomic_t g_shutdown = 0;  // Flaga zakończenia

// Kontrola wątku okienka 2
static pthread_t g_window2_thread;            // Uchwyt wątku okienka 2
static pthread_mutex_t g_window2_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_window2_cond = PTHREAD_COND_INITIALIZER;
static volatile bool g_window2_active = false;    // Czy okienko 2 aktywne
static volatile bool g_window2_should_run = false; // Czy okienko 2 ma działać

// ============================================================================
// DEKLARACJE FUNKCJI
// ============================================================================

void initIPC();
void signalHandler(int sig);
void setupSignals();
void* windowThread(void* arg);
void* queueControllerThread(void* arg);
void processPatient(int window_id, SORMessage& msg);

// ============================================================================
// FUNKCJA GŁÓWNA
// ============================================================================

int main() {
    // Inicjalizacja IPC
    initIPC();
    
    // Ustaw handlery sygnałów
    setupSignals();
    
    // Loguj start
    logMessage(g_state, g_semid, "Okienko rejestracji 1 rozpoczyna pracę");
    
    // Utwórz wątek kontrolera kolejki (decyduje o otwarciu/zamknięciu okienka 2)
    pthread_t controller_thread;
    if (pthread_create(&controller_thread, nullptr, queueControllerThread, nullptr) != 0) {
        handleError("pthread_create controller");
    }
    
    // Utwórz wątek okienka 2 (początkowo nieaktywny, czeka na sygnał)
    int window2_id = 2;
    if (pthread_create(&g_window2_thread, nullptr, windowThread, (void*)(intptr_t)window2_id) != 0) {
        handleError("pthread_create window2");
    }
    
    // Wątek główny = okienko 1
    int window1_id = 1;
    
    while (!g_shutdown && !g_state->shutdown) {
        SORMessage msg;
        
        // Blokujące odbieranie z priorytetem VIP:
        // Ujemny mtype oznacza: odbierz wiadomość z mtype <= |wartość|,
        // najniższy mtype pierwszy. VIP (mtype=1) przed zwykłym (mtype=2).
        ssize_t ret = msgrcv(g_msgid, &msg, sizeof(SORMessage) - sizeof(long), 
                             -MSG_PATIENT_TO_REGISTRATION, 0);
        
        if (ret == -1) {
            if (errno == EINTR) continue;
            if (errno == EIDRM || errno == EINVAL) break;
            printError("rejestracja okienko 1: msgrcv");
            continue;
        }
        
        // Mamy pacjenta do obsługi
        processPatient(window1_id, msg);
    }
    
    // Zakończenie - poczekaj na wątki
    g_shutdown = 1;
    
    // Obudź wątek okienka 2 jeśli czeka
    pthread_mutex_lock(&g_window2_mutex);
    g_window2_should_run = false;
    pthread_cond_signal(&g_window2_cond);
    pthread_mutex_unlock(&g_window2_mutex);
    
    pthread_join(g_window2_thread, nullptr);
    pthread_join(controller_thread, nullptr);
    
    logMessage(g_state, g_semid, "Rejestracja kończy pracę");
    
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
    g_shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (g_shmid == -1) {
        handleError("rejestracja: shmget");
    }
    
    g_state = (SharedState*)shmat(g_shmid, nullptr, 0);
    if (g_state == (void*)-1) {
        handleError("rejestracja: shmat");
    }
    
    // Podłącz semafory
    key_t sem_key = getIPCKey(SEM_KEY_ID);
    g_semid = semget(sem_key, SEM_COUNT, 0);
    if (g_semid == -1) {
        handleError("rejestracja: semget");
    }
    
    // Podłącz kolejkę komunikatów
    key_t msg_key = getIPCKey(MSG_KEY_ID);
    g_msgid = msgget(msg_key, 0);
    if (g_msgid == -1) {
        handleError("rejestracja: msgget");
    }
}

// ============================================================================
// OBSŁUGA SYGNAŁÓW
// ============================================================================

/**
 * @brief Handler sygnałów
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
    
    sigaction(SIGUSR1, &sa, nullptr);  // Do budzenia wątku okienka 2 (EINTR)
    sigaction(SIGUSR2, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

// ============================================================================
// OBSŁUGA PACJENTA
// ============================================================================

/**
 * @brief Obsługuje pacjenta przy okienku rejestracji
 * @param window_id Numer okienka (1 lub 2)
 * @param msg Wiadomość od pacjenta
 */
void processPatient(int window_id, SORMessage& msg) {
    logMessage(g_state, g_semid, "Pacjent %d podchodzi do okienka rejestracji %d%s",
              msg.patient_id, window_id, msg.is_vip ? " [VIP]" : "");
    
    // Zmniejsz licznik kolejki
    semWait(g_semid, SEM_SHM_MUTEX);
    if (g_state->reg_queue_count > 0) {
        g_state->reg_queue_count--;
    }
    if (msg.is_vip && g_state->reg_queue_vip_count > 0) {
        g_state->reg_queue_vip_count--;
    }
    semSignal(g_semid, SEM_SHM_MUTEX);
    
    // Powiadom kontroler kolejki o zmianie
    semSignal(g_semid, SEM_REG_QUEUE_CHANGED);
    
    // Symulacja czasu rejestracji
    randomSleep(REGISTRATION_MIN_MS, REGISTRATION_MAX_MS);
    
    // Wyślij odpowiedź do pacjenta
    SORMessage response = msg;
    response.mtype = MSG_REGISTRATION_RESPONSE + msg.patient_id;
    
    if (msgsnd(g_msgid, &response, sizeof(SORMessage) - sizeof(long), 0) == -1) {
        if (errno != EINTR && errno != EIDRM) {
            printError("rejestracja: msgsnd response");
        }
    }
    
    logMessage(g_state, g_semid, "Pacjent %d przekazany do triażu, czeka na lekarza POZ",
              msg.patient_id);
}

// ============================================================================
// WĄTEK OKIENKA 2
// ============================================================================

/**
 * @brief Wątek okienka rejestracji (używany dla okienka 2)
 * Czeka na sygnał aktywacji, potem obsługuje pacjentów dopóki jest aktywny
 */
void* windowThread(void* arg) {
    int window_id = (int)(intptr_t)arg;
    
    while (!g_shutdown && !g_state->shutdown) {
        // Czekaj na aktywację
        pthread_mutex_lock(&g_window2_mutex);
        while (!g_window2_should_run && !g_shutdown && !g_state->shutdown) {
            pthread_cond_wait(&g_window2_cond, &g_window2_mutex);
        }
        
        if (g_shutdown || g_state->shutdown) {
            pthread_mutex_unlock(&g_window2_mutex);
            break;
        }
        
        g_window2_active = true;
        pthread_mutex_unlock(&g_window2_mutex);
        
        logMessage(g_state, g_semid, "Okienko rejestracji %d rozpoczyna pracę", window_id);
        
        // Obsługuj pacjentów dopóki okienko jest aktywne
        while (g_window2_should_run && !g_shutdown && !g_state->shutdown) {
            SORMessage msg;
            
            // Blokujące odbieranie z priorytetem VIP (ujemny mtype)
            ssize_t ret = msgrcv(g_msgid, &msg, sizeof(SORMessage) - sizeof(long), 
                                 -MSG_PATIENT_TO_REGISTRATION, 0);
            
            if (ret == -1) {
                if (errno == EINTR) continue;
                if (errno == EIDRM || errno == EINVAL) break;
                printError("rejestracja okienko 2: msgrcv");
                continue;
            }
            
            // Obsłuż pacjenta
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

/**
 * @brief Wątek kontrolera kolejki - decyduje o otwarciu/zamknięciu okienka 2
 * Monitoruje długość kolejki i aktywuje/deaktywuje okienko 2
 */
void* queueControllerThread(void* arg) {
    (void)arg;
    
    logMessage(g_state, g_semid, "[RegCtrl] Kontroler rejestracji startuje (K_OPEN=%d, K_CLOSE=%d)", 
              K_OPEN, K_CLOSE);
    
    while (!g_shutdown && !g_state->shutdown) {
        // Blokujące czekanie na zmianę kolejki (zero CPU w idle)
        semWait(g_semid, SEM_REG_QUEUE_CHANGED);
        
        if (g_shutdown || g_state->shutdown) break;
        
        // Sprawdź długość kolejki
        semWait(g_semid, SEM_SHM_MUTEX);
        int queue_count = g_state->reg_queue_count;
        bool window2_open = g_state->reg_window_2_open;
        semSignal(g_semid, SEM_SHM_MUTEX);
        
        // Decyzja o otwarciu okienka 2
        if (!window2_open && queue_count >= K_OPEN) {
            // Otwórz okienko 2
            semWait(g_semid, SEM_SHM_MUTEX);
            g_state->reg_window_2_open = 1;
            semSignal(g_semid, SEM_SHM_MUTEX);
            
            logMessage(g_state, g_semid, "[RegCtrl] Otwieram okienko 2 (kolejka: %d >= %d)", 
                      queue_count, K_OPEN);
            
            // Aktywuj wątek okienka 2
            pthread_mutex_lock(&g_window2_mutex);
            g_window2_should_run = true;
            pthread_cond_signal(&g_window2_cond);
            pthread_mutex_unlock(&g_window2_mutex);
        }
        // Decyzja o zamknięciu okienka 2
        else if (window2_open && queue_count < K_CLOSE) {
            // Zamknij okienko 2
            semWait(g_semid, SEM_SHM_MUTEX);
            g_state->reg_window_2_open = 0;
            semSignal(g_semid, SEM_SHM_MUTEX);
            
            logMessage(g_state, g_semid, "[RegCtrl] Zamykam okienko 2 (kolejka: %d < %d)", 
                      queue_count, K_CLOSE);
            
            // Deaktywuj wątek okienka 2
            pthread_mutex_lock(&g_window2_mutex);
            g_window2_should_run = false;
            pthread_mutex_unlock(&g_window2_mutex);
            
            // Wybudź wątek okienka 2 z blokującego msgrcv (EINTR)
            pthread_kill(g_window2_thread, SIGUSR1);
        }
    }
    
    return nullptr;
}
