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
#include <sys/prctl.h>
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
#include <vector>

// ============================================================================
// STAŁE KONFIGURACYJNE SYMULACJI
// ============================================================================

constexpr int N = 1;                    // Pojemność poczekalni SOR
constexpr int K_OPEN = N / 2;            // Próg otwarcia drugiego okienka (>=10 osób w kolejce)
constexpr int K_CLOSE = N / 3;           // Próg zamknięcia drugiego okienka (<7 osób)

// Klucze IPC - generowane na podstawie ścieżki i identyfikatorów
constexpr int SHM_KEY_ID = 'S';          // Klucz pamięci dzielonej
constexpr int SEM_KEY_ID = 'E';          // Klucz semaforów
constexpr int MSG_KEY_ID = 'M';          // Klucz kolejki komunikatów
constexpr int MSG_GATE_KEY_ID = 'G';     // Klucz kolejki tokenów poczekalni (FIFO gate)
constexpr int MSG_ORDER_GATE_LOG_KEY_ID = 'h';  // Kolejka FIFO kolejności logowania wejścia
constexpr int MSG_ORDER_REG_KEY_ID = 'i';       // Kolejka FIFO kolejności rejestracji
constexpr int MSG_ORDER_TRIAGE_KEY_ID = 'j';    // Kolejka FIFO kolejności triażu
constexpr int MSG_ORDER_EXIT_KEY_ID = 'k';      // Kolejka FIFO kolejności wyjścia

// Czasy operacji w milisekundach
constexpr int PATIENT_GEN_MIN_MS = 0;   // Min czas między generowaniem pacjentów
constexpr int PATIENT_GEN_MAX_MS = 0;   // Max czas między generowaniem pacjentów
constexpr int REGISTRATION_MIN_MS = 0;  // Min czas rejestracji
constexpr int REGISTRATION_MAX_MS = 0; // Max czas rejestracji
constexpr int TRIAGE_MIN_MS = 5000;        // Min czas triażu
constexpr int TRIAGE_MAX_MS = 5000;       // Max czas triażu
constexpr int TREATMENT_MIN_MS = 10000;    // Min czas leczenia u specjalisty
constexpr int TREATMENT_MAX_MS = 10000;    // Max czas leczenia u specjalisty
constexpr int DOCTOR_BREAK_MIN_MS = 5000; // Min czas przerwy lekarza
constexpr int DOCTOR_BREAK_MAX_MS = 5000;// Max czas przerwy lekarza

// FIXED_PROCESS_COUNT — definiowany niżej (po DoctorType i DOCTOR_ENABLED)

// ============================================================================
// PANEL KONFIGURACYJNY — TRYBY I PRAWDOPODOBIEŃSTWA
// ============================================================================

// --- Triaż (POZ): prawdopodobieństwa kolorów w promilach [suma MUSI = 1000] ---
constexpr int TRIAGE_RED_PM     = 333;   // ‰ czerwony (natychmiastowa pomoc) — 10%
constexpr int TRIAGE_YELLOW_PM  = 334;   // ‰ żółty (pilny) — 35%
constexpr int TRIAGE_GREEN_PM   = 333;   // ‰ zielony (stabilny) — 50%
constexpr int TRIAGE_HOME_PM    = 0;    // ‰ odesłany do domu z triażu — 5%
static_assert(TRIAGE_RED_PM + TRIAGE_YELLOW_PM + TRIAGE_GREEN_PM + TRIAGE_HOME_PM == 1000,
              "Suma promili triazu musi wynosic 1000");

// --- Przypisanie specjalisty (dorośli): promile [suma MUSI = 1000] ---
// Dzieci (<18 lat) ZAWSZE trafiają do pediatry, te ‰ dotyczą tylko dorosłych.
// Ustaw 0 = żaden dorosły pacjent nie trafi do tego specjalisty.
constexpr int SPEC_KARDIOLOG_PM   = 0;  // ‰ dorosłych → kardiolog (20%)
constexpr int SPEC_NEUROLOG_PM    = 1000;  // ‰ dorosłych → neurolog (20%)
constexpr int SPEC_OKULISTA_PM    = 0;  // ‰ dorosłych → okulista (20%)
constexpr int SPEC_LARYNGOLOG_PM  = 0;  // ‰ dorosłych → laryngolog (20%)
constexpr int SPEC_CHIRURG_PM     = 0;  // ‰ dorosłych → chirurg (20%)
static_assert(SPEC_KARDIOLOG_PM + SPEC_NEUROLOG_PM + SPEC_OKULISTA_PM +
              SPEC_LARYNGOLOG_PM + SPEC_CHIRURG_PM == 1000,
              "Suma promili specjalistow musi wynosic 1000");

