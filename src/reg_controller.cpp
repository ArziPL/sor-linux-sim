#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/common.h"
#include "../include/roles.h"
#include "../include/config.h"
#include "../include/protocol.h"
#include "../include/ipc.h"
#include "../include/util.h"

// Kontroler okienka 2 rejestracji: otwiera gdy reg_queue.count > K, zamyka gdy < N/3
// PROMPT 13: Handle SIGUSR2 for graceful shutdown

static volatile sig_atomic_t regctrl_shutdown = 0;
static void regctrl_sigterm(int) { regctrl_shutdown = 1; }
static void regctrl_sigusr2(int) { regctrl_shutdown = 1; }

static pid_t spawn_registration_window(int window_id, const Config& config, const char* argv0) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork (reg_window)");
        return -1;
    }
    if (pid == 0) {
        char arg_win[8], arg_N[16], arg_K[16], arg_dur[16], arg_speed[16], arg_seed[16];
        snprintf(arg_win, sizeof(arg_win), "%d", window_id);
        snprintf(arg_N, sizeof(arg_N), "%d", config.N);
        snprintf(arg_K, sizeof(arg_K), "%d", config.K);
        snprintf(arg_dur, sizeof(arg_dur), "%d", config.duration);
        snprintf(arg_speed, sizeof(arg_speed), "%.2f", config.speed);
        snprintf(arg_seed, sizeof(arg_seed), "%u", config.seed);

        char* const args[] = {
            const_cast<char*>(argv0),
            const_cast<char*>("registration"),
            arg_win, arg_N, arg_K, arg_dur, arg_speed, arg_seed,
            nullptr
        };
        execvp(argv0, args);
        perror("execvp (registration)");
        _exit(1);
    }
    return pid;
}

int run_reg_controller(const Config& config) {
    // Obsługa SIGTERM i SIGUSR2 do łagodnego zamknięcia
    struct sigaction sa{};
    sa.sa_handler = regctrl_sigterm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    
    // PROMPT 13: Also handle SIGUSR2 for graceful shutdown
    struct sigaction sa2{};
    sa2.sa_handler = regctrl_sigusr2;
    sigemptyset(&sa2.sa_mask);
    sigaction(SIGUSR2, &sa2, nullptr);

    // Ustal ścieżkę do binarki (dla exec registration)
    char exe_path[256] = "./sor";
    ssize_t r = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (r > 0) {
        exe_path[r] = '\0';
    }

    // Attach do IPC
    if (ipc_attach() == -1) {
        perror("ipc_attach (reg_controller)");
        return 1;
    }

    log_event("[RegCtrl] Kontroler rejestracji startuje (K=%d)", config.K);

    pid_t window2_pid_local = 0;

    while (!regctrl_shutdown) {
        // Odczytaj długość kolejki rejestracji
        int qlen = 0;
        if (sem_P(g_sem_id, SEM_REG_MUTEX) == 0) {
            qlen = g_sor_state->reg_queue.count;
            sem_V(g_sem_id, SEM_REG_MUTEX);
        }

        // Odczytaj stan okienka2
        int w2_open = 0;
        pid_t w2_pid = 0;
        if (sem_P(g_sem_id, SEM_STATE_MUTEX) == 0) {
            w2_open = g_sor_state->window2_open;
            w2_pid = g_sor_state->window2_pid;
            sem_V(g_sem_id, SEM_STATE_MUTEX);
        }

        // Otwarcie okienka2
        if (qlen > config.K && w2_open == 0) {
            pid_t pid = spawn_registration_window(2, config, exe_path);
            if (pid != -1) {
                window2_pid_local = pid;
                if (sem_P(g_sem_id, SEM_STATE_MUTEX) == 0) {
                    g_sor_state->window2_open = 1;
                    g_sor_state->window2_pid = pid;
                    sem_V(g_sem_id, SEM_STATE_MUTEX);
                }
                log_event("Okienko rejestracji 2 zostaje otwarte (PID=%d)", pid);
            }
        }

        // Zamknięcie okienka2
        if (qlen < config.N / 3 && w2_open == 1 && w2_pid > 0) {
            kill(w2_pid, SIGTERM);
            waitpid(w2_pid, nullptr, 0);
            if (sem_P(g_sem_id, SEM_STATE_MUTEX) == 0) {
                g_sor_state->window2_open = 0;
                g_sor_state->window2_pid = 0;
                sem_V(g_sem_id, SEM_STATE_MUTEX);
            }
            log_event("Okienko rejestracji 2 zostaje zamknięte (PID=%d)", w2_pid);
            window2_pid_local = 0;
        }

        usleep(200000); // 200ms
    }

    // Przy zamknięciu dopilnuj okienka 2
    if (window2_pid_local > 0) {
        kill(window2_pid_local, SIGTERM);
        waitpid(window2_pid_local, nullptr, 0);
    }

    return 0;
}
