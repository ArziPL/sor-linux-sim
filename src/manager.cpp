#include <cstdio>
#include "../include/common.h"
#include "../include/roles.h"
#include "../include/config.h"
#include "../include/util.h"

// Manager - główny proces zarządzający symulacją

int run_manager(int argc, char* argv[]) {
    printf("=== SOR Manager - uruchamianie ===\n");
    
    // Parser argumentów CLI (podstawowy - TODO PROMPT 2: pełna wersja)
    Config config = parse_arguments(argc, argv);
    
    // Walidacja
    if (!validate_config(config)) {
        return 1;
    }
    
    printf("Konfiguracja:\n");
    printf("  N (miejsca w poczekalni): %d\n", config.N);
    printf("  K (próg drugiego okienka): %d\n", config.K);
    printf("  Duration: %d sekund (0 = nieskończoność)\n", config.duration);
    printf("  Speed: %.1f\n", config.speed);
    printf("  Seed: %u\n", config.seed);
    printf("  Buf_size (rozmiar kolejki): %d\n", config.buf_size);
    
    // TODO PROMPT 3: IPC create
    // TODO PROMPT 4: Uruchomienie loggera
    // TODO PROMPT 5: Uruchomienie procesów (director, registration, triage, doctors, patients)
    
    printf("Manager: symulacja zakończona\n");
    return 0;
}