// --- Włączanie/wyłączanie lekarzy (false = nie zostanie uruchomiony) ---
// POZ (indeks 0) powinien być ZAWSZE true!
constexpr bool DOCTOR_ENABLED[] = {
    true,   // [0] POZ (triaż) — ZAWSZE włączony
    true,   // [1] kardiolog
    true,   // [2] neurolog
    true,   // [3] okulista
    true,   // [4] laryngolog
    true,   // [5] chirurg
    true,   // [6] pediatra
};

// --- Wynik leczenia u specjalisty: prawdopodobieństwa [suma MUSI = 1000] ---
// Podajemy w PROMILACH (‰) żeby obsłużyć ułamki procentów (np. 14,5% = 145‰)
constexpr int OUTCOME_HOME_PM   = 0;   // ‰ wypisany do domu (85.0%)
constexpr int OUTCOME_WARD_PM   = 1000;   // ‰ skierowany na oddział (14.5%)
constexpr int OUTCOME_OTHER_PM  = 0;     // ‰ skierowany do innej placówki (0.5%)
static_assert(OUTCOME_HOME_PM + OUTCOME_WARD_PM + OUTCOME_OTHER_PM == 1000,
              "Suma promili wynikow leczenia musi wynosic 1000");

// --- Tryb dzieci ---
enum ChildrenMode { CHILDREN_NORMAL, CHILDREN_ONLY, NO_CHILDREN };
constexpr ChildrenMode CHILDREN_MODE = NO_CHILDREN;
// CHILDREN_NORMAL  — losowy wiek 1-90 (20% dzieci, 80% dorośli)
// CHILDREN_ONLY    — tylko dzieci (wiek 1-17)
// NO_CHILDREN      — tylko dorośli (wiek 18-90)

// --- Tryb VIP ---
enum VipMode { VIP_NORMAL, VIP_ONLY, NO_VIP };
constexpr VipMode VIP_MODE = NO_VIP;
// VIP_NORMAL — 10% szans na VIP
// VIP_ONLY   — każdy pacjent jest VIP
// NO_VIP     — żaden pacjent nie jest VIP

// --- Pre-generacja pacjentów ---
enum PregenMode { NORMAL_GEN, PREGEN_ONLY, PREGEN_THEN_NORMAL };
constexpr PregenMode PREGEN_MODE = PREGEN_ONLY;
constexpr int PREGEN_COUNT = 1;
// NORMAL_GEN         — ignoruje PREGEN_COUNT, normalna generacja z sleep(min,max)
// PREGEN_ONLY        — generuje PREGEN_COUNT pacjentów back-to-back, potem KOŃCZY
// PREGEN_THEN_NORMAL — generuje PREGEN_COUNT back-to-back, potem normalna generacja

// --- Opóźnienia startowe procesów [ms] (0 = brak opóźnienia) ---
constexpr int STARTUP_DELAY_GENERATOR_MS   = 0;   // Generator pacjentów
constexpr int STARTUP_DELAY_REJESTRACJA_MS = 0;     // Rejestracja (okienka)
constexpr int STARTUP_DELAY_POZ_MS         = 0;     // Lekarz POZ (triaż)
constexpr int STARTUP_DELAY_SPECIALIST_MS  = 0;     // Lekarze specjaliści

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

static_assert(sizeof(DOCTOR_ENABLED) / sizeof(DOCTOR_ENABLED[0]) == DOCTOR_COUNT,
              "Tablica DOCTOR_ENABLED musi mieć DOCTOR_COUNT elementów");

/**
 * @brief Zlicza włączonych lekarzy (constexpr, czas kompilacji)
 */
constexpr int countEnabledDoctors() {
    int c = 0;
    for (int i = 0; i < DOCTOR_COUNT; i++) {
        if (DOCTOR_ENABLED[i]) c++;
    }
    return c;
}

