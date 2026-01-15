#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <unistd.h>
#include "../include/common.h"
#include "../include/ipc.h"
#include "../include/protocol.h"

// ============================================================================
// ZMIENNE GLOBALNE - IDENTYFIKATORY IPC
// ============================================================================

int g_shm_id = -1;
int g_sem_id = -1;
int g_msgq_id = -1;
int g_msgq_triage = -1;
int g_msgq_doctors = -1;

SORState* g_sor_state = nullptr;

// ============================================================================
// FUNKCJE POMOCNICZE DO SEMAFORÓW
// ============================================================================

// Operacja P (wait) - dekrementuj semafor
int sem_P(int sem_id, int sem_num) {
    struct sembuf sb;
    sb.sem_num = sem_num;
    sb.sem_op = -1;        // Dekrementuj
    sb.sem_flg = 0;        // Czekaj jeśli potrzeba
    
    if (semop(sem_id, &sb, 1) == -1) {
        perror("semop(P)");
        return -1;
    }
    return 0;
}

// Operacja V (signal) - inkrementuj semafor
int sem_V(int sem_id, int sem_num) {
    struct sembuf sb;
    sb.sem_num = sem_num;
    sb.sem_op = 1;         // Inkrementuj
    sb.sem_flg = 0;
    
    if (semop(sem_id, &sb, 1) == -1) {
        perror("semop(V)");
        return -1;
    }
    return 0;
}

// Inicjalizacja semaforu na wartość
int sem_init(int sem_id, int sem_num, int value) {
    union semun {
        int val;
        struct semid_ds *buf;
        ushort *array;
    } arg;
    
    arg.val = value;
    
    if (semctl(sem_id, sem_num, SETVAL, arg) == -1) {
        perror("semctl(SETVAL)");
        return -1;
    }
    return 0;
}

// ============================================================================
// FUNKCJE POMOCNICZE - RING BUFFER
// ============================================================================

// Wstawienie pacjenta na POCZĄTEK kolejki (dla VIP)
int queue_enqueue_front(RegistrationQueue* queue, const PatientInQueue& patient) {
    if (queue->count >= queue->max_size) {
        fprintf(stderr, "Błąd: kolejka rejestracji pełna\n");
        return -1;
    }
    
    // Przesuń front wstecz (kolistnie)
    queue->front = (queue->front - 1 + queue->max_size) % queue->max_size;
    queue->patients[queue->front] = patient;
    queue->count++;
    
    return 0;
}

// Wstawienie pacjenta na KONIEC kolejki (dla zwykłych)
int queue_enqueue_back(RegistrationQueue* queue, const PatientInQueue& patient) {
    if (queue->count >= queue->max_size) {
        fprintf(stderr, "Błąd: kolejka rejestracji pełna\n");
        return -1;
    }
    
    queue->patients[queue->rear] = patient;
    queue->rear = (queue->rear + 1) % queue->max_size;
    queue->count++;
    
    return 0;
}

// Pobranie pacjenta z przodu kolejki
int queue_dequeue(RegistrationQueue* queue, PatientInQueue& patient) {
    if (queue->count <= 0) {
        fprintf(stderr, "Błąd: kolejka rejestracji pusta\n");
        return -1;
    }
    
    patient = queue->patients[queue->front];
    queue->front = (queue->front + 1) % queue->max_size;
    queue->count--;
    
    return 0;
}

// ============================================================================
// GŁÓWNE FUNKCJE IPC
// ============================================================================

