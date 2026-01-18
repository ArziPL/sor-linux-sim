#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <sys/msg.h>
#include <unistd.h>
#include <signal.h>
#include "../include/common.h"
#include "../include/roles.h"
#include "../include/config.h"
#include "../include/protocol.h"
#include "../include/ipc.h"
#include "../include/util.h"

// Doctor - lekarz specjalista
// PROMPT 11: Odbiera pacjentów priorytetowo (mtype 1=czerwony, 2=żółty, 3=zielony)
// Losuje wynik: 85% wypis, 14.5% oddział, 0.5% inna placówka
// PROMPT 13: Obsługuje SIGUSR1 (idzie na oddział), SIGUSR2 (koniec)

static volatile sig_atomic_t doctor_interrupted = 0;
static volatile sig_atomic_t sigusr2_received = 0;

// PROMPT 13: Signal handler for SIGUSR1 (go to ward)
static void signal_handler_usr1(int sig) {
    (void)sig;
    doctor_interrupted = 1;
}

// PROMPT 13: Signal handler for SIGUSR2 (shutdown)
static void signal_handler_usr2(int sig) {
    (void)sig;
    sigusr2_received = 1;
}

// Signal handler dla SIGTERM (normalny koniec)
static void signal_handler_term(int sig) {
    (void)sig;
    sigusr2_received = 1;  // SIGTERM działa tak samo jak SIGUSR2
}

