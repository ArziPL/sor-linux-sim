#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/common.h"
#include "../include/roles.h"
#include "../include/config.h"
#include "../include/protocol.h"
#include "../include/ipc.h"
#include "../include/util.h"

// Patient - pacjent
// PROMPT 13: Handle SIGUSR2 for graceful shutdown

// PROMPT 13: Signal handler for graceful shutdown
static volatile sig_atomic_t sigusr2_received = 0;

static void signal_handler_usr2(int sig) {
    (void)sig;
    sigusr2_received = 1;
}

static void signal_handler_term(int sig) {
    (void)sig;
    sigusr2_received = 1;  // SIGTERM działa tak samo jak SIGUSR2
}

int run_patient(int patient_id, const Config& config) {
    (void)config;  // Parametr `config` może być użyty w przyszłości
    
    // PROMPT 13: Setup signal handler for graceful shutdown
    // Ignoruj SIGINT - tylko Director reaguje na Ctrl+C
    struct sigaction sa_ign{};
    sa_ign.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa_ign, nullptr);
    
    struct sigaction sa{};
    sa.sa_handler = signal_handler_usr2;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR2, &sa, nullptr);
    
    // Setup handler dla SIGTERM (normalny koniec symulacji)
    sa.sa_handler = signal_handler_term;
    sigaction(SIGTERM, &sa, nullptr);
    
    // Attach do istniejących zasobów IPC
    if (ipc_attach() == -1) {
        perror("ipc_attach (patient)");
        fprintf(stderr, "Pacjent %d: nie mogę attachować do IPC (zasoby już usunięte?)\n", patient_id);
        return 1;
    }

    // Przygotuj dane pacjenta (prosta deterministyczna reguła)
    PatientInQueue p{};
    p.patient_id = patient_id;
    p.patient_pid = getpid();
    
    // PROMPT 12: Generuj też dzieci (20% szansa na wiek < 18)
    srand(patient_id + time(NULL));
    int is_child_roll = rand() % 100;
    if (is_child_roll < 20) {
        p.age = 5 + (rand() % 13);  // 5-17 lat (dziecko)
        p.has_guardian = 1;
    } else {
        p.age = 20 + (patient_id % 40);  // 20-59 (dorosły)
        p.has_guardian = 0;
    }
    
    p.is_vip = (patient_id % 5 == 0) ? 1 : 0;  // co piąty VIP
    strncpy(p.symptoms, "objawy nieokreslone", MAX_NAME_LEN - 1);

    if (p.has_guardian) {
        log_event("Dziecko %d pojawia się przed SOR (wiek %d, z opiekunem)", patient_id, p.age);
    } else {
        log_event("Pacjent %d pojawia się przed SOR (wiek %d)", patient_id, p.age);
    }

    // Próba wejścia do poczekalni
    // PROMPT 12: Dziecko z opiekunem zajmuje 2 miejsca (P(WAITROOM) x2)
    int waitroom_slots = p.has_guardian ? 2 : 1;
    for (int i = 0; i < waitroom_slots; i++) {
        while (true) {
            if (sem_P(g_sem_id, SEM_WAITROOM) == 0) {
                break;  // zdobył miejsce
            }
            // Jeśli sem_P zwróciło -1: mogło być EIDRM (semafor usunięty) lub timeout
            if (errno == EIDRM) {
                fprintf(stderr, "Pacjent %d: semafor został usunięty (symulacja skończona?)\n", patient_id);
                // Zwolnij już zajęte sloty
                for (int j = 0; j < i; j++) {
                    sem_V(g_sem_id, SEM_WAITROOM);
                }
                return 1;
            }
            if (i == 0) {
                log_event("Pacjent %d czeka przed budynkiem – brak miejsc", patient_id);
            }
            usleep(200000); // 200ms i spróbuj ponownie
        }
    }
    
    if (p.has_guardian) {
        log_event("Dziecko %d przychodzi z opiekunem (zajmuje 2 miejsca)", patient_id);
        log_event("Dziecko %d wchodzi do budynku", patient_id);
    } else {
        log_event("Pacjent %d wchodzi do budynku", patient_id);
    }

    // Aktualizacja inside_count pod mutexem stanu
    if (sem_P(g_sem_id, SEM_STATE_MUTEX) == -1) {
        // Zwolnij zajęte sloty
        for (int i = 0; i < waitroom_slots; i++) {
            sem_V(g_sem_id, SEM_WAITROOM);
        }
        return 1;
    }
    // Licznik pacjentów: +1 (opiekun jest logiczny, nie liczony osobno)
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

    const char* patient_type = p.has_guardian ? "Dziecko" : "Pacjent";
    if (p.is_vip) {
        log_event("%s %d (VIP) omija kolejkę rejestracji", patient_type, patient_id);
    } else {
        log_event("%s %d dołącza do kolejki rejestracji", patient_type, patient_id);
    }

    sem_V(g_sem_id, SEM_REG_MUTEX);   // wyjdź z sekcji krytycznej
    sem_V(g_sem_id, SEM_REG_ITEMS);   // sygnalizuj nowy element w kolejce

    // Na tym etapie pacjent czeka na dalsze etapy (prompty 8+).
    // Pacjent czeka w pętli aż zostanie obsłużony i wyjdzie, lub dostanie SIGUSR2/SIGTERM
    // Albo gdy generator się wyłączy (PPID == 1)
    pid_t parent_pid = getppid();
    while (!sigusr2_received && getppid() == parent_pid) {
        usleep(500000);  // czekaj 500ms i sprawdzaj czy parent żyje
    }
    
    // Graceful shutdown - zwolnij zasoby (bez logowania)
    return 0;
}

// Generator pacjentów - PROMPT 14: generowanie procesów pacjentów w pętli
int run_patient_generator(const Config& config) {
    // Ignoruj SIGINT - tylko Director reaguje na Ctrl+C
    struct sigaction sa_ign{};
    sa_ign.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa_ign, nullptr);
    
    struct sigaction sa{};
    sa.sa_handler = signal_handler_usr2;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR2, &sa, nullptr);
    
    // Obsługuj SIGTERM (normalny koniec)
    sa.sa_handler = signal_handler_term;
    sigaction(SIGTERM, &sa, nullptr);
    
    if (ipc_attach() == -1) {
        perror("ipc_attach (generator)");
        return 1;
    }
    
    log_event("[Generator] Generator pacjentów startuje");
    
    srand(time(NULL) + getpid());
    
    int patient_id = 1;
    
    while (!sigusr2_received) {
        // Fork nowy proces pacjenta
        pid_t pid = fork();
        
        if (pid == -1) {
            perror("fork (patient)");
            usleep(500000);  // 500ms delay on error
            continue;
        }
        
        if (pid == 0) {
            // Proces dziecka - wykonaj run_patient
            run_patient(patient_id, config);
            exit(0);
        }
        
        // Rodzic: kontynuuj generowanie
        patient_id++;
        
        // Czekaj według config.interval (±30% losowości)
        // np. interval=3.0 → 2.1-3.9 sekund
        // speed=2.0 → delay / 2.0 = szybsza symulacja
        double rand_factor = 0.7 + (rand() % 60) / 100.0;  // 0.7 - 1.3
        int delay_ms = (int)(config.interval * rand_factor * 1000 / config.speed);
        usleep(delay_ms * 1000);
        
        // Cleanup zombies
        waitpid(-1, nullptr, WNOHANG);
    }
    
    return 0;
}
