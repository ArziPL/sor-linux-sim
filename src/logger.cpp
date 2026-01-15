#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <signal.h>
#include <ctime>
#include "../include/common.h"
#include "../include/roles.h"
#include "../include/config.h"
#include "../include/ipc.h"
#include "../include/protocol.h"

// ============================================================================
// LOGGER - PROCES LOGOWANIA
// ============================================================================

// Flaga do graceful shutdown
volatile sig_atomic_t logger_shutdown = 0;

// Handler dla SIGUSR2 (IGNORUJ - pacjenty go dostaną, a logger czeka na SIGTERM)
void logger_sigusr2_handler(int sig) {
    (void)sig;
    // Logger ignoruje SIGUSR2 - czeka na SIGTERM zamiast tego
}

// Handler dla SIGTERM (zamknięcie)
void logger_sigterm_handler(int sig) {
    (void)sig;
    logger_shutdown = 1;
}

int run_logger(const Config& config) {
    (void)config;  // Aby uniknąć warningów
    
    // Ustawienie handlera sygnałów
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = logger_sigusr2_handler;
    sigaction(SIGUSR2, &sa, nullptr);
    
    sa.sa_handler = logger_sigterm_handler;
    sigaction(SIGTERM, &sa, nullptr);
    
    // Otwórz plik logu - O_CREAT | O_WRONLY | O_TRUNC, uprawnienia 0600
    int log_fd = open("sor.log", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (log_fd == -1) {
        perror("open(sor.log)");
        exit(1);
    }

    fprintf(stderr, "[Logger] Plik sor.log otwarty (fd=%d)\n", log_fd);

    // Attachment do IPC
    if (ipc_attach() == -1) {
        fprintf(stderr, "[Logger] Błąd: nie mogę się attachować do IPC\n");
        close(log_fd);
        exit(1);
    }

    fprintf(stderr, "[Logger] Podłączony do IPC (MSGQ ID: %d)\n", g_msgq_id);

    // Helper formatujący logi z timestampem od startu symulacji
    auto emit_log = [&](double elapsed_sec, const char* text) {
        char formatted[MAX_MSG_SIZE + 64];
        int len = snprintf(formatted, sizeof(formatted), "[%7.2fs] %s\n", elapsed_sec, text);
        if (len < 0) {
            return;
        }
        // write() może przyciąć, ale w praktyce bufor mieści się w jednym syscallu
        if (len > (int)sizeof(formatted)) {
            len = sizeof(formatted);
        }
        write(log_fd, formatted, len);
        fprintf(stderr, "%s", formatted);
        fsync(log_fd);
    };

    // Początkowy log (t=0)
    emit_log(0.0, "[Logger] Symulacja SOR startuje");

    // Główna pętla - odbieraj komunikaty z MSGQ
    fprintf(stderr, "[Logger] Czekam na komunikaty...\n");
    
    while (!logger_shutdown) {
        LogMessage msg;
        
        // Odbierz wiadomość z kolejki (BLOKUJĄCY, czeka aż pojawi się wiadomość)
        // msgrcv - trzeci argument to rozmiar BEZ pola mtype!
        int ret = msgrcv(g_msgq_id, &msg, LOG_PAYLOAD_SIZE, 1, 0);

        if (ret == -1) {
            if (errno == EAGAIN || errno == ENOMSG) {
                // Brak wiadomości - normalne, poczekaj
                usleep(100000);  // 100ms
                continue;
            }
            if (errno == EIDRM || errno == EINVAL) {
                // Kolejka usunięta lub niepoprawny ID - wychodzimy
                break;
            }
            perror("msgrcv");
            usleep(100000);
            continue;
        }
        
        // Mamy wiadomość - oblicz timestamp względem startu symulacji
        double elapsed_sec = 0.0;
        if (g_sor_state != nullptr) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            time_t start = g_sor_state->sim_start_time;
            elapsed_sec = difftime(now.tv_sec, start) + (now.tv_nsec / 1e9);
            if (elapsed_sec < 0) {
                elapsed_sec = 0.0;
            }
        }

        emit_log(elapsed_sec, msg.text);
    }
    
    // Zamknij plik
    fprintf(stderr, "[Logger] Zamykam plik i wyłączam\n");
    close(log_fd);
    
    // Detach z SHM
    ipc_detach();
    
    exit(0);
}

