#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_infrared.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

/*
 * CY9 IR Protocol Specs:
 * - 38kHz Carrier
 * - 3000 Baud
 * - 8N1 (Start=0, 8 Data LSB-first, Stop=1)
 */

#define BAUD_RATE 3000
#define BIT_TIME_US (1000000 / BAUD_RATE)
#define CARRIER_FREQ 38000
#define MAX_DURATIONS 1000
#define MAX_LOGGED_IDS 10
#define SETTINGS_PATH "/ext/apps/Infrared/cy9_remote.settings"

// Custom Badge Font Icons
#define ICON_HEART   0x0F
#define ICON_SPADE   0x10
#define ICON_DIAMOND 0x12

typedef enum {
    Cy9ViewSubmenu,
    Cy9ViewSniffer,
    Cy9ViewTextInput,
    Cy9ViewVariableList,
} Cy9View;

typedef enum {
    Cy9ClassGhost,
    Cy9ClassFounder,
    Cy9ClassExtreme,
    Cy9ClassLifetime,
    Cy9ClassSpeaker,
    Cy9ClassGeneral,
    Cy9ClassVendor,
    Cy9ClassChaos,
    Cy9ClassCount
} Cy9BadgeClass;

const char* class_names[] = {"Ghost", "Founder", "Extreme", "Lifetime", "Speaker", "General", "Vendor", "Chaos"};

typedef struct {
    uint32_t durations[MAX_DURATIONS];
    uint32_t count;
    uint32_t index;
} Cy9Burst;

typedef struct {
    bool level;
    uint32_t duration;
} Cy9RxMessage;

typedef struct {
    uint16_t logged_ids[MAX_LOGGED_IDS];
    uint8_t logged_count;
    int8_t selected_index;
    uint32_t total_packets;
} Cy9SnifferModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    View* sniffer_view;
    TextInput* text_input;
    VariableItemList* variable_list;
    NotificationApp* notifications;
    
    FuriThread* rx_thread;
    FuriMessageQueue* rx_queue;
    
    Cy9Burst* tx_burst;
    char greet_msg[17];
    char founder_msg[17];
    Cy9BadgeClass selected_class;
    
    volatile bool sniffing;
    volatile bool rx_active;
    volatile bool running;
    Cy9View current_view;
} Cy9RemoteApp;

void cy9_save_settings(Cy9RemoteApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, app->greet_msg, 17);
        storage_file_write(file, app->founder_msg, 17);
        uint8_t cls = (uint8_t)app->selected_class;
        storage_file_write(file, &cls, 1);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

