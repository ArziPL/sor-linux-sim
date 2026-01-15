# Makefile dla projektu SOR
# Kompilator: g++ 8.5.0, Standard: C++17
# System: Debian 11.3

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -I./include
LDFLAGS = 

# Katalogi
SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj
BIN_DIR = .

# Pliki
TARGET = $(BIN_DIR)/sor
SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SOURCES))

# Kolory dla czytelności (opcjonalne)
RED = \033[0;31m
GREEN = \033[0;32m
YELLOW = \033[0;33m
NC = \033[0m # No Color

# Główny cel
all: $(TARGET)
	@echo "$(GREEN)✓ Kompilacja zakończona pomyślnie$(NC)"
	@echo "$(YELLOW)Uruchom: ./sor --help$(NC)"

# Linkowanie
$(TARGET): $(OBJECTS) | $(BIN_DIR)
	@echo "$(YELLOW)Linkowanie: $@$(NC)"
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Kompilacja plików .cpp do .o
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	@echo "$(YELLOW)Kompilacja: $<$(NC)"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Tworzenie katalogów
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Czyszczenie
clean:
	@echo "$(YELLOW)Czyszczenie plików obiektowych...$(NC)"
	rm -rf $(OBJ_DIR)
	@echo "$(GREEN)✓ Pliki obiektowe usunięte$(NC)"

distclean: clean
	@echo "$(YELLOW)Czyszczenie wszystkich plików wygenerowanych...$(NC)"
	rm -f $(TARGET)
	rm -f sor.log
	@echo "$(GREEN)✓ Projekt wyczyszczony$(NC)"

# Pomoc
help:
	@echo "Dostępne cele:"
	@echo "  make         - kompiluje projekt (domyślnie)"
	@echo "  make clean   - usuwa pliki obiektowe"
	@echo "  make distclean - usuwa wszystkie wygenerowane pliki"
	@echo "  make help    - wyświetla tę pomoc"

.PHONY: all clean distclean help
