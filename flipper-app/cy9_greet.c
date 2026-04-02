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

typedef struct {
    uint8_t packet[47];
    uint32_t bit_index;
    uint32_t total_bits;
} Cy9TxContext;

static Cy9TxContext tx_ctx;

typedef enum {
    Cy9WorkerEventStop,
    Cy9WorkerEventTx,
} Cy9WorkerEvent;

static FuriHalInfraredTxGetDataState cy9_tx_callback(void* context, uint32_t* duration, bool* level) {
    Cy9TxContext* ctx = context;
    if(ctx->bit_index >= ctx->total_bits) return FuriHalInfraredTxGetDataStateLastDone;
    
    uint32_t byte_index = ctx->bit_index / 10;
    uint32_t bit_in_byte = ctx->bit_index % 10;
    uint8_t byte = ctx->packet[byte_index];
    
    bool bit_val;
    if(bit_in_byte == 0) bit_val = false; // Start
    else if(bit_in_byte == 9) bit_val = true; // Stop
    else bit_val = (byte & (1 << (bit_in_byte - 1))) != 0; // Data
    
    *duration = BIT_TIME_US;
    *level = !bit_val; 
    ctx->bit_index++;
    
    return (ctx->bit_index >= ctx->total_bits) ? FuriHalInfraredTxGetDataStateDone : FuriHalInfraredTxGetDataStateOk;
}

static int32_t cy9_worker_thread(void* context) {
    FuriMessageQueue* worker_queue = context;
    Cy9WorkerEvent event;
    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);

    while(furi_message_queue_get(worker_queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event == Cy9WorkerEventStop) break;
        if(event == Cy9WorkerEventTx) {
            if(furi_hal_infrared_is_busy()) furi_hal_infrared_async_tx_stop();
            
            notification_message(notifications, &sequence_blink_blue_100);
            furi_hal_power_insomnia_enter();
            
            furi_hal_infrared_async_tx_set_data_isr_callback(cy9_tx_callback, &tx_ctx);
            furi_hal_infrared_async_tx_start(CARRIER_FREQ, 0.5f);
            furi_hal_infrared_async_tx_wait_termination();
            furi_hal_infrared_async_tx_stop();
            
            furi_hal_power_insomnia_exit();
        }
    }

    furi_record_close(RECORD_NOTIFICATION);
    return 0;
}

void cy9_broadcast_greeting(FuriMessageQueue* worker_queue) {
    if(furi_hal_infrared_is_busy()) return;

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
    
    for(int i = 0; i < 47; i++) tx_ctx.packet[i] = tx_buffer[46 - i];
    tx_ctx.bit_index = 0;
    tx_ctx.total_bits = 47 * 10;
    
    Cy9WorkerEvent event = Cy9WorkerEventTx;
    furi_message_queue_put(worker_queue, &event, 0);
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
    FuriMessageQueue* worker_queue = furi_message_queue_alloc(1, sizeof(Cy9WorkerEvent));
    FuriThread* thread = furi_thread_alloc_ex("Cy9Worker", 1024, cy9_worker_thread, worker_queue);
    furi_thread_start(thread);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, cy9_greet_draw_callback, NULL);
    view_port_input_callback_set(view_port, cy9_greet_input_callback, event_queue);
    
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    
    InputEvent event;
    while(furi_message_queue_get(event_queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.type == InputTypeShort && event.key == InputKeyOk) {
            cy9_broadcast_greeting(worker_queue);
        } else if(event.key == InputKeyBack) {
            break;
        }
    }
    
    Cy9WorkerEvent stop_event = Cy9WorkerEventStop;
    furi_message_queue_put(worker_queue, &stop_event, FuriWaitForever);
    furi_thread_join(thread);
    furi_thread_free(thread);
    furi_message_queue_free(worker_queue);

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_record_close(RECORD_GUI);
    
    return 0;
}
