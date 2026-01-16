#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <cstdlib>
#include <ctime>
#include <cstdarg>
#include <sys/ipc.h>
#include <sys/msg.h>
#include "../include/common.h"
#include "../include/util.h"
#include "../include/config.h"
#include "../include/protocol.h"
#include "../include/ipc.h"

// Util - funkcje pomocnicze (parser argumentów, walidacja)

void print_help(const char* program_name) {
    printf("Użycie: %s [opcje]\n\n", program_name);
    printf("Opcje:\n");
    printf("  --N <liczba>       Liczba miejsc w poczekalni (domyślnie: 20, zakres: 1-1000)\n");
    printf("  --K <liczba>       Próg otwarcia drugiego okienka (domyślnie: ceil(N/2))\n");
    printf("  --duration <sek>   Czas trwania symulacji w sekundach (0 = nieskończoność, domyślnie: 0)\n");
    printf("  --speed <mnożnik>  Mnożnik czasu (domyślnie: 2.0)\n");
    printf("  --interval <sek>   Średni czas między pacjentami w sekundach (domyślnie: 3.0)\n");
    printf("  --seed <liczba>    Ziarno generatora losowego (domyślnie: time(NULL))\n");
    printf("  --help             Wyświetla tę pomoc\n");

    printf("\nPrzykłady:\n");
    printf("  %s                 # Uruchomienie z domyślnymi parametrami\n", program_name);
    printf("  %s --N 30          # 30 miejsc w poczekalni\n", program_name);
    printf("  %s --duration 60   # Symulacja przez 60 sekund\n", program_name);
    printf("  %s --interval 5.0  # Pacjent co ~5 sekund\n", program_name);
}

bool validate_config(const Config& config) {
    if (config.N < 1 || config.N > 1000) {
        fprintf(stderr, "Błąd: N musi być w zakresie 1-1000 (podano: %d)\n", config.N);
        return false;
    }
    
    if (config.duration < 0) {
        fprintf(stderr, "Błąd: duration nie może być ujemna (podano: %d)\n", config.duration);
        return false;
    }
    
    if (config.speed <= 0) {
        fprintf(stderr, "Błąd: speed musi być > 0 (podano: %.2f)\n", config.speed);
        return false;
    }
    
    // Sprawdzenie K >= ceil(N/2)
    int min_K = (int)ceil(config.N / 2.0);
    if (config.K < min_K) {
        fprintf(stderr, "Błąd: K musi być >= ceil(N/2) = %d (podano: %d)\n", min_K, config.K);
        return false;
    }
    
    return true;
}

