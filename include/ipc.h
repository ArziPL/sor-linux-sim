#ifndef IPC_H
#define IPC_H

#include "config.h"

// Deklaracje funkcji IPC (System V)

// Tworzenie zasobów IPC
int ipc_create(const Config& config);

// Podłączanie się do zasobów IPC
int ipc_attach();

// Czyszczenie zasobów IPC
void ipc_cleanup();

// Pomocnicze funkcje semaforowe
int sem_P(int sem_id, int sem_num);
int sem_V(int sem_id, int sem_num);

#endif // IPC_H
