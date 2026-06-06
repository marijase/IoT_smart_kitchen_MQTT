#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <signal.h>

#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900
#define AUTH_TOKEN "smart_kitchen_pass"
#define MAX_DEVICES 20


#define CP_FRIENDLY_NAME "Glavni_Kontroler_RT-RK"
#define CP_UUID "uuid:cc8e-4f32-a5b1-kontroler001"
#define USER_AGENT_STR "Linux/5.0 UPnP/2.0 Kontroler/1.0"

#define CLEANUP_INTERVAL 5    // Provjeravaj mrtve uređaje svakih 5 sekundi --DODANO
#define MSEARCH_INTERVAL 60   // Ponovi potragu svakih 60 sekundi (1 minut)  --DODANO
#define TIMEOUT_LIMIT 20      // Uređaj je mrtav ako se ne javi 20 sekundi  --DODANO

// za cuvanje informacija o uredjajima 
typedef struct {
    char usn[128];
    char location[128];
    char nt[128];
    char status[16]; //dodano za on/off stanje
    time_t last_seen;
    int active; //1 = online a 0=unavailable
} Device;

Device device_registry[MAX_DEVICES];
int g_sock = -1;

void init_registry() {
    for(int i = 0; i < MAX_DEVICES; i++) device_registry[i].active = 0;
}

// Realizacija komponente za cuvanje informacija o uredjajima
void update_device(const char* usn, const char* location, const char* nt, const char* status) {
    int found = 0;
    for(int i = 0; i < MAX_DEVICES; i++) {
        if(device_registry[i].active && strcmp(device_registry[i].usn, usn) == 0) {
            device_registry[i].last_seen = time(NULL);
            strncpy(device_registry[i].status, status, 16);
            strncpy(device_registry[i].location, location, 128);
            found = 1;
            break;
        }
    }
    if(!found) {
        for(int i = 0; i < MAX_DEVICES; i++) {
            if(!device_registry[i].active) {
                strncpy(device_registry[i].usn, usn, 128);
                strncpy(device_registry[i].location, location, 128);
                strncpy(device_registry[i].nt, nt, 128);
                device_registry[i].last_seen = time(NULL);
                device_registry[i].active = 1;
                printf("\n[REGISTAR] Novi uređaj: %s | Lokacija: %s\n", usn, location);
                break;
            }
        }
    }
}

// Slanje M-SEARCH upita -modula za oglasavanje
void send_msearch() {
    struct sockaddr_in mcast_addr;
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(SSDP_PORT);
    mcast_addr.sin_addr.s_addr = inet_addr(SSDP_ADDR);

    char msearch[1024];
    snprintf(msearch, sizeof(msearch),
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: %s:%d\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: ssdp:all\r\n"
        "USER-AGENT: %s\r\n"               // OS i verzija
        "CPFN.UPNP.ORG: %s\r\n"            // Prijateljsko ime
        "CPUUID.UPNP.ORG: %s\r\n"          // UUID
        "AUTH: %s\r\n"                     // Kontrola pristup
        "\r\n", 
        SSDP_ADDR, SSDP_PORT, USER_AGENT_STR, CP_FRIENDLY_NAME, CP_UUID, AUTH_TOKEN);

    sendto(g_sock, msearch, strlen(msearch), 0, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));
    printf("[KONTROLER] Poslat M-SEARCH upit sa autentifikacijom.\n");
}
//DODANO
void print_active_devices() {
    printf("\n======= REGISTAR UREĐAJA =======\n");
    int found = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        // Provjeravamo da li u slotu postoji ikakav podatak (makar i star)
        if (strlen(device_registry[i].usn) > 0) { 
            printf("Uređaj: %s\n", device_registry[i].usn);
            
            // --- OVDJE IDE TVOJ KOD ZA PROVJERU STANJA ---
            if(device_registry[i].active) {
                // Ako se javio u zadnjih 20 sekundi
                printf("Status: %s (Online)\n", device_registry[i].status);
            } else {
                // Ako je prošao timeout ili je poslao byebye
                printf("Status: Unavailable\n");
            }
            printf("--------------------------------\n");
            found = 1;
        }
    }
    if(!found) printf("Nema detektovanih uređaja.\n");
}
//DODANO

