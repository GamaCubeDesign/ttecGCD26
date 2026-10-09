# Makefile — every command the team runs, one word each.
#
# CMake builds the code and CTest runs the tests; this file only strings
# together the command lines that used to be typed by hand, so that the bench
# on the Raspberry Pi is a list of targets rather than a list of commands to
# get right. Each underlying command still works on its own.
#
#   make help        every target, by section
#
# The same targets run on the development PC and on the Pi. From the PC,
# make pi-<target> copies this tree to the Pi and runs <target> there.
# Settings for one machine, such as PI_HOST, go in local.mk (ignored by git).
# How to use all of it: flight/ttcd/README.md.

SHELL         := /bin/bash
.SHELLFLAGS   := -eu -o pipefail -c
.DEFAULT_GOAL := build
MAKEFLAGS     += --no-print-directory
# Every target is already parallel inside (cmake --build -j, ctest) or drives
# the radios, which cannot be shared: never run two targets at once.
.NOTPARALLEL:

-include local.mk

# Empty on the development PC, 1 on a Raspberry Pi.
ON_PI      ?= $(shell grep -qs 'Raspberry Pi' /proc/device-tree/model && echo 1)

BUILD_DIR  ?= build
ASAN_DIR   ?= build-asan
comma      := ,
# ASan cannot start on the Pi (39-bit address space, see CMakeLists.txt):
# there make asan runs UBSan alone.
SANITIZERS ?= $(if $(ON_PI),undefined,address$(comma)undefined)
BUILD_TYPE ?= RelWithDebInfo
# The Pi Zero 2 W has 512 MB: more than two compilers at once exhausts it.
JOBS       ?= $(if $(ON_PI),2,$(shell nproc))
# Under make -jN, the make that cmake runs shares that job server instead.
CMAKE_JOBS  = $(if $(filter -j%,$(MAKEFLAGS)),,-j$(JOBS))
PYTHON     ?= python3
SUDO       ?= sudo

need-pi = $(if $(ON_PI),,$(error $@ is for the Raspberry Pi — from the PC, make pi-$@ (ON_PI=1 forces it here)))

##@ Build and test — on the PC and on the Pi

.PHONY: build test asan check clean vectors

# Configures on every run, like the one-liner it replaces: that is what
# refreshes TTEC_VERSION (git describe), which ttcd writes into every log.
build: ## Configure and build everything (the default target)
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	cmake --build $(BUILD_DIR) $(CMAKE_JOBS)

test: build ## Run the tests; T=regex runs only those whose name matches
	cd $(BUILD_DIR) && ctest --output-on-failure $(if $(T),-R '$(T)')

# The configuration every merge must pass (AGENTS.md): sanitizers, and any
# warning is an error — zero warnings is a rule of this project.
asan: ## Build with ASan+UBSan (UBSan alone on the Pi) and warnings as errors, then run the tests
	cmake -S . -B $(ASAN_DIR) -DCMAKE_BUILD_TYPE=Debug -DTTEC_SANITIZE=ON \
	      -DTTEC_SANITIZERS='$(SANITIZERS)' -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
	cmake --build $(ASAN_DIR) $(CMAKE_JOBS)
	cd $(ASAN_DIR) && ctest --output-on-failure $(if $(T),-R '$(T)')

check: test asan requirements ## Before any merge: tests, sanitizers, zero warnings, quotations

clean: ## Remove the build trees, with the simulated bench logs inside them
	rm -rf $(BUILD_DIR) $(ASAN_DIR)

vectors: build ## Print the golden wire vectors — only on a deliberate version bump
	$(BUILD_DIR)/tools/gen_vectors/gen_vectors

##@ Analysis

.PHONY: budget budget-check requirements toa

budget: ## Link and data budget tables (tools/analysis/lora_budget.py)
	$(PYTHON) tools/analysis/lora_budget.py

budget-check: ## Check the figures the ADRs quote (make test runs it too)
	$(PYTHON) tools/analysis/lora_budget.py --check

requirements: ## Check that docs/requisitos.md quotes the rules verbatim (pdftotext)
	$(PYTHON) tools/analysis/check_requirements.py

toa: ## Time on air, measured against the model, in a ttcd log: LOG=file
	@test -f '$(LOG)' || { echo "toa: no ttcd log at $(LOG); name one with LOG=<file>" >&2; exit 1; }
	$(PYTHON) tools/analysis/toa_from_log.py $(LOG)

##@ Bench — two RA-02 (RADIO=sx1278) or the simulated link (RADIO=udp)

