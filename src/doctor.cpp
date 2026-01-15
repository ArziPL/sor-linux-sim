#include <cstdio>
#include "../include/common.h"
#include "../include/roles.h"
#include "../include/config.h"

// Doctor - lekarz specjalista

int run_doctor(const char* specialization, const Config& config) {
    printf("Lekarz %s: rozpoczynam pracę\n", specialization);
    
    // TODO PROMPT 11: Odbieranie pacjentów priorytetowo (czerwony>żółty>zielony)
    // TODO PROMPT 13: Obsługa SIGUSR1 (wyjście na oddział)
    
    return 0;
}