// Stałe procesy: dyrektor + generator + rejestracja + włączeni lekarze
constexpr int ENABLED_DOCTOR_COUNT = countEnabledDoctors();
constexpr int FIXED_PROCESS_COUNT = 3 + ENABLED_DOCTOR_COUNT;

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
 * - SEM_REG_QUEUE_MUTEX: mutex dla kolejki rejestracji
 * - SEM_SPECIALIST_*: semafory poszczególnych specjalistów
 * - SEM_SHM_MUTEX: mutex dla dostępu do pamięci dzielonej
 * - SEM_LOG_MUTEX: mutex dla logowania
 */
enum SemIndex {
    SEM_SPECIALIST_KARDIOLOG = 0,
    SEM_SPECIALIST_NEUROLOG,
    SEM_SPECIALIST_OKULISTA,
    SEM_SPECIALIST_LARYNGOLOG,
    SEM_SPECIALIST_CHIRURG,
    SEM_SPECIALIST_PEDIATRA,
    SEM_SHM_MUTEX,           // Mutex pamięci dzielonej
    SEM_LOG_MUTEX,           // Mutex logowania do pliku
    SEM_REG_QUEUE_CHANGED,   // Sygnał zmiany kolejki rejestracji (budzi kontroler)
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
 * @brief Token poczekalni (kolejka FIFO zamiast semafora)
 * Jeden token = jedno wolne miejsce w poczekalni
 */
struct GateToken {
    long mtype;   // Numer biletu (ticket) — unikalne mtype per pacjent
    char data[1]; // Minimalny payload
};
constexpr size_t GATE_TOKEN_SIZE = sizeof(GateToken) - sizeof(long);

/**
 * @brief Typy wiadomości w kolejce komunikatów
 */
enum MessageType {
    MSG_PATIENT_TO_REGISTRATION_VIP = 1,  // VIP - niższy mtype = wyższy priorytet w msgrcv
    MSG_PATIENT_TO_REGISTRATION = 2,      // Pacjent zwykły
    MSG_PATIENT_TO_TRIAGE = 3,            // Pacjent po rejestracji idzie na triaż
    MSG_REGISTRATION_RESPONSE = 10000,    // Odpowiedź rejestracji (bazowy + patient_id)
    MSG_TRIAGE_RESPONSE = 20000,          // Odpowiedź POZ (bazowy + patient_id) - kolor i specjalista
    MSG_SPECIALIST_RESPONSE = 30000,      // Odpowiedź specjalisty (bazowy + patient_id)
};

/**
 * @brief Mtype w kolejkach specjalistów — koduje priorytet koloru triażu
 * 
 * Każdy specjalista ma osobną kolejkę komunikatów.
 * W kolejce specjalisty mtype oznacza kolor triażu:
 *   COLOR_RED=1, COLOR_YELLOW=2, COLOR_GREEN=3
 * 
 * Blokujący msgrcv(-3, 0) automatycznie wybiera najniższy mtype (RED) pierwszy.
 */
constexpr long SPECIALIST_MTYPE_RED    = 1;  // Czerwony — najwyższy priorytet
constexpr long SPECIALIST_MTYPE_YELLOW = 2;  // Żółty
constexpr long SPECIALIST_MTYPE_GREEN  = 3;  // Zielony — najniższy priorytet

/**
 * @brief Konwertuje kolor triażu na mtype do kolejki specjalisty
 */
inline long colorToMtype(TriageColor color) {
    switch (color) {
        case COLOR_RED:    return SPECIALIST_MTYPE_RED;
        case COLOR_YELLOW: return SPECIALIST_MTYPE_YELLOW;
        case COLOR_GREEN:  return SPECIALIST_MTYPE_GREEN;
        default:           return SPECIALIST_MTYPE_GREEN; // Domyślnie najniższy priorytet
    }
}

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
    int triage_ticket;       // Bilet triażowy (przydzielony przez rejestrację)
    int exit_ticket;         // Bilet wyjściowy (przydzielony przez lekarza)
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
    volatile sig_atomic_t shutdown;
    
    // PID dyrektora (głównego procesu)
    pid_t director_pid;
    
    // PID procesu rejestracji
    pid_t registration_pid;
    
