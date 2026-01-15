#ifndef UTIL_H
#define UTIL_H

#include "config.h"

// Funkcje pomocnicze

// Parser argumentów linii poleceń
Config parse_arguments(int argc, char* argv[]);

// Wyświetlenie pomocy
void print_help(const char* program_name);

// Walidacja konfiguracji
bool validate_config(const Config& config);

// ============================================================================
// FUNKCJE DO LOGOWANIA
// ============================================================================

// Wysłanie komunikatu do loggera (przez MSGQ)
// Format: [ XX.XXs ] opis zdarzenia
// Zwraca 0 jeśli OK, -1 jeśli błąd
int log_event(const char* format, ...);

// Wysłanie surowego komunikatu (bez automatycznego formatu)
// Zwraca 0 jeśli OK, -1 jeśli błąd
int log_raw(const char* text);

#endif // UTIL_H
