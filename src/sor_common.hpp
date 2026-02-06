/**
 * @file sor_common.hpp
 * @brief Wspólne definicje, stałe i funkcje pomocnicze dla symulacji SOR
 * 
 * Plik zawiera:
 * - Stałe konfiguracyjne symulacji (pojemność poczekalni, progi kolejek, czasy)
 * - Struktury IPC (pamięć dzielona, kolejki komunikatów)
 * - Funkcje pomocnicze do logowania i obsługi błędów
 */

#ifndef SOR_COMMON_HPP
#define SOR_COMMON_HPP

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <cstdarg>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <pthread.h>
#include <fcntl.h>
#include <chrono>
#include <string>
#include <random>

// ============================================================================
// STAŁE KONFIGURACYJNE SYMULACJI
// ============================================================================

constexpr int N = 20;                    // Pojemność poczekalni SOR
constexpr int K_OPEN = N / 2;            // Próg otwarcia drugiego okienka (>=10 osób w kolejce)
constexpr int K_CLOSE = N / 3;           // Próg zamknięcia drugiego okienka (<7 osób)

// Klucze IPC - generowane na podstawie ścieżki i identyfikatorów
constexpr int SHM_KEY_ID = 'S';          // Klucz pamięci dzielonej
constexpr int SEM_KEY_ID = 'E';          // Klucz semaforów
constexpr int MSG_KEY_ID = 'M';          // Klucz kolejki komunikatów

// Czasy operacji w milisekundach (skrócone dla dynamicznej symulacji)
constexpr int PATIENT_GEN_MIN_MS = 500;  // Min czas między generowaniem pacjentów
constexpr int PATIENT_GEN_MAX_MS = 1500; // Max czas między generowaniem pacjentów
constexpr int REGISTRATION_MIN_MS = 500; // Min czas rejestracji
constexpr int REGISTRATION_MAX_MS = 1000; // Max czas rejestracji
constexpr int TRIAGE_MIN_MS = 500;       // Min czas triażu
constexpr int TRIAGE_MAX_MS = 1000;      // Max czas triażu
constexpr int TREATMENT_MIN_MS = 1000;   // Min czas leczenia u specjalisty
constexpr int TREATMENT_MAX_MS = 2000;   // Max czas leczenia u specjalisty
constexpr int DOCTOR_BREAK_MIN_MS = 3000; // Min czas przerwy lekarza
constexpr int DOCTOR_BREAK_MAX_MS = 5000; // Max czas przerwy lekarza

// ============================================================================
// TYPY ENUMERACYJNE
// ============================================================================

/**
 * @brief Typy lekarzy w SOR
 */
enum DoctorType {
    DOCTOR_POZ = 0,      // Lekarz POZ (triaż)
    DOCTOR_KARDIOLOG,    // Kardiolog
    DOCTOR_NEUROLOG,     // Neurolog  
    DOCTOR_OKULISTA,     // Okulista
    DOCTOR_LARYNGOLOG,   // Laryngolog
    DOCTOR_CHIRURG,      // Chirurg
    DOCTOR_PEDIATRA,     // Pediatra
    DOCTOR_COUNT         // Liczba typów lekarzy
};

/**
 * @brief Nazwy lekarzy do wyświetlania
 */
inline const char* getDoctorName(DoctorType type) {
    static const char* names[] = {
        "POZ (triaż)", "kardiolog", "neurolog", 
        "okulista", "laryngolog", "chirurg", "pediatra"
    };
    return (type >= 0 && type < DOCTOR_COUNT) ? names[type] : "nieznany";
}

/**
 * @brief Kolory triażu określające priorytet pacjenta
 */
enum TriageColor {
    COLOR_NONE = 0,      // Brak przypisanego koloru
    COLOR_RED = 1,       // Czerwony - natychmiastowa pomoc
    COLOR_YELLOW = 2,    // Żółty - pilny
    COLOR_GREEN = 3,     // Zielony - może czekać
    COLOR_SENT_HOME = 4  // Odesłany do domu z triażu
};

/**
 * @brief Nazwy kolorów triażu
 */
inline const char* getColorName(TriageColor color) {
    static const char* names[] = {
        "brak", "czerwony", "żółty", "zielony", "odesłany"
    };
    return (color >= 0 && color <= COLOR_SENT_HOME) ? names[color] : "nieznany";
}

// ============================================================================
// INDEKSY SEMAFORÓW W TABLICY
// ============================================================================

