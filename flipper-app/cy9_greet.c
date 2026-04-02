#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_infrared.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>

/*
 * CY9 IR Protocol Specs:
 * - 38kHz Carrier
 * - 3000 Baud (333.33 microseconds per bit)
 * - 8N1 (1 start bit '0', 8 data bits LSB-first, 1 stop bit '1')
 * - Idle state: IR OFF (UART HIGH)
 * - Active state: IR ON (UART LOW)
 */

#define BAUD_RATE 3000
#define BIT_TIME_US (1000000 / BAUD_RATE)
#define CARRIER_FREQ 38000
#define SYNC_PREAMBLE_COUNT 8
#define TOTAL_PACKET_BYTES 47
#define MAX_DURATIONS ((TOTAL_PACKET_BYTES + SYNC_PREAMBLE_COUNT) * 10 + 1)

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
    *level = (b->index % 2 == 0); // Pulse, Space, Pulse, Space...
    b->index++;
    
    return (b->index >= b->count) ? FuriHalInfraredTxGetDataStateDone : FuriHalInfraredTxGetDataStateOk;
}

static void cy9_append_bit(bool level, uint32_t* current_duration, bool* current_level) {
    if(global_burst.count == 0 && *current_duration == 0) {
        *current_level = level;
        *current_duration = BIT_TIME_US;
    } else if(level == *current_level) {
        *current_duration += BIT_TIME_US;
    } else {
        if(global_burst.count < MAX_DURATIONS) {
            global_burst.durations[global_burst.count++] = *current_duration;
        }
        *current_level = level;
        *current_duration = BIT_TIME_US;
    }
}

static void cy9_append_byte(uint8_t byte, uint32_t* current_duration, bool* current_level) {
    for(int b = 0; b < 10; b++) {
        bool bit;
        if(b == 0) bit = false; // Start
        else if(b == 9) bit = true; // Stop
        else bit = (byte & (1 << (b - 1))) != 0; // Data (LSB)
        cy9_append_bit(!bit, current_duration, current_level);
    }
}

void cy9_broadcast_greeting() {
    if(furi_hal_infrared_is_busy()) return;

    // 1. Prepare Packet (47 bytes total)
    uint8_t packet[47] = {0};
    uint16_t body_len = 32;
    uint8_t event_id = 4; // Broadcast
    uint16_t from_id = 0; // Ghost
    uint16_t to_id = 0;   // Broadcast scope

    // [0:4] Syncword (0x16 = 22)
    packet[0] = 22; packet[1] = 22; packet[2] = 22; packet[3] = 22;
    // [8:10] Body Len (Big Endian)
    packet[8] = (body_len >> 8) & 0xFF; packet[9] = body_len & 0xFF;
    // [10] Event ID
    packet[10] = event_id;
    // [11:13] From ID (Big Endian)
    packet[11] = (from_id >> 8) & 0xFF; packet[12] = from_id & 0xFF;
    // [13:15] To ID (Big Endian)
    packet[13] = (to_id >> 8) & 0xFF; packet[14] = to_id & 0xFF;
    
    // Body [15:47]
    char alias_buf[16];
    memset(alias_buf, ' ', 16);
    const char* flipper_name = furi_hal_version_get_name_ptr();
    if(flipper_name) {
        size_t name_len = strlen(flipper_name);
        memcpy(alias_buf, flipper_name, (name_len > 16) ? 16 : name_len);
    }
    memcpy(&packet[15], alias_buf, 16);
    memcpy(&packet[31], "Greetz from Cy9!", 16);
    
    // Checksum [4:8] (sum of bytes 8 to 46)
    uint32_t tally = 0;
    for(int i = 8; i < 47; i++) tally += packet[i];
    packet[4] = (tally >> 24) & 0xFF;
    packet[5] = (tally >> 16) & 0xFF;
    packet[6] = (tally >> 8) & 0xFF;
    packet[7] = tally & 0xFF;
    
    // 2. Build Durations
    global_burst.count = 0;
    global_burst.index = 0;
    uint32_t current_duration = 0;
    bool current_level = false;

    // Extra syncwords for preamble
    for(int i = 0; i < SYNC_PREAMBLE_COUNT; i++) {
        cy9_append_byte(22, &current_duration, &current_level);
    }

    // Packet in REVERSED order (as per tx() function in blue-badge.py)
    // tx() sends byte 46, then 45, ..., then 0.
    for(int i = 46; i >= 0; i--) {
        cy9_append_byte(packet[i], &current_duration, &current_level);
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
