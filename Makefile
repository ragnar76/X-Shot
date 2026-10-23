# ---------------------------------------------------------------------
# X-Shot - Makefile (nur Accessory)
#
# Baut xshot.acc / xshot.nac fuer mehrere Zielplattformen:
#
#   make           alle MiNT-Varianten (68000, 68030, 68040, 68060, firebee)
#   make 68000     nur 68000 (laeuft auf jedem Atari mit 68k-CPU)
#   make 68030     68030 (TT, Falcon, 030-Beschleuniger)
#   make 68040     68040
#   make 68060     68060
#   make firebee   ColdFire V4e (FireBee, -mcpu=5475)
#   make newtos    NewTOS-Version (dist/newtos/xshot.nac)
#   make info      zeigt, ob die noetigen Bibliotheken je CPU vorhanden sind
#   make release   packt je Architektur & NewTOS ein ZIP in release/ mit Binary, Readme.md und License
#   make clean
#
# Ergebnis: dist/<variante>/xshot.acc bzw. dist/newtos/xshot.nac
# Release:  release/xshot-<version>-<arch>.zip
# ---------------------------------------------------------------------

VERSION  = 1.0
NAME     = xshot
SRC      = src/xshot.c

CROSS    = m68k-atari-mint-
CC       = $(CROSS)gcc

# NewTOS (eigener Cross-Compiler, die Shell expandiert das ~ beim Aufruf)
NEWTOS_CC  = ~/src/NewTOS/sdk/bin/newtos-cc
NEWTOS_OUT = dist/newtos/$(NAME).nac

CFLAGS   = -O2 -fomit-frame-pointer -fno-strict-aliasing -Wall \
           -DXSHOT_VERSION='"$(VERSION)"'
LDFLAGS  = -s
LIBS     = -lgem

VARIANTS = 68000 68030 68040 68060 firebee

FLAGS_68000   = -m68000
FLAGS_68030   = -m68030
FLAGS_68040   = -m68040
FLAGS_68060   = -m68060
FLAGS_firebee = -mcpu=5475

# Zusatzdateien, die in jedes Release-ZIP integriert werden sollen
EXTRA_FILES = Readme.md License

.PHONY: all clean info release newtos $(VARIANTS)

all: $(VARIANTS)

define VARIANT_RULES
$(1): dist/$(1)/$(NAME).acc

dist/$(1)/$(NAME).acc: $(SRC) Makefile
	@mkdir -p dist/$(1)
	$$(CC) $$(CFLAGS) $$(FLAGS_$(1)) $$(LDFLAGS) -o $$@ $$< $$(LIBS)
endef

$(foreach v,$(VARIANTS),$(eval $(call VARIANT_RULES,$(v))))

newtos: $(NEWTOS_OUT)

$(NEWTOS_OUT): $(SRC) Makefile
	@mkdir -p dist/newtos
	$(NEWTOS_CC) -Wall -nac -o $(NEWTOS_OUT) $(SRC)

info:
	@$(CC) --version | head -n 1
	@$(foreach v,$(VARIANTS),echo "$(v): $$($(CC) $(FLAGS_$(v)) -print-file-name=libgem.a)";)
	@echo "(steht nur 'libgem.a' ohne Pfad da, fehlt die Bibliothek fuer diese CPU)"

release: all newtos
	@mkdir -p release
	@for v in $(VARIANTS); do \
		zip_name="$(NAME)-$(VERSION)-$$v.zip"; \
		rm -f "release/$$zip_name"; \
		cp $(EXTRA_FILES) "dist/$$v/"; \
		(cd "dist/$$v" && zip -r "../../release/$$zip_name" $(NAME).acc $(EXTRA_FILES)); \
		rm -f $(addprefix dist/$$v/,$(EXTRA_FILES)); \
	done
	@zip_name="$(NAME)-$(VERSION)-newtos.zip"; \
	rm -f "release/$$zip_name"; \
	cp $(EXTRA_FILES) "dist/newtos/"; \
	(cd "dist/newtos" && zip -r "../../release/$$zip_name" $(NAME).nac $(EXTRA_FILES)); \
	rm -f $(addprefix dist/newtos/,$(EXTRA_FILES))

clean:
	rm -rf dist release
