#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <signal.h>
#include <sys/msg.h>
#include "../include/common.h"
#include "../include/roles.h"
#include "../include/config.h"
#include "../include/protocol.h"
#include "../include/ipc.h"
#include "../include/util.h"

// Triage - lekarz POZ (ocena stanu pacjenta, triaż)
// Lekarz odbiera pacjentów z MSGQ, losuje kolor (10% czerwony, 35% żółty, 50% zielony),
// 5% wysyła do domu, resztę kieruje do specjalisty z priorytetem (mtype = kolor)
// PROMPT 13: Handle SIGUSR2 for graceful shutdown

static volatile sig_atomic_t should_exit = 0;

static void signal_handler_usr2(int sig) {
    (void)sig;
    should_exit = 1;
}

static void signal_handler_term(int sig) {
    (void)sig;
    should_exit = 1;
}

int run_triage(const Config& config) {
    struct sigaction sa{};
    sa.sa_handler = signal_handler_usr2;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR2, &sa, nullptr);
    
    sa.sa_handler = signal_handler_term;
    sigaction(SIGTERM, &sa, nullptr);
    
    // Podłącz do istniejących zasobów IPC
    if (ipc_attach() == -1) {
        perror("ipc_attach (triage)");
        return 1;
    }

    srand(config.seed + getpid());  // Losowość

    log_event("Lekarz POZ (triaż) rozpoczyna pracę");

    while (!should_exit) {
        PatientMessage msg{};

        // Odbierz pacjenta z kolejki triażu (msgrcv, typ 1, blokujący)
        int ret = msgrcv(g_msgq_triage, &msg, PATIENT_MSG_SIZE, 1, 0);
        if (ret == -1) {
            if (errno == EIDRM || errno == EINVAL) {
                break;  // Kolejka usunięta
            }
            perror("msgrcv (triage)");
            continue;
        }

        log_event("Pacjent %d jest weryfikowany przez lekarza POZ", msg.patient_id);

        // Losuj kolor: 10% czerwony, 35% żółty, 50% zielony, 5% do domu
        int r = rand() % 100;
        int color_code = 0;  // 1=czerwony, 2=żółty, 3=zielony
        const char* color_name = "zielony";

        if (r < 10) {
            color_code = 1;
            color_name = "czerwony";
        } else if (r < 45) {
            color_code = 2;
            color_name = "żółty";
        } else if (r < 95) {
            color_code = 3;
            color_name = "zielony";
        } else {
            // 5% do domu
            log_event("Pacjent %d zostaje odesłany do domu po triażu", msg.patient_id);
            
            // PROMPT 12: Jeśli dziecko z opiekunem, zwolnij 2 miejsca
            int slots_to_free = msg.has_guardian ? 2 : 1;
            
            // Zwolnij miejsce w poczekalni
            if (sem_P(g_sem_id, SEM_STATE_MUTEX) == 0) {
                g_sor_state->inside_count -= 1;
                sem_V(g_sem_id, SEM_STATE_MUTEX);
            }
            
            for (int i = 0; i < slots_to_free; i++) {
                sem_V(g_sem_id, SEM_WAITROOM);
            }
            
            continue;
        }

        // Losuj specjalizację (0-5: kardiolog, neurolog, okulista, laryngolog, chirurg, pediatra)
        int spec = rand() % 6;
        const char* spec_names[] = {"kardiolog", "neurolog", "okulista", "laryngolog", "chirurg", "pediatra"};
        
        log_event("Pacjent %d uzyskuje status [%s] — kierowany do lekarza: %s", msg.patient_id, color_name, spec_names[spec]);

        // Zbuduj wiadomość do lekarza
        TriageMessage tmsg{};
        // mtype koduje zarówno kolor jak i specjalizację: color*10 + spec
        // Specjalizacje: 0=kardiolog, 1=neurolog, 2=okulista, 3=laryngolog, 4=chirurg, 5=pediatra
        tmsg.mtype = color_code * 10 + spec;
        tmsg.patient_id = msg.patient_id;
        tmsg.patient_pid = msg.patient_pid;
        tmsg.age = msg.age;
        tmsg.is_vip = msg.is_vip;
        tmsg.has_guardian = msg.has_guardian;
        strncpy(tmsg.symptoms, msg.symptoms, MAX_NAME_LEN - 1);
        tmsg.symptoms[MAX_NAME_LEN - 1] = '\0';
        tmsg.specialization = spec;
        strncpy(tmsg.triage_color, color_name, 15);
        tmsg.triage_color[15] = '\0';

        // Wyślij do specjalisty (msgq_doctors)
        if (msgsnd(g_msgq_doctors, &tmsg, sizeof(TriageMessage) - sizeof(long), 0) == -1) {
            perror("msgsnd (triage->doctor)");
            log_event("[BŁĄD] Nie udało się wysłać pacjenta %d do lekarza %s", msg.patient_id, spec_names[spec]);
            continue;
        }

        log_event("Pacjent %d czeka na lekarza: %s (kolor: %s)", msg.patient_id, spec_names[spec], color_name);
    }

    return 0;
}