/**
 * Semafory używane w symulacji:
 * - SEM_POCZEKALNIA: kontroluje wejście do poczekalni (inicjalizacja: N)
 * - SEM_REG_QUEUE: mutex dla kolejki rejestracji
 * - SEM_REG_WINDOW_1/2: semafory okienek rejestracji
 * - SEM_TRIAGE_QUEUE: mutex dla kolejki triażu
 * - SEM_TRIAGE_READY: semafor gotowości lekarza POZ
 * - SEM_SPECIALIST_*: semafory poszczególnych specjalistów
 * - SEM_SHM_MUTEX: mutex dla dostępu do pamięci dzielonej
 * - SEM_LOG_MUTEX: mutex dla logowania
 */
enum SemIndex {
    SEM_POCZEKALNIA = 0,     // Licznik wolnych miejsc w poczekalni
    SEM_REG_QUEUE_MUTEX,     // Mutex kolejki rejestracji
    SEM_REG_WINDOW_1,        // Okienko rejestracji 1 (0=zajęte, 1=wolne)
    SEM_REG_WINDOW_2,        // Okienko rejestracji 2
    SEM_TRIAGE_QUEUE_MUTEX,  // Mutex kolejki do triażu
    SEM_TRIAGE_READY,        // Lekarz POZ gotowy do przyjęcia
    SEM_SPECIALIST_KARDIOLOG,
    SEM_SPECIALIST_NEUROLOG,
    SEM_SPECIALIST_OKULISTA,
    SEM_SPECIALIST_LARYNGOLOG,
    SEM_SPECIALIST_CHIRURG,
    SEM_SPECIALIST_PEDIATRA,
    SEM_SHM_MUTEX,           // Mutex pamięci dzielonej
    SEM_LOG_MUTEX,           // Mutex logowania do pliku
    SEM_COUNT                // Liczba semaforów
};

/**
 * @brief Zwraca indeks semafora dla danego specjalisty
 */
inline int getSpecialistSemIndex(DoctorType type) {
    switch(type) {
        case DOCTOR_KARDIOLOG: return SEM_SPECIALIST_KARDIOLOG;
        case DOCTOR_NEUROLOG: return SEM_SPECIALIST_NEUROLOG;
        case DOCTOR_OKULISTA: return SEM_SPECIALIST_OKULISTA;
        case DOCTOR_LARYNGOLOG: return SEM_SPECIALIST_LARYNGOLOG;
        case DOCTOR_CHIRURG: return SEM_SPECIALIST_CHIRURG;
        case DOCTOR_PEDIATRA: return SEM_SPECIALIST_PEDIATRA;
        default: return -1;
    }
}

// ============================================================================
// STRUKTURY KOMUNIKATÓW (KOLEJKA KOMUNIKATÓW)
// ============================================================================

/**
 * @brief Typy wiadomości w kolejce komunikatów
 */
enum MessageType {
    MSG_PATIENT_TO_REGISTRATION = 1,      // Pacjent zgłasza się do rejestracji (zwykły)
    MSG_PATIENT_TO_REGISTRATION_VIP = 2,  // Pacjent VIP zgłasza się do rejestracji
    MSG_REGISTRATION_RESPONSE = 100,      // Odpowiedź rejestracji (bazowy + patient_id)
    MSG_PATIENT_TO_TRIAGE = 3,            // Pacjent po rejestracji idzie na triaż
    MSG_TRIAGE_RESPONSE = 150,            // Odpowiedź POZ (bazowy + patient_id) - kolor i specjalista
    MSG_PATIENT_TO_SPECIALIST = 4,        // Pacjent po triażu idzie do specjalisty
    MSG_SPECIALIST_RESPONSE = 200,        // Odpowiedź specjalisty (bazowy + patient_id)
};

/**
 * @brief Struktura wiadomości w kolejce komunikatów
 * Używana do komunikacji między pacjentami a lekarzami
 */
struct SORMessage {
    long mtype;              // Typ wiadomości (wymagane przez msgrcv/msgsnd)
    int patient_id;          // ID pacjenta
    int patient_pid;         // PID procesu pacjenta
    int age;                 // Wiek pacjenta
    int is_vip;              // Czy pacjent VIP
    TriageColor color;       // Kolor triażu
    DoctorType assigned_doctor; // Przypisany lekarz specjalista
    int outcome;             // Wynik: 0=do domu, 1=oddział, 2=inna placówka
};

