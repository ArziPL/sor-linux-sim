#include <cstdio>
#include <cstring>
#include "../include/common.h"
#include "../include/roles.h"
#include "../include/util.h"

// Główny plik - dispatcher ról
// Program uruchamia się jako: ./sor [role] [args...]

int main(int argc, char* argv[]) {
    // Jeśli brak argumentów - uruchom managera
    if (argc < 2) {
        return run_manager(argc, argv);
    }
    
    const char* role = argv[1];
    
    // Rozpoznaj rolę i wywołaj odpowiednią funkcję
    if (strcmp(role, "manager") == 0 || strcmp(role, "--help") == 0 || strcmp(role, "-h") == 0 || role[0] == '-') {
        // Jeśli to opcja (zaczyna się od -) lub manager - uruchom managera
        return run_manager(argc, argv);
    }
    else if (strcmp(role, "director") == 0) {
        // TODO: parsowanie konfiguracji z argumentów
        Config config = {}; // Tymczasowo pusty
        return run_director(config);
    }
    else if (strcmp(role, "registration") == 0) {
        // TODO: parsowanie window_id i konfiguracji
        int window_id = 1;
        Config config = {};
        return run_registration(window_id, config);
    }
    else if (strcmp(role, "triage") == 0) {
        Config config = {};
        return run_triage(config);
    }
    else if (strcmp(role, "doctor") == 0) {
        // TODO: parsowanie specjalizacji
        const char* spec = argc > 2 ? argv[2] : "kardiolog";
        Config config = {};
        return run_doctor(spec, config);
    }
    else if (strcmp(role, "patient") == 0) {
        // TODO: parsowanie patient_id
        int patient_id = argc > 2 ? atoi(argv[2]) : 0;
        Config config = {};
        return run_patient(patient_id, config);
    }
    else if (strcmp(role, "patient_gen") == 0) {
        Config config = {};
        return run_patient_generator(config);
    }
    else if (strcmp(role, "logger") == 0) {
        Config config = {};
        return run_logger(config);
    }
    else {
        fprintf(stderr, "Nieznana rola: %s\n", role);
        fprintf(stderr, "Dostępne role: manager, director, registration, triage, doctor, patient, logger\n");
        return 1;
    }
}
