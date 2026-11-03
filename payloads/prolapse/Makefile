# Prolapse P1/P2 payload — 14.00 V59 dev path.
#
# PS5 build (requires the payload SDK):
#   PS5_PAYLOAD_SDK=/opt/ps5-payload-dev/sdk make
#   PS5SDK=/opt/ps5-payload-dev/sdk make
#
# Host verification (no SDK, no console):
#   make check

ROOT        := ../..
GEN         := python3 $(ROOT)/tools/gen_prolapse_config.py
LEDGER_HDR  := include/prolapse_config.h

SRCS        := src/main.c src/config.c src/classify.c src/jsonl.c src/sys.c \
               src/transport.c src/p1_race.c src/p2_win.c
HDRS        := include/prolapse.h include/prolapse_config.h

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
CFLAGS      += -Wall -Wextra -Iinclude
LDFLAGS     += -lps5 -lkernel_sys -pthread
ELF         := prolapse_p1p2.elf
all: $(ELF)
$(ELF): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)
else ifdef PS5SDK
CC          := $(PS5SDK)/bin/prospero-clang
CFLAGS      := -target x86_64-sie-ps5 -Os -Wall -Wextra -Iinclude
LDFLAGS     := -target x86_64-sie-ps5 -lps5 -lkernel_sys -pthread
ELF         := prolapse_p1p2.elf
all: $(ELF)
$(ELF): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)
else
CC          ?= cc
HOST        := 1
all:
	@echo "PS5_PAYLOAD_SDK or PS5SDK is not set; run 'make check' for host checks."
	@exit 2
endif

HOST_CC     ?= cc
HOST_CFLAGS := -DPROLAPSE_HOST -D_GNU_SOURCE -Wall -Wextra -Werror -Iinclude
HOST_BIN    := build/prolapse_host
TEST_BIN    := build/test_core

.PHONY: all check gen check-gen host clean

gen:
	$(GEN)

check-gen:
	$(GEN) --check

$(LEDGER_HDR): $(ROOT)/docs/PROLAPSE_OFFSETS.md $(ROOT)/tools/gen_prolapse_config.py
	$(GEN)

$(HOST_BIN): $(SRCS) $(HDRS)
	@mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(SRCS) -pthread

$(TEST_BIN): tests/test_core.c src/classify.c src/jsonl.c src/config.c $(HDRS)
	@mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/test_core.c src/classify.c \
		src/jsonl.c src/config.c

host: $(HOST_BIN) $(TEST_BIN)

check: check-gen host
	./$(TEST_BIN)
	@mkdir -p build/out
	./$(HOST_BIN) --stage p2 --input tests/fixtures/p1_frozen.jsonl \
		--out build/out/p2.jsonl >/dev/null
	python3 tests/check_p2.py build/out/p2.jsonl

clean:
	rm -rf build $(ELF)