void cy9_load_settings(Cy9RemoteApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_read(file, app->greet_msg, 17);
        storage_file_read(file, app->founder_msg, 17);
        uint8_t cls;
        if(storage_file_read(file, &cls, 1) == 1) app->selected_class = (Cy9BadgeClass)cls;
    } else {
        snprintf(app->greet_msg, 17, "Greetz from Cy9!");
        snprintf(app->founder_msg, 17, "Obey the system.");
        app->selected_class = Cy9ClassExtreme;
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static FuriHalInfraredTxGetDataState cy9_tx_callback(void* context, uint32_t* duration, bool* level) {
    Cy9Burst* b = (Cy9Burst*)context;
    if(!b || b->index >= b->count) return FuriHalInfraredTxGetDataStateLastDone;
    *duration = b->durations[b->index];
    *level = (b->index % 2 == 0); 
    b->index++;
    return (b->index >= b->count) ? FuriHalInfraredTxGetDataStateDone : FuriHalInfraredTxGetDataStateOk;
}

static void cy9_add_duration(Cy9Burst* burst, uint32_t duration, bool level, uint32_t* current_duration, bool* current_level) {
    if(burst->count == 0 && *current_duration == 0) {
        *current_level = level;
        *current_duration = duration;
    } else if(level == *current_level) {
        *current_duration += duration;
    } else {
        if(burst->count < MAX_DURATIONS) burst->durations[burst->count++] = *current_duration;
        *current_level = level;
        *current_duration = duration;
    }
}

uint16_t cy9_get_random_id(Cy9BadgeClass cls) {
    switch(cls) {
        case Cy9ClassGhost: return 0;
        case Cy9ClassFounder: return (furi_hal_random_get() % 25) + 1;
        case Cy9ClassExtreme: return (furi_hal_random_get() % 75) + 26;
        case Cy9ClassLifetime: return (furi_hal_random_get() % 10) + 101;
        case Cy9ClassSpeaker: return (furi_hal_random_get() % 110) + 111;
        case Cy9ClassGeneral: return (furi_hal_random_get() % 440) + 221;
        case Cy9ClassVendor: return (furi_hal_random_get() % 15) + 661;
        case Cy9ClassChaos: return furi_hal_random_get() % 676;
        default: return 1;
    }
}

void cy9_send_packet(Cy9RemoteApp* app, uint16_t from_id, uint16_t to_id, uint8_t event_id, const char* alias, const char* msg) {
    if(!app || furi_hal_infrared_is_busy()) return;
    uint8_t packet[47] = {0};
    uint16_t body_len = 32;
    packet[0] = 22; packet[1] = 22; packet[2] = 22; packet[3] = 22;
    packet[8] = (body_len >> 8) & 0xFF; packet[9] = body_len & 0xFF;
    packet[10] = event_id;
    packet[11] = (from_id >> 8) & 0xFF; packet[12] = from_id & 0xFF;
    packet[13] = (to_id >> 8) & 0xFF; packet[14] = to_id & 0xFF;
    char alias_buf[16], msg_buf[16];
    memset(alias_buf, ' ', 16); memset(msg_buf, ' ', 16);
    
    if(alias) {
        size_t len = strlen(alias);
        if(len > 14) len = 14;
        memcpy(alias_buf, alias, len);
        
        // Correct Icon Injection Logic
        if(from_id >= 1 && from_id <= 25) { // Founder
            alias_buf[len] = ' ';
            alias_buf[len+1] = ICON_DIAMOND;
        } else if(from_id >= 26 && from_id <= 100) { // Extreme
            alias_buf[len] = ' ';
            alias_buf[len+1] = ICON_SPADE;
        }
    }
    
    if(msg) memcpy(msg_buf, msg, (strlen(msg) > 16) ? 16 : strlen(msg));
    memcpy(&packet[15], alias_buf, 16); memcpy(&packet[31], msg_buf, 16);
    uint32_t tally = 0;
    for(int i = 8; i < 47; i++) tally += packet[i];
    packet[4] = (tally >> 24) & 0xFF; packet[5] = (tally >> 16) & 0xFF;
    packet[6] = (tally >> 8) & 0xFF; packet[7] = tally & 0xFF;
    app->tx_burst->count = 0; app->tx_burst->index = 0;
    uint32_t current_duration = 0; bool current_level = false; uint32_t last_time_us = 0;
    for(uint32_t bit_idx = 0; bit_idx < 47 * 10; bit_idx++) {
        uint8_t byte = packet[46 - (bit_idx / 10)];
        uint32_t b = bit_idx % 10;
        bool bit = (b == 0) ? false : (b == 9) ? true : (byte & (1 << (b - 1))) != 0;
        uint32_t next_time_us = (uint32_t)((float)(bit_idx + 1) * (1000000.0f / 3000.0f));
        cy9_add_duration(app->tx_burst, next_time_us - last_time_us, !bit, &current_duration, &current_level);
        last_time_us = next_time_us;
    }
    if(current_duration > 0) app->tx_burst->durations[app->tx_burst->count++] = current_duration;
    furi_hal_power_insomnia_enter();
    furi_hal_infrared_async_tx_set_data_isr_callback(cy9_tx_callback, app->tx_burst);
    furi_hal_infrared_async_tx_start(CARRIER_FREQ, 0.5f);
    furi_hal_infrared_async_tx_wait_termination();
    furi_hal_power_insomnia_exit();
}

static void sniffer_draw_callback(Canvas* canvas, void* model) {
    Cy9SnifferModel* m = model;
    if(!m) return;
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "IR Sniffer Log");
    canvas_set_font(canvas, FontSecondary);
    if(m->logged_count == 0) {
        canvas_draw_str(canvas, 0, 30, "Scanning for badges...");
    } else {
        for(uint8_t i = 0; i < m->logged_count; i++) {
            char buf[16]; snprintf(buf, 16, "Badge ID: %03d", m->logged_ids[i]);
            if(i == m->selected_index) {
                canvas_draw_str(canvas, 0, 22 + (i * 10), ">");
                canvas_draw_str(canvas, 10, 22 + (i * 10), buf);
            } else {
                canvas_draw_str(canvas, 10, 22 + (i * 10), buf);
            }
        }
        canvas_draw_str(canvas, 80, 60, "OK: Greet");
    }
}

static bool sniffer_input_callback(InputEvent* event, void* context) {
    Cy9RemoteApp* app = context;
    if(!app) return false;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyDown || event->key == InputKeyUp) {
            with_view_model(app->sniffer_view, Cy9SnifferModel * model, {
                if(model->logged_count > 0) {
                    if(event->key == InputKeyDown) model->selected_index = (model->selected_index + 1) % model->logged_count;
                    else model->selected_index = (model->selected_index - 1 + model->logged_count) % model->logged_count;
                }
            }, true);
            return true;
        } else if(event->key == InputKeyOk) {
            uint16_t target = 0; bool has_target = false;
            with_view_model(app->sniffer_view, Cy9SnifferModel * model, {
                if(model->selected_index >= 0) { target = model->logged_ids[model->selected_index]; has_target = true; }
            }, false);
            if(has_target) {
                cy9_send_packet(app, cy9_get_random_id(app->selected_class), target, 3, furi_hal_version_get_name_ptr(), app->greet_msg);
                notification_message(app->notifications, &sequence_blink_blue_100);
            }
            return true;
        }
    }
    return false;
}