// ============================================================================
// STRUKTURA PAMIĘCI DZIELONEJ
// ============================================================================

/**
 * @brief Stan współdzielony symulacji SOR
 * Przechowuje wszystkie informacje o stanie kolejek, okienek i pracy lekarzy
 */
struct SharedState {
    // Czas startu symulacji (do obliczania timestampów)
    time_t start_time_sec;
    long start_time_nsec;
    
    // Flaga zakończenia symulacji
    volatile int shutdown;
    
    // PID dyrektora (głównego procesu)
    pid_t director_pid;
    
    // PID procesu rejestracji
    pid_t registration_pid;
    
    // PIDy lekarzy (do wysyłania sygnałów)
    pid_t doctor_pids[DOCTOR_COUNT];
    
    // Stan okienek rejestracji
    int reg_window_2_open;           // Czy okienko 2 jest otwarte
    int reg_queue_count;             // Liczba osób w kolejce do rejestracji
    int reg_queue_vip_count;         // Liczba VIPów w kolejce
    
    // Liczniki do statystyk
    int total_patients;              // Całkowita liczba pacjentów
    int patients_in_sor;             // Aktualnie w SOR
    int patients_waiting_outside;    // Czekający przed wejściem
    
    // Stan lekarzy (czy są na oddziale)
    volatile int doctor_on_break[DOCTOR_COUNT];
    
    // Licznik pacjentów w kolejkach do specjalistów (wg koloru i typu)
    int specialist_queue_count[DOCTOR_COUNT];
    
    // Ścieżka do pliku logu
    char log_file[256];
};

// ============================================================================
// FUNKCJE POMOCNICZE - OBSŁUGA BŁĘDÓW
// ============================================================================

/**
 * @brief Wyświetla błąd i kończy program
 * Używa perror() do wyświetlenia opisu błędu systemowego
 */
inline void handleError(const char* msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

/**
 * @brief Wyświetla błąd bez kończenia programu
 */
inline void printError(const char* msg) {
    perror(msg);
}

// ============================================================================
// FUNKCJE POMOCNICZE - SEMAFORY (System V)
// ============================================================================

/**
 * @brief Wykonuje operację P (wait/czekaj) na semaforze
 * Blokuje aż semafor > 0, potem dekrementuje
 * @param semid ID zestawu semaforów
 * @param sem_num Numer semafora w zestawie
 */
inline void semWait(int semid, int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = -1;          // Dekrementacja (czekanie)
    op.sem_flg = 0;          // Blokujące czekanie (bez IPC_NOWAIT!)
    
    // Powtarzaj operację jeśli przerwana przez sygnał
    while (semop(semid, &op, 1) == -1) {
        if (errno != EINTR) {
            // Ignoruj błędy przy zamykaniu
            if (errno != EIDRM && errno != EINVAL) {
                printError("semWait");
            }
            return;
        }
    }
}

/**
 * @brief Wykonuje operację V (signal/sygnalizuj) na semaforze
 * Inkrementuje wartość semafora
 * @param semid ID zestawu semaforów
 * @param sem_num Numer semafora w zestawie
 */
inline void semSignal(int semid, int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = 1;           // Inkrementacja (sygnalizacja)
    op.sem_flg = 0;
    
    while (semop(semid, &op, 1) == -1) {
        if (errno != EINTR) {
            if (errno != EIDRM && errno != EINVAL) {
                printError("semSignal");
            }
            return;
        }
    }
}

/**
 * @brief Próbuje wykonać operację P bez blokowania
 * @return true jeśli udało się zająć semafor, false w przeciwnym razie
 */
inline bool semTryWait(int semid, int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = -1;
    op.sem_flg = IPC_NOWAIT; // Nieblokujące (tylko tutaj!)
    
    if (semop(semid, &op, 1) == -1) {
        return false;
    }
    return true;
}

/**
 * @brief Pobiera aktualną wartość semafora
 */
inline int semGetValue(int semid, int sem_num) {
    return semctl(semid, sem_num, GETVAL);
}

// ============================================================================
// FUNKCJE POMOCNICZE - LOGOWANIE
// ============================================================================

/**
 * @brief Pobiera czas od startu symulacji w sekundach
 */
inline double getElapsedTime(SharedState* state) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    double elapsed = (now.tv_sec - state->start_time_sec) + 
                     (now.tv_nsec - state->start_time_nsec) / 1e9;
    return elapsed;
}

