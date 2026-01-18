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
// Na Ctrl+C: Director wysyła SIGUSR2 do wszystkich = ewakuacja (zgodnie z tematem)

static volatile sig_atomic_t sigusr2_received = 0;
static volatile sig_atomic_t sigint_received = 0;

// Handler SIGINT (Ctrl+C) - Director zarządza ewakuacją
static void signal_handler_int(int sig) {
    (void)sig;
    sigint_received = 1;
}

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
    
    // PROMPT 13: Setup signal handlers
    // SIGINT (Ctrl+C) - Director zarządza ewakuacją SOR
    struct sigaction sa_int{};
    sa_int.sa_handler = signal_handler_int;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    sigaction(SIGINT, &sa_int, nullptr);
    
    // SIGUSR2 - reaguj na ewakuację
    struct sigaction sa_usr2{};
    sa_usr2.sa_handler = signal_handler_usr2;
    sigemptyset(&sa_usr2.sa_mask);
    sa_usr2.sa_flags = 0;
    sigaction(SIGUSR2, &sa_usr2, nullptr);
    
    // SIGTERM - normalne zakończenie
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
    
    // PROMPT 13: Co 8-12 sekund wysyłaj SIGUSR1 do losowego lekarza
    while (!sigusr2_received && !sigint_received) {
        sleep(8 + rand() % 5);  // 8-12 sekund
        
        if (sigusr2_received || sigint_received) break;
        
        // Losuj specjalizację (0-5)
        int spec = rand() % 6;
        pid_t doctor_pid = g_sor_state->doctor_pids[spec];
        
        if (doctor_pid > 0) {
            const char* spec_names[] = {"kardiolog", "neurolog", "okulista", "laryngolog", "chirurg", "pediatra"};
            log_event("[Director] Dyrektor wysyła lekarza na oddział - %s", spec_names[spec], doctor_pid);
            
            if (kill(doctor_pid, SIGUSR1) == -1) {
                perror("kill(SIGUSR1)");
            }
        }
    }
    
    // ZGODNIE Z TEMATEM: Na polecenie Dyrektora (sygnał 2) wszyscy opuszczają budynek
    if (sigint_received) {
        log_event("[Director] EWAKUACJA! Dyrektor zarządza opuszczenie budynku");
        
        // KLUCZOWE: Czekaj aby logger zdążył odebrać i zapisać wiadomość PRZED wysłaniem SIGUSR2
        // Logger IGNORUJE SIGUSR2, więc będzie działał dalej i odbierze kolejne logi
        usleep(500000);  // 500ms - ważne aby log się zapisał
        
        // Wyślij SIGUSR2 do całej grupy procesów = ewakuacja
        // Logger zignoruje SIGUSR2, zamknie się dopiero na SIGTERM od Managera
        if (killpg(0, SIGUSR2) == -1) {
            perror("killpg(SIGUSR2)");
        }
        
        // Krótkie czekanie - Manager zajmie się resztą
        usleep(100000);  // 100ms
    }
    
    exit(0);
}
