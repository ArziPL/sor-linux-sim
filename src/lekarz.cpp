/**
 * @file lekarz.cpp
 * @brief Proces lekarza SOR
 * 
 * Obsługuje dwa typy lekarzy:
 * 1. Lekarz POZ (triaż) — wstępna ocena, przypisanie koloru i specjalisty
 * 2. Lekarze specjaliści — badania, leczenie, decyzja o dalszym postępowaniu
 * 
 * Sygnały:
 * - SIGUSR1: lekarz kończy obecnego pacjenta i idzie na oddział (przerwa)
 * - SIGUSR2: natychmiastowe zakończenie (ewakuacja)
 * - SIGTERM/SIGINT: czyste zamknięcie
 */

#include "sor_common.hpp"

// ============================================================================
// ZMIENNE GLOBALNE
// ============================================================================

static DoctorType g_doctor_type;
static SharedState* g_state = nullptr;
static int g_semid = -1;
static int g_msgid = -1;

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_go_to_ward = 0;
static volatile sig_atomic_t g_treating = 0;  // SIGUSR1 czeka aż lekarz skończy pacjenta

// ============================================================================
// HELPERY
// ============================================================================

/// Prefix "[Dziecko] " dla pacjentów < 18 lat, pusty string dla dorosłych
static const char* childTag(int age) { return (age < 18) ? " [Dziecko]" : ""; }

/// Nazwy wyników leczenia (indeksowane przez outcome: 0=dom, 1=oddział, 2=inna placówka)
static const char* const OUTCOME_NAMES[] = {
    "wypisany do domu",
    "skierowany na oddział szpitalny",
    "skierowany do innej placówki"
};

/// Bezpieczny msgsnd z obsługą EINTR/EIDRM — zwraca true jeśli sukces
static bool safeMsgsnd(int qid, SORMessage& msg, const char* ctx) {
    if (msgsnd(qid, &msg, sizeof(SORMessage) - sizeof(long), 0) == -1) {
        if (errno != EINTR && errno != EIDRM)
            SOR_WARN("%s msgsnd pacjent %d", ctx, msg.patient_id);
        return false;
    }
    return true;
}

// ============================================================================
// INICJALIZACJA IPC
// ============================================================================

static void initIPC() {
    key_t shm_key = getIPCKey(SHM_KEY_ID);
    int shmid = shmget(shm_key, sizeof(SharedState), 0);
    if (shmid == -1) SOR_FATAL("lekarz %s: shmget", getDoctorName(g_doctor_type));

    g_state = (SharedState*)shmat(shmid, nullptr, 0);
    if (g_state == (void*)-1) SOR_FATAL("lekarz %s: shmat", getDoctorName(g_doctor_type));

    key_t sem_key = getIPCKey(SEM_KEY_ID);
    g_semid = semget(sem_key, SEM_COUNT, 0);
    if (g_semid == -1) SOR_FATAL("lekarz %s: semget", getDoctorName(g_doctor_type));

    key_t msg_key = getIPCKey(MSG_KEY_ID);
    g_msgid = msgget(msg_key, 0);
    if (g_msgid == -1) SOR_FATAL("lekarz %s: msgget", getDoctorName(g_doctor_type));
}

// ============================================================================
// OBSŁUGA SYGNAŁÓW
// ============================================================================

static void signalHandler(int sig) {
    if (sig == SIGUSR1)
        g_go_to_ward = 1;
    else
        g_shutdown = 1;  // SIGUSR2, SIGTERM, SIGINT
}

