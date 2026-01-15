#ifndef IPC_H
#define IPC_H

#include "config.h"
#include "protocol.h"

// ============================================================================
// ZMIENNE GLOBALNE - IDENTYFIKATORY IPC (dostępne ze wszystkich procesów)
// ============================================================================

extern int g_shm_id;      // ID pamięci dzielonej
extern int g_sem_id;      // ID zestawu semaforów
extern int g_msgq_id;     // ID kolejki komunikatów (logger)
extern int g_msgq_triage; // ID kolejki komunikatów (rejestracja -> triaż)
extern int g_msgq_doctors; // ID kolejki komunikatów (triaż -> lekarze)

extern SORState* g_sor_state;  // Wskaźnik do pamięci dzielonej (stan SOR)

// ============================================================================
// FUNKCJE IPC
// ============================================================================

// Tworzenie zasobów IPC (shmget, semget, msgget)
// Zwraca 0 jeśli OK, -1 jeśli błąd
int ipc_create(const Config& config);

// Podłączanie się do istniejących zasobów IPC (shmat)
// Zwraca 0 jeśli OK, -1 jeśli błąd
int ipc_attach();

// Czyszczenie zasobów IPC - usunięcie (shmctl IPC_RMID, semctl IPC_RMID, msgctl IPC_RMID)
// Zwraca 0 jeśli OK, -1 jeśli błąd
int ipc_cleanup();

// Detach z pamięci dzielonej (shmdt)
// Zwraca 0 jeśli OK, -1 jeśli błąd
int ipc_detach();

// ============================================================================
// FUNKCJE POMOCNICZE - SEMAFORY
// ============================================================================

// Operacja P (wait/lock) na semaforze
// sem_id - ID zestawu semaforów
// sem_num - numer semafora w zestawie
// Zwraca 0 jeśli OK, -1 jeśli błąd
int sem_P(int sem_id, int sem_num);

// Operacja V (signal/unlock) na semaforze
// sem_id - ID zestawu semaforów
// sem_num - numer semafora w zestawie
// Zwraca 0 jeśli OK, -1 jeśli błąd
int sem_V(int sem_id, int sem_num);

// Inicjalizacja semaforu na wartość
// sem_id - ID zestawu semaforów
// sem_num - numer semafora w zestawie
// value - wartość początkowa
// Zwraca 0 jeśli OK, -1 jeśli błąd
int sem_init(int sem_id, int sem_num, int value);

// ============================================================================
// FUNKCJE POMOCNICZE - RING BUFFER (Kolejka Rejestracji)
// ============================================================================

// Wstawienie pacjenta na POCZĄTEK kolejki (dla VIP)
// Zwraca 0 jeśli OK, -1 jeśli błąd (kolejka pełna)
int queue_enqueue_front(RegistrationQueue* queue, const PatientInQueue& patient);

// Wstawienie pacjenta na KONIEC kolejki (dla zwykłych)
// Zwraca 0 jeśli OK, -1 jeśli błąd (kolejka pełna)
int queue_enqueue_back(RegistrationQueue* queue, const PatientInQueue& patient);

// Pobranie pacjenta z przodu kolejki
// Zwraca 0 jeśli OK, -1 jeśli błąd (kolejka pusta)
int queue_dequeue(RegistrationQueue* queue, PatientInQueue& patient);

#endif // IPC_H
