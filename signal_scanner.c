#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <infrared_worker.h>
#include <furi_hal_subghz.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <string.h>
#include <stdio.h>

typedef enum { ViewMenu, ViewLog } AppView;
typedef enum {
    MenuSubGHz,
    MenuInfrared,
    MenuBluetooth,
    MenuWiFi,
    MenuFrequency,
    MenuAllSignals,
} MenuItem;

typedef struct {
    ViewDispatcher* vd;
    Submenu* menu;
    TextBox* log_box;
    FuriString* log;
    InfraredWorker* irw;
    FuriTimer* scan_timer;
    bool subghz_on;
    bool all_signals;
    uint32_t freq_index;
} App;

static const uint32_t FREQS[] = {
    300000000, 315000000, 345000000, 390000000,
    418000000, 433920000, 438000000, 868350000, 915000000,
};
static const int FREQS_COUNT = 9;

static void subghz_rx_cb(void* ctx) {
    App* app = ctx;
    float rssi = furi_hal_subghz_get_rssi();
    if(rssi > -90.0f) {
        char buf[48];
        uint32_t f = FREQS[app->freq_index % FREQS_COUNT];
        snprintf(buf, sizeof(buf), "[SubGHz] %.0f MHz RSSI:%.0f\n",
            (double)(f / 1000000.0f), (double)rssi);
        furi_string_cat_str(app->log, buf);
        if(furi_string_size(app->log) > 2048) furi_string_left(app->log, 2048);
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
    }
}

static void ir_cb(void* ctx, InfraredWorkerSignal* sig) {
    App* app = ctx;
    char buf[72];
    if(infrared_worker_signal_is_decoded(sig)) {
        const InfraredMessage* m = infrared_worker_get_decoded_signal(sig);
        snprintf(buf, sizeof(buf), "[IR] %s addr=0x%lX cmd=0x%lX\n",
            infrared_get_protocol_name(m->protocol),
            (unsigned long)m->address,
            (unsigned long)m->command);
    } else {
        const uint32_t* t; size_t n;
        infrared_worker_get_raw_signal(sig, &t, &n);
        snprintf(buf, sizeof(buf), "[IR-RAW] %zu pulses\n", n);
    }
    furi_string_cat_str(app->log, buf);
    if(furi_string_size(app->log) > 2048) furi_string_left(app->log, 2048);
    text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
}

static void freq_timer_cb(void* ctx) {
    App* app = ctx;
    if(!app->subghz_on) return;
    float rssi = furi_hal_subghz_get_rssi();
    uint32_t f = FREQS[app->freq_index % FREQS_COUNT];
    char buf[48];
    snprintf(buf, sizeof(buf), "%.3f MHz | RSSI: %.1f dBm\n",
        (double)(f / 1000000.0f), (double)rssi);
    furi_string_cat_str(app->log, buf);
    if(furi_string_size(app->log) > 2048) furi_string_left(app->log, 2048);
    text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
    // sweep to next freq
    app->freq_index++;
    uint32_t next = FREQS[app->freq_index % FREQS_COUNT];
    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency_and_path(next);
    furi_hal_subghz_rx();
}

static void start_subghz(App* app, uint32_t freq) {
    furi_hal_subghz_reset();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);
    furi_hal_subghz_set_frequency_and_path(freq);
    furi_hal_subghz_start_async_rx(subghz_rx_cb, app);
    app->subghz_on = true;
}

static void stop_subghz(App* app) {
    if(app->subghz_on) {
        furi_hal_subghz_stop_async_rx();
        furi_hal_subghz_idle();
        furi_hal_subghz_sleep();
        app->subghz_on = false;
    }
}

static void stop_all(App* app) {
    furi_timer_stop(app->scan_timer);
    stop_subghz(app);
    if(app->irw) {
        infrared_worker_rx_stop(app->irw);
        infrared_worker_free(app->irw);
        app->irw = NULL;
    }
    app->all_signals = false;
}

