# SOR - Symulacja Szpitalnego Oddziału Ratunkowego

Projekt symulacji SOR (Szpitalnego Oddziału Ratunkowego) dla systemu Debian 11.3 wykorzystujący mechanizmy IPC System V.

## Wymagania systemowe

- **System**: Debian 11.3 (lub kompatybilny Linux)
- **Kompilator**: g++ 8.5.0
- **Standard**: C++17

## Struktura projektu

```
sor-linux-sim/
├── src/              # Pliki źródłowe (.cpp)
│   ├── main.cpp      # Główny dispatcher ról
│   ├── manager.cpp   # Proces zarządzający
│   ├── director.cpp  # Dyrektor (sygnały)
│   ├── registration.cpp  # Okienka rejestracji
│   ├── triage.cpp    # Lekarz POZ (triaż)
│   ├── doctor.cpp    # Lekarze specjaliści
│   ├── patient.cpp   # Pacjenci
│   ├── logger.cpp    # Logger (terminal + plik)
│   ├── ipc.cpp       # Zarządzanie IPC
│   └── util.cpp      # Funkcje pomocnicze
├── include/          # Pliki nagłówkowe (.h)
│   ├── common.h      # Wspólne definicje
│   ├── config.h      # Struktura konfiguracji
│   ├── roles.h       # Deklaracje ról
│   ├── ipc.h         # Deklaracje IPC
│   └── util.h        # Deklaracje funkcji pomocniczych
├── Makefile          # Plik kompilacji
└── README.md         # Ten plik
```

## Kompilacja

```bash
make
```

Po kompilacji powstaje plik wykonywalny `./sor`.

### Czyszczenie

```bash
make clean       # Usuwa pliki obiektowe
make distclean   # Usuwa wszystkie wygenerowane pliki
```

## Uruchomienie

### Podstawowe użycie

```bash
./sor                 # Uruchomienie z domyślnymi parametrami
./sor --help          # Wyświetlenie pomocy
```

### Parametry

- `--N <liczba>` - Liczba miejsc w poczekalni (domyślnie: 20, zakres: 1-1000)
- `--K <liczba>` - Próg otwarcia drugiego okienka (domyślnie: ceil(N/2))
- `--duration <sek>` - Czas trwania symulacji (0 = nieskończoność, domyślnie: 0)
- `--speed <mnożnik>` - Mnożnik czasu (domyślnie: 2.0)
- `--seed <liczba>` - Ziarno generatora losowego (domyślnie: time(NULL))

**Uwaga**: Rozmiar kolejki rejestracji (buf_size) = N

### Przykłady

```bash
./sor --N 30          # 30 miejsc w poczekalni
./sor --duration 60   # Symulacja przez 60 sekund
./sor --N 50 --K 30 --speed 1.5  # Niestandardowa konfiguracja
```

## Architektura

Program wykorzystuje **architekturę wieloprocesową** z komunikacją przez mechanizmy IPC System V:

### Role procesów

1. **Manager** - główny proces, tworzy i zarządza innymi procesami
2. **Director** - wysyła sygnały do lekarzy
3. **Registration** - 2 okienka rejestracji (dynamiczne)
4. **Triage** - lekarz POZ (ocena stanu pacjenta)
5. **Doctor** - 6 lekarzy specjalistów (kardiolog, neurolog, okulista, laryngolog, chirurg, pediatra)
6. **Patient** - pacjenci (procesy generowane dynamicznie)
7. **Logger** - logowanie do terminala i pliku `sor.log`

### Mechanizmy IPC

- **Semafory** - synchronizacja dostępu (limit N miejsc, kolejki)
- **Pamięć dzielona** - stan symulacji, **jedna kolejka rejestracji** z priorytetem VIP (ring buffer)
- **Kolejki komunikatów** - routing pacjentów między etapami

### Przepływ pacjenta

1. Pojawienie się przed SOR
2. Wejście do budynku (limit N miejsc - semafor WAITROOM)
3. Kolejka do rejestracji (**VIP wstawiają się na początek**, zwykli na koniec)
4. Rejestracja (2 okienka, dynamiczne)
5. Triaż (przypisanie koloru: czerwony/żółty/zielony)
6. Lekarz specjalista (priorytet według koloru)
7. Decyzja: dom (85%) / oddział (14.5%) / inna placówka (0.5%)

## Stan implementacji

### ✅ PROMPT 1 - Ukończone

- [x] Struktura katalogów (`src/`, `include/`)
- [x] Pliki nagłówkowe (`.h`)
- [x] Pliki źródłowe (`.cpp`)
- [x] Makefile dla g++ 8.5.0, C++17
- [x] Dispatcher ról w `main.cpp`
- [x] Podstawowy parser argumentów (`--help`)

**Checkpoint PROMPT 1**: ✅ `make` działa, `./sor` uruchamia managera

### 🔄 Kolejne kroki

- [ ] PROMPT 2: Pełny parser argumentów + walidacja
- [ ] PROMPT 3: IPC (shmget, semget, msgget)
- [ ] PROMPT 4: Logger (plik + terminal)
- [ ] PROMPT 5: Spawning procesów (fork+exec)
- [ ] PROMPT 6-15: Implementacja logiki symulacji

## Testy

Program będzie testowany według 12 scenariuszy:

0. Poprawne tworzenie procesów
1. Poprawna inicjalizacja
2. Limit N pacjentów w poczekalni
3. Wejście tylko po zwolnieniu miejsca
4. Otwieranie drugiego okienka (K >= N/2)
5. Zamykanie drugiego okienka (< N/3)
6. Priorytety (VIP, dzieci)
7. Przypisywanie kolorów triażu
8. Kolejność obsługi wg triażu
9. Rozkład decyzji (85/14.5/0.5%)
10. Sygnał 1 (lekarz na oddział)
11. Sygnał 2 (ewakuacja)

## Autor

Arkadiusz Ogryzek, 156402

## Licencja

Projekt edukacyjny - Politechnika Krakowska
