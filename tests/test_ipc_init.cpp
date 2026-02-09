/**
 * @file test_ipc_init.cpp
 * @brief Test: Weryfikacja poprawności inicjalizacji IPC w działającej symulacji
 *
 * Uruchamia dyrektor, podłącza się do jego prawdziwych zasobów IPC
 * (SharedState, semafory, kolejki) i sprawdza:
 *   1. SharedState.director_pid zgadza się z PID dyrektora
 *   2. SharedState.shutdown == 0
 *   3. Wszystkie 7 doctor_pids[] > 0
 *   4. Wszystkie 6 specialist_msgids[] są poprawne
 *   5. Wartości semaforów w sensownych zakresach
 *   6. Po zamknięciu IPC jest sprzątnięte
 */

#include "../src/sor_common.hpp"
#include <sys/wait.h>

// Pomocnicze: uruchom dyrektor z pipe na stdin, zwróć pid i fd do pisania
static pid_t startDyrektor(int &write_fd) {
    int pipefd[2];
    if (pipe(pipefd) == -1) { perror("pipe"); return -1; }

    pid_t pid = fork();
    if (pid == 0) {
        // Dziecko: stdin z pipe, stdout/stderr do /dev/null
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        execl("./dyrektor", "dyrektor", nullptr);
        perror("execl dyrektor");
        _exit(1);
    }
    close(pipefd[0]);
    write_fd = pipefd[1];
    return pid;
}

int main() {
    printf("[test_ipc_init] START\n");

    int write_fd;
    pid_t dyr_pid = startDyrektor(write_fd);
    if (dyr_pid <= 0) { printf("[test_ipc_init] FAIL\n"); return 1; }

    // Poczekaj na pełną inicjalizację
    usleep(2000000); // 2s

    int pass = 1;

    // Podłącz się do prawdziwego SHM projektu
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    int shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (shmid == -1) {
        printf("  FAIL: nie znaleziono SharedState (shmget)\n");
        pass = 0;
    }

    SharedState* state = nullptr;
    if (pass) {
        state = (SharedState*)shmat(shmid, nullptr, SHM_RDONLY);
        if (state == (void*)-1) {
            printf("  FAIL: shmat\n");
            pass = 0;
            state = nullptr;
        }
    }

    if (state) {
        // CHECK 1: director_pid
        if (state->director_pid != dyr_pid) {
            printf("  FAIL: director_pid=%d, oczekiwano=%d\n", state->director_pid, dyr_pid);
            pass = 0;
        }

        // CHECK 2: shutdown == 0
        if (state->shutdown != 0) {
            printf("  FAIL: shutdown=%d, oczekiwano=0\n", (int)state->shutdown);
            pass = 0;
        }

        // CHECK 3: Wszystkie doctor_pids > 0 (7 lekarzy)
        for (int i = 0; i < DOCTOR_COUNT; i++) {
            if (state->doctor_pids[i] <= 0) {
                printf("  FAIL: doctor_pids[%d]=%d (powinno > 0)\n", i, state->doctor_pids[i]);
                pass = 0;
            }
        }

        // CHECK 4: Wszystkie specialist_msgids poprawne
        for (int i = DOCTOR_KARDIOLOG; i <= DOCTOR_PEDIATRA; i++) {
            if (state->specialist_msgids[i] <= 0) {
                printf("  FAIL: specialist_msgids[%d]=%d (powinno > 0)\n", i, state->specialist_msgids[i]);
                pass = 0;
            }
        }

        // CHECK 5: registration_pid > 0
        if (state->registration_pid <= 0) {
            printf("  FAIL: registration_pid=%d (powinno > 0)\n", state->registration_pid);
            pass = 0;
        }

        shmdt(state);
    }

    // CHECK 6: Semafory — wartości w sensownych zakresach
    key_t sem_key = getIPCKey(SEM_KEY_ID);
    int semid = semget(sem_key, SEM_COUNT, 0);
    if (semid == -1) {
        printf("  FAIL: nie znaleziono semaforów (semget)\n");
        pass = 0;
    } else {
        int val_pocz = semctl(semid, SEM_POCZEKALNIA, GETVAL);
        if (val_pocz < 0 || val_pocz > N) {
            printf("  FAIL: SEM_POCZEKALNIA=%d (powinno 0..%d)\n", val_pocz, N);
            pass = 0;
        }
        // Mutexy powinny być 0 lub 1
        int val_shm = semctl(semid, SEM_SHM_MUTEX, GETVAL);
        int val_log = semctl(semid, SEM_LOG_MUTEX, GETVAL);
        if (val_shm < 0 || val_shm > 1) {
            printf("  FAIL: SEM_SHM_MUTEX=%d (powinno 0..1)\n", val_shm);
            pass = 0;
        }
        if (val_log < 0 || val_log > 1) {
            printf("  FAIL: SEM_LOG_MUTEX=%d (powinno 0..1)\n", val_log);
            pass = 0;
        }
    }

    // CHECK 7: Główna kolejka komunikatów istnieje
    key_t msg_key = getIPCKey(MSG_KEY_ID);
    int msgid = msgget(msg_key, 0);
    if (msgid == -1) {
        printf("  FAIL: nie znaleziono głównej kolejki komunikatów\n");
        pass = 0;
    }

    // Zamknij symulację
    write(write_fd, "q", 1);
    close(write_fd);

    // Czekaj na zakończenie (max 10s)
    int wait_count = 0;
    int status;
    while (waitpid(dyr_pid, &status, WNOHANG) == 0 && wait_count < 20) {
        usleep(500000);
        wait_count++;
    }
    if (wait_count >= 20) {
        kill(dyr_pid, SIGKILL);
        waitpid(dyr_pid, &status, 0);
    }

    // CHECK 8: IPC sprzątnięte po zamknięciu
    usleep(500000);
    if (shmget(shm_key, sizeof(SharedState), 0) != -1) {
        printf("  FAIL: SharedState wciąż istnieje po zamknięciu\n");
        pass = 0;
    }
    if (semget(sem_key, SEM_COUNT, 0) != -1) {
        printf("  FAIL: semafory wciąż istnieją po zamknięciu\n");
        pass = 0;
    }

    if (pass) {
        printf("[test_ipc_init] PASS\n");
        return 0;
    } else {
        printf("[test_ipc_init] FAIL\n");
        return 1;
    }
}
