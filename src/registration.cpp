#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <sys/msg.h>
#include "../include/common.h"
#include "../include/roles.h"
#include "../include/config.h"
#include "../include/protocol.h"
#include "../include/ipc.h"
#include "../include/util.h"

// Registration - okienko rejestracji
// PROMPT 13: Handle SIGUSR2 for graceful shutdown

static void signal_handler_usr2(int sig) {
    (void)sig;
    exit(0);
}

int run_registration(int window_id, const Config& config) {
    (void)config;
    
    signal(SIGUSR2, signal_handler_usr2);

    // Podłącz do istniejących zasobów IPC
    if (ipc_attach() == -1) {
        perror("ipc_attach (registration)");
        return 1;
    }

    log_event("Okienko rejestracji %d rozpoczyna pracę", window_id);

    while (true) {
        // Producer-consumer: czekaj na pacjenta w kolejce
        if (sem_P(g_sem_id, SEM_REG_ITEMS) == -1) {
            continue;
        }
        if (sem_P(g_sem_id, SEM_REG_MUTEX) == -1) {
            sem_V(g_sem_id, SEM_REG_ITEMS);
            continue;
        }

        PatientInQueue patient{};
        int dq = queue_dequeue(&g_sor_state->reg_queue, patient);

        sem_V(g_sem_id, SEM_REG_MUTEX);
        sem_V(g_sem_id, SEM_REG_SLOTS);

        if (dq == -1) {
            continue;
        }

        log_event("Pacjent %d podchodzi do okienka rejestracji %d", patient.patient_id, window_id);

        // Zbuduj komunikat do triażu
        PatientMessage msg{};
        msg.mtype = 1;  // triaż odbiera typ 1
        msg.patient_id = patient.patient_id;
        msg.patient_pid = patient.patient_pid;
        msg.age = patient.age;
        msg.is_vip = patient.is_vip;
        msg.has_guardian = patient.has_guardian;
        strncpy(msg.symptoms, patient.symptoms, MAX_NAME_LEN - 1);
        msg.symptoms[MAX_NAME_LEN - 1] = '\0';

        if (msgsnd(g_msgq_triage, &msg, PATIENT_MSG_SIZE, 0) == -1) {
            perror("msgsnd (registration->triage)");
            log_event("[BŁĄD] Nie udało się wysłać pacjenta %d do triażu", patient.patient_id);
            continue;
        }

        log_event("Pacjent %d przekazany do triażu", patient.patient_id);
    }

    return 0;
}
