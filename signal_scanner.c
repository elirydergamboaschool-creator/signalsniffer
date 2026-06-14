#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <notification/notification_messages.h>
#include <lib/subghz/subghz_setting.h>
#include <lib/subghz/subghz_worker.h>
#include <infrared_worker.h>
#include <furi_hal_infrared.h>
#include <furi_hal_subghz.h>
#include <furi_hal_serial.h>
#include <string.h>
#include <stdio.h>

#define TAG "SignalScanner"

typedef enum {
    SceneMain,
    SceneSubGHz,
    SceneInfrared,
    SceneWiFi,
    SceneCount,
} SceneId;

typedef enum {
    ViewSubmenu,
    ViewTextBox,
} ViewId;

typedef struct {
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextBox* text_box;
    SubGhzWorker* subghz_worker;
    FuriString* subghz_log;
    InfraredWorker* ir_worker;
    FuriString* ir_log;
    FuriString* wifi_log;
} App;

static void subghz_rx_callback(SubGhzWorker* worker, void* context) {
    App* app = context;
    UNUSED(worker);
    SubGhzProtocolDecoderBase* decoder = subghz_worker_get_decoder(app->subghz_worker);
    if(!decoder) return;
    FuriString* tmp = furi_string_alloc();
    subghz_protocol_decoder_base_get_string(decoder, tmp);
    if(furi_string_size(tmp) > 0) {
        furi_string_cat(app->subghz_log, tmp);
        furi_string_cat_str(app->subghz_log, "\n");
        if(furi_string_size(app->subghz_log) > 2048)
            furi_string_left(app->subghz_log, 2048);
        text_box_set_text(app->text_box, furi_string_get_cstr(app->subghz_log));
    }
    furi_string_free(tmp);
}

static void ir_received_callback(void* context, InfraredWorkerSignal* received_signal) {
    App* app = context;
    char buf[64];
    if(infrared_worker_signal_is_decoded(received_signal)) {
        const InfraredMessage* msg = infrared_worker_get_decoded_signal(received_signal);
        snprintf(buf, sizeof(buf), "[IR] %s addr=0x%lX cmd=0x%lX\n",
            infrared_get_protocol_name(msg->protocol),
            (unsigned long)msg->address,
            (unsigned long)msg->command);
    } else {
        const uint32_t* timings;
        size_t cnt;
        infrared_worker_get_raw_signal(received_signal, &timings, &cnt);
        snprintf(buf, sizeof(buf), "[IR-RAW] %zu pulses\n", cnt);
    }
    furi_string_cat_str(app->ir_log, buf);
    if(furi_string_size(app->ir_log) > 2048)
        furi_string_left(app->ir_log, 2048);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->ir_log));
}

static void wifi_scan_start(App* app) {
    FuriHalSerialHandle* handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(!handle) {
        furi_string_cat_str(app->wifi_log, "Serial unavailable\n");
        text_box_set_text(app->text_box, furi_string_get_cstr(app->wifi_log));
        return;
    }
    furi_hal_serial_init(handle, 115200);
    const char* cmd = "SCAN\n";
    furi_hal_serial_tx(handle, (const uint8_t*)cmd, strlen(cmd));
    uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(3000);
    char line[128];
    size_t pos = 0;
    while(furi_get_tick() < deadline) {
        uint8_t byte = 0;
        if(furi_hal_serial_rx(handle, &byte, 1, 10)) {
            if(byte == '\n' || pos >= sizeof(line) - 1) {
                line[pos] = '\0';
                if(pos > 0) {
                    furi_string_cat_str(app->wifi_log, line);
                    furi_string_cat_str(app->wifi_log, "\n");
                }
                pos = 0;
            } else {
                line[pos++] = (char)byte;
            }
        }
    }
    if(furi_string_size(app->wifi_log) <= 24)
        furi_string_cat_str(app->wifi_log, "(no response - check board)\n");
    furi_hal_serial_deinit(handle);
    furi_hal_serial_control_release(handle);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->wifi_log));
}

typedef enum { MenuSubGHz, MenuInfrared, MenuWiFi } MenuIndex;

static void scene_main_on_enter(void* context) {
    App* app = context;
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Signal Scanner");
    submenu_add_item(app->submenu, "Sub-GHz",  MenuSubGHz,   NULL, app);
    submenu_add_item(app->submenu, "Infrared", MenuInfrared, NULL, app);
    submenu_add_item(app->submenu, "WiFi",     MenuWiFi,     NULL, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewSubmenu);
}

static bool scene_main_on_event(void* context, SceneManagerEvent event) {
    App* app = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == MenuSubGHz)
            scene_manager_next_scene(app->scene_manager, SceneSubGHz);
        else if(event.event == MenuInfrared)
            scene_manager_next_scene(app->scene_manager, SceneInfrared);
        else if(event.event == MenuWiFi)
            scene_manager_next_scene(app->scene_manager, SceneWiFi);
        return true;
    }
    return false;
}
static void scene_main_on_exit(void* context) { UNUSED(context); }

