#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>

#define BAUD_RATE 3000
#define BIT_TIME_US_FLOAT (1000000.0f / (float)BAUD_RATE)
#define MAX_EDGES 2000

typedef struct {
    uint32_t durations[MAX_EDGES];
    bool levels[MAX_EDGES];
    uint32_t count;
} EdgeStream;

// --- SHARED LOGIC ---

void build_packet(uint8_t* packet, uint16_t from_id, uint16_t to_id, uint8_t event_id, const char* alias, const char* msg) {
    uint16_t body_len = 32;
    memset(packet, 0, 47);
    packet[0] = 22; packet[1] = 22; packet[2] = 22; packet[3] = 22;
    packet[8] = (body_len >> 8) & 0xFF; packet[9] = body_len & 0xFF;
    packet[10] = event_id;
    packet[11] = (from_id >> 8) & 0xFF; packet[12] = from_id & 0xFF;
    packet[13] = (to_id >> 8) & 0xFF; packet[14] = to_id & 0xFF;
    char alias_buf[16], msg_buf[16];
    memset(alias_buf, ' ', 16); memset(msg_buf, ' ', 16);
    size_t name_len = strlen(alias);
    memcpy(alias_buf, alias, (name_len > 16) ? 16 : name_len);
    size_t msg_len = strlen(msg);
    memcpy(msg_buf, msg, (msg_len > 16) ? 16 : msg_len);
    memcpy(&packet[15], alias_buf, 16); memcpy(&packet[31], msg_buf, 16);
    uint32_t tally = 0;
    for(int i = 8; i < 47; i++) tally += packet[i];
    packet[4] = (tally >> 24) & 0xFF; packet[5] = (tally >> 16) & 0xFF;
    packet[6] = (tally >> 8) & 0xFF; packet[7] = tally & 0xFF;
}

void build_edge_stream(EdgeStream* stream, uint8_t* packet) {
    stream->count = 0;
    bool current_level = false; 
    uint32_t current_duration = 0;
    uint32_t total_time = 0;
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
            current_level = level;
            current_duration = bit_dur;
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
    uint8_t pkt_circular[47];
    uint8_t pkt_idx = 0;
    uint16_t recovered_id = 0;
    for(uint32_t i = 0; i < stream->count; i++) {
        uint32_t duration = stream->durations[i];
        bool uart_bit = !stream->levels[i]; 
        int num_bits = (int)(( (float)duration + (BIT_TIME_US_FLOAT / 2.0f) ) / BIT_TIME_US_FLOAT);
        if(num_bits <= 0) num_bits = 1;
        for(int b = 0; b < num_bits; b++) {
            if(state == DecodeStateIdle) {
                if(!uart_bit) { state = DecodeStateData; bit_acc = 0; bits_in_byte = 0; }
            } else {
                if(bits_in_byte < 8) { if(uart_bit) bit_acc |= (1 << bits_in_byte); bits_in_byte++; }
                else {
                    uint8_t byte = (uint8_t)bit_acc;
                    pkt_circular[pkt_idx] = byte;
                    if(byte == 22) {
                        int id_hi_idx = (pkt_idx + 47 - 11) % 47;
                        int id_lo_idx = (pkt_idx + 47 - 12) % 47;
                        recovered_id = (pkt_circular[id_hi_idx] << 8) | pkt_circular[id_lo_idx];
                    }
                    pkt_idx = (pkt_idx + 1) % 47;
                    state = DecodeStateIdle;
                    if(!uart_bit) { state = DecodeStateData; bit_acc = 0; bits_in_byte = 0; }
                }
            }
        }
    }
    return recovered_id;
}

// --- TESTS ---

void test_packet_logic() {
    printf("Testing Packet Logic...\n");
    uint8_t packet[47];
    build_packet(packet, 221, 0, 3, "Flipper", "Hello");
    // Verify Checksum
    uint32_t tally = 0;
    for(int i = 8; i < 47; i++) tally += packet[i];
    uint32_t pkt_tally = (packet[4] << 24) | (packet[5] << 16) | (packet[6] << 8) | packet[7];
    assert(tally == pkt_tally);
    // Verify ID packing
    assert(packet[11] == 0 && packet[12] == 221);
    printf("  ✓ Checksum and ID packing passed!\n");
}

void test_timing_accuracy() {
    printf("Testing Timing Accuracy...\n");
    uint8_t packet[47];
    EdgeStream stream;
    build_packet(packet, 1, 0, 3, "T", "M");
    build_edge_stream(&stream, packet);
    uint32_t total_time = 0;
    for(uint32_t i = 0; i < stream.count; i++) total_time += stream.durations[i];
    uint32_t expected = (uint32_t)(470.0f * BIT_TIME_US_FLOAT);
    assert(abs((int)total_time - (int)expected) <= 1);
    printf("  ✓ Total signal duration: %u us (No drift!)\n", total_time);
}

void test_full_loopback() {
    printf("Testing Full Loopback (Send -> Edges -> Sniff)...\n");
    uint8_t packet[47];
    EdgeStream stream;
    uint16_t target_id = 221;
    build_packet(packet, target_id, 0, 3, "Flipper", "Test");
    build_edge_stream(&stream, packet);
    uint16_t recovered = sniffer_decode(&stream);
    printf("  Recovered ID: %d\n", recovered);
    assert(recovered == target_id);
    printf("  ✓ End-to-end decoding passed!\n");
}

int main() {
    printf("=== Cyphercon 9 Protocol Test Suite ===\n\n");
    test_packet_logic();
    test_timing_accuracy();
    test_full_loopback();
    printf("\nALL SYSTEMS VERIFIED!\n");
    return 0;
}
