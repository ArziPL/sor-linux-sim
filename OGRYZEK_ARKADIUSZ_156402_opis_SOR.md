GitHub: https://github.com/ArziPL/sor-linux-sim
Jeżeli projekt nie może być po Angielsku proszę pisać - wszystko zamienie jak najszybciej się będzie dało

TESTS:
0. Does proccess for Director, Registration, Doctor, Patient are created correctly
1. Does proccess initially do their tasks correctly (creating director, registration windows, doctors, patients)
2. Limit of patients (N) inside, do they come inside, does the rest waits in front of building
3. Does patient from outside are allowed in only when slot is released (by patient going to registration/doctor/home etc., not sure yet how it will work)
4. Does second registration window opens when given K>=N/2 is in place
5. Closure of second registration window when given N/3 is in place
6. Priority tests (kids, VIP) - do they omit queue when going into registration window
7. Triage tests - are the colors assigned correctly?
8. Doctor allowance based on triage - does patients with red goes first
9. Decision about further help - does simulation reflects given percents
10. Signals - does signal1 works correctly AND simulation doesn't braek
11. Signals - does signal2 works correctly


Na początku wymagania, zaraz wkleje swój temat projektu Uwagi ogólne 1. Zasady wyboru tematu projektu 1.1. Tematy można wybrać z puli przykładowych tematów zawartych w tym dokumencie. 1.2. Możliwe jest zgłoszenie własnego tematu. Należy przesłać prowadzącemu propozycje opisu i testów (w formie opisanej w dalszej części dokumentu). Własne tematy muszą być zgłoszone i zaakceptowane w jednoznaczny sposób przez prowadzącego najpóźniej do 30 listopada 2025. W szczególności stwierdzenie: “temat jest w porządku, ale należy zmienić X” nie jest zaakceptowaniem tematu. 1.3. Opisy projektów i testów mogą ulec niewielkim modyfikacjom mającym na celu usunięcie niejasności lub dodanie autorskich rozwiązań. 1.4. W danej grupie laboratoryjnej możliwa jest realizacja danego tematu niezależnie przez maksymalnie jedną osobę – lista grup jest dostępna na kanale Projekty w zespole SYSTEMY OPERACYJNE ewentualnie o skompletowanie listy zbiorczej tematów dla danej grupy i przesłanie jej za pomocą wiadomości w systemie Moodle proszeni są starostowie danej grupy lub osoby wybrane przez daną grupę. W przypadku braku takiej listy decyzję o przyznaniu lub nie danego tematu (z puli dostępnych poniżej) danej osobie podejmie prowadzący do dnia 1 grudnia 2025. 2. Zasady opisu i zgłoszenia danego zadania 2.1. Opis zadań i testów powinien być analogiczny do zamieszczonych w poniższym dokumencie 2.2. Wybrany temat (zarówno ten z dostępnej poniżej puli jak i indywidualny) musi być opisany w pliku typu Markdown z rozszerzeniem „.md”. 2.3. Nazwa pliku z opisem powinna mieć format: „NAZWISKO_IMIĘ_NR ALBUMU_opis_XXX.md”, gdzie XXX oznacza „temat_wybranego_projektu” (np.: KOWALSKA_ANNA_123456_opis_REJS.md). 2.4. Tematy projektów wraz z ich opisem oraz opisem testów należy przesłać w systemie Moodle w zadaniu pt. „Wybór tematu projektu” 2.5. Dla każdego projektu należy stworzyć jego repozytorium (publiczne) w systemie GitHub oraz zamieścić link do niego w przesyłanym opisie. 3. Zasady oddawania projektów 3.1. Ostateczny termin oddania projektu (odesłania na deltę po prezentacji) to 21 stycznia 2026r. Za każdy dzień zwłoki odejmowany jest 1% z oceny końcowej. 3.2. Raport z projektu spakowany wraz ze źródłami (skopiowanymi z GitHub-a) należy przesłać w systemie Moodle (delta.pk.edu.pl) w zadaniu pt. „Projekt” do 21 stycznia 2026r. 3.3. Przed odesłaniem na deltę obowiązkowa prezentacja u prowadzącego zajęcia projektowe. 4. Wymagania w stosunku do projektów oraz raportu 4.1. Wymagania obowiązkowe: a. Konieczne jest udokumentowanie wymaganych przypadków użycia mieszczących się w ramach opisu zadania. b. Wszystkie dane wprowadzane przez użytkownika powinny być sprawdzane (np: maksymalna dopuszczalna dla danego użytkownika liczba procesów do uruchomienia), w razie wpisania niepoprawnych wartości powinna zostać wyświetlona wiadomość informująca użytkownika. c. Dla wszystkich funkcji systemowych zaimplementuj obsługę błędów używając funkcji bibliotecznej perror() i zmiennej errno. 3 d. Do tworzonych struktur (np.: pamięć dzielona, semafory, kolejki komunikatów, …, itp.) ustawić minimalne prawa dostępu, konieczne do wykonania zadania. e. Po zakończeniu zadania wszystkie używane struktury muszą być usunięte chyba, że w zadaniu wyraźnie określono która z nich ma pozostać (np.: segment pamięci dzielonej). f. Program napisany w C/C++. g. Należy unikać rozwiązań scentralizowanych – symulacja działa na procesach - obowiązkowe użycie funkcji fork() i exec(). 5. Zawartość raportu oraz sposób oceny projektu 5.1. Raport ma być treściwy i w miarę krótki. Ma zawierać założenia projektowe kodu, ogólny opis kodu, co udało się zrobić, z czym były problemy, dodane elementy specjalne, zauważone problemy z testami. 5.2. Na końcu raportu muszą się znaleźć opisane linki do istotnych fragmentów kodu (w źródłach na GitHub) który obrazuje wymagane w projekcie użyte konstrukcje (funkcje systemowe) takie jak: a. Tworzenie i obsługa plików (creat(), open(), close(), read(), write(), unlink()); b. Tworzenie procesów (fork(), ecec(), exit(), wait()); c. Tworzenie i obsługa wątków (pthread_create(), pthread_join(), pthread_detach(), pthread_exit(), pthread_mutex_lock(), pthread_mutex_unlock(), pthread_mutex_trylock(), pthread_cond_wait(), pthread_cond_signal(), pthread_cond_broadcast()); d. Obsługa sygnałów (kill(), raise(), signal(), sigaction()); e. Synchronizacja procesów(wątków) (ftok(), semget(), semctl(), semop()); f. Łącza nazwane i nienazwane (mkfifo(), pipe(), dup(), dup2(), popen()); g. Segmenty pamięci dzielonej (ftok(), shmget(), shmat(), shmdt(), shmctl()); h. Kolejki komunikatów (ftok(), msgget(), msgsnd(), msgrcv(), msgctl()); i. Gniazda (socket(), bind(), listen(), accept(), connect(), • Linki mogą zostać stworzone np. tak jak opisano to w: https://help.github.com/en/github/managing-your-work-ongithub/creating-apermanent-link-to-a-code-snippet • Idea linków do kodu zakłada, że osoba oceniająca projekt szybko się po ich zawartości zorientuje jak zgodny z założeniami jest projekt i ułatwi jego ocenę. 5.3. Punktacja: a. 10% - zgodność programu z opisem w temacie zadania • Wygląd/interfejs/ programu może być inny niż w opisie, pod warunkiem, że wszystkie podstawowe funkcjonalności pozostaną zgodne z opisem; • Dodatkowe funkcjonalności można dodawać wedle uznania, pamiętając jednak, że im większy program tym więcej miejsc w których można popełnić błąd. b. 20% - poprawność funkcjonalna • Seria testów zależna od tematu. Każdy zaliczony test daje 1/N*20% punktów, gdzie N to liczba testów przewidzianych dla danego tematu. Celem testów jest sprawdzenie czy w określonych warunkach nie dochodzi do: blokady, zakleszczenia, 4 przekroczenia maksymalnej liczby procesów, …, itp. Testy opisane szczegółowo w raporcie. Minimalna liczba testów 4. c. 20% - poprawne wykorzystanie czterech wybranych z poniższej listy konstrukcji (każda za 5%): • Tworzenie i obsługa procesów lub/i wątków; • Zastosowanie systemowych mechanizmów synchronizacji procesów i/lub wątków – programowanie współbieżne dla procesów/wątków działających asynchronicznie. • Zastosowane co najmniej dwóch różnych mechanizmów komunikacji między procesami (np. kolejka komunikatów do synchronizacji, pamięć dzielona do wymiany danych); • Obsługa sygnałów (co najmniej dla dwóch różnych sygnałów); • Wyjątki/obsługa błędów (m.in. walidacja danych wprowadzanych przez użytkownika) - zdefiniowanie własnej funkcji do zgłoszenia i obsługi wyjątków. • Własne moduły (podział programu na przynajmniej dwa pliki, np. jeden do obsługi logiki, kolejne do obsługi napisanych procedur). d. 10% - wyróżniające elementy, w szczególności: • Wykorzystanie zaawansowanych konstrukcji programistycznych (ponad opisane/wymienione w pkt. 5.2). • Wykorzystanie zaawansowanych algorytmów zapewniających dodatkową funkcjonalność ponad opisane minimum. • Interfejs graficzny (lub synchronizacja i kolorowanie wyjścia terminala) obrazujący działanie symulacji. e. 40% - Czytelność i udokumentowanie kodu (opis procedur, pseudokody kluczowych algorytmów, komentarze) oraz aktywność na GitHub (ilość i systematyka dokonywania komitów – ogólnie pojęte „development activity”). Tematy i opisy projektów Uwagi ogólne do wszystkich tematów: • należy unikać rozwiązań scentralizowanych – realizacja symulacji na procesach; • program napisany w C/C++;


Temat 8 – SOR.
SOR działa przez całą dobę, zapewniając natychmiastową pomoc osobom w stanach nagłego
zagrożenia zdrowia i życia. Działanie SOR-u opiera się na triażu, czyli ocenie stanu pacjentów, który
określa priorytet udzielania pomocy (nie decyduje kolejność zgłoszenia). W poczekalni jest N miejsc
Zasady działania SOR:
• SOR jest czynny całą dobę;
12
• W poczekalni SOR w danej chwili może się znajdować co najwyżej N pacjentów (pozostali,
jeżeli są czekają przed wejściem);
• Dzieci w wieku poniżej 18 lat na SOR przychodzą pod opieką osoby dorosłej;
• Osoby uprawnione VIP (np. honorowy dawca krwi,) do rejestracji podchodzą bez kolejki;
• Każdy pacjent przed wizytą u lekarza musi się udać do rejestracji;
• W przychodni są 2 okienka rejestracji, zawsze działa min. 1 stanowisko;
• Jeżeli w kolejce do rejestracji stoi więcej niż K pacjentów (K>=N/2) otwiera się drugie okienko
rejestracji. Drugie okienko zamyka się jeżeli liczba pacjentów w kolejce do rejestracji jest
mniejsza niż N/3;
Przebieg wizyty na SOR:
A. Rejestracja:
− Pacjent podaje swoje dane i opisuje objawy.
B. Ocena stanu zdrowia (Triaż):
− Lekarz POZ weryfikuje stan pacjenta i przypisuje mu kolor zgodny z pilnością
udzielenia pomocy (na tej podstawie określa się, kto otrzyma pomoc w pierwszej
kolejności):
o czerwony – wskazuje na bezpośrednie zagrożenie zdrowia bądź życia,
wymaga natychmiastowej pomocy – ok. 10% pacjentów;
o żółty – oznacza pilny przypadek, a pacjent powinien zostać jak najszybciej
przyjęty – 35% pacjentów;
o zielony – świadczy o stabilnym przypadku, w którym nie występuje poważny
uszczerbek na zdrowiu czy stan zagrożenia życia, dlatego pacjent może
zostać przyjęty po osobach z kodem czerwonym oraz żółtym – ok. 50%
pacjentów;
− Ok. 5% pacjentów jest odsyłanych do domu bezpośrednio z triażu;
− Lekarz POZ po przypisaniu koloru, kieruje danego pacjenta do lekarza specjalisty:
kardiologa, neurologa, okulisty, laryngologa, chirurga, pediatry;
C. Wstępna diagnostyka i leczenie:
− Lekarz specjalista wykonuje niezbędne badania (wywiad, badanie fizykalne, EKG,
pomiar ciśnienia, …), aby ustabilizować funkcje życiowe pacjenta.
D. Decyzja o dalszym postępowaniu:
− Po wstępnej diagnozie i stabilizacji stanu pacjent może zostać przez lekarza
specjalistę:
• Wypisany do domu – ok. 85% pacjentów.
• Skierowany na dalsze leczenie do odpowiedniego oddziału szpitalnego – ok.
14.5% pacjentów.
• Skierowany do innej, specjalistycznej placówki (np. z uwagi na brak
specjalisty lub brak miejsca na oddziale szpitalnym) – ok. 0,5% pacjentów.
Na polecenie Dyrektora (sygnał 1) dany lekarz specjalista bada bieżącego pacjenta i przerywa pracę
na SOR-rze i udaje się na oddział. Wraca na SOR po określonym losowo czasie.
Na polecenie Dyrektora (sygnał 2) wszyscy pacjenci i lekarze natychmiast opuszczają budynek.
Napisz procedury Dyrektor, Rejestracja, Lekarz i Pacjent symulujące działanie SOR. Raport z
przebiegu symulacji zapisać w pliku (plikach) tekstowym