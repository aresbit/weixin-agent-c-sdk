debug ?= 0

NAME := weixin-agent-c-sdk
BUILD_DIR := build
BIN_DIR := bin
SRC_DIR := src
INCLUDE_DIR := include
EXAMPLE_DIR := examples
TEST_DIR := tests
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include

CC := cc
AR := ar
PKG_CONFIG := pkg-config
INSTALL := install
QRENCODE_LIB_DIR := /usr/lib/x86_64-linux-gnu
QRENCODE_INCLUDE_DIR ?=

CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl)
OPENSSL_LIBS := $(shell $(PKG_CONFIG) --libs openssl)
THREAD_LIBS := -pthread
QRENCODE_CFLAGS := $(shell $(PKG_CONFIG) --cflags libqrencode 2>/dev/null || $(PKG_CONFIG) --cflags qrencode 2>/dev/null)
QRENCODE_LIBS := $(shell $(PKG_CONFIG) --libs libqrencode 2>/dev/null || $(PKG_CONFIG) --libs qrencode 2>/dev/null)
ifeq ($(strip $(QRENCODE_LIBS)),)
ifneq ($(wildcard $(QRENCODE_LIB_DIR)/libqrencode.so.4),)
QRENCODE_LIBS := -L$(QRENCODE_LIB_DIR) -l:libqrencode.so.4
endif
endif
ifneq ($(strip $(QRENCODE_INCLUDE_DIR)),)
QRENCODE_CFLAGS += -I$(QRENCODE_INCLUDE_DIR)
endif
QRENCODE_HEADER_OK := $(shell printf '%s\n' '#include <qrencode.h>' | $(CC) $(QRENCODE_CFLAGS) -E - >/dev/null 2>&1 && echo 1 || echo 0)

WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow
BASE_CFLAGS := -std=c17 -D_POSIX_C_SOURCE=200809L $(WARNINGS) -I$(INCLUDE_DIR) $(CURL_CFLAGS)
ifneq ($(strip $(QRENCODE_LIBS)),)
ifeq ($(QRENCODE_HEADER_OK),1)
BASE_CFLAGS += $(QRENCODE_CFLAGS) -DWXA_HAVE_QRENCODE=1
endif
endif
DEPFLAGS := -MMD -MP

ifeq ($(debug),1)
	CFLAGS := $(BASE_CFLAGS) $(DEPFLAGS) -O0 -g3
else
	CFLAGS := $(BASE_CFLAGS) $(DEPFLAGS) -O2
endif

LDFLAGS := $(CURL_LIBS) $(OPENSSL_LIBS) $(THREAD_LIBS)
ACP_LDFLAGS := $(LDFLAGS) $(QRENCODE_LIBS)

LIB_OBJS := \
	$(BUILD_DIR)/src/sp_shim.o \
	$(BUILD_DIR)/src/weixin_agent_sdk.o \
	$(BUILD_DIR)/src/weixin_acp_bridge.o

EXAMPLE_OBJS := $(BUILD_DIR)/examples/echo_bot.o
ACP_EXAMPLE_OBJS := $(BUILD_DIR)/examples/weixin_acp_c.o
TEST_OBJS := $(BUILD_DIR)/tests/selftest.o $(BUILD_DIR)/tests/acp_bridge_smoke.o
DEPS := $(LIB_OBJS:.o=.d) $(EXAMPLE_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

.PHONY: all clean dirs smoke test install uninstall

all: $(BUILD_DIR)/libweixin_agent_sdk.a $(BIN_DIR)/echo_bot $(BIN_DIR)/weixin_acp_c $(BIN_DIR)/selftest $(BIN_DIR)/acp_bridge_smoke

dirs:
	@mkdir -p $(BUILD_DIR)/src $(BUILD_DIR)/examples $(BUILD_DIR)/tests $(BIN_DIR)

$(BUILD_DIR)/libweixin_agent_sdk.a: dirs $(LIB_OBJS)
	$(AR) rcs $@ $(LIB_OBJS)

$(BIN_DIR)/echo_bot: dirs $(BUILD_DIR)/libweixin_agent_sdk.a $(EXAMPLE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(EXAMPLE_OBJS) $(BUILD_DIR)/libweixin_agent_sdk.a $(LDFLAGS)

$(BIN_DIR)/weixin_acp_c: dirs $(BUILD_DIR)/libweixin_agent_sdk.a $(ACP_EXAMPLE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(ACP_EXAMPLE_OBJS) $(BUILD_DIR)/libweixin_agent_sdk.a $(ACP_LDFLAGS)

$(BIN_DIR)/selftest: dirs $(BUILD_DIR)/libweixin_agent_sdk.a $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(BUILD_DIR)/tests/selftest.o $(BUILD_DIR)/libweixin_agent_sdk.a $(LDFLAGS)

$(BIN_DIR)/acp_bridge_smoke: dirs $(BUILD_DIR)/libweixin_agent_sdk.a $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(BUILD_DIR)/tests/acp_bridge_smoke.o $(BUILD_DIR)/libweixin_agent_sdk.a $(LDFLAGS)

$(BUILD_DIR)/src/%.o: $(SRC_DIR)/%.c $(INCLUDE_DIR)/weixin_agent_sdk.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/examples/%.o: $(EXAMPLE_DIR)/%.c $(INCLUDE_DIR)/weixin_agent_sdk.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/%.o: $(TEST_DIR)/%.c $(INCLUDE_DIR)/weixin_agent_sdk.h
	$(CC) $(CFLAGS) -c $< -o $@

smoke: all
	./$(BIN_DIR)/echo_bot

test: $(BIN_DIR)/selftest $(BIN_DIR)/acp_bridge_smoke
	./$(BIN_DIR)/selftest
	./$(BIN_DIR)/acp_bridge_smoke

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

install: all
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LIBDIR)" "$(DESTDIR)$(INCLUDEDIR)"
	$(INSTALL) -m 0755 "$(BIN_DIR)/weixin_acp_c" "$(DESTDIR)$(BINDIR)/weixin_acp_c"
	$(INSTALL) -m 0755 "$(BIN_DIR)/weixin_acp_c" "$(DESTDIR)$(BINDIR)/weixin"
	$(INSTALL) -m 0755 "$(BIN_DIR)/echo_bot" "$(DESTDIR)$(BINDIR)/echo_bot"
	$(INSTALL) -m 0644 "$(BUILD_DIR)/libweixin_agent_sdk.a" "$(DESTDIR)$(LIBDIR)/libweixin_agent_sdk.a"
	$(INSTALL) -m 0644 "$(INCLUDE_DIR)/weixin_agent_sdk.h" "$(DESTDIR)$(INCLUDEDIR)/weixin_agent_sdk.h"
	$(INSTALL) -m 0644 "$(INCLUDE_DIR)/weixin_acp_bridge.h" "$(DESTDIR)$(INCLUDEDIR)/weixin_acp_bridge.h"

uninstall:
	rm -f \
		"$(DESTDIR)$(BINDIR)/weixin_acp_c" \
		"$(DESTDIR)$(BINDIR)/weixin" \
		"$(DESTDIR)$(BINDIR)/echo_bot" \
		"$(DESTDIR)$(LIBDIR)/libweixin_agent_sdk.a" \
		"$(DESTDIR)$(INCLUDEDIR)/weixin_agent_sdk.h" \
		"$(DESTDIR)$(INCLUDEDIR)/weixin_acp_bridge.h"

-include $(DEPS)