    // PIDy lekarzy (do wysyłania sygnałów)
    pid_t doctor_pids[DOCTOR_COUNT];
    
    // Stan okienek rejestracji
    int reg_window_2_open;           // Czy okienko 2 jest otwarte
    int reg_queue_count;             // Liczba osób w kolejce do rejestracji
    
    // Liczniki do statystyk
    int total_patients;              // Całkowita liczba wygenerowanych pacjentów
    int patients_in_sor;             // Osób aktualnie w budynku SOR (dziecko+opiekun = 2)
    
    // Stan lekarzy (czy są na oddziale)
    volatile sig_atomic_t doctor_on_break[DOCTOR_COUNT];
    
    // ID kolejek komunikatów specjalistów (indeks = DoctorType; slot [0]=POZ nieużywany=-1)
    int specialist_msgids[DOCTOR_COUNT];
    
    // ID kolejki tokenów poczekalni (FIFO gate)
    int gate_msgid;

    // Ticket system — gwarantuje ścisłe FIFO wejścia do poczekalni
    int gate_next_ticket;            // Następny bilet do wydania (rosnący)
    int gate_now_serving;            // Następny mtype do wysłania gdy ktoś wychodzi
    
    // Bilety porządkujące (FIFO triaż i wyjście)
    int triage_next_ticket;          // Następny bilet triażowy (przydzielany przez rejestrację)
    int exit_next_ticket;            // Następny bilet wyjściowy (przydzielany przez lekarza)
    
    // Kolejki FIFO porządkujące (blokujące zamiast busy-wait spin-loopów)
    int order_gate_log_msgid;        // Kolejka FIFO kolejności logowania wejścia
    int order_reg_msgid;             // Kolejka FIFO kolejności rejestracji
    int order_triage_msgid;          // Kolejka FIFO kolejności triażu
    int order_exit_msgid;            // Kolejka FIFO kolejności wyjścia

    // Limit jednoczesnych procesów (łącznie ze stałymi; 0 = bez limitu)
    int max_patients;
    
    // Licznik aktywnych PROCESÓW pacjentów (1 proces = 1, niezależnie od wątków/opiekunów)
    int active_patient_count;
    
    // Ścieżka do pliku logu
    char log_file[256];
};

// ============================================================================
// FUNKCJE POMOCNICZE - OBSŁUGA BŁĘDÓW (własna funkcja)
// ============================================================================

/**
 * @brief Poziomy błędów w symulacji SOR
 */
enum SorErrorLevel {
    ERR_FATAL,    // Błąd krytyczny - kończy program
    ERR_WARNING,  // Ostrzeżenie - kontynuuje działanie
    ERR_INFO      // Informacja - bez errno
};

/**
 * @brief Centralna funkcja obsługi błędów symulacji SOR
 * 
 * Zapewnia jednolity format zgłaszania błędów w całym projekcie.
 * Automatycznie dołącza informacje o pliku, linii, funkcji i errno.
 * 
 * @param level   Poziom błędu (FATAL/WARNING/INFO)
 * @param file    Plik źródłowy (__FILE__)
 * @param line    Numer linii (__LINE__)
 * @param func    Nazwa funkcji (__func__)
 * @param format  Format printf wiadomości
 * 
 * ERR_FATAL:   wypisuje "[FATAL] plik:linia (func): msg: errno_desc" + exit(1)
 * ERR_WARNING: wypisuje "[WARN]  plik:linia (func): msg: errno_desc"
 * ERR_INFO:    wypisuje "[INFO]  plik:linia (func): msg" (bez errno)
 */
