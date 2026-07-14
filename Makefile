# MemGuard — build with:  make        (library object + demos + tests)
#                         make test   (build and run the self-tests)
#                         make demos  (run every demo)
#                         make clean

CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -O2 -g
BIN     = bin

DEMOS   = demo_basic demo_leak demo_overflow demo_doublefree demo_fragmentation

all: $(BIN)/test_memguard.exe $(addprefix $(BIN)/,$(addsuffix .exe,$(DEMOS)))

$(BIN):
	mkdir -p $(BIN)

$(BIN)/memguard.o: src/memguard.c src/memguard.h | $(BIN)
	$(CC) $(CFLAGS) -c src/memguard.c -o $@

$(BIN)/test_memguard.exe: tests/test_memguard.c $(BIN)/memguard.o
	$(CC) $(CFLAGS) $^ -o $@

$(BIN)/demo_%.exe: demos/demo_%.c $(BIN)/memguard.o
	$(CC) $(CFLAGS) $^ -o $@

test: $(BIN)/test_memguard.exe
	$(BIN)/test_memguard.exe

demos: all
	@for d in $(DEMOS); do echo; echo "--- $$d ---"; $(BIN)/$$d.exe; done

clean:
	rm -rf $(BIN)

.PHONY: all test demos clean
