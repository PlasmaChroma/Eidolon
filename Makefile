# FirmwareHost: Rack C++ frontend + offline, firmware-free Rust static library.
RACK_DIR ?= ../Rack-SDK
CARGO ?= cargo
PYTHON ?= python3
RUST_TARGET ?=
RUST_BUILD_DIR := $(abspath build/rust)
RUST_TARGET_FLAG := $(if $(RUST_TARGET),--target $(RUST_TARGET),)
RUST_TARGET_SUBDIR := $(if $(RUST_TARGET),$(RUST_TARGET)/,)
RUST_LIB := $(RUST_BUILD_DIR)/$(RUST_TARGET_SUBDIR)release/libfm1_rack.a
RUST_SOURCES := $(wildcard runtime/src/*.rs runtime/Cargo.*) \
 $(wildcard vendor/fm1-emu/crates/*/Cargo.toml vendor/fm1-emu/crates/*/build.rs) \
 $(wildcard vendor/fm1-emu/crates/*/src/*.rs vendor/fm1-emu/crates/*/src/jit/*.rs)

SOURCES += $(wildcard src/*.cpp)
OBJECTS += $(RUST_LIB)
FLAGS += -Iinclude
EXTRA_CXXFLAGS += -std=c++17
# CV guards rely on IEEE NaN/Inf checks remaining meaningful.
EXTRA_FLAGS += -fno-unsafe-math-optimizations
DISTRIBUTABLES += res LICENSE THIRD_PARTY.md README.md docs licenses

include $(RACK_DIR)/plugin.mk

ABI_TEST_LIBS := -lpthread -ldl -lm
ifdef ARCH_WIN
ABI_TEST_LIBS := -luserenv -lws2_32 -lbcrypt -lntdll -ladvapi32 -lwinmm -lavrt
endif
ifdef ARCH_MAC
ABI_TEST_LIBS := -lpthread -lm -liconv -framework Security
endif

ifdef ARCH_LIN
LDFLAGS += -ldl -lpthread -lm
endif
ifdef ARCH_MAC
LDFLAGS += -liconv -framework Security
endif
ifdef ARCH_WIN
LDFLAGS += -luserenv -lws2_32 -lbcrypt -lntdll -ladvapi32 -lwinmm -lavrt
endif

$(RUST_LIB): $(RUST_SOURCES) vendor/fm1-emu/Cargo.toml
	$(CARGO) build --manifest-path runtime/Cargo.toml --release --offline --locked $(RUST_TARGET_FLAG) --target-dir $(RUST_BUILD_DIR)

rust-test:
	$(CARGO) test --manifest-path runtime/Cargo.toml --offline --locked
	$(CARGO) test --manifest-path vendor/fm1-emu/Cargo.toml --workspace --offline --locked

cpp-test:
	@mkdir -p build/tests
	@set -eu; for source in tests/cpp/*.cpp; do \
	 name=$$(basename "$$source" .cpp); \
	 $(CXX) -std=c++17 -Wall -Wextra -Werror -Iinclude -Isrc "$$source" -o "build/tests/$$name"; \
	 "build/tests/$$name"; \
	 done

abi-test: $(RUST_LIB)
	@mkdir -p build/tests
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Iinclude tests/abi_smoke.cpp $(RUST_LIB) $(ABI_TEST_LIBS) -o build/tests/abi_smoke
	build/tests/abi_smoke

rack-smoke: all
	@mkdir -p build/tests
	$(CXX) -std=c++17 -Wall -Wextra -Wno-unused-parameter -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include tests/rack_smoke.cpp -L$(RACK_DIR) -Wl,-rpath,$(abspath $(RACK_DIR)) -lRack -ldl -lpthread -o build/tests/rack_smoke
	LD_LIBRARY_PATH=$(abspath $(RACK_DIR)) build/tests/rack_smoke $(abspath plugin.so)

audit:
	$(PYTHON) tools/audit_source.py

test: rust-test cpp-test abi-test audit

source-dist: audit
	bash tools/package_source.sh

.PHONY: rust-test cpp-test abi-test rack-smoke audit test source-dist
