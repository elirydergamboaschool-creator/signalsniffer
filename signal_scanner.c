#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <infrared_worker.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <string.h>
#include <stdio.h>

typedef enum { ViewMenu, ViewLog } AppView;
typedef enum { MenuInfrared, MenuWiFi } MenuItem;

typedef struct {
    ViewDispatcher* vd;
    Submenu* menu;
    TextBox* log_box;
    FuriString* log;
    InfraredWorker* irw;
} App;

static void ir_cb(void* ctx, InfraredWorkerSignal* sig) {
    App* app = ctx;
    char buf[64];
    if(infrared_worker_signal_is_decoded(sig)) {
        const InfraredMessage* m = infrared_worker_get_decoded_signal(sig);
        snprintf(buf, sizeof(buf), "[IR] %s addr=0x%lX cmd=0x%lX\n",
            infrared_get_protocol_name(m->protocol),
            (unsigned long)m->address,
            (unsigned long)m->command);
    } else {
        const uint32_t* t;
        size_t n;
        infrared_worker_get_raw_signal(sig, &t, &n);
        snprintf(buf, sizeof(buf), "[RAW] %zu pulses\n", n);
    }
    furi_string_cat_str(app->log, buf);
    if(furi_string_size(app->log) > 2048) furi_string_left(app->log, 2048);
    text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
}

static void stop_all(App* app) {
    if(app->irw) {
        infrared_worker_rx_stop(app->irw);
        infrared_worker_free(app->irw);
        app->irw = NULL;
    }
}

static void menu_cb(void* ctx, uint32_t idx) {
    App* app = ctx;
    stop_all(app);
    furi_string_reset(app->log);
    view_dispatcher_switch_to_view(app->vd, ViewLog);

    if(idx == MenuInfrared) {
        furi_string_set_str(app->log, "IR Scanning\nPoint remote here\n\n");
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
        app->irw = infrared_worker_alloc();
        infrared_worker_rx_set_received_signal_callback(app->irw, ir_cb, app);
        infrared_worker_rx_start(app->irw);

    } else if(idx == MenuWiFi) {
        furi_string_set_str(app->log, "WiFi Scan\nContacting ESP32...\n\n");
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
        FuriHalSerialHandle* h = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
        if(!h) {
            furi_string_cat_str(app->log, "No serial - check WiFi board\n");
        } else {
            furi_hal_serial_init(h, 115200);
            furi_hal_serial_tx(h, (const uint8_t*)"SCAN\n", 5);
            furi_delay_ms(3000);
            furi_hal_serial_deinit(h);
            furi_hal_serial_control_release(h);
            furi_string_cat_str(app->log, "Scan sent!\n");
        }
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
    }
}

static bool back_cb(void* ctx) {
    App* app = ctx;
    stop_all(app);
    view_dispatcher_switch_to_view(app->vd, ViewMenu);
    return true;
}

int32_t signal_scanner_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    app->irw = NULL;
    app->log = furi_string_alloc();

    app->vd = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_navigation_event_callback(app->vd, back_cb);

    app->menu = submenu_alloc();
    submenu_set_header(app->menu, "Signal Scanner");
    submenu_add_item(app->menu, "Infrared", MenuInfrared, menu_cb, app);
    submenu_add_item(app->menu, "WiFi",     MenuWiFi,     menu_cb, app);
    view_dispatcher_add_view(app->vd, ViewMenu, submenu_get_view(app->menu));

    app->log_box = text_box_alloc();
    text_box_set_font(app->log_box, TextBoxFontText);
    view_dispatcher_add_view(app->vd, ViewLog, text_box_get_view(app->log_box));

    Gui* gui = furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(app->vd, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->vd, ViewMenu);
    view_dispatcher_run(app->vd);

    stop_all(app);
    view_dispatcher_remove_view(app->vd, ViewMenu);
    view_dispatcher_remove_view(app->vd, ViewLog);
    submenu_free(app->menu);
    text_box_free(app->log_box);
    view_dispatcher_free(app->vd);
    furi_string_free(app->log);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
