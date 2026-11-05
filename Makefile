# Naval Hunter for the GBA: a port of ../GB-NavalHunter.
#   make        build navalhunter.gba (devkitpro/devkitarm Docker image)
#   make run    open it in mGBA
#   make test   host tests of the rules (plain cc)
#   make e2e    drive the real ROM headlessly in libmgba; screenshots in shots/

ROM   = navalhunter.gba
SRCS  = src/main.c src/gfx.c
DEPS  = $(wildcard src/*.h)
RUN  := docker run --rm -u $(shell id -u):$(shell id -g) -v "$(CURDIR)":/work -w /work devkitpro/devkitarm
BIN  := /opt/devkitpro/devkitARM/bin/arm-none-eabi-
TONC := /opt/devkitpro/libtonc
MGBA := $(shell brew --prefix mgba 2>/dev/null)

CFLAGS  = -O2 -mcpu=arm7tdmi -mtune=arm7tdmi -mthumb -mthumb-interwork -Wall -Wextra -Isrc -I$(TONC)/include
LDFLAGS = -specs=gba.specs -L$(TONC)/lib -ltonc

$(ROM): $(SRCS) $(DEPS)
	$(RUN) sh -c '$(BIN)gcc $(CFLAGS) -o navalhunter.elf $(SRCS) $(LDFLAGS) && \
	  $(BIN)objcopy -O binary navalhunter.elf $@ && \
	  $(BIN)nm navalhunter.elf > navalhunter.sym && \
	  /opt/devkitpro/tools/bin/gbafix -t"NAVAL HUNTER" -cNVHT -m01 $@'

run: $(ROM)
	mgba $(ROM)

test:
	cc -std=c99 -O2 -Wall -Wextra -Isrc -o test_sim tests/test_sim.c
	./test_sim

emu: tools/emu.c
	cc -O2 -Wall -I$(MGBA)/include -L$(MGBA)/lib -lmgba -Wl,-rpath,$(MGBA)/lib -o emu tools/emu.c

e2e: $(ROM) emu
	python3 tools/e2e.py

clean:
	rm -f $(ROM) navalhunter.elf navalhunter.sym test_sim emu

.PHONY: run test e2e clean
