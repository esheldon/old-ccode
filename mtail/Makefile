CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra

# prefer the wide-char ncurses if pkg-config knows about it
CURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null)
CURSES_LIBS := $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncurses)

EXEC = mtail

PREFIX := /usr/local

default: $(EXEC)

install: $(EXEC)
	mkdir -p $(PREFIX)/bin
	cp $(EXEC) $(PREFIX)/bin/
	chmod a+x $(PREFIX)/bin/$(EXEC)

clean:
	rm -f *.o $(EXEC)

$(EXEC): $(EXEC).c
	$(CC) $(CFLAGS) $(CURSES_CFLAGS) -o $(EXEC) $(EXEC).c $(LDFLAGS) $(CURSES_LIBS)

.PHONY: default install clean
