#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "../include/common.h"
#include "../include/roles.h"
#include "../include/config.h"
#include "../include/protocol.h"
#include "../include/ipc.h"
#include "../include/util.h"

// Patient - pacjent

int run_patient(int patient_id, const Config& config) {
    // Attach do istniejących zasobów IPC
    if (ipc_attach() == -1) {
        perror("ipc_attach (patient)");
        return 1;
    }

    // Przygotuj dane pacjenta (prosta deterministyczna reguła)
    PatientInQueue p{};
    p.patient_id = patient_id;
    p.patient_pid = getpid();
    p.age = 20 + (patient_id % 40);  // 20-59
    p.is_vip = (patient_id % 5 == 0) ? 1 : 0;  // co piąty VIP
    p.has_guardian = (p.age < 18) ? 1 : 0;
    strncpy(p.symptoms, "objawy nieokreslone", MAX_NAME_LEN - 1);

    log_event("Pacjent %d pojawia się przed SOR", patient_id);

    // Próba wejścia do poczekalni: jeśli brak miejsc, loguj oczekiwanie
    while (true) {
        if (sem_P(g_sem_id, SEM_WAITROOM) == 0) {
            break;  // zdobył miejsce
        }
        log_event("Pacjent %d czeka przed budynkiem – brak miejsc", patient_id);
        usleep(200000); // 200ms i spróbuj ponownie
    }
    log_event("Pacjent %d wchodzi do budynku", patient_id);

    // Aktualizacja inside_count pod mutexem stanu
    if (sem_P(g_sem_id, SEM_STATE_MUTEX) == -1) return 1;
    g_sor_state->inside_count += 1;
    sem_V(g_sem_id, SEM_STATE_MUTEX);

    // Dołączenie do kolejki rejestracji (producer-consumer: SLOTS, MUTEX, ITEMS)
    if (sem_P(g_sem_id, SEM_REG_SLOTS) == -1) return 1;   // czekaj na wolne miejsce w kolejce
    if (sem_P(g_sem_id, SEM_REG_MUTEX) == -1) return 1;   // wejdź do sekcji krytycznej

    int qres = p.is_vip ? queue_enqueue_front(&g_sor_state->reg_queue, p)
                        : queue_enqueue_back(&g_sor_state->reg_queue, p);
    if (qres == -1) {
        sem_V(g_sem_id, SEM_REG_MUTEX);
        sem_V(g_sem_id, SEM_REG_SLOTS);
        return 1;
    }

    if (p.is_vip) {
        log_event("Pacjent %d (VIP) omija kolejkę rejestracji", patient_id);
    } else {
        log_event("Pacjent %d dołącza do kolejki rejestracji", patient_id);
    }

    sem_V(g_sem_id, SEM_REG_MUTEX);   // wyjdź z sekcji krytycznej
    sem_V(g_sem_id, SEM_REG_ITEMS);   // sygnalizuj nowy element w kolejce

    // Na tym etapie pacjent czeka na dalsze etapy (prompty 8+).
    return 0;
}

// Generator pacjentów - na razie stub (PROMPT 5: uruchamianie procesu)
int run_patient_generator(const Config& config) {
    (void)config;
    printf("Generator pacjentów: start (stub)\n");
    // TODO PROMPT 14: generowanie procesów pacjentów w pętli
    return 0;
}
