CC      ?= cc
CFLAGS  := -D_XOPEN_SOURCE=700 -std=c11 -Wall -Wextra -pedantic -g -MMD -MP -I. -Ilibs
LDLIBS  := -lpthread

ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
AGENT_SRCS := $(sort $(wildcard agent/*.c))
TOOL_SRCS  := $(sort $(wildcard tools/*.c))
UI_SRCS    := $(sort $(wildcard ui/*.c))

SRCS := main.c config.c message.c util.c http.c \
        $(AGENT_SRCS) $(TOOL_SRCS) $(UI_SRCS)

BUILD     := build
OBJS      := $(SRCS:%.c=$(BUILD)/%.o) $(BUILD)/cJSON.o
DEPS      := $(SRCS:%.c=$(BUILD)/%.d) $(BUILD)/cJSON.d
TARGET    := $(BUILD)/c-agent
ASAN_TGT  := $(BUILD)/c-agent-asan

.PHONY: all clean clean-objs \
        test test-a test-b test-c \
        asan test-asan \
        ref test-ref

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/cJSON.o: libs/cJSON.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# Build the binary with AddressSanitizer + UBSan, no tests run.
# Use it for manual repl sessions against a real (or mock) LLM.
asan: CFLAGS += $(ASAN_FLAGS)
asan: clean-objs $(ASAN_TGT)

$(ASAN_TGT): $(OBJS)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -o $@ $^ $(LDLIBS)

test: all
	python3 tests/run_tests.py --bin ./$(TARGET)

test-a test-b test-c: all
	python3 tests/run_tests.py --bin ./$(TARGET) --phase $(@:test-%=%)

test-asan: asan
	python3 tests/run_tests.py --bin ./$(ASAN_TGT)

clean-objs:
	rm -rf $(BUILD)

clean:
	rm -rf $(BUILD)
	$(MAKE) -C ref clean 2>/dev/null || true

-include $(DEPS)
