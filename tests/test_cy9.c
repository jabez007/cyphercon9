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

typedef enum { DecodeStateIdle, DecodeStateData } DecodeState;

uint16_t sniffer_decode(EdgeStream* stream) {
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
                if(bits_in_byte < 8) {
                    if(uart_bit) bit_acc |= (1 << bits_in_byte);
                    bits_in_byte++;
                } else {
                    uint8_t byte = (uint8_t)bit_acc;
                    pkt_circular[pkt_idx] = byte;
                    
                    // Look for Syncword trailing the ID
                    // Protocol: ID is at indices 11-12. Sync is at 0-3.
                    // In a circular buffer of 47 bytes, if we just saw a Syncword at pkt_idx,
                    // the ID was 11-12 bytes 'behind' it in the wire stream.
                    if(byte == 22) {
                        // Check if we have enough bytes to find an ID
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

void build_edge_stream(EdgeStream* stream, uint16_t from_id) {
    uint8_t packet[47] = {0};
    packet[0] = 22; packet[1] = 22; packet[2] = 22; packet[3] = 22;
    packet[11] = (from_id >> 8) & 0xFF; packet[12] = from_id & 0xFF;

    stream->count = 0;
    bool current_level = false; 
    uint32_t current_duration = 0;
    uint32_t total_time = 0;

    for(uint32_t bit_idx = 0; bit_idx < 47 * 10; bit_idx++) {
        // Correct WIRE ORDER: 46 down to 0
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

int main() {
    EdgeStream stream;
    uint16_t target_id = 221;
    printf("Running Sniffer Unit Test (Reversed Wire Order)...\n");
    build_edge_stream(&stream, target_id);
    uint16_t recovered = sniffer_decode(&stream);
    printf("  Target ID: %d | Recovered ID: %d\n", target_id, recovered);
    if(recovered == target_id) { printf("✓ SUCCESS!\n"); return 0; }
    else { printf("✗ FAILURE!\n"); return 1; }
}
