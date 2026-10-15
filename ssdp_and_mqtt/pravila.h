#ifndef PRAVILA_H
#define PRAVILA_H

#include <time.h>
#include <mosquitto.h>

//			MQTT TEME 


// Senzori 
#define TOPIC_VLAGA                 "kuhinja/senzor/vlaga"
#define TOPIC_UTICNICA_STRUJA       "kuhinja/uticnica/struja"
#define TOPIC_UTICNICA_SNAGA        "kuhinja/uticnica/snaga"

// Aspirator  
#define TOPIC_ASPIRATOR_UKLJUCENOST "kuhinja/aspirator/ukljucenost"
#define TOPIC_ASPIRATOR_BRZINA      "kuhinja/aspirator/brzina"
#define TOPIC_ASPIRATOR_MOD         "kuhinja/aspirator/mod"
#define TOPIC_ASPIRATOR_STATUS      "kuhinja/aspirator/status"

// Uticnica 
#define TOPIC_UTICNICA_UKLJUCENOST  "kuhinja/uticnica/ukljucenost"
#define TOPIC_UTICNICA_STATUS       "kuhinja/uticnica/status"

// Sijalica  
#define TOPIC_SVETLO_UKLJUCENOST    "kuhinja/svetlo/glavno/ukljucenost"
#define TOPIC_SVETLO_STATUS         "kuhinja/svetlo/glavno/status"

// Aplikacija 
#define TOPIC_APP_GET_STATUS        "kuhinja/app/get_status"
#define TOPIC_APP_STATUS            "kuhinja/app/status"
#define TOPIC_APP_CMD_ASPIRATOR     "kuhinja/app/aspirator"
#define TOPIC_APP_CMD_UTICNICA      "kuhinja/app/uticnica"
#define TOPIC_APP_CMD_SVETLO        "kuhinja/app/svetlo"

// SSDP parametri 
#define SSDP_ADDR       "239.255.255.250"
#define SSDP_PORT       1900
#define AUTH_TOKEN      "FTN_Sifra_2024"
#define MQTT_BROKER     "localhost"
#define MQTT_PORT       1883

//PRAGOVI (ucitavaju se i iz config.json u kontroleru)
#define PRAG_VLAGE_LOW    50.0f   /* %RH — brzina 0->1      */
#define PRAG_VLAGE_MID    70.0f   /* %RH — brzina 1->2      */
#define PRAG_VLAGE_HIGH   85.0f   /* %RH — brzina 2->3      */
#define HISTEREZA          3.0f   /* %RH — zona histereze   */
#define MAX_STRUJA        16.0f   /* A   — ARCH-4.1.1       */
#define MAX_SNAGA       3500.0f   /* W   — ARCH-4.1.1       */

//SSDP registar uredjaja
#define MAX_DEVICES     20
#define DEVICE_TIMEOUT  25   /* sekundi bez NOTIFY -> offline */
#define NOTIFY_INTERVAL 20   /* koliko cesto saljemo alive    */
#define MSEARCH_INTERVAL 60  /* koliko cesto kontroler trazi  */

typedef struct {
    char   usn[128];       /* Jedinstveni serijski broj       */
    char   nt[64];         /* Tip uredjaja                    */
    char   location[128];  /* URL opisa (za debug)            */
    char   status[16];     /* ON / OFF / UNAVAILABLE          */
    time_t last_seen;      /* Vreme poslednjeg NOTIFY:alive   */
    int    active;         /* 1 = online, 0 = offline         */
} Device;

// FUNKCIJE 
// Vraca novu brzinu aspiratora (0-3) na osnovu vlage sa histerezom
int pravilo_brzina(float vlaga, int trenutna_brzina);

// Proverava struju i snagu. Ako je prekoracen limit, objavljuje status i gasi uticnicu 
void pravilo_zastita_uticnice(struct mosquitto *mosq,
                               float struja, float snaga);

#endif // PRAVILA_H 
