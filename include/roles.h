#ifndef ROLES_H
#define ROLES_H

#include "config.h"

// Deklaracje funkcji dla poszczególnych ról w symulacji

// Manager - główny proces zarządzający
int run_manager(int argc, char* argv[]);

// Director - wysyła sygnały do lekarzy
int run_director(const Config& config);

// Registration - okienko rejestracji
int run_registration(int window_id, const Config& config);

// Reg controller - monitoruje kolejkę i otwiera/zamyka okienko 2
int run_reg_controller(const Config& config);

// Triage - lekarz POZ (triaż)
int run_triage(const Config& config);

// Doctor - lekarz specjalista
int run_doctor(const char* specialization, const Config& config);

// Patient - pacjent
int run_patient(int patient_id, const Config& config);

// Generator pacjentów (stub na potrzeby PROMPT 5)
int run_patient_generator(const Config& config);

// Logger - proces logowania
int run_logger(const Config& config);

#endif // ROLES_H