int run_doctor(const char* specialization, const Config& config) {
    // Podłącz do istniejących zasobów IPC
    if (ipc_attach() == -1) {
        perror("ipc_attach (doctor)");
        return 1;
    }

    srand(config.seed + getpid());  // Losowość
    
    // PROMPT 13: Setup signal handlers using sigaction()
    // Ignoruj SIGINT - tylko Director reaguje na Ctrl+C
    struct sigaction sa_ign{};
    sa_ign.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa_ign, nullptr);
    
    struct sigaction sa_usr1{}, sa_usr2{};
    
    sa_usr1.sa_handler = signal_handler_usr1;
    sigemptyset(&sa_usr1.sa_mask);
    sa_usr1.sa_flags = 0;
    sigaction(SIGUSR1, &sa_usr1, nullptr);
    
    sa_usr2.sa_handler = signal_handler_usr2;
    sigemptyset(&sa_usr2.sa_mask);
    sa_usr2.sa_flags = 0;
    sigaction(SIGUSR2, &sa_usr2, nullptr);

    // Setup handler dla SIGTERM (normalny koniec symulacji)
    struct sigaction sa_term{};
    sa_term.sa_handler = signal_handler_term;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGTERM, &sa_term, nullptr);

    log_event("Lekarz %s rozpoczyna pracę", specialization);

    while (true) {
        // PROMPT 13: Sprawdzenie SIGUSR2 (graceful shutdown) na początek pętli
        if (sigusr2_received) {
            break;
        }
        
        // PROMPT 13: Jeśli SIGUSR1 wpłynął przed msgrcv (lekarz czekał na pacjenta)
        if (doctor_interrupted) {
            log_event("Lekarz %s opuszcza SOR na rozkaz dyrektora (bez pacjenta)", specialization);
            
            // PROMPT 13: Czekaj losowy czas na oddziale (2-4 sekundy)
            int ward_time_ms = (2000 + rand() % 2000) * config.speed;
            log_event("Lekarz %s pracuje na oddziale", specialization);
            usleep(ward_time_ms * 1000);
            
            log_event("Lekarz %s wraca do SOR", specialization);
            doctor_interrupted = 0;
            continue;
        }
        
        TriageMessage msg{};

        // PROMPT 11: Odbierz pacjenta z kolejki lekarzy priorytetowo
        // Staramy się najpierw mtype=1 (czerwony), potem 2 (żółty), potem 3 (zielony)
        // Odczytaj specjalizację z parametru funkcji (0=kardiolog, 1=neurolog, itd.)
        int my_spec = -1;
        if (strcmp(specialization, "kardiolog") == 0) my_spec = 0;
        else if (strcmp(specialization, "neurolog") == 0) my_spec = 1;
        else if (strcmp(specialization, "okulista") == 0) my_spec = 2;
        else if (strcmp(specialization, "laryngolog") == 0) my_spec = 3;
        else if (strcmp(specialization, "chirurg") == 0) my_spec = 4;
        else if (strcmp(specialization, "pediatra") == 0) my_spec = 5;
        
        // Czytamy pacjentów dla naszej specjalizacji z priorytetem po kolorach
        // mtype = color*10 + spec: najpierw 10+spec (czerwony), potem 20+spec (żółty), potem 30+spec (zielony)
        int ret = msgrcv(g_msgq_doctors, &msg, sizeof(TriageMessage) - sizeof(long), 10 + my_spec, IPC_NOWAIT);
        
        if (ret == -1 && (errno == EAGAIN || errno == ENOMSG)) {
            ret = msgrcv(g_msgq_doctors, &msg, sizeof(TriageMessage) - sizeof(long), 20 + my_spec, IPC_NOWAIT);
            
            if (ret == -1 && (errno == EAGAIN || errno == ENOMSG)) {
                ret = msgrcv(g_msgq_doctors, &msg, sizeof(TriageMessage) - sizeof(long), 30 + my_spec, IPC_NOWAIT);
                
                if (ret == -1 && (errno == EAGAIN || errno == ENOMSG)) {
                    // Brak pacjentów bez blokowania - czekaj w pętli aż coś się pojawi
                    // Próbuj po kolei: czerwony, żółty, zielony (bez blokowania, z sleep)
                    while (ret == -1 && !doctor_interrupted && !sigusr2_received) {
                        usleep(50000);  // 50ms
                        ret = msgrcv(g_msgq_doctors, &msg, sizeof(TriageMessage) - sizeof(long), 10 + my_spec, IPC_NOWAIT);
                        if (ret == -1 && (errno == EAGAIN || errno == ENOMSG)) {
                            ret = msgrcv(g_msgq_doctors, &msg, sizeof(TriageMessage) - sizeof(long), 20 + my_spec, IPC_NOWAIT);
                        }
                        if (ret == -1 && (errno == EAGAIN || errno == ENOMSG)) {
                            ret = msgrcv(g_msgq_doctors, &msg, sizeof(TriageMessage) - sizeof(long), 30 + my_spec, IPC_NOWAIT);
                        }
                    }
                }
            }
        }
        
        // Obsługa błędów msgrcv
        if (ret == -1) {
            if (errno == EINTR) {
                // Sygnał przerwał msgrcv - sprawdzaj flagi
                if (doctor_interrupted) {
                    // SIGUSR1: lekarz idzie na oddział bez pacjenta
                    continue;
                }
                if (sigusr2_received) {
                    break;
                }
                continue;
            }
            
            if (errno == EIDRM || errno == EINVAL) {
                break;  // Kolejka usunięta - wyjdź z pętli
            }
            
            if (errno != EAGAIN) {
                perror("msgrcv (doctor)");
            }
            continue;
        }

        const char* patient_type = msg.has_guardian ? "Dziecko" : "Pacjent";
        log_event("Lekarz %s rozpoczyna badanie - %s %d", specialization, patient_type, msg.patient_id);

        // Symuluj badanie: 0.3-1.0 sekundy * speed (kolor niezależny)
        int exam_time_ms = (300 + rand() % 700) * config.speed;  // 300-999 ms * speed
        int sleep_chunks = exam_time_ms / 100;  // Sleep w kawałkach po 100ms
        
        for (int chunk = 0; chunk < sleep_chunks && !sigusr2_received; chunk++) {
            usleep(100000);  // 100ms
        }
        
        // PROMPT 13: Sprawdzenie SIGUSR2 (graceful shutdown)
        if (sigusr2_received) {
            break;
        }

        // PROMPT 11: Losuj wynik: 85% wypis, 14.5% oddział, 0.5% inna placówka
        int r = rand() % 1000;  // 0-999
        const char* outcome = "wypis";
        
        if (r < 850) {
            outcome = "wypis";
        } else if (r < 995) {
            outcome = "oddział";
        } else {
            outcome = "inna placówka";
        }

        log_event("Lekarz %s kończy badanie - %s %d - wynik: %s", specialization, patient_type, msg.patient_id, outcome);

        // PROMPT 7/11: Pacjent opuszcza SOR: zwolnij WAITROOM i aktualizuj inside_count
        // PROMPT 12: Jeśli dziecko z opiekunem, zwolnij 2 miejsca
        int slots_to_free = msg.has_guardian ? 2 : 1;
        
        if (sem_P(g_sem_id, SEM_STATE_MUTEX) == 0) {
            g_sor_state->inside_count -= 1;
            sem_V(g_sem_id, SEM_STATE_MUTEX);
        }
        
        // Zwolnij miejsca w poczekalni (WAITROOM)
        for (int i = 0; i < slots_to_free; i++) {
            sem_V(g_sem_id, SEM_WAITROOM);
        }

        // PROMPT 13: Jeśli przyszedł SIGUSR1 w trakcie lub przed badaniem, lekarz idzie na oddział po zakończeniu pacjenta
        if (doctor_interrupted && !sigusr2_received) {
            log_event("Lekarz %s opuszcza SOR na rozkaz dyrektora (po zakończeniu badania)", specialization);
            int ward_time_ms = (2000 + rand() % 2000) * config.speed;  // 2-4s
            log_event("Lekarz %s pracuje na oddziale", specialization);
            usleep(ward_time_ms * 1000);
            log_event("Lekarz %s wraca do SOR", specialization);
            doctor_interrupted = 0;
        }
    }

    return 0;
}
