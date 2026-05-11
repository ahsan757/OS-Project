# ─────────────────────────────────────────────────────────────────
#  Smart Parking Allocation System  –  Makefile
#  Target OS : Linux
#  Compiler  : GCC
# ─────────────────────────────────────────────────────────────────

CC      := gcc
TARGET  := parking_system
SRCDIR  := src
INCDIR  := include
OBJDIR  := obj

# Source files
SRCS := $(SRCDIR)/main.c      \
        $(SRCDIR)/parking.c   \
        $(SRCDIR)/dashboard.c \
        $(SRCDIR)/watchdog.c  \
        $(SRCDIR)/vehicle.c

OBJS := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))

# Compiler flags
CFLAGS  := -Wall -Wextra -Wpedantic -std=c11 \
           -I$(INCDIR)                        \
           -D_GNU_SOURCE                      \
           -g

# Linker flags  (pthreads + real-time clock)
LDFLAGS := -lpthread -lrt

# ─── Default target ───────────────────────────────────────────────
.PHONY: all
all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "  Build successful → ./$(TARGET)"
	@echo ""

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# ─── Convenience targets ──────────────────────────────────────────
.PHONY: run
run: all
	./$(TARGET)

.PHONY: clean
clean:
	rm -rf $(OBJDIR) $(TARGET) parking.log

.PHONY: valgrind
valgrind: all
	valgrind --leak-check=full --track-origins=yes ./$(TARGET)

.PHONY: help
help:
	@echo "Targets:"
	@echo "  all       – build the project (default)"
	@echo "  run       – build and run"
	@echo "  clean     – remove build artefacts and log"
	@echo "  valgrind  – run under valgrind"