# On the Pi the bench drives the radios; anywhere else the UDP radio
# simulates the link between two processes on the same machine.
RADIO      ?= $(if $(ON_PI),sx1278,udp)

# A run on the radios is test evidence (HLR-COMM-01, HLR-COMM-03), so its
# logs go where the Design Package collects them, one directory per day. A
# run on the UDP radio says nothing about the hardware and stays in the build
# tree. Every step of one make invocation shares the BENCH_TIME prefix.
BENCH_DATE := $(shell date +%F)
BENCH_TIME := $(shell date +%H%M%S)
BENCH_DIR  ?= $(if $(filter udp,$(RADIO)),$(BUILD_DIR)/bench-udp,docs/vv/bancada/$(BENCH_DATE))
LOG        ?= $(BENCH_DIR)/manual-ttcd.jsonl

# Radio A, the satellite's, is wired as TTCD_CONF says; radio B, the
# ground's, on the second chip select with GPIO16 and GPIO26 (the wiring
# table in flight/ttcd/README.md).
TTCD_CONF  ?= flight/ttcd/ttcd.conf.example
GS_SPI     ?= /dev/spidev0.1
GS_RESET   ?= 16
GS_DIO0    ?= 26
# dBm, both radios. At about 1 m, 20 dBm arrives near the receiver's
# saturation, and the measured error rate would stop representing the link.
POWER      ?= 2
# Listen-before-talk, both radios: header or preamble (step 6, ADR-0007).
LBT        ?= header
SOCK       ?= /tmp/ttec-bench.sock

TTCD_CMD    = $(BUILD_DIR)/flight/ttcd -c $(TTCD_CONF) -o radio=$(RADIO) -o ipc_path=$(SOCK) \
              -o tx_power_dbm=$(POWER) -o lbt=$(LBT) $(TTCD_OPTS)
GS_CMD      = $(BUILD_DIR)/tools/gs_cli/gs_cli --radio $(RADIO) \
              $(if $(filter sx1278,$(RADIO)),--spi $(GS_SPI) --reset $(GS_RESET) \
              --dio0 $(GS_DIO0) --power $(POWER) --lbt $(LBT)) $(GS_OPTS)

# $(call bench-step,COMMANDS[,NAME[,ttcd options[,gs_cli options]]]): one
# step, from tools/bench/COMMANDS.txt, logged as $(BENCH_DIR)/TIME-NAME-*.jsonl.
bench-step  = @TTCD_CMD='$(TTCD_CMD) $(3)' GS_CMD='$(GS_CMD) $(4)' SOCK='$(SOCK)' \
              tools/bench/run_step.sh tools/bench/$(1).txt $(BENCH_DIR)/$(BENCH_TIME)-$(or $(2),$(1))

.PHONY: bench bench-config bench-ping bench-toa bench-per bench-rates bench-lost-ack \
        bench-lbt bench-ttcd bench-gs radio-a-free

bench: bench-ping bench-toa bench-per bench-rates bench-lost-ack bench-lbt ## Steps 1 to 6, in order

bench-config: build ## Step 0: validate ttcd's bench configuration, radio untouched
	$(TTCD_CMD) --check-config
	@echo "bench: each step logs to $(BENCH_DIR)/ instead of that log_path"

bench-ping: build radio-a-free ## Step 1: both radios answer — 10 PINGs, all acked
	$(call bench-step,ping)

bench-toa: build radio-a-free ## Step 2: time on air in each profile, within 5% of the model
	$(call bench-step,toa)
	$(PYTHON) tools/analysis/toa_from_log.py $(BENCH_DIR)/$(BENCH_TIME)-toa-ttcd.jsonl \
	    | tee $(BENCH_DIR)/$(BENCH_TIME)-toa.txt

bench-per: build radio-a-free ## Step 3: packet error rate, 1000 PINGs per profile (200 at SAFE)
	$(call bench-step,per)

bench-rates: build radio-a-free ## Step 4: rate changes between every pair of profiles
	$(call bench-step,rates)

bench-lost-ack: build radio-a-free ## Step 5: the ACK of SET_RATE lost — the revert must recover
	$(call bench-step,lost-ack)

bench-lbt: build radio-a-free ## Step 6: listen-before-talk, header against preamble
	$(call bench-step,lbt,lbt-header,-o lbt=header -o hk_period_ms=1000,--lbt header)
	$(call bench-step,lbt,lbt-preamble,-o lbt=preamble -o hk_period_ms=1000,--lbt preamble)

bench-ttcd: build radio-a-free ## By hand, terminal 1: ttcd on radio A, its log on screen; Ctrl-C stops it
	@mkdir -p $(BENCH_DIR)
	$(TTCD_CMD) -o log_path=- | tee -i -a $(BENCH_DIR)/manual-ttcd.jsonl

