#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_infrared.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <notification/notification_messages.h>

/*
 * CY9 IR Protocol Specs:
 * - 38kHz Carrier
 * - 3000 Baud
 * - 8N1 (Start=0, 8 Data LSB-first, Stop=1)
 * - UART LOW = IR ON | UART HIGH = IR OFF
 */

#define BAUD_RATE 3000
#define BIT_TIME_US (1000000 / BAUD_RATE)
#define CARRIER_FREQ 38000
#define MAX_DURATIONS 1000
#define RX_BUF_SIZE 64

typedef struct {
    uint32_t durations[MAX_DURATIONS];
    uint32_t count;
    uint32_t index;
} Cy9Burst;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    View* sniffer_view;
    NotificationApp* notifications;
    
    FuriThread* rx_thread;
    FuriMessageQueue* rx_queue;
    uint16_t last_seen_id;
    bool sniffing;
    uint32_t total_packets;
} Cy9RemoteApp;

static Cy9Burst global_burst;

static FuriHalInfraredTxGetDataState cy9_tx_callback(void* context, uint32_t* duration, bool* level) {
    Cy9Burst* b = (Cy9Burst*)context;
    if(b->index >= b->count) return FuriHalInfraredTxGetDataStateLastDone;
    *duration = b->durations[b->index];
    *level = (b->index % 2 == 0); 
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
        if(global_burst.count < MAX_DURATIONS) global_burst.durations[global_burst.count++] = *current_duration;
        *current_level = level;
        *current_duration = duration;
    }
}

void cy9_send_packet(uint16_t from_id, uint16_t to_id, uint8_t event_id, const char* alias, const char* msg) {
    if(furi_hal_infrared_is_busy()) return;
    uint8_t packet[47] = {0};
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
    global_burst.count = 0; global_burst.index = 0;
    uint32_t current_duration = 0; bool current_level = false; uint32_t last_time_us = 0;
    for(uint32_t bit_idx = 0; bit_idx < 47 * 10; bit_idx++) {
        uint8_t byte = packet[46 - (bit_idx / 10)];
        uint32_t b = bit_idx % 10;
        bool bit = (b == 0) ? false : (b == 9) ? true : (byte & (1 << (b - 1))) != 0;
        uint32_t next_time_us = (uint32_t)((float)(bit_idx + 1) * (1000000.0f / 3000.0f));
        cy9_add_duration(next_time_us - last_time_us, !bit, &current_duration, &current_level);
        last_time_us = next_time_us;
    }
    if(current_duration > 0) global_burst.durations[global_burst.count++] = current_duration;
    furi_hal_infrared_async_tx_set_data_isr_callback(cy9_tx_callback, &global_burst);
    furi_hal_infrared_async_tx_start(CARRIER_FREQ, 0.5f);
    furi_hal_infrared_async_tx_wait_termination();
}

static void sniffer_draw_callback(Canvas* canvas, void* context) {
    Cy9RemoteApp* app = context;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "IR Sniffer (3000 Baud)");
    canvas_set_font(canvas, FontSecondary);
    if(app->last_seen_id == 0) {
        canvas_draw_str(canvas, 0, 30, "Scanning for badges...");
    } else {
        char buf[32]; 
        snprintf(buf, 32, "Last ID Seen: %03d", app->last_seen_id);
        canvas_draw_str(canvas, 0, 30, buf);
        snprintf(buf, 32, "Total Packets: %ld", app->total_packets);
        canvas_draw_str(canvas, 0, 42, buf);
    }
    canvas_draw_str(canvas, 0, 60, "Point Flipper at Badge");
}

static void cy9_rx_capture_callback(void* context, bool level, uint32_t duration) {
    Cy9RemoteApp* app = context;
    struct { bool level; uint32_t duration; } msg = {level, duration};
    furi_message_queue_put(app->rx_queue, &msg, 0);
}

typedef enum {
    DecodeStateIdle,
    DecodeStateData,
} DecodeState;

