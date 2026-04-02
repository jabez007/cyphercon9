#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_infrared.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>

/*
 * CY9 IR Protocol Specs:
 * - 38kHz Carrier
 * - 3000 Baud
 * - 8N1 (Start=0, 8 Data LSB-first, Stop=1)
 * - UART LOW = IR ON | UART HIGH = IR OFF
 */

#define BAUD_RATE 3000
#define BIT_TIME_US_FLOAT (1000000.0f / (float)BAUD_RATE)
#define CARRIER_FREQ 38000
#define SYNC_PREAMBLE_COUNT 4
#define TOTAL_PACKET_BYTES 47
#define MAX_DURATIONS 1000 // Sufficient for ~500 bits

typedef struct {
    uint32_t durations[MAX_DURATIONS];
    uint32_t count;
    uint32_t index;
} Cy9Burst;

static Cy9Burst global_burst;

static FuriHalInfraredTxGetDataState cy9_tx_callback(void* context, uint32_t* duration, bool* level) {
    Cy9Burst* b = (Cy9Burst*)context;
    if(b->index >= b->count) return FuriHalInfraredTxGetDataStateLastDone;
    
    *duration = b->durations[b->index];
    *level = (b->index % 2 == 0); // Burst starts with IR ON (Start bit)
    b->index++;
    
    return (b->index >= b->count) ? FuriHalInfraredTxGetDataStateDone : FuriHalInfraredTxGetDataStateOk;
}

static void cy9_add_duration(uint32_t duration, bool level, uint32_t* current_duration, bool* current_level) {
    if(global_burst.count == 0 && *current_duration == 0) {
        *current_level = level;
        *current_duration = duration;
    } else if(level == *current_level) {
        *current_duration += duration;
    } else {
        if(global_burst.count < MAX_DURATIONS) {
            global_burst.durations[global_burst.count++] = *current_duration;
        }
        *current_level = level;
        *current_duration = duration;
    }
}

void cy9_broadcast_greeting() {
    if(furi_hal_infrared_is_busy()) return;

    // 1. Prepare Packet
    uint8_t packet[47] = {0};
    uint16_t body_len = 32;
    uint8_t event_id = 3; // 3 = Broadcast from origin
    uint16_t from_id = 1; // 1 = Founder ID
    uint16_t to_id = 0;   // 0 = Broadcast scope

    packet[0] = 22; packet[1] = 22; packet[2] = 22; packet[3] = 22;
    packet[8] = (body_len >> 8) & 0xFF; packet[9] = body_len & 0xFF;
    packet[10] = event_id;
    packet[11] = (from_id >> 8) & 0xFF; packet[12] = from_id & 0xFF;
    packet[13] = (to_id >> 8) & 0xFF; packet[14] = to_id & 0xFF;
    
    char alias_buf[16];
    memset(alias_buf, ' ', 16);
    const char* flipper_name = furi_hal_version_get_name_ptr();
    if(flipper_name) {
        size_t name_len = strlen(flipper_name);
        memcpy(alias_buf, flipper_name, (name_len > 16) ? 16 : name_len);
    }
    memcpy(&packet[15], alias_buf, 16);
    memcpy(&packet[31], "Greetz from Cy9!", 16);
    
    uint32_t tally = 0;
    for(int i = 8; i < 47; i++) tally += packet[i];
    packet[4] = (tally >> 24) & 0xFF; packet[5] = (tally >> 16) & 0xFF;
    packet[6] = (tally >> 8) & 0xFF; packet[7] = tally & 0xFF;
    
    // 2. Build High-Precision Timing
    global_burst.count = 0;
    global_burst.index = 0;
    uint32_t current_duration = 0;
    bool current_level = false;

    uint32_t total_bits = (SYNC_PREAMBLE_COUNT + TOTAL_PACKET_BYTES) * 10;
    uint32_t last_time_us = 0;

    for(uint32_t bit_idx = 0; bit_idx < total_bits; bit_idx++) {
        uint32_t byte_pos = bit_idx / 10;
        uint32_t bit_in_byte = bit_idx % 10;
        uint8_t byte;

        if(byte_pos < SYNC_PREAMBLE_COUNT) {
            byte = 22; // Preamble Sync
        } else {
            // Reversed wire order: packet[46], packet[45]...
            byte = packet[46 - (byte_pos - SYNC_PREAMBLE_COUNT)];
        }

        bool bit;
        if(bit_in_byte == 0) bit = false; // Start (0)
        else if(bit_in_byte == 9) bit = true; // Stop (1)
        else bit = (byte & (1 << (bit_in_byte - 1))) != 0; // Data (LSB)

        bool level = !bit; // bit 0 = IR ON, bit 1 = IR OFF
        
        // Cumulative timing to avoid drift
        uint32_t next_time_us = (uint32_t)((float)(bit_idx + 1) * BIT_TIME_US_FLOAT);
        cy9_add_duration(next_time_us - last_time_us, level, &current_duration, &current_level);
        last_time_us = next_time_us;
    }

    if(current_duration > 0 && global_burst.count < MAX_DURATIONS) {
        global_burst.durations[global_burst.count++] = current_duration;
    }

    // 3. Transmit
    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notifications, &sequence_blink_blue_100);
    furi_hal_power_insomnia_enter();
    
    furi_hal_infrared_async_tx_set_data_isr_callback(cy9_tx_callback, &global_burst);
    furi_hal_infrared_async_tx_start(CARRIER_FREQ, 0.5f);
    furi_hal_infrared_async_tx_wait_termination();

    furi_hal_power_insomnia_exit();
    furi_record_close(RECORD_NOTIFICATION);
}

static void cy9_greet_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 30, 20, "Cy9 Greeter");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 10, 40, "Press OK to broadcast");
}

static void cy9_greet_input_callback(InputEvent* input_event, void* context) {
    FuriMessageQueue* event_queue = context;
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

int32_t cy9_greet_app(void* p) {
    UNUSED(p);
    furi_hal_infrared_set_tx_output(FuriHalInfraredTxPinInternal);
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, cy9_greet_draw_callback, NULL);
    view_port_input_callback_set(view_port, cy9_greet_input_callback, event_queue);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    
    InputEvent event;
    while(furi_message_queue_get(event_queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.type == InputTypeShort && event.key == InputKeyOk) {
            cy9_broadcast_greeting();
        } else if(event.key == InputKeyBack) {
            break;
        }
    }
    
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_record_close(RECORD_GUI);
    return 0;
}
