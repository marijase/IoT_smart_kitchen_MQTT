#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>	// (close)
#include <stdlib.h>	// (exit, malloc)
#include <sys/socket.h>
#include <netinet/in.h> // Strukture za internet adrese (sockaddr_in)
#include <arpa/inet.h>  // Funkcije za konverziju IP adresa (inet_addr)
#include <signal.h>     // Rad sa signalima prekida (SIGINT)
#include <time.h>   
 
/*
#ifndef _IP_MREQ_DEFINED		// Ako struktura za multicast nije definisana:
struct my_ip_mreq {			// Definišemo sopstvenu pomoćnu strukturu
    struct in_addr imr_multiaddr;	// Multicast IP adresa
    struct in_addr imr_interface;	// IP adresa lokalnog interfejsa
};

#define ip_mreq my_ip_mreq		// Mapiramo standardno ime na našu strukturu
#endif
*/
#define SSDP_ADDR "239.255.255.250"	// Standardna IP adresa za SSDP multicast
#define SSDP_PORT 1900			// Standardni UDP port za SSDP

// definisemo identifikatore naseg uređaja 
#define NOTIFY_NT "urn:schemas-upnp-org:device:sensor:1"	// Tip uređaja (Senzor)
#define NOTIFY_USN "uuid:weather-sensor-001::urn:schemas-upnp-org:device:sensor:1" // Jedinstveni serijski broj
#define LOCATION_URL "http://192.168.1.11:5000/desc_2.xml" 	// URL ka XML opisu uređaja
#define AUTH_TOKEN "smart_kitchen_pass"				// Bezbednosni token za filtriranje?

int g_sock = -1;		// Globalna varijabla za deskriptor soketa
struct sockaddr_in g_addr;	// Globalna struktura sa adresom multicast
int boot_id = 0; 		// Identifikator sesije pokretanja uređaja


// Funkcija za slanje NOTIFY (alive) poruka
void send_notify(const char* nts_type) {
    char message[1024];
    snprintf(message, sizeof(message),
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: %s:1900\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: %s\r\n"
        "NT: %s\r\n"
        "NTS: %s\r\n"
        "SERVER: Linux/5.0 UPnP/2.0 Senzor/1.0\r\n" // IZMENJENO: Tip servera[cite: 8]
        "USN: %s\r\n"
        "BOOTID.UPNP.ORG: %d\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "SEARCHPORT.UPNP.ORG: 1900\r\n"
        "X-DEVICE-STATUS: ON\r\n"  	// NOVO: ON/OFF stanje
        "AUTH: %s\r\n"  		// Tvoj filter (Kontrola pristupa) DODANO 
        "\r\n", 
        SSDP_ADDR, LOCATION_URL, NOTIFY_NT, nts_type, NOTIFY_USN, boot_id, AUTH_TOKEN);
    
    // Slanje poruke putem UDP protokola na multicast adresu
    int ret = sendto(g_sock, message, strlen(message), 0, (struct sockaddr*)&g_addr, sizeof(g_addr));
    if (ret < 0) {
        perror("[Senzor] Greška pri slanju NOTIFY");
    }
}

void handle_exit(int sig) {
    if (sig == SIGINT) {
        printf("\n[Senzor] Primljen SIGINT (Ctrl+C). Gasim...\n");
    } else if (sig == SIGHUP) {
        printf("\n[Senzor] Terminal zatvoren (SIGHUP). Gasim...\n");
    }
    
    // poslati byebye pre gašenja
    send_notify("ssdp:byebye"); 
    // dodati kasnije: printf("\n[Senzor] Uredjaj se uspesno odjavio sa mreže.\n", g_usn); kad dodamo vise senzora
    
    close(g_sock);
    exit(0);
}



void get_http_date(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);
    strftime(buf, size, "%a, %d %b %Y %H:%M:%S GMT", gmt);
}

