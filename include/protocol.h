#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstring>

// ============================================================================
// STRUKTURY DANYCH DLA PAMIĘCI DZIELONEJ (SHM)
// ============================================================================

// Maksymalna długość nazwy (dla pacjenta, lekarza, itp.)
#define MAX_NAME_LEN 32

// Struktury dla ring bufferu kolejki rejestracji
// Każdy element w kolejce to pacjent czekający na obsługę
struct PatientInQueue {
    int patient_id;        // ID pacjenta
    pid_t patient_pid;     // PID procesu pacjenta
    int age;               // Wiek pacjenta
    int is_vip;            // 1 jeśli VIP, 0 jeśli zwykły
    int has_guardian;      // 1 jeśli dziecko z opiekunem
    char symptoms[MAX_NAME_LEN];  // Krótki opis objawów
};

// Ring buffer dla kolejki rejestracji
// Pacjenci są przechowywani jako ring buffer
// VIP wstawia się na początek (front), zwykli na koniec (back)
struct RegistrationQueue {
    PatientInQueue patients[1000];  // Max 1000 pacjentów (buf_size max)
    int front;                      // Indeks początku kolejki
    int rear;                       // Indeks końca kolejki
    int count;                      // Liczba pacjentów w kolejce
    int max_size;                   // Maksymalny rozmiar (= buf_size = N)
};

// Główna struktura stanu SOR w pamięci dzielonej
struct SORState {
    // Liczniki
    int inside_count;              // Liczba pacjentów wewnątrz budynku (w poczekalni + u lekarzy)
    int total_patients_processed;  // Łącznie obsłużeni pacjenci
    
    // Status okienek rejestracji
    int window2_open;              // 1 jeśli drugie okienko jest otwarte
    pid_t window2_pid;             // PID procesu okienka 2 (0 jeśli zamknięte)
    
    // Kolejka rejestracji
    RegistrationQueue reg_queue;   // Ring buffer z pacjentami oczekującymi na rejestrację
    
    // Stany lekarzy (czy są dostępni, czy są na oddziale)
    int doctors_available[6];      // 1 jeśli lekarz jest dostępny, 0 jeśli zajęty/na oddziale
    // Indeksy: 0=kardiolog, 1=neurolog, 2=okulista, 3=laryngolog, 4=chirurg, 5=pediatra
    
    // Timestamp symulacji (dla logów)
    time_t sim_start_time;         // Czas startu symulacji
};

// ============================================================================
// INDEKSY SEMAFORÓW (System V Semaphore Set)
// ============================================================================

// Semafory dla synchronizacji dostępu do SHM i kolejek
#define SEM_WAITROOM        0   // Semafor dla limitu N pacjentów w poczekalni (licznikowy)
#define SEM_REG_MUTEX       1   // Mutex dla dostępu do kolejki rejestracji
#define SEM_REG_ITEMS       2   // Licznik pacjentów w kolejce (producer-consumer)
#define SEM_REG_SLOTS       3   // Licznik wolnych miejsc w kolejce
#define SEM_STATE_MUTEX     4   // Mutex dla dostępu do głównego stanu SOR
#define SEM_DOCTORS_MUTEX   5   // Mutex dla dostępu do statusu lekarzy

#define NUM_SEMAPHORES      6   // Łącznie 6 semaforów

// ============================================================================
// TYPY KOMUNIKATÓW (Message Queue)
// ============================================================================

// Maksymalny rozmiar wiadomości
#define MAX_MSG_SIZE 256

// Rozmiar payloadu (msgrcv/msgsnd podają rozmiar BEZ pola mtype)
#define LOG_PAYLOAD_SIZE (sizeof(LogMessage) - sizeof(long))
#define PATIENT_MSG_SIZE  (sizeof(PatientMessage) - sizeof(long))

// Struktura wiadomości do logowania
struct LogMessage {
    long mtype;             // Typ wiadomości (zawsze 1 dla logów)
    char text[MAX_MSG_SIZE]; // Tekst logu
};

// Struktura wiadomości dla pacjenta (rejestracja -> triaż)
struct PatientMessage {
    long mtype;             // Typ wiadomości (1 dla triażu)
    int patient_id;
    pid_t patient_pid;
    int age;
    int is_vip;
    int has_guardian;
    char symptoms[MAX_NAME_LEN];
};

// Struktura wiadomości dla triażu -> lekarze specjaliści
struct TriageMessage {
    long mtype;             // Typ wiadomości (1=czerwony, 2=żółty, 3=zielony)
    int patient_id;
    pid_t patient_pid;
    int age;
    int is_vip;
    int has_guardian;
    char symptoms[MAX_NAME_LEN];
    int specialization;     // 0=kardiolog, 1=neurolog, 2=okulista, 3=laryngolog, 4=chirurg, 5=pediatra
    char triage_color[16];  // "czerwony", "żółty", "zielony"
};

#endif // PROTOCOL_H