static void start_ir(App* app) {
    app->irw = infrared_worker_alloc();
    infrared_worker_rx_set_received_signal_callback(app->irw, ir_cb, app);
    infrared_worker_rx_start(app->irw);
}

static void menu_cb(void* ctx, uint32_t idx) {
    App* app = ctx;
    stop_all(app);
    furi_string_reset(app->log);
    view_dispatcher_switch_to_view(app->vd, ViewLog);

    if(idx == MenuSubGHz) {
        furi_string_set_str(app->log, "[Sub-GHz] 433.92 MHz\n\n");
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
        start_subghz(app, 433920000);

    } else if(idx == MenuInfrared) {
        furi_string_set_str(app->log, "[Infrared] Scanning...\nPoint remote here\n\n");
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
        start_ir(app);

    } else if(idx == MenuBluetooth) {
        furi_string_set_str(app->log, "[Bluetooth]\nNote: BLE passive scan\nrequires bt service.\nUse Flipper BT app\nfor full BLE scanning.\n");
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));

    } else if(idx == MenuWiFi) {
        furi_string_set_str(app->log, "[WiFi] Contacting ESP32...\n\n");
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
        FuriHalSerialHandle* h = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
        if(!h) {
            furi_string_cat_str(app->log, "No serial - attach WiFi board\n");
        } else {
            furi_hal_serial_init(h, 115200);
            furi_hal_serial_tx(h, (const uint8_t*)"SCAN\n", 5);
            furi_delay_ms(3000);
            furi_hal_serial_deinit(h);
            furi_hal_serial_control_release(h);
            furi_string_cat_str(app->log, "Scan sent to ESP32!\n");
        }
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));

    } else if(idx == MenuFrequency) {
        furi_string_set_str(app->log, "[Freq Scanner]\nSweeping 300-915 MHz\n\n");
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
        app->freq_index = 0;
        furi_hal_subghz_reset();
        furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);
        furi_hal_subghz_set_frequency_and_path(FREQS[0]);
        furi_hal_subghz_rx();
        app->subghz_on = true;
        furi_timer_start(app->scan_timer, furi_ms_to_ticks(800));

    } else if(idx == MenuAllSignals) {
        furi_string_set_str(app->log, "[All Signals] Scanning...\n\n");
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
        app->all_signals = true;
        start_subghz(app, 433920000);
        start_ir(app);
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
    app->subghz_on = false;
    app->all_signals = false;
    app->freq_index = 0;
    app->log = furi_string_alloc();
    app->scan_timer = furi_timer_alloc(freq_timer_cb, FuriTimerTypePeriodic, app);

    app->vd = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_navigation_event_callback(app->vd, back_cb);

    app->menu = submenu_alloc();
    submenu_set_header(app->menu, "Signal Scanner");
    submenu_add_item(app->menu, "Sub-GHz",      MenuSubGHz,     menu_cb, app);
    submenu_add_item(app->menu, "Infrared",      MenuInfrared,   menu_cb, app);
    submenu_add_item(app->menu, "Bluetooth",     MenuBluetooth,  menu_cb, app);
    submenu_add_item(app->menu, "WiFi",          MenuWiFi,       menu_cb, app);
    submenu_add_item(app->menu, "Freq Scanner",  MenuFrequency,  menu_cb, app);
    submenu_add_item(app->menu, "All Signals",   MenuAllSignals, menu_cb, app);
    view_dispatcher_add_view(app->vd, ViewMenu, submenu_get_view(app->menu));

    app->log_box = text_box_alloc();
    text_box_set_font(app->log_box, TextBoxFontText);
    view_dispatcher_add_view(app->vd, ViewLog, text_box_get_view(app->log_box));

    Gui* gui = furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(app->vd, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->vd, ViewMenu);
    view_dispatcher_run(app->vd);

    stop_all(app);
    furi_timer_free(app->scan_timer);
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
