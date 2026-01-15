#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <signal.h>
#include "../include/common.h"
#include "../include/roles.h"
#include "../include/config.h"
#include "../include/protocol.h"
#include "../include/ipc.h"
#include "../include/util.h"

// Director - wysyła sygnały SIGUSR1 do lekarzy (PROMPT 13)
// PROMPT 13: Handle SIGUSR2 for graceful shutdown

static volatile sig_atomic_t sigusr2_received = 0;

static void signal_handler_usr2(int sig) {
    (void)sig;
    sigusr2_received = 1;
}

static void signal_handler_term(int sig) {
    (void)sig;
    sigusr2_received = 1;  // SIGTERM działa tak samo
}

// PIDs lekarzy (zapisane w SHM)
extern SORState* g_sor_state;

int run_director(const Config& config) {
    (void)config;  // Can be used for timing settings
    
    // PROMPT 13: Use sigaction() instead of signal()
    struct sigaction sa_usr2{};
    sa_usr2.sa_handler = signal_handler_usr2;
    sigemptyset(&sa_usr2.sa_mask);
    sa_usr2.sa_flags = 0;
    sigaction(SIGUSR2, &sa_usr2, nullptr);
    
    struct sigaction sa_term{};
    sa_term.sa_handler = signal_handler_term;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGTERM, &sa_term, nullptr);
    
    if (ipc_attach() == -1) {
        perror("ipc_attach (director)");
        return 1;
    }

    log_event("[Director] Dyrektor rozpoczyna pracę");
    
    srand(time(NULL) + getpid());
    
    // PROMPT 13: Co 3-7 sekund wysyłaj SIGUSR1 do losowego lekarza
    while (!sigusr2_received) {
        sleep(3 + rand() % 5);  // 3-7 sekund
        
        if (sigusr2_received) break;
        
        // Losuj specjalizację (0-5)
        int spec = rand() % 6;
        pid_t doctor_pid = g_sor_state->doctor_pids[spec];
        
        if (doctor_pid > 0) {
            const char* spec_names[] = {"kardiolog", "neurolog", "okulista", "laryngolog", "chirurg", "pediatra"};
            log_event("[Director] Dyrektor wysyła SIGUSR1 do lekarza %s (PID=%d)", spec_names[spec], doctor_pid);
            
            if (kill(doctor_pid, SIGUSR1) == -1) {
                perror("kill(SIGUSR1)");
            }
        }
    }
    
    log_event("[Director] Dyrektor otrzymał ewakuację i opuszcza SOR");
    exit(0);
}
