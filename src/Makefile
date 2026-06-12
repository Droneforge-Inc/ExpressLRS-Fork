# Thin Makefile wrapper for common ExpressLRS / PlatformIO tasks.
# Lets you use plain `make` (and Neovim's :make) without typing the long pio lines.
#
# Examples:
#   make                  # build the Nimbus target (current regulatory below)
#   make nimbus
#   make compdb           # generate root compile_commands.json for clangd
#   make compdb-native
#   make test             # run the native tests (like CI)
#   make validate         # build + compdb + ensure root compile_commands.json exists
#
# Change REGULATORY here or override on command line:
#   make REGULATORY=EU_CE_2400

REGULATORY ?= ISM_2400
NIMBUS_ENV := Unified_ESP32S3_2400_TX_via_UART
NATIVE_ENV := native
COMPDB := compile_commands.json

PIO_PREFIX := PLATFORMIO_BUILD_FLAGS="-DRegulatory_Domain_$(REGULATORY)"

.PHONY: all nimbus build compdb compdb-nimbus compdb-native test test-native validate clean help

all: nimbus

nimbus build:
	$(PIO_PREFIX) pio run -e $(NIMBUS_ENV)

compdb: compdb-nimbus

compdb-nimbus:
	@test ! -L $(COMPDB) || rm -f $(COMPDB)
	$(PIO_PREFIX) pio run -e $(NIMBUS_ENV) -t compiledb
	@test -s $(COMPDB) || (echo "$(COMPDB) was not generated" >&2; exit 1)
	@echo "$(COMPDB) generated for $(NIMBUS_ENV)"

compdb-native:
	@test ! -L $(COMPDB) || rm -f $(COMPDB)
	pio run -e $(NATIVE_ENV) -t compiledb
	@test -s $(COMPDB) || (echo "$(COMPDB) was not generated" >&2; exit 1)
	@echo "$(COMPDB) generated for $(NATIVE_ENV) (fast host DB)"

test test-native:
	$(PIO_PREFIX) pio test -e $(NATIVE_ENV)

# One command you can run before/after edits to be sure it still builds
validate: nimbus compdb-nimbus
	@echo "Validation complete (Nimbus + fresh root compile DB)."

clean:
	pio run -t clean -e $(NIMBUS_ENV) || true
	pio run -t clean -e $(NATIVE_ENV) || true

help:
	@echo "ExpressLRS Makefile targets:"
	@echo "  make                - build Nimbus TX (regulatory=$(REGULATORY))"
	@echo "  make compdb         - generate root compile_commands.json for clangd"
	@echo "  make compdb-native  - fast native DB (good for common code + tests)"
	@echo "  make test           - run pio test -e native"
	@echo "  make validate       - full build + DB update (recommended pre/post change)"
	@echo "  make clean"
	@echo ""
	@echo "In Neovim (with the accompanying .nvim.lua):"
	@echo "  :make               - same as 'make nimbus' (errors go to quickfix)"
	@echo "  <leader>cb          - build in floating terminal"
	@echo "  <leader>cC          - check DB + restart clangd"
	@echo "  :ELRSHelp           - show all local helpers"