int main() {
    init_registry();
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);

    int reuse = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Pridruzivanje multicast grupi za NOTIFY poruke
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(g_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    struct sockaddr_in local_addr = {0};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(SSDP_PORT);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(g_sock, (struct sockaddr*)&local_addr, sizeof(local_addr));

    send_msearch();

    //Dodajemo timeout na recvfrom da petlja ne bi "stajala" na mjestu --DODANO ODAVDE
    struct timeval tv;
    tv.tv_sec = 1; // Čekaj 1 sekundu na poruku, pa nastavi dalje kroz petlju
    tv.tv_usec = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    //Pomoćne varijable za vrijeme
    time_t last_msearch_time = time(NULL); 
    time_t last_cleanup_time = time(NULL);
    // --DODANO DO OVDJE
    char buffer[2048];
    while(1) {
        time_t now = time(NULL); // Uzmi trenutno vrijeme na početku svakog kruga
            //--DODANO ODAVDE
        //Periodično slanje M-SEARCH (svakih 60-120 sekundi)
        if (now - last_msearch_time >= 60) { 
            send_msearch(); // Pozivamo tvoju postojeću funkciju
            last_msearch_time = now;
            printf("[INFO] Periodični M-SEARCH poslat.\n");
        }

        //Čišćenje neaktivnih uređaja (Timeout > 20s) ---
        if (now - last_cleanup_time >= 5) { // Provjeravaj svakih 5 sekundi
            for (int i = 0; i < MAX_DEVICES; i++) {
                if (device_registry[i].active) {
                    // Ako je prošlo više od 20s od zadnjeg javljanja
                    if (now - device_registry[i].last_seen > 20) {
                        device_registry[i].active = 0;
                        printf("[INFO] Uređaj %s je uklonjen (timeout).\n", device_registry[i].usn);
                    }
                }
            }
            last_cleanup_time = now;
            print_active_devices();
        }
        //--DODANO DO OVDJE
        struct sockaddr_in sender;
        socklen_t len = sizeof(sender);
        int n = recvfrom(g_sock, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&sender, &len);
        
        if (n > 0) {
            buffer[n] = '\0';

            // Realizacija komponente za upravljanje lokalnom mrezom[cite: 1]
            if (strstr(buffer, "HTTP/1.1 200 OK") || strstr(buffer, "NOTIFY")) {
                char *usn_p = strstr(buffer, "USN: ");
                char *loc_p = strstr(buffer, "LOCATION: ");
                char *nt_p = strstr(buffer, "ST: "); // Kod RESPONSE je ST, kod NOTIFY je NT
                if(!nt_p) nt_p = strstr(buffer, "NT: ");

                if (usn_p && loc_p) {
                    char usn[128], loc[128], nt[128] = "unknown";
                    sscanf(usn_p, "USN: %127s", usn);
                    sscanf(loc_p, "LOCATION: %127s", loc);
                    if(nt_p) sscanf(nt_p, "%*s %127s", nt);

                    // --DODANO KOD ZA STATUS
                    char *status_p = strstr(buffer, "X-DEVICE-STATUS: ");
                    char status[16] = "UNKNOWN";
                    if(status_p) sscanf(status_p, "X-DEVICE-STATUS: %15s", status);

                    // Provera odjave uređaja (ssdp:byebye)
                    if (strstr(buffer, "ssdp:byebye")) {
                        for(int i=0; i<MAX_DEVICES; i++) {
                            if(device_registry[i].active && strcmp(device_registry[i].usn, usn) == 0) {
                                device_registry[i].active = 0;
                                printf("[KONTROLER] Uređaj %s uklonjen (byebye).\n", usn);
                            }
                        }
                    } else {
                        update_device(usn, loc, nt, status);
                    }
                }
            }
        }
    }
    return 0;
}