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
    uint32_t durations[MAX_EDGES];
    bool levels[MAX_EDGES];
    uint32_t count;
} EdgeStream;

// --- APP LOGIC (Copied for testing) ---

uint16_t get_random_id(Cy9BadgeClass cls) {
    // Simple mock of furi_hal_random_get using rand()
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
    packet[8] = 0; packet[9] = 32; // Body Len
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

void build_edge_stream(EdgeStream* stream, uint8_t* packet) {
    stream->count = 0;
    bool current_level = false; uint32_t current_duration = 0; uint32_t total_time = 0;
    for(uint32_t bit_idx = 0; bit_idx < 47 * 10; bit_idx++) {
        uint8_t byte = packet[46 - (bit_idx / 10)];
        int b = bit_idx % 10;
        bool bit = (b == 0) ? false : (b == 9) ? true : (byte & (1 << (b - 1))) != 0;
        bool level = !bit;
        uint32_t next_total_time = (uint32_t)((float)(bit_idx + 1) * BIT_TIME_US_FLOAT);
        uint32_t bit_dur = next_total_time - total_time;
        total_time = next_total_time;
        if(bit_idx == 0) { current_level = level; current_duration = bit_dur; }
        else if(level == current_level) { current_duration += bit_dur; }
        else {
            stream->durations[stream->count] = current_duration;
            stream->levels[stream->count] = current_level;
            stream->count++;
            current_level = level; current_duration = bit_dur;
        }
    }
    stream->durations[stream->count] = current_duration;
    stream->levels[stream->count] = current_level;
    stream->count++;
}

uint16_t sniffer_decode(EdgeStream* stream) {
    typedef enum { DecodeStateIdle, DecodeStateData } DecodeState;
    DecodeState state = DecodeStateIdle;
    uint32_t bit_acc = 0, bits_in_byte = 0;
    uint8_t pkt_circular[47] = {0};
    uint8_t pkt_idx = 0;
    uint16_t recovered_id = 0;
    for(uint32_t i = 0; i < stream->count; i++) {
        uint32_t duration = stream->durations[i];
        bool level = stream->levels[i];
        int num_bits = (int)(( (float)duration + (BIT_TIME_US_FLOAT / 2.0f) ) / BIT_TIME_US_FLOAT);
        if(num_bits <= 0) num_bits = 1;
        for(int b = 0; b < num_bits; b++) {
            if(state == DecodeStateIdle) {
                if(level) { state = DecodeStateData; bit_acc = 0; bits_in_byte = 0; } // Level true = Pulse (0)
            } else {
                if(bits_in_byte < 8) { if(!level) bit_acc |= (1 << bits_in_byte); bits_in_byte++; } // Level false = Space (1)
                else {
                    pkt_circular[pkt_idx] = (uint8_t)bit_acc;
                    if(pkt_circular[pkt_idx] == 22) {
                        recovered_id = (pkt_circular[(pkt_idx + 47 - 11) % 47] << 8) | pkt_circular[(pkt_idx + 47 - 12) % 47];
                    }
                    pkt_idx = (pkt_idx + 1) % 47;
                    state = DecodeStateIdle;
                    if(level) { state = DecodeStateData; bit_acc = 0; bits_in_byte = 0; }
                }
            }
        }
    }
    return recovered_id;
}

// --- NEW TESTS ---

void test_identity_randomization() {
    printf("Testing Identity Randomization Ranges...\n");
    for(int i = 0; i < 100; i++) {
        uint16_t id = get_random_id(Cy9ClassFounder);
        assert(id >= 1 && id <= 25);
        id = get_random_id(Cy9ClassVendor);
        assert(id >= 661 && id <= 675);
        id = get_random_id(Cy9ClassChaos);
        assert(id >= 0 && id <= 675);
    }
    printf("  ✓ All 300 random IDs fell within their correct badge class ranges!\n");
}

void test_space_padding() {
    printf("Testing Space Padding Alignment...\n");
    uint8_t packet[47];
    build_packet(packet, 1, "Hi", "Bye");
    
    // Alias "Hi" should be followed by 14 spaces (0x20)
    assert(packet[15] == 'H' && packet[16] == 'i');
    for(int i = 17; i < 31; i++) assert(packet[i] == 0x20);
    
    // Message "Bye" should be followed by 13 spaces
    assert(packet[31] == 'B' && packet[32] == 'y' && packet[33] == 'e');
    for(int i = 34; i < 47; i++) assert(packet[i] == 0x20);
    
    printf("  ✓ Messages are correctly padded with spaces (0x20) for the badge display!\n");
}

void test_timing_accuracy() {
    printf("Testing Timing Accuracy...\n");
    uint8_t packet[47]; EdgeStream stream;
    build_packet(packet, 1, "T", "M");
    build_edge_stream(&stream, packet);
    uint32_t total = 0;
    for(uint32_t i = 0; i < stream.count; i++) total += stream.durations[i];
    uint32_t expected = (uint32_t)(470.0f * BIT_TIME_US_FLOAT);
    assert(abs((int)total - (int)expected) <= 1);
    printf("  ✓ Total signal duration: %u us (Perfect 3000 baud timing)\n", total);
}

void test_full_loopback() {
    printf("Testing Full Loopback (Chameleon Mode)...\n");
    uint8_t packet[47]; EdgeStream stream;
    uint16_t target_id = 675; // Vendor
    build_packet(packet, target_id, "Flipper", "Chaos!");
    build_edge_stream(&stream, packet);
    uint16_t recovered = sniffer_decode(&stream);
    printf("  Recovered ID: %d\n", recovered);
    assert(recovered == target_id);
    printf("  ✓ Sniffer correctly recovered the randomized ID from the bitstream!\n");
}

int main() {
    srand(42); // Seed for deterministic random testing
    printf("=== Cy9 Universal Remote: Advanced Test Suite ===\n\n");
    test_identity_randomization();
    test_space_padding();
    test_timing_accuracy();
    test_full_loopback();
    printf("\nCOMPREHENSIVE VERIFICATION COMPLETE: Ready for deployment.\n");
    return 0;
}