// Tworzenie zasobów IPC
int ipc_create(const Config& config) {
    printf("IPC: tworzenie zasobów System V...\n");
    
    // Klucz do zasobów IPC - używamy ftok
    key_t key = ftok(".", 'S');
    if (key == -1) {
        perror("ftok");
        return -1;
    }
    printf("IPC: klucz = %d\n", key);
    
    // ========== PAMIĘĆ DZIELONA ==========
    printf("IPC: tworzenie pamięci dzielonej...\n");
    g_shm_id = shmget(key, sizeof(SORState), IPC_CREAT | IPC_EXCL | 0600);
    if (g_shm_id == -1) {
        // Może już istnieje - spróbuj usunąć
        if (errno == EEXIST) {
            printf("IPC: pamięć dzielona już istnieje, usuwam...\n");
            int old_shm_id = shmget(key, sizeof(SORState), 0600);
            if (old_shm_id != -1) {
                shmctl(old_shm_id, IPC_RMID, nullptr);
            }
            // Spróbuj ponownie
            g_shm_id = shmget(key, sizeof(SORState), IPC_CREAT | IPC_EXCL | 0600);
            if (g_shm_id == -1) {
                perror("shmget (retry)");
                return -1;
            }
        } else {
            perror("shmget");
            return -1;
        }
    }
    printf("IPC: SHM ID = %d\n", g_shm_id);
    
    // Attach do pamięci dzielonej
    g_sor_state = (SORState*)shmat(g_shm_id, nullptr, 0);
    if (g_sor_state == (SORState*)-1) {
        perror("shmat");
        return -1;
    }
    printf("IPC: SHM attached\n");
    
    // Inicjalizacja stanu SOR
    memset(g_sor_state, 0, sizeof(SORState));
    g_sor_state->inside_count = 0;
    g_sor_state->total_patients_processed = 0;
    g_sor_state->window2_open = 0;
    g_sor_state->window2_pid = 0;
    g_sor_state->sim_start_time = time(nullptr);
    
    // Inicjalizacja kolejki rejestracji
    g_sor_state->reg_queue.front = 0;
    g_sor_state->reg_queue.rear = 0;
    g_sor_state->reg_queue.count = 0;
    g_sor_state->reg_queue.max_size = config.N;
    
    // Inicjalizacja dostępności lekarzy (wszyscy dostępni)
    for (int i = 0; i < 6; i++) {
        g_sor_state->doctors_available[i] = 1;
    }
    
    // ========== SEMAFORY ==========
    printf("IPC: tworzenie semaforów...\n");
    g_sem_id = semget(key, NUM_SEMAPHORES, IPC_CREAT | IPC_EXCL | 0600);
    if (g_sem_id == -1) {
        if (errno == EEXIST) {
            printf("IPC: semafory już istnieją, usuwam...\n");
            int old_sem_id = semget(key, NUM_SEMAPHORES, 0600);
            if (old_sem_id != -1) {
                semctl(old_sem_id, 0, IPC_RMID);
            }
            g_sem_id = semget(key, NUM_SEMAPHORES, IPC_CREAT | IPC_EXCL | 0600);
            if (g_sem_id == -1) {
                perror("semget (retry)");
                return -1;
            }
        } else {
            perror("semget");
            return -1;
        }
    }
    printf("IPC: SEM ID = %d\n", g_sem_id);
    
    // Inicjalizacja semaforów
    if (sem_init(g_sem_id, SEM_WAITROOM, config.N) == -1) return -1;          // N miejsc w poczekalni
    if (sem_init(g_sem_id, SEM_REG_MUTEX, 1) == -1) return -1;               // Mutex = 1
    if (sem_init(g_sem_id, SEM_REG_ITEMS, 0) == -1) return -1;               // Licznik pacjentów = 0
    if (sem_init(g_sem_id, SEM_REG_SLOTS, config.N) == -1) return -1;        // Słoty w kolejce = N
    if (sem_init(g_sem_id, SEM_STATE_MUTEX, 1) == -1) return -1;             // Mutex = 1
    if (sem_init(g_sem_id, SEM_DOCTORS_MUTEX, 1) == -1) return -1;           // Mutex = 1
    printf("IPC: semafory zainicjalizowane\n");
    
    // ========== KOLEJKI KOMUNIKATÓW ==========
    printf("IPC: tworzenie kolejek komunikatów...\n");
    
    // Kolejka dla logów
    g_msgq_id = msgget(key + 1, IPC_CREAT | IPC_EXCL | 0600);
    if (g_msgq_id == -1) {
        if (errno == EEXIST) {
            int old_msgq_id = msgget(key + 1, 0600);
            if (old_msgq_id != -1) {
                msgctl(old_msgq_id, IPC_RMID, nullptr);
            }
            g_msgq_id = msgget(key + 1, IPC_CREAT | IPC_EXCL | 0600);
            if (g_msgq_id == -1) {
                perror("msgget (logs retry)");
                return -1;
            }
        } else {
            perror("msgget (logs)");
            return -1;
        }
    }
    printf("IPC: MSGQ (logs) ID = %d\n", g_msgq_id);
    
    // Kolejka dla triażu
    g_msgq_triage = msgget(key + 2, IPC_CREAT | IPC_EXCL | 0600);
    if (g_msgq_triage == -1) {
        if (errno == EEXIST) {
            int old_msgq_id = msgget(key + 2, 0600);
            if (old_msgq_id != -1) {
                msgctl(old_msgq_id, IPC_RMID, nullptr);
            }
            g_msgq_triage = msgget(key + 2, IPC_CREAT | IPC_EXCL | 0600);
            if (g_msgq_triage == -1) {
                perror("msgget (triage retry)");
                return -1;
            }
        } else {
            perror("msgget (triage)");
            return -1;
        }
    }
    printf("IPC: MSGQ (triage) ID = %d\n", g_msgq_triage);
    
    // Kolejka dla lekarzy
    g_msgq_doctors = msgget(key + 3, IPC_CREAT | IPC_EXCL | 0600);
    if (g_msgq_doctors == -1) {
        if (errno == EEXIST) {
            int old_msgq_id = msgget(key + 3, 0600);
            if (old_msgq_id != -1) {
                msgctl(old_msgq_id, IPC_RMID, nullptr);
            }
            g_msgq_doctors = msgget(key + 3, IPC_CREAT | IPC_EXCL | 0600);
            if (g_msgq_doctors == -1) {
                perror("msgget (doctors retry)");
                return -1;
            }
        } else {
            perror("msgget (doctors)");
            return -1;
        }
    }
    printf("IPC: MSGQ (doctors) ID = %d\n", g_msgq_doctors);
    
    printf("IPC: wszystkie zasoby stworzone pomyślnie\n");
    return 0;
}

