
CC     = gcc
CFLAGS = -Wall -Wextra -g -I ssdp_and_mqtt
LIBS   = -lmosquitto
SRC    = ssdp_and_mqtt
APP    = app
BIN    = bin

.PHONY: all sensors actuators controller app clean \
        run-senzor-vlage run-senzor-uticnice \
        run-aspirator run-uticnica run-sijalica \
        run-controller run-app run-all help


all: $(BIN) sensors actuators controller app
	@echo ""
	@echo "  Sve komponente kompajlirane. Pokrenite 'make help'."

$(BIN):
	mkdir -p $(BIN)

# ============================================================
#   SENZORI
# ============================================================
sensors: $(BIN)/senzor_vlage $(BIN)/senzor_uticnice

$(BIN)/senzor_vlage: $(SRC)/senzor_vlage.c $(SRC)/pravila.h | $(BIN)
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)
	@echo "  [OK] senzor_vlage"

$(BIN)/senzor_uticnice: $(SRC)/senzor_uticnice.c $(SRC)/pravila.h | $(BIN)
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)
	@echo "  [OK] senzor_uticnice"

# ============================================================
#   AKTUATORI
# ============================================================
actuators: $(BIN)/aktuator_aspirator $(BIN)/aktuator_uticnica $(BIN)/aktuator_sijalica

$(BIN)/aktuator_aspirator: $(SRC)/aktuator_aspirator.c $(SRC)/pravila.h | $(BIN)
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)
	@echo "  [OK] aktuator_aspirator"

$(BIN)/aktuator_uticnica: $(SRC)/aktuator_uticnica.c $(SRC)/pravila.h | $(BIN)
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)
	@echo "  [OK] aktuator_uticnica"

$(BIN)/aktuator_sijalica: $(SRC)/aktuator_sijalica.c $(SRC)/pravila.h | $(BIN)
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)
	@echo "  [OK] aktuator_sijalica"

# ============================================================
#   KONTROLER (linkuje i pravila.c)
# ============================================================
controller: $(BIN)/controller

$(BIN)/controller: $(SRC)/controller.c $(SRC)/pravila.c $(SRC)/pravila.h | $(BIN)
	$(CC) $(CFLAGS) $(SRC)/controller.c $(SRC)/pravila.c -o $@ $(LIBS)
	@echo "  [OK] controller"

# ============================================================
#   APLIKACIJA
# ============================================================
app: $(BIN)/aplikacija

$(BIN)/aplikacija: $(APP)/aplikacija.c | $(BIN)
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)
	@echo "  [OK] aplikacija"

# ============================================================
#   POKRETANJE — svaki proces u novom terminalu
#   Napomena: kontroler se pokrece iz svog direktorijuma
#             zbog config.json (fopen("config.json")).
# ============================================================
run-senzor-vlage: $(BIN)/senzor_vlage
	./$(BIN)/senzor_vlage

run-senzor-uticnice: $(BIN)/senzor_uticnice
	./$(BIN)/senzor_uticnice

run-aspirator: $(BIN)/aktuator_aspirator
	./$(BIN)/aktuator_aspirator

run-uticnica: $(BIN)/aktuator_uticnica
	./$(BIN)/aktuator_uticnica

run-sijalica: $(BIN)/aktuator_sijalica
	./$(BIN)/aktuator_sijalica

run-controller: $(BIN)/controller
	cd $(SRC) && ../$(BIN)/controller

run-app: $(BIN)/aplikacija
	./$(BIN)/aplikacija

# Pokretanje svega odjednom (svi osim aplikacije u pozadini)
run-all: all
	@echo ""
	@echo "  Pokrecemo sve komponente..."
	@echo "  Zaustavite sa: kill \$\$(cat .pids)"
	@echo ""
	./$(BIN)/senzor_vlage      & echo $$! >> .pids
	./$(BIN)/senzor_uticnice   & echo $$! >> .pids
	./$(BIN)/aktuator_aspirator & echo $$! >> .pids
	./$(BIN)/aktuator_uticnica  & echo $$! >> .pids
	./$(BIN)/aktuator_sijalica  & echo $$! >> .pids
	(cd $(SRC) && ../$(BIN)/controller) & echo $$! >> .pids
	@sleep 2
	./$(BIN)/aplikacija
	@rm -f .pids

# ============================================================
#   CISCENJE
# ============================================================
clean:
	rm -rf $(BIN) .pids
	@echo "  Obrisani binarni fajlovi."


help:
	@echo ""
	@echo "  KOMPAJLIRANJE:"
	@echo "    make              — kompajlira sve"
	@echo "    make sensors      — senzori"
	@echo "    make actuators    — aktuatori"
	@echo "    make controller   — kontroler"
	@echo "    make app          — aplikacija"
	@echo ""
	@echo "  POKRETANJE (svaki u posebnom terminalu):"
	@echo "    make run-senzor-vlage    "
	@echo "    make run-senzor-uticnice "
	@echo "    make run-aspirator       "
	@echo "    make run-uticnica        "
	@echo "    make run-sijalica        "
	@echo "    make run-controller      "
	@echo "    make run-app             "
	@echo "    make run-all   — sve odjednom"
	@echo ""
	@echo "  OSTALO:"
	@echo "    make clean — brise bin/"
	@echo "    make help  — ova poruka"
	@echo ""
