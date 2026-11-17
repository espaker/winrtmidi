CC      ?= x86_64-w64-mingw32-gcc
MINGW   := x86_64-w64-mingw32-gcc
CFLAGS  := -O2 -Wall -Wno-unused-function

all: winrtmidi.dll

winrtmidi.dll: src/winrtmidi.c src/guids.h
	$(MINGW) $(CFLAGS) -shared -o $@ src/winrtmidi.c -lwinmm -lole32 -static-libgcc

tests: tests/test.exe tests/test2.exe tests/fakedev

tests/%.exe: tests/%.c src/guids.h
	$(MINGW) -O1 -Isrc -Wno-incompatible-pointer-types -o $@ $< -static-libgcc

tests/fakedev: tests/fakedev.c
	gcc -O2 -o $@ $< -lasound

clean:
	rm -f winrtmidi.dll tests/*.exe tests/fakedev

.PHONY: all tests clean
