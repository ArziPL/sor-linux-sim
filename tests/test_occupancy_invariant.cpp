/**
 * @file test_occupancy_invariant.cpp
 * @brief Test: Zajętość SOR nigdy nie przekracza N=20 podczas działania
 *
 * Uruchamia symulację na ~12s i co 200ms sprawdza z prawdziwej SharedState:
 *   1. patients_in_sor <= N (20)
 *   2. SEM_POCZEKALNIA + patients_in_sor == N (niezmiennik)
 *   3. reg_queue_count >= 0
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
    printf("[test_occupancy_invariant] START\n");

    int write_fd;
    pid_t dyr_pid = startDyrektor(write_fd);
    if (dyr_pid <= 0) { printf("[test_occupancy_invariant] FAIL\n"); return 1; }

    // Poczekaj na inicjalizację
    usleep(2000000);

    // Podłącz do IPC
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    int shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (shmid == -1) {
        printf("  FAIL: nie znaleziono SharedState\n");
        write(write_fd, "q", 1); close(write_fd);
        waitpid(dyr_pid, nullptr, 0);
        printf("[test_occupancy_invariant] FAIL\n");
        return 1;
    }

    SharedState* state = (SharedState*)shmat(shmid, nullptr, SHM_RDONLY);
    if (state == (void*)-1) {
        printf("  FAIL: shmat\n");
        write(write_fd, "q", 1); close(write_fd);
        waitpid(dyr_pid, nullptr, 0);
        printf("[test_occupancy_invariant] FAIL\n");
        return 1;
    }

    key_t sem_key = getIPCKey(SEM_KEY_ID);
    int semid = semget(sem_key, SEM_COUNT, 0);
    if (semid == -1) {
        printf("  FAIL: nie znaleziono semaforów\n");
        shmdt(state);
        write(write_fd, "q", 1); close(write_fd);
        waitpid(dyr_pid, nullptr, 0);
        printf("[test_occupancy_invariant] FAIL\n");
        return 1;
    }

    int pass = 1;
    int samples = 0;
    int max_occupancy = 0;
    int invariant_violations = 0;

    // Próbkuj co 200ms przez ~10s (50 próbek)
    for (int i = 0; i < 50; i++) {
        usleep(200000);

        int in_sor = state->patients_in_sor;
        int sem_val = semctl(semid, SEM_POCZEKALNIA, GETVAL);
        int queue_count = state->reg_queue_count;
        samples++;

        if (in_sor > max_occupancy) max_occupancy = in_sor;

        // CHECK 1: patients_in_sor <= N
        if (in_sor > N) {
            printf("  FAIL: patients_in_sor=%d > N=%d (próbka %d)\n", in_sor, N, i);
            pass = 0;
            break;
        }

        // CHECK 2: SEM_POCZEKALNIA + patients_in_sor == N
        // (semafor liczy wolne miejsca, patients_in_sor liczy zajęte)
        if (sem_val >= 0 && (sem_val + in_sor != N)) {
            invariant_violations++;
            // Dopuszczamy pojedyncze naruszenia z powodu braku atomowości odczytu
            if (invariant_violations > 3) {
                printf("  FAIL: niezmiennik SEM_POCZEKALNIA(%d) + patients_in_sor(%d) = %d != N=%d\n",
                       sem_val, in_sor, sem_val + in_sor, N);
                pass = 0;
                break;
            }
        }

        // CHECK 3: reg_queue_count >= 0
        if (queue_count < 0) {
            printf("  FAIL: reg_queue_count=%d < 0\n", queue_count);
            pass = 0;
            break;
        }
    }

    if (pass) {
        printf("  OK: %d próbek, max zajętość=%d/%d, naruszenia niezmiennika=%d\n",
               samples, max_occupancy, N, invariant_violations);
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
        printf("[test_occupancy_invariant] PASS\n");
        return 0;
    } else {
        printf("[test_occupancy_invariant] FAIL\n");
        return 1;
    }
}