static void cy9_rx_capture_callback(void* context, bool level, uint32_t duration) {
    Cy9RemoteApp* app = context;
    if(!app || !app->sniffing || !app->rx_queue) return;
    Cy9RxMessage msg = {level, duration};
    furi_message_queue_put(app->rx_queue, &msg, 0);
}

static int32_t cy9_rx_thread(void* context) {
    Cy9RemoteApp* app = context;
    if(!app) return -1;
    Cy9RxMessage msg;
    typedef enum { DecodeStateIdle, DecodeStateData } DecodeState;
    DecodeState state = DecodeStateIdle;
    uint32_t bit_acc = 0, bits = 0;
    uint8_t pkt_circ[47] = {0}, p_idx = 0;
    while(app->running) {
        if(furi_message_queue_get(app->rx_queue, &msg, 100) == FuriStatusOk) {
            if(!app->sniffing) continue;
            bool bit = !msg.level;
            int n = (int)(( (float)msg.duration + (BIT_TIME_US / 2.0f) ) / BIT_TIME_US);
            if(n <= 0) n = 1;
            for(int i = 0; i < n; i++) {
                if(state == DecodeStateIdle) {
                    if(!bit) { state = DecodeStateData; bit_acc = 0; bits = 0; }
                } else {
                    if(bits < 8) { if(bit) bit_acc |= (1 << bits); bits++; }
                    else {
                        pkt_circ[p_idx] = (uint8_t)bit_acc;
                        if(pkt_circ[p_idx] == 22) {
                            uint16_t id = (pkt_circ[(p_idx + 47 - 11) % 47] << 8) | pkt_circ[(p_idx + 47 - 12) % 47];
                            bool known = false;
                            with_view_model(app->sniffer_view, Cy9SnifferModel * model, {
                                for(int k=0; k<model->logged_count; k++) if(model->logged_ids[k] == id) known = true;
                                if(!known && id > 0 && model->logged_count < MAX_LOGGED_IDS) {
                                    model->logged_ids[model->logged_count++] = id;
                                    if(model->selected_index < 0) model->selected_index = 0;
                                    notification_message(app->notifications, &sequence_blink_green_100);
                                }
                                model->total_packets++;
                            }, true);
                        }
                        p_idx = (p_idx + 1) % 47;
                        state = DecodeStateIdle;
                        if(!bit) { state = DecodeStateData; bit_acc = 0; bits = 0; }
                    }
                }
            }
        }
    }
    return 0;
}

static void text_input_done_callback(void* context) {
    Cy9RemoteApp* app = context;
    cy9_save_settings(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, Cy9ViewSubmenu);
}

static void class_change_callback(VariableItem* item) {
    Cy9RemoteApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->selected_class = (Cy9BadgeClass)index;
    variable_item_set_current_value_text(item, class_names[index]);
    cy9_save_settings(app);
}

static void submenu_callback(void* context, uint32_t index) {
    Cy9RemoteApp* app = context;
    if(!app) return;
    const char* name = furi_hal_version_get_name_ptr();
    if(index == 4) {
        app->sniffing = true; app->rx_active = true;
        furi_hal_infrared_async_rx_set_capture_isr_callback(cy9_rx_capture_callback, app);
        furi_hal_infrared_async_rx_start();
        app->current_view = Cy9ViewSniffer;
        view_dispatcher_switch_to_view(app->view_dispatcher, Cy9ViewSniffer);
    } else if(index == 5 || index == 6) {
        char* target = (index == 5) ? app->greet_msg : app->founder_msg;
        text_input_set_header_text(app->text_input, (index == 5) ? "Greet Msg" : "Founder Msg");
        text_input_set_result_callback(app->text_input, text_input_done_callback, app, target, 17, true);
        app->current_view = Cy9ViewTextInput;
        view_dispatcher_switch_to_view(app->view_dispatcher, Cy9ViewTextInput);
    } else if(index == 7) {
        app->current_view = Cy9ViewVariableList;
        view_dispatcher_switch_to_view(app->view_dispatcher, Cy9ViewVariableList);
    } else {
        if(index == 0) cy9_send_packet(app, cy9_get_random_id(app->selected_class), 0, 3, name, app->greet_msg);
        else if(index == 1) cy9_send_packet(app, cy9_get_random_id(Cy9ClassFounder), 0, 3, name, app->founder_msg);
        else if(index == 2) for(int i=0; i<5; i++) cy9_send_packet(app, cy9_get_random_id(app->selected_class), 0, 4, "NUKE", "Flood...");
        else if(index == 3) for(int i=1; i<6; i++) cy9_send_packet(app, cy9_get_random_id(Cy9ClassChaos), 0, 4, "CHAOS", "Flood...");
        notification_message(app->notifications, &sequence_blink_blue_100);
    }
}

