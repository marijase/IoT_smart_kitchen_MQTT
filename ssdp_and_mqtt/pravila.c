// Kontroler poziva ove funkcije kada dobije senzorske podatke

#include "pravila.h"
#include <stdio.h>
#include <string.h>

//  pravilo_brzina() Racuna brzinu aspiratora na osnovu izmerene vlage.
int pravilo_brzina(float vlaga, int trenutna_brzina) {
    
    if (trenutna_brzina < 0) trenutna_brzina = 0;
    if (trenutna_brzina == 0 && vlaga >= (PRAG_VLAGE_LOW + HISTEREZA)) {
        return 1;
    }
    if (trenutna_brzina == 1 && vlaga >= (PRAG_VLAGE_MID + HISTEREZA)) {
        return 2;
    }
    if (trenutna_brzina == 2 && vlaga >= (PRAG_VLAGE_HIGH + HISTEREZA)) {
        return 3;
    }

    /* --- LOGIKA ZA PAD VLAGE (Prelazak u NIŽU brzinu) --- */
    if (trenutna_brzina == 3 && vlaga < (PRAG_VLAGE_HIGH - HISTEREZA)) {
        return 2;
    }
    if (trenutna_brzina == 2 && vlaga < (PRAG_VLAGE_MID - HISTEREZA)) {
        return 1;
    }
    if (trenutna_brzina == 1 && vlaga < (PRAG_VLAGE_LOW - HISTEREZA)) {
        return 0;
    }

    /* --- ZONA HISTEREZE ---
     * Ako vlaga šeta između granica (npr. oko 51% ili 72%), 
     * nijedan gornji uslov se neće ispuniti i zadržava se trenutna brzina.
     */
    return trenutna_brzina;
}
/* =========================================================
 * pravilo_zastita_uticnice() Proverava da li su vrednosti struje i snage u dozvoljenim granicama. Ako nije:
 *   - objavljuje novi status (OVERCURRENT / OVERPOWER)
 *   - gasi uticnicu 
 *
 * Ovo je prioritetna akcija 
 * ========================================================= */
void pravilo_zastita_uticnice(struct mosquitto *mosq,
                               float struja, float snaga) {
    const char *novi_status = "OK";
    int         treba_gasiti = 0;

    //negativne vrednosti su greska senzora 
    if (struja < 0.0f) {
        fprintf(stderr, "[PRAVILO] Greska senzora: negativna struja %.2f A\n", struja);
        return;
    }
    if (snaga < 0.0f) {
        fprintf(stderr, "[PRAVILO] Greska senzora: negativna snaga %.2f W\n", snaga);
        return;
    }

    // Provera granica 
    if (struja > MAX_STRUJA) {
        novi_status  = "OVERCURRENT";
        treba_gasiti = 1;
        printf("[PRAVILO] OVERCURRENT: %.2fA > %.1fA\n", struja, MAX_STRUJA);
    } else if (snaga > MAX_SNAGA) {
        novi_status  = "OVERPOWER";
        treba_gasiti = 1;
        printf("[PRAVILO] OVERPOWER: %.2fW > %.1fW\n", snaga, MAX_SNAGA);
    }


    mosquitto_publish(mosq, NULL, TOPIC_UTICNICA_STATUS, (int)strlen(novi_status), novi_status, 1, true);

    // Gasi uticnicu ako je prekoracen limit 
    if (treba_gasiti) {
        mosquitto_publish(mosq, NULL, TOPIC_UTICNICA_UKLJUCENOST, 3, "OFF", 2, true);
        printf("[PRAVILO] Uticnica iskljucena -> %s\n", novi_status);
    }
}