Config parse_arguments(int argc, char* argv[]) {
    // Domyślna konfiguracja
    Config config;
    config.N = 20;
    config.K = -1;  // Będzie obliczone automatycznie
    config.duration = 0;
    config.speed = 2.0;
    config.seed = time(NULL);
    config.interval = 0.25;  // Domyślnie 0.25 sekundy między pacjentami
    
    // Parser argumentów linii poleceń
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        // Pomoc
        if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            exit(0);
        }
        
        // Liczba miejsc w poczekalni
        else if (arg == "--N") {
            if (i + 1 >= argc) {
                fprintf(stderr, "Błąd: --N wymaga wartości\n");
                exit(1);
            }
            config.N = atoi(argv[++i]);
            if (config.N < 1 || config.N > 1000) {
                fprintf(stderr, "Błąd: N musi być w zakresie 1-1000 (podano: %d)\n", config.N);
                exit(1);
            }
        }
        
        // Próg drugiego okienka
        else if (arg == "--K") {
            if (i + 1 >= argc) {
                fprintf(stderr, "Błąd: --K wymaga wartości\n");
                exit(1);
            }
            config.K = atoi(argv[++i]);
        }
        
        // Czas trwania symulacji
        else if (arg == "--duration") {
            if (i + 1 >= argc) {
                fprintf(stderr, "Błąd: --duration wymaga wartości\n");
                exit(1);
            }
            config.duration = atoi(argv[++i]);
            if (config.duration < 0) {
                fprintf(stderr, "Błąd: duration nie może być ujemna (podano: %d)\n", config.duration);
                exit(1);
            }
        }
        
        // Mnożnik czasu
        else if (arg == "--speed") {
            if (i + 1 >= argc) {
                fprintf(stderr, "Błąd: --speed wymaga wartości\n");
                exit(1);
            }
            config.speed = atof(argv[++i]);
            if (config.speed <= 0) {
                fprintf(stderr, "Błąd: speed musi być > 0 (podano: %.2f)\n", config.speed);
                exit(1);
            }
        }
        
        // Ziarno generatora
        else if (arg == "--seed") {
            if (i + 1 >= argc) {
                fprintf(stderr, "Błąd: --seed wymaga wartości\n");
                exit(1);
            }
            config.seed = (unsigned int)atoi(argv[++i]);
        }
        
        // Interwał między pacjentami
        else if (arg == "--interval") {
            if (i + 1 >= argc) {
                fprintf(stderr, "Błąd: --interval wymaga wartości\n");
                exit(1);
            }
            config.interval = atof(argv[++i]);
            if (config.interval <= 0) {
                fprintf(stderr, "Błąd: interval musi być > 0 (podano: %.2f)\n", config.interval);
                exit(1);
            }
        }
        
        // Nieznana opcja
        else {
            fprintf(stderr, "Błąd: nieznana opcja '%s'\n", argv[i]);
            fprintf(stderr, "Użyj: %s --help\n", argv[0]);
            exit(1);
        }
    }
    
    // Obliczenie K jeśli nie podane
    if (config.K == -1) {
        config.K = (int)ceil(config.N / 2.0);
    }
    
    // Walidacja: K >= ceil(N/2)
    int min_K = (int)ceil(config.N / 2.0);
    if (config.K < min_K) {
        fprintf(stderr, "Błąd: K musi być >= ceil(N/2) = %d (podano: %d)\n", min_K, config.K);
        exit(1);
    }
    

    
    return config;
}

// ============================================================================
// FUNKCJE DO LOGOWANIA
// ============================================================================

// Wysłanie komunikatu do loggera (przez MSGQ)
// Format: [ XX.XXs ] opis zdarzenia
int log_event(const char* format, ...) {
    // Zawsze wypisz na stderr dla debugowania
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    
    // Jeśli MSGQ nie istnieje, koniec
    if (g_msgq_id == -1) {
        return 0;
    }
    
    // Sformatuj wiadomość
    char msg_text[MAX_MSG_SIZE];
    va_start(args, format);
    vsnprintf(msg_text, sizeof(msg_text), format, args);
    va_end(args);
    
    // Przygotuj komunikat do wysłania
    LogMessage msg;
    msg.mtype = 1;  // Typ wiadomości = 1 dla logów
    strncpy(msg.text, msg_text, MAX_MSG_SIZE - 1);
    msg.text[MAX_MSG_SIZE - 1] = '\0';
    
    // Wyślij do loggera (rozmiar BEZ pola mtype)
    if (msgsnd(g_msgq_id, &msg, LOG_PAYLOAD_SIZE, IPC_NOWAIT) == -1) {
        // Jeśli nie można wysłać, wypisz na stderr
        fprintf(stderr, "[LOG ERROR] Nie mogę wysłać do MSGQ: %s\n", msg_text);
        return -1;
    }
    
    return 0;
}

// Wysłanie surowego komunikatu
int log_raw(const char* text) {
    if (g_msgq_id == -1) {
        fprintf(stderr, "%s\n", text);
        return 0;
    }
    
    LogMessage msg;
    msg.mtype = 1;
    strncpy(msg.text, text, MAX_MSG_SIZE - 1);
    msg.text[MAX_MSG_SIZE - 1] = '\0';
    
    // msgsnd - trzeci argument to rozmiar BEZ pola mtype!
    if (msgsnd(g_msgq_id, &msg, LOG_PAYLOAD_SIZE, IPC_NOWAIT) == -1) {
        fprintf(stderr, "%s\n", text);
        return -1;
    }
    
    return 0;
}
