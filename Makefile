CC = gcc
CFLAGS = -g
TARGET = oss user

OBJS = oss.o user.o

all: oss user

oss: oss.o
	gcc -o oss oss.o -Wall -lm -pthread

	
user: user.o
	gcc -o user user.o -Wall -lm -pthread

.c.o:
	$(CC) $(CFLAGS) -c $<

clean:
	/bin/rm -f *.o $(TARGET)
