
CC = gcc
CFLAGS = -g
BIN_NATIVE = oss
BIN_TARGET = user
OBJ_NATIVE = oss.o
OBJ_TARGET = user.o

.SUFFIXES:
.SUFFIXES: .c .o .h

all: oss usr
$(BIN_NATIVE): $(OBJ_NATIVE)
	$(CC) -o $(BIN_NATIVE) $(OBJ_NATIVE) -lpthread
$(BIN_TARGET): $(OBJ_TARGET)
	$(CC) -o $(BIN_TARGET) $(OBJ_TARGET) -lpthread
$(OBJ_NATIVE): oss.c
	$(CC) $(CFLAGS) -c oss.c sharememory.h
$(OBJ_TARGET): user.c
	$(CC) $(CFLAGS) -c user.c sharememory.h
clean:
	/bin/rm -f *.o $(BIN_NATIVE) $(BIN_TARGET)