// reiranje UDP soketa i podešavaje mrežnih parametara, kako bi uređaj mogao da sluša multicast
int main() {
    boot_id = (int)time(NULL); 

    g_sock = socket(AF_INET, SOCK_DGRAM, 0); // Kreira UDP soket (SOCK_DGRAM)        (za TCP je SOCK_STREAM)
    
    int reuse = 1;		// Dozvoljava više aplikacija da koriste isti port istovremeno
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // Postavlja timeout za primanje podataka na 5 sekundi
    struct timeval tv;
    tv.tv_sec = 5; 
    tv.tv_usec = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    // Podešavanje adrese na kojoj soket sluša
    struct sockaddr_in local_addr = {0};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(SSDP_PORT);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);	// Prihvata saobraćaj na svim interfejsima
    bind(g_sock, (struct sockaddr*)&local_addr, sizeof(local_addr));   // Vezuje soket za adresu

    struct ip_mreq mreq;				// Učlanjivanje u multicast grupu
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_ADDR); // Grupa: 239.255.255.250
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(g_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    // Priprema adrese za slanje (multicast destinacija)
    g_addr.sin_family = AF_INET;
    g_addr.sin_port = htons(SSDP_PORT);
    g_addr.sin_addr.s_addr = inet_addr(SSDP_ADDR);

    
    signal(SIGINT, handle_exit); // Registruje Ctrl+C (SIGINT) i poziva handle_exit 
    signal(SIGHUP, handle_exit); // Registruje klik na "x" (zatvaranje terminala) 

    printf("Senzor online (BOOTID: %d). Čekam upite...\n", boot_id); // IZMENJENO: Log poruka[cite: 8]

    char buffer[2048];

    //--DODANO
    time_t last_notify_time = 0;
    
    while (1) {
        //DODANO - Svakih 7 sekundi šalje objavu da je i dalje online
        time_t now = time(NULL);
        if (now - last_notify_time >= 7) {
            send_notify("ssdp:alive");
            printf("[SENZOR] Poslat periodični NOTIFY:alive\n");
            last_notify_time = now;
        }
        
        //DODANO - Struktura koja će čuvati adresu pošiljaoca (kontrolera)
        struct sockaddr_in client;
        socklen_t addr_len = sizeof(client);
        int len = recvfrom(g_sock, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&client, &addr_len);
        
        if (len > 0) {
            buffer[len] = '\0'; // Terminira primljeni string
            // Provera: Da li je poruka M-SEARCH i da li sadrži ispravan AUTH_TOKEN
            if (strstr(buffer, "M-SEARCH") && strstr(buffer, AUTH_TOKEN)) {
                char date_buf[64];
                get_http_date(date_buf, sizeof(date_buf));
                
                // Kreiranje direktnog odgovora (Unicast) kontroleru:
                char resp[1024]; 
                snprintf(resp, sizeof(resp), 
                    "HTTP/1.1 200 OK\r\n"
                    "CACHE-CONTROL: max-age=1800\r\n"
                    "DATE: %s\r\n"
                    "EXT:\r\n"                                 
                    "LOCATION: %s\r\n"
                    "SERVER: Linux/5.0 UPnP/2.0 Senzor/1.0\r\n" // IZMENJENO: Tip servera[cite: 8]
                    "ST: %s\r\n"
                    "USN: %s\r\n"
                    "BOOTID.UPNP.ORG: %d\r\n"
                    "CONFIGID.UPNP.ORG: 1\r\n"        
                    "SEARCHPORT.UPNP.ORG: 1900\r\n"
                    "X-DEVICE-STATUS: ON\r\n"  // NOVO: ON/OFF stanje
                    "AUTH: %s\r\n"             // Tvoj filter (Kontrola pristupa) DODANO
                    "\r\n", 
                    date_buf, LOCATION_URL, NOTIFY_NT, NOTIFY_USN, boot_id, AUTH_TOKEN);


		 // Šalje odgovor direktno na IP adresu kontrolera koji je poslao upit:
                sendto(g_sock, resp, strlen(resp), 0, (struct sockaddr*)&client, addr_len);
                printf("Odgovoreno autorizovanom kontroleru na M-SEARCH.\n");
            }
        }
    }
    return 0;
}