bench-gs: build ## By hand, terminal 2: gs_cli on radio B, commands typed in; quit leaves
	@mkdir -p $(BENCH_DIR)
	$(GS_CMD) | tee -i -a $(BENCH_DIR)/manual-ground.jsonl

# One radio, one owner: a ttcd already running — the service, or make
# bench-ttcd in another terminal — holds radio A (or the UDP port).
radio-a-free:
	@if pgrep -x ttcd >/dev/null; then \
	    echo "bench: a ttcd is already running and holds radio A (or the UDP port):" >&2; \
	    pgrep -a -x ttcd | sed 's/^/  /' >&2; \
	    echo "stop it first: make service-stop for the service, Ctrl-C for make bench-ttcd" >&2; \
	    exit 1; \
	fi

##@ ADS-B without an SDR — a recorded or simulated mission through the chain

# sbs_replay plays dump1090; ttcd runs on the UDP radio; gs_cli is the ground,
# streaming at 5 s and sending its clock on contact; tracks_to_ndjson turns
# what arrives into the NDJSON of the ground's estimator (ground/host).
# Real time: a 10-minute mission takes 10 minutes. How to read the result:
# flight/adsbd/README.md.
INPUT      ?=
CHAIN_DIR  ?= $(BUILD_DIR)/adsb-chain

.PHONY: adsb-chain

adsb-chain: build ## A mission through adsbd, ttcd and the ground, no hardware: INPUT=<ndjson|.sbs>
	@test -f '$(INPUT)' || { echo "adsb-chain: name the mission with INPUT=<ndjson or .sbs file>" >&2; exit 1; }
	BUILD_DIR=$(BUILD_DIR) PYTHON=$(PYTHON) tools/bench/adsb_chain.sh '$(INPUT)' \
	    $(CHAIN_DIR)/$(BENCH_DATE)-$(BENCH_TIME)

##@ On the Pi — ttcd and adsbd as systemd services

.PHONY: provision install service-enable service-stop service-status service-logs \
        adsbd-enable adsbd-stop adsbd-status

# Once, on a fresh Pi: the build tools, tmux (a long bench run inside it
# survives a dropped ssh), SPI0 (spidev0.0 and spidev0.1) and the unprivileged
# user the service runs as. Plain sudo, not $(SUDO): the same line runs over
# ssh in pi-provision.
PROVISION   = sudo apt-get update && sudo apt-get install -y build-essential cmake git python3 rsync tmux \
              && sudo raspi-config nonint do_spi 0 \
              && { id -u gama >/dev/null 2>&1 || sudo useradd --system --no-create-home --groups spi,gpio gama; }
# log_path in /etc/gama/ttcd.conf.
SERVICE_LOG ?= /var/lib/gama/ttcd.jsonl

provision: ## Once: build tools, SPI0 enabled, the gama user for the service
	$(need-pi)
	$(PROVISION)

# ttcd.service runs /usr/local/bin/ttcd -c /etc/gama/ttcd.conf, and
# adsbd.service /usr/local/bin/adsbd -c /etc/gama/adsbd.conf. An existing
# file in /etc/gama is kept: it may hold this board's own settings.
install: build ## Install ttcd and adsbd, their systemd units and, if absent, their /etc/gama files
	$(need-pi)
	$(SUDO) install -m 0755 $(BUILD_DIR)/flight/ttcd /usr/local/bin/ttcd
	$(SUDO) install -m 0755 $(BUILD_DIR)/flight/adsbd /usr/local/bin/adsbd
	$(SUDO) install -m 0644 flight/ttcd/ttcd.service /etc/systemd/system/ttcd.service
	$(SUDO) install -m 0644 flight/adsbd/adsbd.service /etc/systemd/system/adsbd.service
	for d in ttcd adsbd; do \
	    if [ -e /etc/gama/$$d.conf ]; then \
	        echo "kept /etc/gama/$$d.conf; compare it with flight/$$d/$$d.conf.example"; \
	    else \
	        $(SUDO) install -D -m 0644 flight/$$d/$$d.conf.example /etc/gama/$$d.conf; \
	    fi; \
	done
	$(SUDO) systemctl daemon-reload
	for d in ttcd adsbd; do \
	    if systemctl is-active --quiet $$d; then $(SUDO) systemctl restart $$d; echo "$$d restarted"; fi; \
	done

service-enable: ## Start ttcd now and at every boot — the flight configuration
	$(need-pi)
	$(SUDO) systemctl enable --now ttcd

