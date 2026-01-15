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
    printf("  Buf_size (rozmiar kolejki): %d\n", config.buf_size);
    
    printf("\n");
    
    // Helper do konwersji konfigu na argv
    auto make_config_strings = [&](char* n, char* k, char* dur, char* spd, char* seed) {
        snprintf(n, 16, "%d", config.N);
        snprintf(k, 16, "%d", config.K);
        snprintf(dur, 16, "%d", config.duration);
        snprintf(spd, 16, "%.2f", config.speed);
        snprintf(seed, 16, "%u", config.seed);
    };

    auto spawn_role = [&](const char* role, const std::vector<const char*>& extra_args) -> pid_t {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            return -1;
        }
        if (pid == 0) {
            char arg_N[16], arg_K[16], arg_duration[16], arg_speed[16], arg_seed[16];
            make_config_strings(arg_N, arg_K, arg_duration, arg_speed, arg_seed);

            // Budujemy argv: ./sor <role> [extra...] N K duration speed seed
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
            args.push_back(nullptr);

            execvp(argv[0], args.data());
            perror("execvp");
            exit(1);
        }
        return pid;
    };

    std::vector<pid_t> children;

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

    // Triage
    pid_t triage_pid = spawn_role("triage", {});
    if (triage_pid != -1) {
        children.push_back(triage_pid);
        log_event("[Manager] Lekarz POZ (triaż) uruchomiony (PID=%d)", triage_pid);
    }

    // Lekarze specjaliści (6 sztuk)
    const char* specs[] = {"kardiolog", "neurolog", "okulista", "laryngolog", "chirurg", "pediatra"};
    for (const char* spec : specs) {
        pid_t pid = spawn_role("doctor", {spec});
        if (pid != -1) {
            children.push_back(pid);
            log_event("[Manager] Lekarz %s uruchomiony (PID=%d)", spec, pid);
        }
    }

    // Generator pacjentów (stub)
    pid_t gen_pid = spawn_role("patient_gen", {});
    if (gen_pid != -1) {
        children.push_back(gen_pid);
        log_event("[Manager] Generator pacjentów uruchomiony (PID=%d)", gen_pid);
    }

    // Pętla sprzątająca zombie (krótka, bo role na razie są stubami)
    for (int i = 0; i < 5; ++i) {
        int status = 0;
        while (true) {
            pid_t reap = waitpid(-1, &status, WNOHANG);
            if (reap <= 0) {
                break;
            }
        }
        usleep(100000);
    }

    printf("Manager: symulacja zakończona\n");

    // Czyszczenie zasobów IPC
    printf("\n=== Sprzątanie ===\n");

    // Sygnalizuj zakończenie wszystkim dzieciom (SIGTERM) aby logger nie blokował
    for (pid_t pid : children) {
        if (pid > 0) {
            kill(pid, SIGTERM);
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
