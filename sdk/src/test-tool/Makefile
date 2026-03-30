CC ?= gcc
CFLAGS ?= -O2 -Wall
OUTDIR ?= ../../tools

# Detect Windows (mingw32-make sets OS=Windows_NT)
ifeq ($(OS),Windows_NT)
  EXE = .exe
  RM = del /q
else
  EXE =
  RM = rm -f
endif

all:
	$(CC) $(CFLAGS) -o $(OUTDIR)/test-tool$(EXE) main.c

clean:
	-$(RM) $(OUTDIR)/test-tool$(EXE)