service-stop: ## Stop ttcd until the next boot: the bench needs radio A
	$(need-pi)
	$(SUDO) systemctl stop ttcd

service-status: ## systemd's view of ttcd, and the end of its log
	$(need-pi)
	systemctl status ttcd --no-pager || true
	tail -n 20 $(SERVICE_LOG)

service-logs: ## Follow ttcd's log; Ctrl-C leaves
	$(need-pi)
	tail -F $(SERVICE_LOG)

# log_path in /etc/gama/adsbd.conf.
ADSBD_LOG   ?= /var/lib/gama/adsbd.jsonl

adsbd-enable: ## Start adsbd now and at every boot (dump1090-fa must be installed)
	$(need-pi)
	@test -x /usr/bin/dump1090-fa || echo "warning: /usr/bin/dump1090-fa not found; adsbd will keep retrying it" >&2
	$(SUDO) systemctl enable --now adsbd

adsbd-stop: ## Stop adsbd until the next boot: frees the SDR
	$(need-pi)
	$(SUDO) systemctl stop adsbd

adsbd-status: ## systemd's view of adsbd, and the end of its log
	$(need-pi)
	systemctl status adsbd --no-pager || true
	tail -n 20 $(ADSBD_LOG)

##@ From the PC — the same targets, run on the Pi over ssh

# user@host of the Pi — echo 'PI_HOST = pi@raspberrypi.local' > local.mk —
# and where this tree goes there, relative to that user's home.
PI_HOST    ?=
PI_DIR     ?= ttec

need-pi-host = $(if $(PI_HOST),,$(error PI_HOST is not set: echo 'PI_HOST = user@host' > local.mk))

# What pi-sync leaves alone on the Pi: its build trees, the evidence its bench
# runs produced (pi-fetch brings it here), any other log, and its local.mk.
# Everything else becomes identical to this tree — .git included, so that
# git describe names the build in every log ttcd writes on the Pi.
PI_KEEP     = --exclude='/build*/' --exclude='/docs/vv/bancada/' --exclude='/local.mk' \
              --exclude='*.jsonl' --exclude='*.ndjson' --exclude='__pycache__/'
FETCH       = rsync -av --ignore-missing-args $(PI_HOST):$(PI_DIR)/docs/vv/bancada/ docs/vv/bancada/

.PHONY: pi-sync pi-fetch pi-provision

pi-sync: ## Copy this tree to the Pi: only what changed, uncommitted work included
	$(need-pi-host)
	rsync -az --delete $(PI_KEEP) ./ $(PI_HOST):$(PI_DIR)/
	@echo "synced to $(PI_HOST):$(PI_DIR)"

# Variables given on the command line (T=..., LBT=...) go along to the Pi. A
# bench target then brings the Pi's bench logs here, even when a step failed.
pi-%: pi-sync ## Sync, then run the target on the Pi: pi-test, pi-bench, pi-install, ...
	status=0; \
	ssh $$([ -t 0 ] && echo -t) $(PI_HOST) 'make -C $(PI_DIR) $* $(MAKEOVERRIDES)' || status=$$?; \
	$(if $(filter bench%,$*),$(FETCH) || true;) \
	exit $$status

pi-fetch: ## Bring the Pi's bench logs into docs/vv/bancada/
	$(need-pi-host)
	$(FETCH)

pi-provision: ## Once: make provision on a Pi that has only ssh so far
	$(need-pi-host)
	ssh -t $(PI_HOST) '$(PROVISION)'

##@ Help

.PHONY: help

help: ## This list
	@awk 'BEGIN { FS = ":.*## " } \
	     /^##@/ { printf "\n%s\n", substr($$0, 5) } \
	     /^[a-zA-Z0-9_%-]+:.*## / { t = $$1; sub(/%/, "<target>", t); printf "  %-18s %s\n", t, $$2 }' \
	    $(MAKEFILE_LIST)
	@echo
	@echo "Variables (make <target> VAR=value, or in local.mk), as on this machine:"
	@echo "  RADIO=$(RADIO)  POWER=$(POWER)  LBT=$(LBT)  JOBS=$(JOBS)  SANITIZERS=$(SANITIZERS)"
	@echo "  BENCH_DIR=$(BENCH_DIR)"
	@echo "  PI_HOST=$(or $(PI_HOST),(unset))  PI_DIR=$(PI_DIR)"
	@echo "  T=<regex> for test and asan, LOG=<file> for toa, TTCD_OPTS and GS_OPTS for extra options"
	@echo "  INPUT=<ndjson|.sbs> for adsb-chain, which writes under $(CHAIN_DIR)/"