static bool cy9_navigation_callback(void* context) {
    Cy9RemoteApp* app = context;
    if(app && app->current_view != Cy9ViewSubmenu) {
        app->sniffing = false;
        if(app->rx_active) { furi_hal_infrared_async_rx_stop(); app->rx_active = false; }
        app->current_view = Cy9ViewSubmenu;
        view_dispatcher_switch_to_view(app->view_dispatcher, Cy9ViewSubmenu);
        return true;
    }
    return false;
}

int32_t cy9_remote_app(void* p) {
    UNUSED(p);
    Cy9RemoteApp* app = malloc(sizeof(Cy9RemoteApp));
    furi_check(app); memset(app, 0, sizeof(Cy9RemoteApp));
    app->tx_burst = malloc(sizeof(Cy9Burst)); furi_check(app->tx_burst);
    cy9_load_settings(app);
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->rx_queue = furi_message_queue_alloc(128, sizeof(Cy9RxMessage));
    furi_check(app->gui && app->notifications && app->rx_queue);
    app->running = true;
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, cy9_navigation_callback);
    app->submenu = submenu_alloc();
    submenu_set_header(app->submenu, "Cy9 Remote");
    submenu_add_item(app->submenu, "Quick Greet", 0, submenu_callback, app);
    submenu_add_item(app->submenu, "Spoof Founder", 1, submenu_callback, app);
    submenu_add_item(app->submenu, "Inbox Nuke", 2, submenu_callback, app);
    submenu_add_item(app->submenu, "Chaos Mode", 3, submenu_callback, app);
    submenu_add_item(app->submenu, "Sniffer Log", 4, submenu_callback, app);
    submenu_add_item(app->submenu, "Config Greet", 5, submenu_callback, app);
    submenu_add_item(app->submenu, "Config Founder", 6, submenu_callback, app);
    submenu_add_item(app->submenu, "Identity", 7, submenu_callback, app);
    app->sniffer_view = view_alloc();
    view_allocate_model(app->sniffer_view, ViewModelTypeLockFree, sizeof(Cy9SnifferModel));
    view_set_draw_callback(app->sniffer_view, sniffer_draw_callback);
    view_set_input_callback(app->sniffer_view, sniffer_input_callback);
    view_set_context(app->sniffer_view, app);
    app->text_input = text_input_alloc();
    app->variable_list = variable_item_list_alloc();
    VariableItem* item = variable_item_list_add(app->variable_list, "Badge Class", Cy9ClassCount, class_change_callback, app);
    variable_item_set_current_value_index(item, app->selected_class);
    variable_item_set_current_value_text(item, class_names[app->selected_class]);
    view_dispatcher_add_view(app->view_dispatcher, Cy9ViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, Cy9ViewSniffer, app->sniffer_view);
    view_dispatcher_add_view(app->view_dispatcher, Cy9ViewTextInput, text_input_get_view(app->text_input));
    view_dispatcher_add_view(app->view_dispatcher, Cy9ViewVariableList, variable_item_list_get_view(app->variable_list));
    view_dispatcher_switch_to_view(app->view_dispatcher, Cy9ViewSubmenu);
    app->current_view = Cy9ViewSubmenu;
    app->rx_thread = furi_thread_alloc_ex("Cy9Rx", 2048, cy9_rx_thread, app);
    furi_thread_start(app->rx_thread);
    furi_hal_infrared_set_tx_output(FuriHalInfraredTxPinInternal);
    view_dispatcher_run(app->view_dispatcher);
    app->running = false;
    if(app->rx_active) furi_hal_infrared_async_rx_stop();
    furi_thread_join(app->rx_thread); furi_thread_free(app->rx_thread);
    furi_message_queue_free(app->rx_queue);
    view_dispatcher_remove_view(app->view_dispatcher, Cy9ViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, Cy9ViewSniffer);
    view_dispatcher_remove_view(app->view_dispatcher, Cy9ViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, Cy9ViewVariableList);
    submenu_free(app->submenu); view_free(app->sniffer_view); text_input_free(app->text_input); variable_item_list_free(app->variable_list);
    view_dispatcher_free(app->view_dispatcher); furi_record_close(RECORD_GUI); furi_record_close(RECORD_NOTIFICATION);
    free(app->tx_burst); free(app);
    return 0;
}
