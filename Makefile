# ===== Compiler, flags, includes, libs =====
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g -MMD -MP
INCS    := -Iinclude
# POSIX threads + shared memory (on Linux/WSL the -lrt is needed)
LDLIBS  := -pthread -lrt

# ===== Shared sources/objects used by multiple programs =====
COMMON_SRCS := common/net.c src/protocol.c src/floor.c
# If you later add a non-empty common/sync.c, append it here:
# COMMON_SRCS += common/sync.c
COMMON_OBJS := $(COMMON_SRCS:.c=.o)
COMMON_DEPS := $(COMMON_OBJS:.o=.d)

# ===== Final executables expected by the marker =====
ALL_BINS := call internal safety controller car

.PHONY: all clean
all: $(ALL_BINS)

# ===== Link rules (each program links its own .o + common objs) =====
call: src/call.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

internal: src/internal.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

safety: src/safety.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

controller: src/controller.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

car: src/car.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# ===== Generic compile rule (covers src/*.c and common/*.c) =====
# Creates .o and a matching .d (auto dependency) file.
%.o: %.c
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@

# ===== Optional: tiny mock controller for local smoke tests =====
# (safe to keep; doesn’t affect autograder)
mock: mock_controller.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# ===== Housekeeping =====
clean:
	rm -f $(ALL_BINS) $(COMMON_OBJS) src/*.o common/*.o src/*.d common/*.d mock

# Include auto-generated dependency files if present
# (Do not error if they don't exist yet)
-include $(COMMON_DEPS) src/*.d common/*.d
