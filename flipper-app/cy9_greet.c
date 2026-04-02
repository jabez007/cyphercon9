#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>

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

static const uint8_t SYNCWORD[4] = {22, 22, 22, 22};

typedef struct {
    uint8_t packet[47];
} Cy9App;

void cy9_send_bit(bool on) {
    if(on) {
        furi_hal_infrared_async_tx_start(CARRIER_FREQ, 0.5f);
    } else {
        furi_hal_infrared_async_tx_stop();
    }
    furi_delay_us(BIT_TIME_US);
}

void cy9_send_byte(uint8_t byte) {
    // Start bit (0) -> IR ON
    cy9_send_bit(true);
    
    // 8 Data bits (LSB-first) -> 0=ON, 1=OFF
    for(int i = 0; i < 8; i++) {
        cy9_send_bit(!(byte & (1 << i)));
    }
    
    // Stop bit (1) -> IR OFF
    cy9_send_bit(false);
}

void cy9_broadcast_greeting() {
    uint8_t tx_buffer[48] = {0};
    uint16_t body_len = 32;
    uint8_t event_id = 4; // Broadcast
    uint16_t from_id = 0; // Ghost
    uint16_t to_id = 0;   // Broadcast

    char alias_buf[16];
    memset(alias_buf, ' ', 16);
    const char* flipper_name = furi_hal_version_get_name_ptr();
    if(flipper_name) {
        size_t name_len = strlen(flipper_name);
        if(name_len > 16) name_len = 16;
        memcpy(alias_buf, flipper_name, name_len);
    }

    const char* msg = "Ukttzm ykgd En9!";   // 16 bytes

    // Build packet as the badge does (Big Endian fields)
    tx_buffer[0] = 22; tx_buffer[1] = 22; tx_buffer[2] = 22; tx_buffer[3] = 22;
    tx_buffer[8] = (body_len >> 8) & 0xFF; tx_buffer[9] = body_len & 0xFF;
    tx_buffer[10] = event_id;
    tx_buffer[11] = (from_id >> 8) & 0xFF; tx_buffer[12] = from_id & 0xFF;
    tx_buffer[13] = (to_id >> 8) & 0xFF; tx_buffer[14] = to_id & 0xFF;
    memcpy(&tx_buffer[15], alias_buf, 16);
    memcpy(&tx_buffer[31], msg, 16);
    
    // Tally checksum (sum of bytes 8 to 46)
    uint32_t tally = 0;
    for(int i = 8; i < 47; i++) tally += tx_buffer[i];
    tx_buffer[4] = (tally >> 24) & 0xFF;
    tx_buffer[5] = (tally >> 16) & 0xFF;
    tx_buffer[6] = (tally >> 8) & 0xFF;
    tx_buffer[7] = tally & 0xFF;
    
    // Send REVERSED on the wire
    furi_hal_power_enable_otg(); // Power up the IR LED
    for(int i = 46; i >= 0; i--) {
        cy9_send_byte(tx_buffer[i]);
    }
    furi_hal_infrared_async_tx_stop();
    furi_hal_power_disable_otg();
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
