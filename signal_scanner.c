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

static const uint32_t FREQS[] = {
    300000000, 315000000, 345000000, 390000000,
    418000000, 433920000, 438000000, 868350000, 915000000,
};
#define FREQS_COUNT 9

typedef struct {
    ViewDispatcher* vd;
    Submenu* menu;
    TextBox* log_box;
    FuriString* log;
    InfraredWorker* irw;
    FuriTimer* scan_timer;
    uint8_t freq_index;
    uint8_t mode; // 0=subghz 1=freq_sweep
} App;

static void append_log(App* app, const char* str) {
    furi_string_cat_str(app->log, str);
    if(furi_string_size(app->log) > 2048) furi_string_left(app->log, 2048);
    text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
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
    append_log(app, buf);
}

static void timer_cb(void* ctx) {
    App* app = ctx;
    char buf[56];
    float rssi = furi_hal_subghz_get_rssi();

    if(app->mode == 0) {
        // fixed 433.92 SubGHz scan
        snprintf(buf, sizeof(buf), "433.92MHz RSSI:%.1fdBm\n", (double)rssi);
        append_log(app, buf);
    } else {
        // frequency sweep
        uint32_t f = FREQS[app->freq_index];
        snprintf(buf, sizeof(buf), "%.3fMHz RSSI:%.1fdBm\n",
            (double)(f / 1000000.0f), (double)rssi);
        append_log(app, buf);
        app->freq_index = (app->freq_index + 1) % FREQS_COUNT;
        furi_hal_subghz_idle();
        furi_hal_subghz_set_frequency_and_path(FREQS[app->freq_index]);
        furi_hal_subghz_rx();
    }
}

static void stop_subghz(App* app) {
    furi_timer_stop(app->scan_timer);
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
}

static void stop_ir(App* app) {
    if(app->irw) {
        infrared_worker_rx_stop(app->irw);
        infrared_worker_free(app->irw);
        app->irw = NULL;
    }
}

static void stop_all(App* app) {
    stop_subghz(app);
    stop_ir(app);
}

static void start_subghz_fixed(App* app, uint32_t freq) {
    furi_hal_subghz_reset();
    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency_and_path(freq);
    furi_hal_subghz_rx();
    app->mode = 0;
    furi_timer_start(app->scan_timer, furi_ms_to_ticks(600));
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
        append_log(app, "[Sub-GHz] 433.92MHz\n\n");
        start_subghz_fixed(app, 433920000);

    } else if(idx == MenuInfrared) {
        append_log(app, "[Infrared] Scanning...\nPoint remote here\n\n");
        start_ir(app);

    } else if(idx == MenuBluetooth) {
        append_log(app, "[Bluetooth]\nPassive BLE scan not\navailable in ext apps.\nUse Flipper BT app.\n");

    } else if(idx == MenuWiFi) {
        append_log(app, "[WiFi] Contacting ESP32...\n\n");
        FuriHalSerialHandle* h = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
        if(!h) {
            append_log(app, "No serial - attach WiFi board\n");
        } else {
            furi_hal_serial_init(h, 115200);
            furi_hal_serial_tx(h, (const uint8_t*)"SCAN\n", 5);
            furi_delay_ms(3000);
            furi_hal_serial_deinit(h);
            furi_hal_serial_control_release(h);
            append_log(app, "Scan sent to ESP32!\n");
        }

    } else if(idx == MenuFrequency) {
        append_log(app, "[Freq Sweep] 300-915MHz\n\n");
        app->freq_index = 0;
        furi_hal_subghz_reset();
        furi_hal_subghz_idle();
        furi_hal_subghz_set_frequency_and_path(FREQS[0]);
        furi_hal_subghz_rx();
        app->mode = 1;
        furi_timer_start(app->scan_timer, furi_ms_to_ticks(800));

    } else if(idx == MenuAllSignals) {
        append_log(app, "[All Signals] Scanning...\n\n");
        start_subghz_fixed(app, 433920000);
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
    app->freq_index = 0;
    app->mode = 0;
    app->log = furi_string_alloc();
    app->scan_timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);

    app->vd = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_navigation_event_callback(app->vd, back_cb);

    app->menu = submenu_alloc();
    submenu_set_header(app->menu, "Signal Scanner");
    submenu_add_item(app->menu, "Sub-GHz",     MenuSubGHz,     menu_cb, app);
    submenu_add_item(app->menu, "Infrared",    MenuInfrared,   menu_cb, app);
    submenu_add_item(app->menu, "Bluetooth",   MenuBluetooth,  menu_cb, app);
    submenu_add_item(app->menu, "WiFi",        MenuWiFi,       menu_cb, app);
    submenu_add_item(app->menu, "Freq Sweep",  MenuFrequency,  menu_cb, app);
    submenu_add_item(app->menu, "All Signals", MenuAllSignals, menu_cb, app);
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
