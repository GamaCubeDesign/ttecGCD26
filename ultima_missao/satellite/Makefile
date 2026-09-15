
# Compiladores
CXX = g++
CC = gcc

# Opções de compilação
CXXFLAGS = -Wall -O2 -std=c++17
CFLAGS = -Wall -O2 -std=c11 -D_DEFAULT_SOURCE

# Bibliotecas necessárias
LIBS = -lpigpio -lpthread -lrt -lm

# Nome do executável
TARGET = lora_app

# Fontes
CPP_SRC = main.cpp Integration.cpp Moden.cpp Module.cpp
C_SRC = LoRa.c

# Gera a lista de objetos (.o)
OBJ = $(CPP_SRC:.cpp=.o) $(C_SRC:.c=.o)

# Regra padrão
all: $(TARGET)

# Linkagem final (usa g++ para incluir símbolos C++)
$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# Compilação de arquivos .cpp
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compilação de arquivos .c
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Limpeza
clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean

# Compiladores
#CXX = g++
#CC = gcc

# Opções de compilação
#CXXFLAGS = -Wall -O2 -std=c++17
#CFLAGS = -Wall -O2 -std=c11

# Bibliotecas necessárias
#LIBS = -lpigpio -lpthread -lrt -lm

# Nome do executável
#TARGET = lora_app

# Fontes e objetos
#CPP_SRC = main.cpp Integration.cpp Moden.cpp Module.cpp
#C_SRC = LoRa.c
#OBJ = $(CPP_SRC:.cpp=.o) $(C_SRC:.c=.o)

# Regra padrão
#all: $(TARGET)

# Linkagem final
#$(TARGET): $(OBJ)
#	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# Compilação de arquivos .cpp
#%.o: %.cpp
#	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compilação de arquivos .c
#%.o: %.c
#	$(CC) $(CFLAGS) -c $< -o $@

# Limpeza
#clean:
#	rm -f $(OBJ) $(TARGET)

#.PHONY: all clean
