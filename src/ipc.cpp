#include <cstdio>
#include "../include/common.h"
#include "../include/ipc.h"
#include "../include/config.h"

// IPC - tworzenie, podłączanie i czyszczenie zasobów System V

int ipc_create(const Config& config) {
    printf("IPC: tworzenie zasobów...\n");
    
    // TODO PROMPT 3: shmget, semget, msgget z obsługą błędów
    
    return 0;
}

int ipc_attach() {
    printf("IPC: podłączanie do zasobów...\n");
    
    // TODO PROMPT 3: shmat, semafory
    
    return 0;
}

void ipc_cleanup() {
    printf("IPC: czyszczenie zasobów...\n");
    
    // TODO PROMPT 3: shmctl, semctl, msgctl (IPC_RMID)
}

int sem_P(int sem_id, int sem_num) {
    // TODO PROMPT 3: semop (czekaj)
    return 0;
}

int sem_V(int sem_id, int sem_num) {
    // TODO PROMPT 3: semop (sygnalizuj)
    return 0;
}
