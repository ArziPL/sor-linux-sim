/**
 * @file test_doctor_signal.cpp
 * @brief Test: Sygnał SIGUSR1 wysyła lekarza na oddział i wraca
 *
 * Uruchamia symulację, odczytuje PID kardiologa z prawdziwej SharedState,
 * wysyła SIGUSR1 i sprawdza:
 *   1. doctor_on_break[DOCTOR_KARDIOLOG] zmienia się na 1
 *   2. Po pewnym czasie wraca do 0 (lekarz wraca z oddziału)
 */

#include "../src/sor_common.hpp"
#include <sys/wait.h>

static pid_t startDyrektor(int &write_fd) {
    int pipefd[2];
    if (pipe(pipefd) == -1) { perror("pipe"); return -1; }

    pid_t pid = fork();
    if (pid == 0) {
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
    printf("[test_doctor_signal] START\n");

    int write_fd;
    pid_t dyr_pid = startDyrektor(write_fd);
    if (dyr_pid <= 0) { printf("[test_doctor_signal] FAIL\n"); return 1; }

    // Poczekaj na pełną inicjalizację
    usleep(2500000); // 2.5s

    // Podłącz do SharedState
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    int shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (shmid == -1) {
        printf("  FAIL: nie znaleziono SharedState\n");
        write(write_fd, "q", 1); close(write_fd);
        waitpid(dyr_pid, nullptr, 0);
        printf("[test_doctor_signal] FAIL\n");
        return 1;
    }

    SharedState* state = (SharedState*)shmat(shmid, nullptr, SHM_RDONLY);
    if (state == (void*)-1) {
        printf("  FAIL: shmat\n");
        write(write_fd, "q", 1); close(write_fd);
        waitpid(dyr_pid, nullptr, 0);
        printf("[test_doctor_signal] FAIL\n");
        return 1;
    }

    int pass = 1;

    // Odczytaj PID kardiologa z prawdziwej pamięci dzielonej
    pid_t kardiolog_pid = state->doctor_pids[DOCTOR_KARDIOLOG];
    if (kardiolog_pid <= 0) {
        printf("  FAIL: doctor_pids[KARDIOLOG]=%d\n", kardiolog_pid);
        pass = 0;
    }

    if (pass) {
        // CHECK 1: Przed sygnałem — lekarz nie jest na przerwie
        int before = state->doctor_on_break[DOCTOR_KARDIOLOG];
        if (before != 0) {
            printf("  INFO: kardiolog już na przerwie przed testem, czekam...\n");
            // Poczekaj aż wróci (max 5s)
            for (int i = 0; i < 25 && state->doctor_on_break[DOCTOR_KARDIOLOG] != 0; i++)
                usleep(200000);
        }

        // Wyślij SIGUSR1 — ten sam sygnał który dyrektor wysyła klawiaturą
        if (kill(kardiolog_pid, SIGUSR1) == -1) {
            printf("  FAIL: kill(SIGUSR1) do kardiologa: %s\n", strerror(errno));
            pass = 0;
        }

        // CHECK 2: doctor_on_break powinno zmienić się na 1 (max 3s)
        if (pass) {
            int went_on_break = 0;
            for (int i = 0; i < 15; i++) {
                usleep(200000);
                if (state->doctor_on_break[DOCTOR_KARDIOLOG] == 1) {
                    went_on_break = 1;
                    printf("  OK: kardiolog poszedł na oddział po %.1fs\n", (i + 1) * 0.2);
                    break;
                }
            }
            if (!went_on_break) {
                printf("  FAIL: doctor_on_break[KARDIOLOG] nie zmienił się na 1 w 3s\n");
                pass = 0;
            }
        }

        // CHECK 3: Lekarz wraca z oddziału (max 5s — przerwa trwa 500-1000ms)
        if (pass) {
            int came_back = 0;
            for (int i = 0; i < 25; i++) {
                usleep(200000);
                if (state->doctor_on_break[DOCTOR_KARDIOLOG] == 0) {
                    came_back = 1;
                    printf("  OK: kardiolog wrócił z oddziału po kolejnych %.1fs\n", (i + 1) * 0.2);
                    break;
                }
            }
            if (!came_back) {
                printf("  FAIL: kardiolog nie wrócił z oddziału w 5s\n");
                pass = 0;
            }
        }
    }

    shmdt(state);

    // Zamknij symulację
    write(write_fd, "q", 1);
    close(write_fd);

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

    if (pass) {
        printf("[test_doctor_signal] PASS\n");
        return 0;
    } else {
        printf("[test_doctor_signal] FAIL\n");
        return 1;
    }
}