inline void sorError(SorErrorLevel level, const char* file, int line,
                     const char* func, const char* format, ...) {
    int saved_errno = errno;
    
    const char* level_str;
    switch (level) {
        case ERR_FATAL:   level_str = "FATAL"; break;
        case ERR_WARNING: level_str = "WARN";  break;
        case ERR_INFO:    level_str = "INFO";  break;
        default:          level_str = "???";   break;
    }
    
    // Wyodrębnij nazwę pliku (bez ścieżki)
    const char* basename = file;
    for (const char* p = file; *p; p++) {
        if (*p == '/') basename = p + 1;
    }
    
    fprintf(stderr, "[%s] %s:%d (%s): ", level_str, basename, line, func);
    
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    
    // Dołącz opis errno dla FATAL i WARNING (jeśli errno ustawione)
    if (level != ERR_INFO && saved_errno != 0) {
        fprintf(stderr, ": %s (errno=%d)", strerror(saved_errno), saved_errno);
    }
    
    fprintf(stderr, "\n");
    
    // Użycie perror() dla błędów krytycznych (wymaganie projektowe 4.1c)
    if (level == ERR_FATAL && saved_errno != 0) {
        errno = saved_errno;
        perror("  perror szczegóły");
    }
    
    fflush(stderr);
    
    if (level == ERR_FATAL) {
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Makra do zgłaszania błędów - automatycznie wstawiają plik/linię/funkcję
 * 
 * SOR_FATAL("msg", ...) - błąd krytyczny, kończy program
 * SOR_WARN("msg", ...)  - ostrzeżenie, kontynuuje
 * SOR_INFO("msg", ...)  - informacja, bez errno
 */
#define SOR_FATAL(fmt, ...) sorError(ERR_FATAL,   __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define SOR_WARN(fmt, ...)  sorError(ERR_WARNING,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define SOR_INFO(fmt, ...)  sorError(ERR_INFO,     __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

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
                SOR_WARN("semWait sem_num=%d", sem_num);
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
                SOR_WARN("semSignal sem_num=%d", sem_num);
            }
            return;
        }
    }
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
 * 
 * Plik logu otwierany jest raz per proces (lazy init) przy użyciu niskopoziomowego
 * open(O_WRONLY|O_APPEND|O_CREAT) zamiast fopen/fclose — eliminuje overhead
 * wielokrotnego otwierania/zamykania pliku przy dużej liczbie logów.
 * Zapis odbywa się przez write() (niskopoziomowe I/O, wymaganie projektowe 5.2a).
 * 
 * @param state Wskaźnik do stanu współdzielonego
 * @param semid ID semaforów (do blokady mutex)
 * @param format Format printf
 */
inline void logMessage(SharedState* state, int semid, const char* format, ...) {
    if (!state) return;
    
    // Blokada mutex logowania
    semWait(semid, SEM_LOG_MUTEX);
    
    // Lazy-open: plik logu otwierany raz per proces (open() — niskopoziomowe I/O)
    static int log_fd = -1;
    if (log_fd == -1) {
        log_fd = open(state->log_file, O_WRONLY | O_APPEND | O_CREAT, 0644);
    }
    
    double elapsed = getElapsedTime(state);
    
    // Formatuj wiadomość do bufora
    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "[%7.2fs] ", elapsed);
    
    va_list args;
    va_start(args, format);
    len += vsnprintf(buf + len, sizeof(buf) - len, format, args);
    va_end(args);
    
    if (len < (int)sizeof(buf) - 1) {
        buf[len++] = '\n';
    }
    
    // Zapis do pliku logu (write() — niskopoziomowe I/O)
    if (log_fd != -1) {
        write(log_fd, buf, len);
    }
    
    // Zapis na stdout
    write(STDOUT_FILENO, buf, len);
    
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
 * @brief Usypia wątek na podaną liczbę milisekund (odporne na sygnały)
 * Używa nanosleep z restartem po EINTR — NIE jest busy-wait.
 * Po przerwaniu sygnałem kontynuuje spanie do pełnego czasu.
 */
inline void msleep(int ms) {
    struct timespec req;
    req.tv_sec  = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        // nanosleep zapisuje pozostały czas w req — kontynuuj sen
    }
}

/**
 * @brief Usypia wątek na losowy czas z zakresu [minMs, maxMs] milisekund
 */
inline void randomSleep(int minMs, int maxMs) {
    msleep(randomInt(minMs, maxMs));
}

/**
 * @brief Losuje kolor triażu według konfigurowalnych prawdopodobieństw (promile)
 * Używa stałych TRIAGE_*_PM z panelu konfiguracyjnego
 */
inline TriageColor randomTriageColor() {
    int r = randomInt(1, 1000);
    if (r <= TRIAGE_RED_PM) return COLOR_RED;
    if (r <= TRIAGE_RED_PM + TRIAGE_YELLOW_PM) return COLOR_YELLOW;
    if (r <= TRIAGE_RED_PM + TRIAGE_YELLOW_PM + TRIAGE_GREEN_PM) return COLOR_GREEN;
    return COLOR_SENT_HOME;
}

