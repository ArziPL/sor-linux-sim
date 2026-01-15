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

#endif // UTIL_H
