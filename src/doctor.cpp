#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

int run_doctor(const char* specialization, const Config& config) {
    // Podłącz do istniejących zasobów IPC
    if (ipc_attach() == -1) {
        perror("ipc_attach (doctor)");
        return 1;
    }

    srand(config.seed + getpid());  // Losowość
    
    // PROMPT 13: Setup signal handlers using sigaction()
    struct sigaction sa_usr1{}, sa_usr2{};
    
    sa_usr1.sa_handler = signal_handler_usr1;
    sigemptyset(&sa_usr1.sa_mask);
    sa_usr1.sa_flags = 0;
    sigaction(SIGUSR1, &sa_usr1, nullptr);
    
    sa_usr2.sa_handler = signal_handler_usr2;
    sigemptyset(&sa_usr2.sa_mask);
    sa_usr2.sa_flags = 0;
    sigaction(SIGUSR2, &sa_usr2, nullptr);

    log_event("Lekarz %s rozpoczyna pracę", specialization);

    while (true) {
        // PROMPT 13: Sprawdzenie SIGUSR2 (graceful shutdown) na początek pętli
        if (sigusr2_received) {
            log_event("Lekarz %s otrzymał ewakuację i opuszcza SOR", specialization);
            break;
        }
        
        // PROMPT 13: Jeśli SIGUSR1 wpłynął przed msgrcv (lekarz czekał na pacjenta)
        if (doctor_interrupted) {
            log_event("Lekarz %s opuszcza SOR na rozkaz dyrektora (bez pacjenta)", specialization);
            
            // PROMPT 13: Czekaj losowy czas na oddziale (5-10 sekund)
            int ward_time_ms = (5000 + rand() % 5000);
            log_event("Lekarz %s pracuje na oddziale", specialization);
            usleep(ward_time_ms * 1000);
            
            log_event("Lekarz %s wraca do SOR", specialization);
            doctor_interrupted = 0;
            continue;
        }
        
        TriageMessage msg{};

        // PROMPT 11: Odbierz pacjenta z kolejki lekarzy priorytetowo
        // Staramy się najpierw mtype=1 (czerwony), potem 2 (żółty), potem 3 (zielony)
        int ret = msgrcv(g_msgq_doctors, &msg, sizeof(TriageMessage) - sizeof(long), 1, IPC_NOWAIT);
        
        if (ret == -1 && errno == EAGAIN) {
            // Brak pacjenta mtype=1, spróbuj mtype=2
            ret = msgrcv(g_msgq_doctors, &msg, sizeof(TriageMessage) - sizeof(long), 2, IPC_NOWAIT);
            
            if (ret == -1 && errno == EAGAIN) {
                // Brak pacjenta mtype=2, spróbuj mtype=3
                ret = msgrcv(g_msgq_doctors, &msg, sizeof(TriageMessage) - sizeof(long), 3, IPC_NOWAIT);
                
                if (ret == -1 && errno == EAGAIN) {
                    // Brak żadnego pacjenta - czekaj z blokowaniem (może być przerwany sygnałem)
                    ret = msgrcv(g_msgq_doctors, &msg, sizeof(TriageMessage) - sizeof(long), 0, 0);
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

        log_event("Lekarz %s rozpoczyna badanie pacjenta %d", specialization, msg.patient_id);

        // Symuluj badanie: czekaj losowy czas (0.5-2 sekundy * speed) z przerwaniem
        int exam_time_ms = (500 + rand() % 1500) * config.speed;
        int sleep_chunks = exam_time_ms / 100;  // Sleep w kawałkach po 100ms
        
        for (int chunk = 0; chunk < sleep_chunks && !doctor_interrupted && !sigusr2_received; chunk++) {
            usleep(100000);  // 100ms
        }
        
        // PROMPT 13: Jeśli SIGUSR1 przerwał badanie
        if (doctor_interrupted) {
            log_event("Lekarz %s opuszcza SOR na rozkaz dyrektora (pacjent %d wraca do kolejki)", specialization, msg.patient_id);
            
            // PROMPT 13: Zwróć pacjenta do kolejki lekarzy
            if (msgsnd(g_msgq_doctors, &msg, sizeof(TriageMessage) - sizeof(long), 0) == -1) {
                perror("msgsnd (return patient to queue)");
            }
            
            // PROMPT 13: Czekaj losowy czas na oddziale (5-10 sekund)
            int ward_time_ms = (5000 + rand() % 5000);
            log_event("Lekarz %s pracuje na oddziale", specialization);
            usleep(ward_time_ms * 1000);
            
            log_event("Lekarz %s wraca do SOR", specialization);
            doctor_interrupted = 0;
            continue;
        }
        
        // PROMPT 13: Sprawdzenie SIGUSR2 (graceful shutdown)
        if (sigusr2_received) {
            log_event("Lekarz %s otrzymał ewakuację i opuszcza SOR", specialization);
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

        log_event("Lekarz %s kończy badanie pacjenta %d - wynik: %s", specialization, msg.patient_id, outcome);

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
    }

    log_event("Lekarz %s opuszcza SOR", specialization);
    return 0;
}
