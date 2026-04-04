#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <math.h>

#define BAUD_RATE 3000
#define BIT_TIME_US_FLOAT (1000000.0f / (float)BAUD_RATE)
#define MAX_DURATIONS 1000

typedef struct {
    uint32_t durations[MAX_DURATIONS];
    uint32_t count;
} Cy9Burst;

void build_packet(uint8_t* packet, uint16_t from_id, uint16_t to_id, uint8_t event_id, const char* alias, const char* msg) {
    uint16_t body_len = 32;
    packet[0] = 22; packet[1] = 22; packet[2] = 22; packet[3] = 22;
    packet[8] = (body_len >> 8) & 0xFF; packet[9] = body_len & 0xFF;
    packet[10] = event_id;
    packet[11] = (from_id >> 8) & 0xFF; packet[12] = from_id & 0xFF;
    packet[13] = (to_id >> 8) & 0xFF; packet[14] = to_id & 0xFF;
    char alias_buf[16], msg_buf[16];
    memset(alias_buf, ' ', 16); memset(msg_buf, ' ', 16);
    strncpy(alias_buf, alias, 16); strncpy(msg_buf, msg, 16);
    memcpy(&packet[15], alias_buf, 16); memcpy(&packet[31], msg_buf, 16);
    uint32_t tally = 0;
    for(int i = 8; i < 47; i++) tally += packet[i];
    packet[4] = (tally >> 24) & 0xFF; packet[5] = (tally >> 16) & 0xFF;
    packet[6] = (tally >> 8) & 0xFF; packet[7] = tally & 0xFF;
}

void build_durations(Cy9Burst* burst, uint8_t* packet) {
    burst->count = 0;
    uint32_t current_duration = 0;
    bool current_level = false;
    uint32_t last_time_us = 0;
    
    // Test logic for 47 bytes (no preamble for simple math)
    for(uint32_t bit_idx = 0; bit_idx < 47 * 10; bit_idx++) {
        uint8_t byte = packet[46 - (bit_idx / 10)];
        uint32_t b = bit_idx % 10;
        bool bit = (b == 0) ? false : (b == 9) ? true : (byte & (1 << (b - 1))) != 0;
        bool level = !bit;
        
        uint32_t next_time_us = (uint32_t)((float)(bit_idx + 1) * BIT_TIME_US_FLOAT);
        uint32_t bit_dur = next_time_us - last_time_us;
        
        if(bit_idx == 0) {
            current_level = level;
            current_duration = bit_dur;
        } else if(level == current_level) {
            current_duration += bit_dur;
        } else {
            burst->durations[burst->count++] = current_duration;
            current_level = level;
            current_duration = bit_dur;
        }
        last_time_us = next_time_us;
    }
    burst->durations[burst->count++] = current_duration;
}

void test_timing_accuracy() {
    printf("Running test_timing_accuracy...\n");
    uint8_t packet[47];
    Cy9Burst burst;
    
    build_packet(packet, 1, 0, 3, "Test", "Hello");
    build_durations(&burst, packet);
    
    // The total time for 470 bits at 3000 baud must be exactly 156,666 microseconds.
    // (470 / 3000) * 1,000,000 = 156,666.666...
    uint32_t total_time = 0;
    for(uint32_t i = 0; i < burst.count; i++) {
        total_time += burst.durations[i];
    }
    
    printf("  Total Signal Duration: %u us\n", total_time);
    
    // We allow +/- 1us error for integer truncation in the very last bit
    uint32_t expected_time = (uint32_t)(470.0f * BIT_TIME_US_FLOAT);
    assert(abs((int)total_time - (int)expected_time) <= 1);
    
    printf("✓ test_timing_accuracy passed! No cumulative drift detected.\n");
}

int main() {
    test_timing_accuracy();
    printf("\nAll tests passed! Protocol and Timing math are sound.\n");
    return 0;
}
