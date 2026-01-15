#ifndef CONFIG_H
#define CONFIG_H

// Struktura konfiguracji symulacji SOR
struct Config {
    int N;              // Liczba miejsc w poczekalni/rozmiar kolejki (domyślnie 20)
    int K;              // Próg otwarcia drugiego okienka (domyślnie ceil(N/2))
    int duration;       // Czas trwania symulacji w sekundach (0 = nieskończoność)
    double speed;       // Mnożnik czasu (domyślnie 2.0)
    unsigned int seed;  // Ziarno generatora losowego
    double interval;    // Średni czas między pacjentami w sekundach (domyślnie 3.0)
};

#endif // CONFIG_H