static void scene_subghz_on_enter(void* context) {
    App* app = context;
    furi_string_reset(app->subghz_log);
    furi_string_cat_str(app->subghz_log, "Sub-GHz Scanning\n433.92 MHz OOK\n\n");
    text_box_set_text(app->text_box, furi_string_get_cstr(app->subghz_log));
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewTextBox);
    app->subghz_worker = subghz_worker_alloc();
    subghz_worker_set_overrun_callback(app->subghz_worker, subghz_rx_callback, app);
    furi_hal_subghz_reset();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);
    furi_hal_subghz_set_frequency_and_path(433920000);
    subghz_worker_start(app->subghz_worker);
}
static bool scene_subghz_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context); UNUSED(event); return false;
}
static void scene_subghz_on_exit(void* context) {
    App* app = context;
    if(app->subghz_worker) {
        subghz_worker_stop(app->subghz_worker);
        subghz_worker_free(app->subghz_worker);
        app->subghz_worker = NULL;
    }
    furi_hal_subghz_sleep();
}

static void scene_infrared_on_enter(void* context) {
    App* app = context;
    furi_string_reset(app->ir_log);
    furi_string_cat_str(app->ir_log, "IR Scanning\nPoint remote at Flipper\n\n");
    text_box_set_text(app->text_box, furi_string_get_cstr(app->ir_log));
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewTextBox);
    app->ir_worker = infrared_worker_alloc();
    infrared_worker_rx_set_received_signal_callback(app->ir_worker, ir_received_callback, app);
    infrared_worker_rx_start(app->ir_worker);
}
static bool scene_infrared_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context); UNUSED(event); return false;
}
static void scene_infrared_on_exit(void* context) {
    App* app = context;
    if(app->ir_worker) {
        infrared_worker_rx_stop(app->ir_worker);
        infrared_worker_free(app->ir_worker);
        app->ir_worker = NULL;
    }
}

static void scene_wifi_on_enter(void* context) {
    App* app = context;
    furi_string_reset(app->wifi_log);
    furi_string_cat_str(app->wifi_log, "WiFi Scan\nRequesting ESP32...\n\n");
    text_box_set_text(app->text_box, furi_string_get_cstr(app->wifi_log));
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewTextBox);
    wifi_scan_start(app);
}
static bool scene_wifi_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context); UNUSED(event); return false;
}
static void scene_wifi_on_exit(void* context) { UNUSED(context); }

static const SceneManagerHandlers scene_handlers = {
    .on_enter_handlers = {
        [SceneMain]     = scene_main_on_enter,
        [SceneSubGHz]   = scene_subghz_on_enter,
        [SceneInfrared] = scene_infrared_on_enter,
        [SceneWiFi]     = scene_wifi_on_enter,
    },
    .on_event_handlers = {
        [SceneMain]     = scene_main_on_event,
        [SceneSubGHz]   = scene_subghz_on_event,
        [SceneInfrared] = scene_infrared_on_event,
        [SceneWiFi]     = scene_wifi_on_event,
    },
    .on_exit_handlers = {
        [SceneMain]     = scene_main_on_exit,
        [SceneSubGHz]   = scene_subghz_on_exit,
        [SceneInfrared] = scene_infrared_on_exit,
        [SceneWiFi]     = scene_wifi_on_exit,
    },
    .scene_num = SceneCount,
};

static bool nav_callback(void* context) {
    App* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void submenu_callback(void* context, uint32_t index) {
    App* app = context;
    scene_manager_handle_custom_event(app->scene_manager, index);
}

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    app->scene_manager   = scene_manager_alloc(&scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, nav_callback, app);
    app->submenu = submenu_alloc();
    submenu_set_callback(app->submenu, submenu_callback, app);
    view_dispatcher_add_view(app->view_dispatcher, ViewSubmenu, submenu_get_view(app->submenu));
    app->text_box = text_box_alloc();
    view_dispatcher_add_view(app->view_dispatcher, ViewTextBox, text_box_get_view(app->text_box));
    app->subghz_log = furi_string_alloc();
    app->ir_log     = furi_string_alloc();
    app->wifi_log   = furi_string_alloc();
    app->subghz_worker = NULL;
    app->ir_worker     = NULL;
    return app;
}

static void app_free(App* app) {
    view_dispatcher_remove_view(app->view_dispatcher, ViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewTextBox);
    submenu_free(app->submenu);
    text_box_free(app->text_box);
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);
    furi_string_free(app->subghz_log);
    furi_string_free(app->ir_log);
    furi_string_free(app->wifi_log);
    free(app);
}

int32_t signal_scanner_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();
    Gui* gui = furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);
    scene_manager_next_scene(app->scene_manager, SceneMain);
    view_dispatcher_run(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    app_free(app);
    return 0;
}
