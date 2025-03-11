# Makefile for rcopy and server (CPE464 remote file copy using UDP)
# Based on Hugh Smith's Makefile template - updated April 2023

CC      = gcc
CFLAGS  = -g -Wall
LIBS    =

OBJS    = networks.o gethostbyname.o pollLib.o safeUtil.o debug.o

# Uncomment the next two lines if you are using the sendtoErr() library
LIBS   += libcpe464.2.21.a -lstdc++ -ldl
CFLAGS += -D__LIBCPE464_ -Wno-unused-but-set-variable

all: rcopy server

rcopy: rcopy.c window.c circular_buffer.c $(OBJS)
	$(CC) $(CFLAGS) -o rcopy rcopy.c window.c circular_buffer.c $(OBJS) $(LIBS)

server: server.c window.c circular_buffer.c $(OBJS)
	$(CC) $(CFLAGS) -o server server.c window.c circular_buffer.c $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

cleano:
	rm -f *.o

clean:
	rm -f rcopy server *.o
