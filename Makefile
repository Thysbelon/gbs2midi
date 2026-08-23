CC=gcc
CPPC=g++

all: bin/gbs2midi

bin:
	mkdir -p bin

bin/gbs2midi: main.cpp from_gbsplay.cpp to_midi.cpp libsmfc.o libsmfcx.o | bin
	$(CPPC) -I./libsmf/ -static -Wall -Wextra -o $@ $^

libsmfc.o: libsmf/libsmfc.c
	$(CC) -I./libsmf/ -c $^ -o $@ 

libsmfcx.o: libsmf/libsmfcx.c
	$(CC) -I./libsmf/ -c $^ -o $@ 

clean:
	-rm *.o
	-rm gbs2midi
	-rm bin/gbs2midi