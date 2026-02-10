cmake_minimum_required(VERSION 3.10)
project(sor VERSION 1.0 LANGUAGES CXX)

# Standard C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Flagi kompilacji - ostrzeżenia i debugowanie
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g")

# Wyszukanie biblioteki pthread (wymagana dla wątków)
find_package(Threads REQUIRED)

# Główny program - Dyrektor SOR
add_executable(dyrektor src/main.cpp)
target_link_libraries(dyrektor PRIVATE Threads::Threads)

# Proces rejestracji
add_executable(rejestracja src/rejestracja.cpp)
target_link_libraries(rejestracja PRIVATE Threads::Threads)

# Proces lekarza
add_executable(lekarz src/lekarz.cpp)
target_link_libraries(lekarz PRIVATE Threads::Threads)

# Proces pacjenta
add_executable(pacjent src/pacjent.cpp)
target_link_libraries(pacjent PRIVATE Threads::Threads)

# Generator pacjentów (uruchamiany przez dyrektora via execl)
add_executable(generator src/generator.cpp)
target_link_libraries(generator PRIVATE Threads::Threads)

# Instalacja (opcjonalna)
install(TARGETS dyrektor rejestracja lekarz pacjent generator RUNTIME DESTINATION bin)

# ============================================================================
# TESTY (podłączają się do żywej symulacji)
# ============================================================================

add_executable(test_queues tests/test_queues.cpp)
add_executable(test_semaphores tests/test_semaphores.cpp)
add_executable(test_shared_memory tests/test_shared_memory.cpp)
add_executable(test_doctor_signal tests/test_doctor_signal.cpp)
add_executable(test_startup_cleanup tests/test_startup_cleanup.cpp)