static void setupSignals() {
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // Bez SA_RESTART — chcemy przerwać blokujące msgrcv

    sigaction(SIGUSR1, &sa, nullptr);
    sigaction(SIGUSR2, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

// ============================================================================
// PRZERWA NA ODDZIALE (SIGUSR1)
// ============================================================================

static void goToWard() {
    logMessage(g_state, g_semid, "Lekarz %s idzie na oddział (przerwa)",
              getDoctorName(g_doctor_type));

    semWait(g_semid, SEM_SHM_MUTEX);
    g_state->doctor_on_break[g_doctor_type] = 1;
    semSignal(g_semid, SEM_SHM_MUTEX);

    randomSleep(DOCTOR_BREAK_MIN_MS, DOCTOR_BREAK_MAX_MS);

    semWait(g_semid, SEM_SHM_MUTEX);
    g_state->doctor_on_break[g_doctor_type] = 0;
    semSignal(g_semid, SEM_SHM_MUTEX);

    logMessage(g_state, g_semid, "Lekarz %s wraca z oddziału", getDoctorName(g_doctor_type));
    g_go_to_ward = 0;
}

// ============================================================================
// LEKARZ POZ (TRIAŻ) — nie reaguje na SIGUSR1
// ============================================================================

static void runPOZ() {
    while (!g_shutdown && !g_state->shutdown) {
        SORMessage msg;
        ssize_t ret = msgrcv(g_msgid, &msg, sizeof(SORMessage) - sizeof(long),
                             MSG_PATIENT_TO_TRIAGE, 0);
        if (ret == -1) {
            if (errno == EIDRM || errno == EINVAL) break;
            continue;  // EINTR lub inny — sprawdź warunki pętli
        }

        logMessage(g_state, g_semid, "Pacjent %d%s jest weryfikowany przez lekarza POZ",
                  msg.patient_id, childTag(msg.age));

        randomSleep(TRIAGE_MIN_MS, TRIAGE_MAX_MS);

        TriageColor color = randomTriageColor();
        msg.color = color;

        if (color == COLOR_SENT_HOME) {
            // Pacjent odsyłany do domu bezpośrednio z triażu
            logMessage(g_state, g_semid, "Pacjent %d%s odesłany do domu z triażu",
                      msg.patient_id, childTag(msg.age));

            msg.mtype = MSG_TRIAGE_RESPONSE + msg.patient_id;
            msg.assigned_doctor = DOCTOR_POZ;
            msg.outcome = 0;

            semWait(g_semid, SEM_SHM_MUTEX);
            msg.exit_ticket = g_state->exit_next_ticket++;
            semSignal(g_semid, SEM_SHM_MUTEX);

            safeMsgsnd(g_msgid, msg, "POZ");
        } else {
            // Przypisz specjalistę i kolor
            DoctorType specialist = randomSpecialist(msg.age);
            msg.assigned_doctor = specialist;

            logMessage(g_state, g_semid,
                      "Pacjent %d%s uzyskuje status [%s] — kierowany do lekarza: %s",
                      msg.patient_id, childTag(msg.age), getColorName(color), getDoctorName(specialist));

            logMessage(g_state, g_semid,
                      "Pacjent %d%s czeka na lekarza: %s (kolor: %s)",
                      msg.patient_id, childTag(msg.age), getDoctorName(specialist), getColorName(color));

            // Wyślij do dedykowanej kolejki specjalisty (mtype koduje priorytet koloru)
            msg.mtype = colorToMtype(color);
            safeMsgsnd(g_state->specialist_msgids[specialist], msg, "POZ→specjalista");

            // Wyślij odpowiedź triażu do pacjenta
            msg.mtype = MSG_TRIAGE_RESPONSE + msg.patient_id;
            safeMsgsnd(g_msgid, msg, "POZ→pacjent");
        }
    }
}

// ============================================================================
// LEKARZ SPECJALISTA
// ============================================================================

static void runSpecialist() {
    int sem_idx = getSpecialistSemIndex(g_doctor_type);
    int spec_msgid = g_state->specialist_msgids[g_doctor_type];

    while (!g_shutdown && !g_state->shutdown) {
        SORMessage msg;

        // Blokujący odbiór z priorytetem koloru: -3 → RED(1) przed YELLOW(2) przed GREEN(3)
        ssize_t ret = msgrcv(spec_msgid, &msg, sizeof(SORMessage) - sizeof(long),
                             -SPECIALIST_MTYPE_GREEN, 0);
        if (ret == -1) {
            if (errno == EINTR) {
                if (g_go_to_ward && !g_treating) goToWard();
                continue;
            }
            if (errno == EIDRM || errno == EINVAL) break;
            continue;
        }

        semWait(g_semid, sem_idx);
        g_treating = 1;

        logMessage(g_state, g_semid, "Pacjent %d%s jest badany przez lekarza %s (kolor: %s)",
                  msg.patient_id, childTag(msg.age), getDoctorName(g_doctor_type), getColorName(msg.color));

        randomSleep(TREATMENT_MIN_MS, TREATMENT_MAX_MS);

        int outcome = randomOutcome();
        msg.outcome = outcome;

        const char* outcome_str = (outcome >= 0 && outcome <= 2) ? OUTCOME_NAMES[outcome] : "nieznany";
        logMessage(g_state, g_semid, "Pacjent %d%s — %s",
                  msg.patient_id, childTag(msg.age), outcome_str);

        // Przydziel bilet wyjścia
        semWait(g_semid, SEM_SHM_MUTEX);
        msg.exit_ticket = g_state->exit_next_ticket++;
        semSignal(g_semid, SEM_SHM_MUTEX);

        msg.mtype = MSG_SPECIALIST_RESPONSE + msg.patient_id;
        safeMsgsnd(g_msgid, msg, getDoctorName(g_doctor_type));

        g_treating = 0;
        semSignal(g_semid, sem_idx);

        if (g_go_to_ward) goToWard();
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Użycie: lekarz <typ>\n");
        return EXIT_FAILURE;
    }

    g_doctor_type = (DoctorType)atoi(argv[1]);
    if (g_doctor_type < 0 || g_doctor_type >= DOCTOR_COUNT) {
        fprintf(stderr, "Nieprawidłowy typ lekarza: %d\n", g_doctor_type);
        return EXIT_FAILURE;
    }

    // Opóźnienie startowe (POZ i specjaliści niezależnie)
    if (g_doctor_type == DOCTOR_POZ) {
        if constexpr (STARTUP_DELAY_POZ_MS > 0) msleep(STARTUP_DELAY_POZ_MS);
    } else {
        if constexpr (STARTUP_DELAY_SPECIALIST_MS > 0) msleep(STARTUP_DELAY_SPECIALIST_MS);
    }

    initIPC();
    setupSignals();

    logMessage(g_state, g_semid, "Lekarz %s rozpoczyna pracę", getDoctorName(g_doctor_type));

    if (g_doctor_type == DOCTOR_POZ)
        runPOZ();
    else
        runSpecialist();

    logMessage(g_state, g_semid, "Lekarz %s kończy pracę", getDoctorName(g_doctor_type));
    shmdt(g_state);
    return 0;
}
