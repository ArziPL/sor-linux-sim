#ifndef CONFIG_H
#define CONFIG_H

// Struktura konfiguracji symulacji SOR
struct Config {
    int N;              // Liczba miejsc w poczekalni (domyślnie 20)
    int K;              // Próg otwarcia drugiego okienka (domyślnie ceil(N/2))
    int duration;       // Czas trwania symulacji w sekundach (0 = nieskończoność)
    double speed;       // Mnożnik czasu (domyślnie 2.0)
    unsigned int seed;  // Ziarno generatora losowego
    int buf_size;       // Rozmiar kolejki rejestracji = N
};

#endif // CONFIG_H