// Podłączanie się do zasobów IPC (dla procesów potomnych)
int ipc_attach() {
    key_t key = ftok(".", 'S');
    if (key == -1) {
        perror("ftok");
        return -1;
    }
    
    // Attach do pamięci dzielonej
    g_shm_id = shmget(key, sizeof(SORState), 0600);
    if (g_shm_id == -1) {
        perror("shmget (attach)");
        return -1;
    }
    
    g_sor_state = (SORState*)shmat(g_shm_id, nullptr, 0);
    if (g_sor_state == (SORState*)-1) {
        perror("shmat (attach)");
        return -1;
    }
    
    // Attach do semaforów
    g_sem_id = semget(key, NUM_SEMAPHORES, 0600);
    if (g_sem_id == -1) {
        perror("semget (attach)");
        return -1;
    }
    
    // Attach do kolejek
    g_msgq_id = msgget(key + 1, 0600);
    if (g_msgq_id == -1) {
        perror("msgget logs (attach)");
        return -1;
    }
    
    g_msgq_triage = msgget(key + 2, 0600);
    if (g_msgq_triage == -1) {
        perror("msgget triage (attach)");
        return -1;
    }
    
    g_msgq_doctors = msgget(key + 3, 0600);
    if (g_msgq_doctors == -1) {
        perror("msgget doctors (attach)");
        return -1;
    }
    
    return 0;
}

// Czyszczenie zasobów IPC
int ipc_cleanup() {
    printf("IPC: czyszczenie zasobów...\n");
    
    // Detach z pamięci dzielonej
    if (g_sor_state != nullptr) {
        if (shmdt(g_sor_state) == -1) {
            perror("shmdt");
        }
        g_sor_state = nullptr;
    }
    
    // Usunięcie pamięci dzielonej
    if (g_shm_id != -1) {
        if (shmctl(g_shm_id, IPC_RMID, nullptr) == -1) {
            perror("shmctl(IPC_RMID)");
        }
        g_shm_id = -1;
    }
    
    // Usunięcie semaforów
    if (g_sem_id != -1) {
        if (semctl(g_sem_id, 0, IPC_RMID) == -1) {
            perror("semctl(IPC_RMID)");
        }
        g_sem_id = -1;
    }
    
    // Usunięcie kolejek komunikatów
    if (g_msgq_id != -1) {
        if (msgctl(g_msgq_id, IPC_RMID, nullptr) == -1) {
            perror("msgctl(IPC_RMID) logs");
        }
        g_msgq_id = -1;
    }
    
    if (g_msgq_triage != -1) {
        if (msgctl(g_msgq_triage, IPC_RMID, nullptr) == -1) {
            perror("msgctl(IPC_RMID) triage");
        }
        g_msgq_triage = -1;
    }
    
    if (g_msgq_doctors != -1) {
        if (msgctl(g_msgq_doctors, IPC_RMID, nullptr) == -1) {
            perror("msgctl(IPC_RMID) doctors");
        }
        g_msgq_doctors = -1;
    }
    
    printf("IPC: zasoby wyczyszczone\n");
    return 0;
}

// Detach z pamięci dzielonej
int ipc_detach() {
    if (g_sor_state != nullptr) {
        if (shmdt(g_sor_state) == -1) {
            perror("shmdt");
            return -1;
        }
        g_sor_state = nullptr;
    }
    return 0;
}