/**
 * @brief Losuje specjalistę dla pacjenta według konfigurowalnych promili
 * Dzieci (<18 lat) zawsze → pediatra. Dorośli → losowanie wg SPEC_*_PM.
 */
inline DoctorType randomSpecialist(int age) {
    if (age < 18) {
        return DOCTOR_PEDIATRA;
    }
    int r = randomInt(1, 1000);
    int cumulative = 0;
    cumulative += SPEC_KARDIOLOG_PM;
    if (r <= cumulative) return DOCTOR_KARDIOLOG;
    cumulative += SPEC_NEUROLOG_PM;
    if (r <= cumulative) return DOCTOR_NEUROLOG;
    cumulative += SPEC_OKULISTA_PM;
    if (r <= cumulative) return DOCTOR_OKULISTA;
    cumulative += SPEC_LARYNGOLOG_PM;
    if (r <= cumulative) return DOCTOR_LARYNGOLOG;
    return DOCTOR_CHIRURG;
}

/**
 * @brief Losuje wynik leczenia według konfigurowalnych prawdopodobieństw
 * Używa stałych OUTCOME_*_PCT z panelu konfiguracyjnego
 */
inline int randomOutcome() {
    int r = randomInt(1, 1000);
    if (r <= OUTCOME_HOME_PM) return 0;                            // do domu
    if (r <= OUTCOME_HOME_PM + OUTCOME_WARD_PM) return 1;         // oddział
    return 2;                                                       // inna placówka
}

/**
 * @brief Losuje wiek pacjenta — zachowanie zależy od CHILDREN_MODE
 * CHILDREN_NORMAL: 20% dzieci (1-17), 80% dorośli (18-90)
 * CHILDREN_ONLY:   tylko dzieci (1-17)
 * NO_CHILDREN:     tylko dorośli (18-90)
 */
inline int randomAge() {
    if constexpr (CHILDREN_MODE == CHILDREN_ONLY) {
        return randomInt(1, 17);
    } else if constexpr (CHILDREN_MODE == NO_CHILDREN) {
        return randomInt(18, 90);
    } else {
        int r = randomInt(1, 100);
        if (r <= 20) return randomInt(1, 17);  // 20% dzieci
        return randomInt(18, 90);              // 80% dorośli
    }
}

/**
 * @brief Losuje czy pacjent jest VIP — zachowanie zależy od VIP_MODE
 * VIP_NORMAL: 10% szans
 * VIP_ONLY:   zawsze VIP
 * NO_VIP:     nigdy VIP
 */
inline bool randomVIP() {
    if constexpr (VIP_MODE == VIP_ONLY) {
        return true;
    } else if constexpr (VIP_MODE == NO_VIP) {
        return false;
    } else {
        return randomInt(1, 100) <= 10;
    }
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

/**
 * @brief Klucz IPC kolejki tokenów poczekalni
 */
inline key_t getGateQueueKey() {
    return getIPCKey(MSG_GATE_KEY_ID);
}

/**
 * @brief Klucze IPC kolejek porządkujących (FIFO blokujące zamiast busy-wait)
 */
inline key_t getOrderGateLogKey() { return getIPCKey(MSG_ORDER_GATE_LOG_KEY_ID); }
inline key_t getOrderRegKey()     { return getIPCKey(MSG_ORDER_REG_KEY_ID); }
inline key_t getOrderTriageKey()  { return getIPCKey(MSG_ORDER_TRIAGE_KEY_ID); }
inline key_t getOrderExitKey()    { return getIPCKey(MSG_ORDER_EXIT_KEY_ID); }

/**
 * @brief Klucze IPC kolejek specjalistów
 * Każdy specjalista dostaje osobną kolejkę z unikalnym kluczem
 * Klucze: 'a' + (doctor_type - 1), tj. kardiolog='a', neurolog='b', ..., pediatra='f'
 */
inline key_t getSpecialistQueueKey(DoctorType doctor) {
    return getIPCKey('a' + (doctor - 1));
}

#endif // SOR_COMMON_HPP
