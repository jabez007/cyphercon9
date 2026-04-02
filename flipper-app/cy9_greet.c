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
#define MAX_DURATIONS 471

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
    *level = (b->index % 2 == 0); // Our timing starts with level=true (Pulse)
    b->index++;
    
    return (b->index >= b->count) ? FuriHalInfraredTxGetDataStateDone : FuriHalInfraredTxGetDataStateOk;
}

void cy9_broadcast_greeting() {
    if(furi_hal_infrared_is_busy()) return;

    // 1. Prepare packet
    uint16_t body_len = 32;
    uint8_t event_id = 4;
    uint16_t from_id = 0;
    uint16_t to_id = 0;

    char alias_buf[16];
    memset(alias_buf, ' ', 16);
    const char* flipper_name = furi_hal_version_get_name_ptr();
    if(flipper_name) {
        size_t name_len = strlen(flipper_name);
        memcpy(alias_buf, flipper_name, (name_len > 16) ? 16 : name_len);
    }

    uint8_t tx_buffer[47] = {0};
    tx_buffer[0] = 22; tx_buffer[1] = 22; tx_buffer[2] = 22; tx_buffer[3] = 22;
    tx_buffer[8] = (body_len >> 8) & 0xFF; tx_buffer[9] = body_len & 0xFF;
    tx_buffer[10] = event_id;
    tx_buffer[11] = (from_id >> 8) & 0xFF; tx_buffer[12] = from_id & 0xFF;
    tx_buffer[13] = (to_id >> 8) & 0xFF; tx_buffer[14] = to_id & 0xFF;
    memcpy(&tx_buffer[15], alias_buf, 16);
    memcpy(&tx_buffer[31], "Greetz from Cy9!", 16);
    
    uint32_t tally = 0;
    for(int i = 8; i < 47; i++) tally += tx_buffer[i];
    tx_buffer[4] = (tally >> 24) & 0xFF; tx_buffer[5] = (tally >> 16) & 0xFF;
    tx_buffer[6] = (tally >> 8) & 0xFF; tx_buffer[7] = tally & 0xFF;
    
    // 2. Convert to timing
    global_burst.count = 0;
    global_burst.index = 0;
    bool current_level = false;
    uint32_t current_duration = 0;

    for(int i = 46; i >= 0; i--) {
        uint8_t byte = tx_buffer[i];
        for(int b = 0; b < 10; b++) {
            bool bit;
            if(b == 0) bit = false; // Start
            else if(b == 9) bit = true; // Stop
            else bit = (byte & (1 << (b - 1))) != 0; // Data
            
            bool level = !bit;
            if(i == 46 && b == 0) {
                current_level = level;
                current_duration = BIT_TIME_US;
            } else if(level == current_level) {
                current_duration += BIT_TIME_US;
            } else {
                if(global_burst.count < MAX_DURATIONS)
                    global_burst.durations[global_burst.count++] = current_duration;
                current_level = level;
                current_duration = BIT_TIME_US;
            }
        }
    }
    if(global_burst.count < MAX_DURATIONS)
        global_burst.durations[global_burst.count++] = current_duration;

    // 3. Send
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
