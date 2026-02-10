# SOR-LINUX-SIM

### Projekt testowany na:
System: Debian GNU/Linux 13 (trixie), kernel 6.12.63  
Język: C++17  
Kompilator: g++ 14.2.0 (Debian 14.2.0-19)  
Build system: CMake 3.31.6 (min. 3.10)  
Biblioteki: pthreads (POSIX Threads)  
Architektura:	x86_64 (amd64)  
> _Do uruchomienia wymagane nowsze lub kompatybilne_

### Opis działania
Projekt realizuje symulacje Szpitalnego Oddziału Ratunkowego opierającego swoje działanie na niescentralizowanym bez-busy-wait tworzeniu/zarządzaniu procesami oraz mechanizammi IPC. Projekt bazowo tworzy 10 procesów (1 dyrektor, 1 okienko rejestracji, 1 generator pacjentów, 7 lekarzy), korzysta z 1 pamięci dzielonej, z 13 semafor oraz 7 kolejek. Flow projektu polega na wywołaniu dyrektora, który iniciuje wszystkie procesy (fork+exec) i mechanizmy IPC. Generator tworzy pacjentów, którzy:
- pojawiają się przed wejściem przed wejściem
- wchodzą do poczekalni (ograniczone miejscami)
- rejestrują się w okienku
- trafiają do lekarza POZ (ich stan jest weryfikowany)
- trafiają do konkretnego lekarza specjalisty (lekarz leczy/wystawia diagnozę, bierze pacjentów w gorszym stanie)
- symulacja obsługuje dodatkowo pacjentów dzieci (2 sloty w poczekalni), pacjentów VIP (szybsza rejestracja), dwa okienka rejestracji (kiedy w poczekalni za dużo ludzi), sygnały dyrektora (przerwa dla lekarza/ewakuacja całego SOR)

### Kompilacja

1. Pobierz ZIP
2. `cd` do wypakowanego folderu (dalej jako /)
3. `mkdir build && cd build` - stwórz folder /build i wejdź do niego
4. `cmake ..` - zbuduj Makefile za pomocą CMake
5. `make -j$(nproc)` - skompiluj program
6. `cp dyrektor rejestracja lekarz pacjent generator ..` - skopiuj binarki z /build do /
7. `cd ..` - wyjdź do /
8. Program skompilowany :)

### Uruchomianie
`./dyrektor` - uruchamia program z standardowymi parametrami (naturalna symulacja)  
`./dyrektor -t 30` - uruchamia program, który zatrzyma się po 30sek (>0)  
`./dyrektor -p 30` - program pozwoli na stworzenie maks 30 procesów (przynajmniej >11)  
`./dyrektor -g 100 200` - program będzie generować pacjentów co 100ms-200ms (L<R)  

### W trakcie działania
Klawisz: `1-6` - dyrektor wysyła odpowiedniego doktora na oddział (doktor nie bierze przez ten czas udziału w symulacji)  
Klawisz: `7 / q` - ewakuacja SOR, graceful zamknięcie programu

### Testy
###### Test 01 — Start i sprzątanie (test_01_startup_cleanup.sh)
Uruchamia ./dyrektor -t 5, czeka 2s, sprawdza czy procesy SOR żyją (≥10) i czy IPC (shm, sem, msg) istnieją. Po zakończeniu sprawdza czy wszystkie procesy zniknęły i IPC posprzątane  
###### Test 02 - Limit procesów (test_02_process_limit.sh)
Uruchamia z -p 20 -t 10 -g 50 100 (szybkie generowanie, limit 20 procesów). Co sekundę liczy procesy przez 14s. Sprawdza czy limit nigdy nie został przekroczony i czy pacjenci w ogóle się pojawili  
###### Test 03 - Stress test + zakleszczenia (test_03_stress_no_deadlock.sh)
Uruchamia z -g 10 20 (ekstremalnie szybkie generowanie) na 12s. Monitoruje 10s czy system działa. Po upływie czasu sprawdza czy dyrektor sam się zakończył (nie zakleszczyło się) i czy posprzątał procesy + IPC.
###### Test 04 - Zabicie potomnych (test_04_kill_children.sh)
Uruchamia z -t 12, czeka 3s, potem kill -9 na 2 lekarzy + rejestrację. Sprawdza czy dyrektor przeżył mimo utraty dzieci, czy sam się potem zakończył, i czy procesy + IPC zostały posprzątane.
###### Wszystkie testy (run_tests.sh)
Skrypt uruchamiający wszystkie 4 testy integracyjne po kolei. Przed startem buduje projekt (`cmake` + `make`). Między testami sprząta ewentualne pozostałe procesy SOR i zasoby IPC. Na końcu wypisuje podsumowanie: ile testów przeszło, ile nie, z kolorowym oznaczeniem (✓/✗). Użycie: `bash tests/run_tests.sh`



