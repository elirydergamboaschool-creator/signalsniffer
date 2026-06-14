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
typedef enum { MenuSubGHz, MenuInfrared, MenuWiFi } MenuItem;

typedef struct {
    ViewDispatcher* vd;
    Submenu* menu;
    TextBox* log_box;
    FuriString* log;
    InfraredWorker* irw;
    FuriTimer* timer;
    bool scanning_subghz;
} App;

// ── IR ───────────────────────────────────────────────────────────────────────
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
        const uint32_t* t; size_t n;
        infrared_worker_get_raw_signal(sig, &t, &n);
        snprintf(buf, sizeof(buf), "[RAW] %zu pulses\n", n);
    }
    furi_string_cat_str(app->log, buf);
    if(furi_string_size(app->log) > 2048) furi_string_left(app->log, 2048);
    text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
}

// ── SubGHz RSSI timer ────────────────────────────────────────────────────────
static void subghz_timer_cb(void* ctx) {
    App* app = ctx;
    if(!app->scanning_subghz) return;
    float rssi = furi_hal_subghz_get_rssi();
    char buf[32];
    snprintf(buf, sizeof(buf), "RSSI: %.1f dBm\n", (double)rssi);
    furi_string_cat_str(app->log, buf);
    if(furi_string_size(app->log) > 2048) furi_string_left(app->log, 2048);
    text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
}

// ── Stop all scanning ────────────────────────────────────────────────────────
static void stop_all(App* app) {
    if(app->timer) {
        furi_timer_stop(app->timer);
    }
    if(app->scanning