/**
 * @brief Loguje wiadomość do pliku z timestampem
 * Format: [XXX.XXs] wiadomość
 * @param state Wskaźnik do stanu współdzielonego
 * @param semid ID semaforów (do blokady mutex)
 * @param format Format printf
 */
inline void logMessage(SharedState* state, int semid, const char* format, ...) {
    if (!state || state->shutdown) return;
    
    // Blokada mutex logowania
    semWait(semid, SEM_LOG_MUTEX);
    
    double elapsed = getElapsedTime(state);
    
    // Otwieramy plik w trybie dopisywania
    FILE* logFile = fopen(state->log_file, "a");
    if (logFile) {
        fprintf(logFile, "[%7.2fs] ", elapsed);
        
        va_list args;
        va_start(args, format);
        vfprintf(logFile, format, args);
        va_end(args);
        
        fprintf(logFile, "\n");
        fclose(logFile);
    }
    
    // Wyświetlamy też na stdout
    printf("[%7.2fs] ", elapsed);
    va_list args2;
    va_start(args2, format);
    vprintf(format, args2);
    va_end(args2);
    printf("\n");
    fflush(stdout);
    
    // Odblokowanie mutex
    semSignal(semid, SEM_LOG_MUTEX);
}

// ============================================================================
// FUNKCJE POMOCNICZE - LOSOWOŚĆ
// ============================================================================

/**
 * @brief Generuje losową liczbę całkowitą z zakresu [min, max]
 */
inline int randomInt(int min, int max) {
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dis(min, max);
    return dis(gen);
}

/**
 * @brief Usypia wątek na losowy czas z zakresu [minMs, maxMs] milisekund
 */
inline void randomSleep(int minMs, int maxMs) {
    int ms = randomInt(minMs, maxMs);
    usleep(ms * 1000);
}

/**
 * @brief Losuje kolor triażu według prawdopodobieństw z wymagań
 * 10% czerwony, 35% żółty, 50% zielony, 5% odesłany
 */
inline TriageColor randomTriageColor() {
    int r = randomInt(1, 100);
    if (r <= 10) return COLOR_RED;       // 10%
    if (r <= 45) return COLOR_YELLOW;    // 35%
    if (r <= 95) return COLOR_GREEN;     // 50%
    return COLOR_SENT_HOME;              // 5%
}

/**
 * @brief Losuje specjalistę dla pacjenta (poza POZ)
 * Dla dzieci (<18 lat) zawsze pediatra
 */
inline DoctorType randomSpecialist(int age) {
    if (age < 18) {
        return DOCTOR_PEDIATRA;
    }
    // Losowy specjalista (bez POZ i pediatry dla dorosłych)
    int r = randomInt(0, 4);
    DoctorType specialists[] = {
        DOCTOR_KARDIOLOG, DOCTOR_NEUROLOG, DOCTOR_OKULISTA,
        DOCTOR_LARYNGOLOG, DOCTOR_CHIRURG
    };
    return specialists[r];
}

/**
 * @brief Losuje wynik leczenia według prawdopodobieństw
 * 85% do domu, 14.5% oddział, 0.5% inna placówka
 */
inline int randomOutcome() {
    int r = randomInt(1, 1000);
    if (r <= 850) return 0;      // 85% do domu
    if (r <= 995) return 1;      // 14.5% oddział
    return 2;                     // 0.5% inna placówka
}

/**
 * @brief Losuje wiek pacjenta (1-90 lat, z większą szansą na dorosłych)
 */
inline int randomAge() {
    int r = randomInt(1, 100);
    if (r <= 20) {
        // 20% - dzieci (1-17 lat)
        return randomInt(1, 17);
    }
    // 80% - dorośli (18-90 lat)
    return randomInt(18, 90);
}

/**
 * @brief Losuje czy pacjent jest VIP (10% szans)
 */
inline bool randomVIP() {
    return randomInt(1, 100) <= 10;
}

// ============================================================================
// FUNKCJE POMOCNICZE - IPC KLUCZE
// ============================================================================

/**
 * @brief Generuje klucz IPC na podstawie identyfikatora
 */
inline key_t getIPCKey(int id) {
    // Używamy /tmp jako ścieżki bazowej
    key_t key = ftok("/tmp", id);
    if (key == -1) {
        // Fallback - stały klucz
        key = 0x50520000 + id;
    }
    return key;
}

#endif // SOR_COMMON_HPP
