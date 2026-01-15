#include <cstdio>
#include <cstring>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "../include/common.h"
#include "../include/roles.h"
#include "../include/config.h"
#include "../include/util.h"
#include "../include/ipc.h"

// Manager - główny proces zarządzający symulacją
// PROMPT 13: Signal handling for Ctrl+C (SIGINT) and graceful shutdown

static std::vector<pid_t>* g_children_ptr = nullptr;
static volatile sig_atomic_t sigint_received = 0;

// PROMPT 13: Signal handler for graceful shutdown
static void signal_handler_sigint(int sig) {
    (void)sig;
    sigint_received = 1;
}

int run_manager(int argc, char* argv[]) {
    printf("=== SOR Manager - uruchamianie ===\n");
    
    // Parser argumentów CLI (podstawowy - TODO PROMPT 2: pełna wersja)
    Config config = parse_arguments(argc, argv);
    
    // Walidacja
    if (!validate_config(config)) {
        return 1;
    }
    
    printf("\n");
    
    // Tworzenie zasobów IPC
    printf("=== Inicjalizacja IPC ===\n");
    if (ipc_create(config) == -1) {
        fprintf(stderr, "Błąd: nie udało się stworzyć zasobów IPC\n");
        return 1;
    }
    
    printf("\n=== Symulacja SOR ===\n");
    printf("Wciśnij Ctrl+C aby przerwać\n\n");
    
    printf("Konfiguracja:\n");
    printf("  N (miejsca w poczekalni): %d\n", config.N);
    printf("  K (próg drugiego okienka): %d\n", config.K);
    printf("  Duration: %d sekund (0 = nieskończoność)\n", config.duration);
    printf("  Speed: %.1f\n", config.speed);
    printf("  Seed: %u\n", config.seed);
    printf("  Rozmiar kolejki rejestracji: %d (= N)\n", config.N);
    
    printf("\n");
    
    // Helper do konwersji konfigu na argv
    auto make_config_strings = [&](char* n, char* k, char* dur, char* spd, char* seed, char* interval) {
        snprintf(n, 16, "%d", config.N);
        snprintf(k, 16, "%d", config.K);
        snprintf(dur, 16, "%d", config.duration);
        snprintf(spd, 16, "%.2f", config.speed);
        snprintf(seed, 16, "%u", config.seed);
        snprintf(interval, 16, "%.2f", config.interval);
    };

    auto spawn_role = [&](const char* role, const std::vector<const char*>& extra_args) -> pid_t {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            return -1;
        }
        if (pid == 0) {
            char arg_N[16], arg_K[16], arg_duration[16], arg_speed[16], arg_seed[16], arg_interval[16];
            make_config_strings(arg_N, arg_K, arg_duration, arg_speed, arg_seed, arg_interval);

            // Budujemy argv: ./sor <role> [extra...] N K duration speed seed interval
            std::vector<char*> args;
            args.push_back(argv[0]);
            args.push_back(const_cast<char*>(role));
            for (const char* ea : extra_args) {
                args.push_back(const_cast<char*>(ea));
            }
            args.push_back(arg_N);
            args.push_back(arg_K);
            args.push_back(arg_duration);
            args.push_back(arg_speed);
            args.push_back(arg_seed);
            args.push_back(arg_interval);
            args.push_back(nullptr);

            execvp(argv[0], args.data());
            perror("execvp");
            exit(1);
        }
        return pid;
    };

    std::vector<pid_t> children;    
    // PROMPT 13: Setup signal handler for Ctrl+C using sigaction()
    g_children_ptr = &children;
    struct sigaction sa_sigint{};
    sa_sigint.sa_handler = signal_handler_sigint;
    sigemptyset(&sa_sigint.sa_mask);
    sa_sigint.sa_flags = 0;
    sigaction(SIGINT, &sa_sigint, nullptr);
    // Uruchomienie loggera przez fork+exec
    printf("Uruchamianie loggera...\n");
    pid_t logger_pid = spawn_role("logger", {});
    if (logger_pid == -1) {
        ipc_cleanup();
        return 1;
    }
    printf("Logger uruchomiony (PID: %d)\n", logger_pid);
    children.push_back(logger_pid);

    // Czekaj 500ms aby logger się uruchomił i attachował do IPC
    usleep(500000);

    // Log startu symulacji
    log_event("[Manager] Symulacja rozpoczyna pracę");
    log_event("[Manager] Parametry: N=%d, K=%d, duration=%d", config.N, config.K, config.duration);

    // Director
    pid_t director_pid = spawn_role("director", {});
    if (director_pid != -1) {
        children.push_back(director_pid);
        log_event("[Manager] Director uruchomiony (PID=%d)", director_pid);
    }

    // Registration window 1
    pid_t reg1_pid = spawn_role("registration", {"1"});
    if (reg1_pid != -1) {
        children.push_back(reg1_pid);
        log_event("[Manager] Okienko rejestracji 1 uruchomione (PID=%d)", reg1_pid);
    }

    // Reg controller (otwiera/zamyka okienko 2)
    pid_t reg_ctrl_pid = spawn_role("reg_controller", {});
    if (reg_ctrl_pid != -1) {
        children.push_back(reg_ctrl_pid);
        log_event("[Manager] Kontroler rejestracji uruchomiony (PID=%d)", reg_ctrl_pid);
    }

    // Triage
    pid_t triage_pid = spawn_role("triage", {});
    if (triage_pid != -1) {
        children.push_back(triage_pid);
        log_event("[Manager] Lekarz POZ (triaż) uruchomiony (PID=%d)", triage_pid);
    }

    // Lekarze specjaliści (6 sztuk)
    const char* specs[] = {"kardiolog", "neurolog", "okulista", "laryngolog", "chirurg", "pediatra"};
    for (int i = 0; i < 6; i++) {
        pid_t pid = spawn_role("doctor", {specs[i]});
        if (pid != -1) {
            children.push_back(pid);
            
            // PROMPT 13: Store doctor PID in shared memory for director signals
            if (sem_P(g_sem_id, SEM_STATE_MUTEX) == 0) {
                g_sor_state->doctor_pids[i] = pid;
                sem_V(g_sem_id, SEM_STATE_MUTEX);
            }
            
            log_event("[Manager] Lekarz %s uruchomiony (PID=%d)", specs[i], pid);
        }
    }

    // Generator pacjentów (stub)
    pid_t gen_pid = spawn_role("patient_gen", {});
    if (gen_pid != -1) {
        children.push_back(gen_pid);
        log_event("[Manager] Generator pacjentów uruchomiony (PID=%d)", gen_pid);
    }

    // PROMPT 13: Czekaj na zakończenie symulacji lub Ctrl+C
    if (config.duration > 0) {
        // Symulacja z określonym czasem trwania
        int elapsed = 0;
        while (elapsed < config.duration && !sigint_received) {
            sleep(1);
            elapsed++;
            
            // Cleanup zombies
            int status = 0;
            while (waitpid(-1, &status, WNOHANG) > 0) {
                // Zbieraj zombie
            }
        }
    } else {
        // Symulacja nieskończona - czekaj na Ctrl+C
        while (!sigint_received) {
            int status = 0;
            pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid == -1 && errno != ECHILD) {
                // Błąd (ale nie ECHILD - brak procesów)
                break;
            }
            usleep(100000);  // 100ms
        }
    }

    // PROMPT 13: Manager otrzymał Ctrl+C
    if (sigint_received) {
        log_event("[Manager] Ctrl+C — zarządzam ewakuację SOR");
        printf("Manager: Ctrl+C — wysyłam SIGUSR2 do ewakuacji...\n");
        
        // Wysyłaj SIGUSR2 do bezpośrednich dzieci (pacjenci spawniowani przez generator zakończą się gdy generator umrze)
        if (children.size() > 1) {
            for (size_t i = 1; i < children.size(); i++) {  // Pomijaj logger (index 0)
                if (children[i] > 0) {
                    kill(children[i], SIGUSR2);
                }
            }
        }
        
        // Czekaj aby procesy zdążyły się wyłączyć
        for (int i = 0; i < 30; i++) {
            usleep(100000);  // 100ms x 30 = 3 sekundy
            int status = 0;
            while (waitpid(-1, &status, WNOHANG) > 0) {
                // Zbieraj zombie
            }
        }
        
        // Wyślij SIGTERM do loggera aby się zamknął (ignoruje SIGUSR2)
        if (children.size() > 0 && children[0] > 0) {
            kill(children[0], SIGTERM);
        }
    } else if (config.duration > 0) {
        // PROMPT 13: Okres duration skończył się - wysłij SIGTERM do normalnego zamknięcia
        log_event("[Manager] Czas symulacji skończył się — zarządzam normalnym zamknięciem SOR");
        printf("Manager: Duration skończył się — wysyłam SIGTERM do normalnego zamknięcia...\n");
        
        // Wysyłaj SIGTERM do bezpośrednich dzieci (pacjenci spawniowani przez generator zakończą się gdy generator umrze)
        if (children.size() > 1) {
            for (size_t i = 1; i < children.size(); i++) {  // Pomijaj logger (index 0)
                if (children[i] > 0) {
                    kill(children[i], SIGTERM);
                }
            }
        }
        
        // Czekaj aby procesy zdążyły się wyłączyć (do 3 sekund)
        for (int i = 0; i < 30; i++) {
            usleep(100000);  // 100ms x 30 = 3 sekundy
            // Sprawdzaj czy jeszcze są procesy
            int status = 0;
            while (waitpid(-1, &status, WNOHANG) > 0) {
                // Zbieraj zombie
            }
        }
        
        // Wyślij SIGTERM do loggera aby się zamknął
        if (children.size() > 0 && children[0] > 0) {
            kill(children[0], SIGTERM);
        }
    }

    printf("Manager: symulacja zakończona\n");

    // Czyszczenie zasobów IPC
    printf("\n=== Sprzątanie ===\n");

    // Czekaj na wszystkie dzieci
    for (pid_t pid : children) {
        if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        }
    }

    // Zaczekaj na wszystkie dzieci (blokująco) – jeśli już zakończone, ECHILD przerwie
    while (true) {
        pid_t r = waitpid(-1, nullptr, 0);
        if (r == -1) {
            break;
        }
    }

    ipc_cleanup();
    
    printf("Manager: zasoby wyczyszczone\n");
    return 0;
}
