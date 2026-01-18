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

// Handler dla SIGUSR2 (evacuation signal)
// Logger IGNORUJE SIGUSR2 - zamyka się dopiero na SIGTERM od Managera
// Dzięki temu zdąży zapisać wszystkie logi przed zamknięciem
void logger_sigusr2_handler(int sig) {
    (void)sig;
    // Logger NIE reaguje na SIGUSR2 - czeka na SIGTERM
    // logger_shutdown = 1;  // WYŁĄCZONE
}

// Handler dla SIGTERM (normal termination)
void logger_sigterm_handler(int sig) {
    (void)sig;
    logger_shutdown = 1;
}

int run_logger(const Config& config) {
    (void)config;  // Aby uniknąć warningów
    
    // Ignoruj SIGINT - tylko Director reaguje na Ctrl+C
    struct sigaction sa_ign;
    memset(&sa_ign, 0, sizeof(sa_ign));
    sa_ign.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa_ign, nullptr);
    
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
    
    // KLUCZOWE: Po dostaniu SIGUSR2 sprawdź MSGQ jeszcze raz (non-blocking)
    // Director mógł wysłać ostatni log o ewakuacji
    if (logger_shutdown) {
        fprintf(stderr, "[Logger] Sprawdzam ostatnie wiadomości przed zamknięciem...\n");
        
        // Próbuj odczytać wszystkie pozostałe wiadomości (non-blocking)
        for (int i = 0; i < 10; i++) {
            LogMessage msg;
            int ret = msgrcv(g_msgq_id, &msg, LOG_PAYLOAD_SIZE, 1, IPC_NOWAIT);
            
            if (ret == -1) {
                if (errno == ENOMSG) {
                    // Brak więcej wiadomości - OK
                    break;
                }
                if (errno == EIDRM || errno == EINVAL) {
                    // Kolejka usunięta - koniec
                    break;
                }
                // Inny błąd - ignoruj i kontynuuj
                break;
            }
            
            // Mamy wiadomość - zapisz
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
    }
    
    // Zamknij plik
    fprintf(stderr, "[Logger] Zamykam plik i wyłączam\n");
    close(log_fd);
    
    // Detach z SHM
    ipc_detach();
    
    exit(0);
}

