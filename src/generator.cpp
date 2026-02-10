/**
 * @file generator.cpp
 * @brief Generator pacjentów — osobny proces uruchamiany przez dyrektora (execl)
 *
 * Podłącza się do istniejących zasobów IPC (pamięć dzielona, semafory)
 * i w pętli tworzy nowych pacjentów (fork + execl pacjent).
 * Respektuje limit max_patients z SharedState.
 * Obsługuje SIGTERM — czyste zamknięcie z zebraniem procesów potomnych.
 */

#include "sor_common.hpp"

// ============================================================================
// ZMIENNE GLOBALNE GENERATORA
// ============================================================================

static volatile sig_atomic_t g_gen_shutdown = 0;

// Tablica PIDów pacjentów-dzieci
static pid_t g_patient_pids[1000];
static int g_patient_count = 0;

// ============================================================================
// HANDLER SYGNAŁU
// ============================================================================

static void genSigHandler(int sig) {
    (void)sig;
    g_gen_shutdown = 1;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    // Ustaw handler SIGTERM/SIGINT (bez SA_RESTART — usleep/semop przerywalne)
    struct sigaction sa{};
    sa.sa_handler = genSigHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    // Podłącz pamięć dzieloną
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    int shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (shmid == -1) {
        SOR_FATAL("Generator: shmget");
    }

    SharedState* state = (SharedState*)shmat(shmid, nullptr, 0);
    if (state == (void*)-1) {
        SOR_FATAL("Generator: shmat");
    }

    // Podłącz semafory
    key_t sem_key = getIPCKey(SEM_KEY_ID);
    int semid = semget(sem_key, SEM_COUNT, 0);
    if (semid == -1) {
        SOR_FATAL("Generator: semget");
    }

    logMessage(state, semid, "[Generator] Generator pacjentów startuje (PID %d)", getpid());

    int patient_id = 0;

    while (!state->shutdown && !g_gen_shutdown) {
        // Losowy czas do następnego pacjenta
        randomSleep(PATIENT_GEN_MIN_MS, PATIENT_GEN_MAX_MS);

        if (state->shutdown || g_gen_shutdown) break;

        // Czekaj jeśli osiągnięto limit jednoczesnych procesów pacjentów
        // max_patients to całkowity limit, odejmujemy stałe procesy
        int patient_limit = state->max_patients - FIXED_PROCESS_COUNT;
        if (state->max_patients > 0 && patient_limit > 0) {
            while (!state->shutdown && !g_gen_shutdown) {
                semWait(semid, SEM_SHM_MUTEX);
                int active = state->active_patient_count;
                semSignal(semid, SEM_SHM_MUTEX);

                if (active < patient_limit) break;
                usleep(100000);  // 100ms polling
            }
            if (state->shutdown || g_gen_shutdown) break;
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

        // Zapamiętaj ile pacjentów + inkrementuj licznik aktywnych PRZED forkiem
        semWait(semid, SEM_SHM_MUTEX);
        state->total_patients = patient_id;
        state->active_patient_count++;
        semSignal(semid, SEM_SHM_MUTEX);

        // Fork dla nowego pacjenta
        pid_t pid = fork();

        if (pid == 0) {
            // Gdy generator umrze, kernel wyśle SIGTERM do pacjenta
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            
            // Proces pacjenta
            char id_str[16], age_str[16], vip_str[16];
            snprintf(id_str, sizeof(id_str), "%d", patient_id);
            snprintf(age_str, sizeof(age_str), "%d", age);
            snprintf(vip_str, sizeof(vip_str), "%d", is_vip);

            execl("./pacjent", "pacjent", id_str, age_str, vip_str, nullptr);

            SOR_FATAL("execl pacjent id=%d", patient_id);
            exit(EXIT_FAILURE);

        } else if (pid > 0) {
            // Zapisz PID pacjenta do tablicy
            if (g_patient_count < 1000) {
                g_patient_pids[g_patient_count++] = pid;
            }
        } else {
            SOR_WARN("fork pacjenta %d", patient_id);
            // Cofnij licznik bo fork się nie udał
            semWait(semid, SEM_SHM_MUTEX);
            if (state->active_patient_count > 0) state->active_patient_count--;
            semSignal(semid, SEM_SHM_MUTEX);
        }

        // Zbierz zakończone procesy pacjentów (unikamy zombie)
        int status;
        while (waitpid(-1, &status, WNOHANG) > 0) {}
    }

    // ==== CZYSTE ZAMKNIĘCIE ====
    logMessage(state, semid, "[Generator] Zamykanie — wyślij SIGTERM do %d pacjentów", g_patient_count);

    // Wyślij SIGTERM do wszystkich pacjentów (obudzi ich z sleep/msgrcv)
    for (int i = 0; i < g_patient_count; i++) {
        if (g_patient_pids[i] > 0) {
            kill(g_patient_pids[i], SIGTERM);
        }
    }

    // Czekaj na dzieci (maks ~3s = 30 × 100ms)
    for (int attempt = 0; attempt < 30; attempt++) {
        int status;
        pid_t ret = waitpid(-1, &status, WNOHANG);
        if (ret <= 0 && errno == ECHILD) break;  // brak dzieci
        if (ret > 0) continue;  // zebraliśmy jedno — sprawdź kolejne od razu
        usleep(100000);  // 100ms
    }

    // Dobij pozostałe (safety net)
    for (int i = 0; i < g_patient_count; i++) {
        if (g_patient_pids[i] > 0) {
            kill(g_patient_pids[i], SIGKILL);  // ESRCH jeśli już nie żyje — OK
        }
    }
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}  // zbierz ostatnie zombie

    logMessage(state, semid, "[Generator] Generator zakończony czysto");
    shmdt(state);
    return 0;
}
