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

### GIT TEST