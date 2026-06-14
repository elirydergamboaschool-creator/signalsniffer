#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/widget.h>
#include <notification/notification_messages.h>
#include <lib/subghz/subghz_setting.h>
#include <lib/subghz/receiver.h>
#include <lib/subghz/transmitter.h>
#include <lib/subghz/subghz_worker.h>
#include <infrared_worker.h>
#include <bt/bt_service/bt.h>
#include <furi_hal_infrared.h>
#include <furi_hal_subghz.h>
#include <furi_hal_bt.h>
#include <string.h>
#include <stdio.h>

#define MAX_SIGNALS 32
#define MAX_SIG_LEN 64
#define TAG "SignalScanner"

typedef enum {
    SceneMain,
    SceneSubGHz,
    SceneInfrared,
    SceneBluetooth,
    SceneWiFi,
    SceneCount,
} SceneId;

typedef enum {
    ViewSubmenu,
    ViewTextBox,
    ViewCount,
} ViewId;

typedef struct {
    SceneManager*    scene_manager;
    ViewDispatcher*  view_dispatcher;
    Submenu*         submenu;
    TextBox*         text_box;
    SubGhzWorker*    subghz_worker;
    FuriString*      subghz_log;
    InfraredWorker*  ir_worker;
    FuriString*      ir_log;
    FuriString*      ble_log;
    FuriString*      wifi_log;
    SceneId          active_scan;
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
    if(infrared_worker_signal_is_decoded(received_signal)) {
        const InfraredMessage* msg = infrared_worker_get_decoded_signal(received_signal);
        char buf[MAX_SIG_LEN];
        snprintf(buf, sizeof(buf), "[IR] %s addr=0x%lX cmd=0x%lX\n",
            infrared_get_protocol_name(msg->protocol),
            (unsigned long)msg->address,
            (unsigned long)msg->command);
        furi_string_cat_str(app->ir_log, buf);
    } else {
        const uint32_t* timings;
        size_t timings_cnt;
        infrared_worker_get_raw_signal(received_signal, &timings, &timings_cnt);
        char buf[MAX_SIG_LEN];
        snprintf(buf, sizeof(buf), "[IR-RAW] %zu pulses\n", timings_cnt);
        furi_string_cat_str(app->ir_log, buf);
    }
    if(furi_string_size(app->ir_log) > 2048)
        furi_string_left(app->ir_log, 2048);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->ir_log));
}

static void wifi_scan_start(App* app) {
    furi_hal_serial_init(FuriHalSerialIdUsart, 115200);
    const char* cmd = "SCAN\n";
    furi_hal_serial_tx(FuriHalSerialIdUsart, (const uint8_t*)cmd, strlen(cmd));
    furi_string_reset(app->wifi_log);
    furi_string_cat_str(app->wifi_log, "-- WiFi Scan (ESP32) --\n");
    uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(3000);
    char line[128];
    size_t pos = 0;
    while(furi_get_tick() < deadline) {
        uint8_t byte = 0;
        if(furi_hal_serial_rx(FuriHalSerialIdUsart, &byte, 1, 10)) {
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
    furi_hal_serial_deinit(FuriHalSerialIdUsart);
    text_box_set_text(app->text_box, furi_string_get_cstr(app->wifi_log));
}

static void ble_scan_tick(App* app) {
    BleAdRecord rec;
    if(furi_hal_bt_get_ad