static int32_t cy9_rx_thread(void* context) {
    Cy9RemoteApp* app = context;
    struct { bool level; uint32_t duration; } msg;
    
    DecodeState state = DecodeStateIdle;
    uint32_t bit_accumulator = 0;
    uint32_t bits_collected = 0;
    uint8_t packet_buf[47];
    uint8_t packet_idx = 0;

    while(app->sniffing) {
        if(furi_message_queue_get(app->rx_queue, &msg, 100) == FuriStatusOk) {
            // Flipper IR receiver: level true = pulse (UART LOW), level false = space (UART HIGH)
            bool uart_bit = !msg.level;
            uint32_t num_bits = (msg.duration + (BIT_TIME_US / 2)) / BIT_TIME_US;
            if(num_bits == 0) num_bits = 1;

            for(uint32_t i = 0; i < num_bits; i++) {
                if(state == DecodeStateIdle) {
                    if(!uart_bit) { // Start bit (0) detected
                        state = DecodeStateData;
                        bit_accumulator = 0;
                        bits_collected = 0;
                    }
                } else {
                    if(bits_collected < 8) {
                        if(uart_bit) bit_accumulator |= (1 << bits_collected);
                        bits_collected++;
                    } else { // 9th bit is Stop Bit (1)
                        uint8_t byte = (uint8_t)bit_accumulator;
                        
                        // Badge protocol: Look for Syncwords (0x16)
                        if(byte == 22) {
                            packet_idx = 0;
                            packet_buf[packet_idx++] = byte;
                        } else if(packet_idx > 0 && packet_idx < 47) {
                            packet_buf[packet_idx++] = byte;
                            if(packet_idx == 13) { // Bytes 11-12 are From ID (Big Endian)
                                app->last_seen_id = (packet_buf[11] << 8) | packet_buf[12];
                                app->total_packets++;
                                notification_message(app->notifications, &sequence_blink_green_100);
                            }
                        }
                        state = DecodeStateIdle;
                    }
                }
            }
        }
    }
    return 0;
}

static void submenu_callback(void* context, uint32_t index) {
    Cy9RemoteApp* app = context;
    if(index == 4) {
        app->sniffing = true;
        furi_hal_infrared_async_rx_set_capture_isr_callback(cy9_rx_capture_callback, app);
        furi_hal_infrared_async_rx_start();
        view_dispatcher_switch_to_view(app->view_dispatcher, 1);
    } else {
        if(index == 0) cy9_send_packet(1, 0, 3, "Flipper", "Quick Greet!");
        else if(index == 1) cy9_send_packet(1, 0, 3, "FOUNDER", "Obey.");
        else if(index == 2) for(int i=0; i<5; i++) cy9_send_packet(0, 0, 4, "NUKE", "Flood...");
        else if(index == 3) for(int i=1; i<6; i++) cy9_send_packet(i, 0, 4, "CHAOS", "Flood...");
        notification_message(app->notifications, &sequence_blink_blue_100);
    }
}

static bool cy9_navigation_callback(void* context) {
    Cy9RemoteApp* app = context;
    if(app->sniffing) {
        app->sniffing = false;
        furi_hal_infrared_async_rx_stop();
        view_dispatcher_switch_to_view(app->view_dispatcher, 0);
        return true;
    }
    return false;
}

int32_t cy9_remote_app(void* p) {
    UNUSED(p);
    Cy9RemoteApp* app = malloc(sizeof(Cy9RemoteApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->last_seen_id = 0;
    app->total_packets = 0;
    app->sniffing = false;
    app->rx_queue = furi_message_queue_alloc(128, 8);
    
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, cy9_navigation_callback);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    app->submenu = submenu_alloc();
    submenu_set_header(app->submenu, "Cy9 Remote");
    submenu_add_item(app->submenu, "Quick Greet", 0, submenu_callback, app);
    submenu_add_item(app->submenu, "Spoof Founder", 1, submenu_callback, app);
    submenu_add_item(app->submenu, "Inbox Nuke", 2, submenu_callback, app);
    submenu_add_item(app->submenu, "Chaos Mode", 3, submenu_callback, app);
    submenu_add_item(app->submenu, "Sniffer", 4, submenu_callback, app);
    
    app->sniffer_view = view_alloc();
    view_set_draw_callback(app->sniffer_view, sniffer_draw_callback);
    view_set_context(app->sniffer_view, app);
    
    view_dispatcher_add_view(app->view_dispatcher, 0, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, 1, app->sniffer_view);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
    
    app->rx_thread = furi_thread_alloc_ex("Cy9Rx", 1024, cy9_rx_thread, app);
    furi_thread_start(app->rx_thread);

    furi_hal_infrared_set_tx_output(FuriHalInfraredTxPinInternal);
    view_dispatcher_run(app->view_dispatcher);
    
    app->sniffing = false;
    furi_thread_join(app->rx_thread);
    furi_thread_free(app->rx_thread);
    furi_message_queue_free(app->rx_queue);
    
    view_dispatcher_remove_view(app->view_dispatcher, 0);
    view_dispatcher_remove_view(app->view_dispatcher, 1);
    submenu_free(app->submenu);
    view_free(app->sniffer_view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
    return 0;
}
