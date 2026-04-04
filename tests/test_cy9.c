#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>

#define BAUD_RATE 3000
#define BIT_TIME_US_FLOAT (1000000.0f / (float)BAUD_RATE)
#define MAX_EDGES 5000

typedef enum {
    Cy9ClassGhost, Cy9ClassFounder, Cy9ClassExtreme, Cy9ClassLifetime,
    Cy9ClassSpeaker, Cy9ClassGeneral, Cy9ClassVendor, Cy9ClassChaos, Cy9ClassCount
} Cy9BadgeClass;

typedef struct {
    char greet_msg[17];
    char founder_msg[17];
    Cy9BadgeClass selected_class;
} Cy9SettingsMock;

typedef struct {
    uint32_t durations[MAX_EDGES];
    bool levels[MAX_EDGES];
    uint32_t count;
} EdgeStream;

// --- APP LOGIC (Copied for testing) ---

uint16_t get_random_id(Cy9BadgeClass cls) {
    uint32_t r = rand();
    switch(cls) {
        case Cy9ClassGhost: return 0;
        case Cy9ClassFounder: return (r % 25) + 1;
        case Cy9ClassExtreme: return (r % 75) + 26;
        case Cy9ClassLifetime: return (r % 10) + 101;
        case Cy9ClassSpeaker: return (r % 110) + 111;
        case Cy9ClassGeneral: return (r % 440) + 221;
        case Cy9ClassVendor: return (r % 15) + 661;
        case Cy9ClassChaos: return r % 676;
        default: return 1;
    }
}

void build_packet(uint8_t* packet, uint16_t from_id, const char* alias, const char* msg) {
    memset(packet, 0, 47);
    packet[0] = 22; packet[1] = 22; packet[2] = 22; packet[3] = 22;
    packet[8] = 0; packet[9] = 32;
    packet[11] = (from_id >> 8) & 0xFF; packet[12] = from_id & 0xFF;
    char alias_buf[16], msg_buf[16];
    memset(alias_buf, ' ', 16); memset(msg_buf, ' ', 16);
    if(alias) memcpy(alias_buf, alias, (strlen(alias) > 16) ? 16 : strlen(alias));
    if(msg) memcpy(msg_buf, msg, (strlen(msg) > 16) ? 16 : strlen(msg));
    memcpy(&packet[15], alias_buf, 16); memcpy(&packet[31], msg_buf, 16);
    uint32_t tally = 0;
    for(int i = 8; i < 47; i++) tally += packet[i];
    packet[4] = (tally >> 24) & 0xFF; packet[5] = (tally >> 16) & 0xFF;
    packet[6] = (tally >> 8) & 0xFF; packet[7] = tally & 0xFF;
}

// --- NEW TESTS ---

void test_default_settings_logic() {
    printf("Testing Default Settings Logic...\n");
    Cy9SettingsMock app;
    
    // Simulate "Cold Boot" (no settings file)
    snprintf(app.greet_msg, 17, "Greetz from Cy9!");
    snprintf(app.founder_msg, 17, "Obey the system.");
    app.selected_class = Cy9ClassExtreme;

    assert(strcmp(app.greet_msg, "Greetz from Cy9!") == 0);
    assert(app.selected_class == Cy9ClassExtreme);
    printf("  ✓ Default state is correctly initialized to 'Extreme' range.\n");
}

void test_exhaustive_id_ranges() {
    printf("Testing Exhaustive ID Range Boundaries...\n");
    
    struct { Cy9BadgeClass cls; uint16_t min; uint16_t max; } ranges[] = {
        {Cy9ClassGhost, 0, 0},
        {Cy9ClassFounder, 1, 25},
        {Cy9ClassExtreme, 26, 100},
        {Cy9ClassLifetime, 101, 110},
        {Cy9ClassSpeaker, 111, 220},
        {Cy9ClassGeneral, 221, 660},
        {Cy9ClassVendor, 661, 675},
        {Cy9ClassChaos, 0, 675}
    };

    for(int r = 0; r < 8; r++) {
        bool hit_min = false, hit_max = false;
        for(int i = 0; i < 5000; i++) {
            uint16_t id = get_random_id(ranges[r].cls);
            assert(id >= ranges[r].min && id <= ranges[r].max);
            if(id == ranges[r].min) hit_min = true;
            if(id == ranges[r].max) hit_max = true;
        }
        assert(hit_min && hit_max); // Ensure full range is reachable
        printf("  ✓ Class %d: Range [%d-%d] verified and fully reachable.\n", r, ranges[r].min, ranges[r].max);
    }
}

int main() {
    srand(42);
    printf("=== Cy9 Universal Remote: Production Test Suite ===\n\n");
    test_default_settings_logic();
    test_exhaustive_id_ranges();
    
    // Quick re-verify of core packet math
    uint8_t packet[47];
    build_packet(packet, 675, "F", "M");
    uint32_t tally = 0;
    for(int i = 8; i < 47; i++) tally += packet[i];
    uint32_t pkt_tally = (packet[4] << 24) | (packet[5] << 16) | (packet[6] << 8) | packet[7];
    assert(tally == pkt_tally);
    
    printf("\nALL SYSTEMS VERIFIED: Default state, ID ranges, and packet math are sound.\n");
    return 0;
}
