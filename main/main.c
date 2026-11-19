/* ESP32-Mic: Bluetooth HFP hands-free headset (INMP441 mic) + A2DP sink speaker (MAX98357A).
 * Based on ESP-IDF examples/bluetooth/bluedroid/classic_bt/hfp_hf (no console). */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_hidd_api.h"

#define TAG "ESP32-Mic"
#include "nvs.h"
static volatile bool hfp_conn = false, a2dp_conn = false;
static void save_last_peer(const uint8_t *bda);
#define DEVICE_NAME "ESP32-Mic"
#define PIN_BCLK 32
#define PIN_WS   25
#define PIN_DIN  33
#define GAIN_SHIFT CONFIG_MIC_GAIN_SHIFT
#define SPK_BCLK 26
#define SPK_WS   27
#define SPK_DOUT 14
#define SPK_RB_SIZE (12 * 1024)   /* ~70 ms of 44.1k stereo */
#define FRAMES 120              /* samples per I2S read (7.5 ms @16k) */

static i2s_chan_handle_t rx_chan;
static RingbufHandle_t rb;
static volatile bool sco_on = false;
static volatile uint32_t want_rate = 8000, cur_rate = 0;

/* speaker state */
static i2s_chan_handle_t tx_chan;
static RingbufHandle_t spk_rb;
static volatile uint32_t a2dp_rate = 44100, spk_want = 44100, spk_cur = 0;
static volatile bool a2dp_playing = false;
static volatile uint8_t avrc_vol = CONFIG_SPK_DEFAULT_VOLUME;   /* 0..127 */
static volatile int hf_spk_vol = 12;                             /* 0..15 */

/* ---------- I2S ---------- */
static void i2s_setup(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&cc, NULL, &rx_chan));
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = PIN_BCLK, .ws = PIN_WS,
            .dout = I2S_GPIO_UNUSED, .din = PIN_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;   /* L/R tied to GND */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &sc));
}

static void set_rate(uint32_t rate)
{
    if (cur_rate) i2s_channel_disable(rx_chan);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(rx_chan, &clk));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));
    cur_rate = rate;
    ESP_LOGI(TAG, "I2S sample rate %lu Hz", (unsigned long)rate);
}

static void mic_task(void *arg)
{
    static int32_t raw[FRAMES];
    static int16_t pcm[FRAMES];
    for (;;) {
        if (!sco_on) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (cur_rate != want_rate) set_rate(want_rate);
        size_t n = 0;
        if (i2s_channel_read(rx_chan, raw, sizeof(raw), &n, pdMS_TO_TICKS(100)) != ESP_OK) continue;
        int cnt = n / 4;
        for (int i = 0; i < cnt; i++) {
            int32_t s = raw[i] >> GAIN_SHIFT;
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            pcm[i] = (int16_t)s;
        }
        if (xRingbufferSend(rb, pcm, cnt * 2, 0) != pdTRUE) {
            /* buffer full: drop oldest data */
            size_t got; void *p = xRingbufferReceiveUpTo(rb, &got, 0, cnt * 2);
            if (p) vRingbufferReturnItem(rb, p);
            xRingbufferSend(rb, pcm, cnt * 2, 0);
        }
        esp_hf_client_outgoing_data_ready();
    }
}


/* ---------- Speaker: I2S1 TX -> MAX98357A ---------- */
static void spk_setup(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 240;
    cc.auto_clear = true;               /* output silence on underrun */
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &tx_chan, NULL));
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = SPK_BCLK, .ws = SPK_WS,
            .dout = SPK_DOUT, .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &sc));
}

static void spk_set_rate(uint32_t rate)
{
    if (spk_cur) i2s_channel_disable(tx_chan);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx_chan, &clk));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    spk_cur = rate;
    ESP_LOGI(TAG, "Speaker I2S rate %lu Hz", (unsigned long)rate);
}

static void spk_flush(void)
{
    size_t got; void *p;
    while ((p = xRingbufferReceiveUpTo(spk_rb, &got, 0, SPK_RB_SIZE))) vRingbufferReturnItem(spk_rb, p);
}

static void spk_task(void *arg)
{
    for (;;) {
        if (spk_cur != spk_want) { spk_flush(); spk_set_rate(spk_want); }
        size_t got = 0;
        uint8_t *p = xRingbufferReceiveUpTo(spk_rb, &got, pdMS_TO_TICKS(50), 1024);
        if (!p) continue;
        size_t w;
        i2s_channel_write(tx_chan, p, got, &w, portMAX_DELAY);
        vRingbufferReturnItem(spk_rb, p);
    }
}

static void spk_push(const void *d, size_t len)
{
    if (xRingbufferSend(spk_rb, d, len, 0) != pdTRUE) {
        size_t got; void *p = xRingbufferReceiveUpTo(spk_rb, &got, 0, len);  /* drop oldest */
        if (p) vRingbufferReturnItem(spk_rb, p);
        xRingbufferSend(spk_rb, d, len, 0);
    }
}

/* A2DP decoded PCM: 16-bit stereo interleaved */
static void a2d_data_cb(const uint8_t *buf, uint32_t len)
{
    if (sco_on) return;
    static int16_t tmp[512];
    int32_t v = avrc_vol;
    const int16_t *in = (const int16_t *)buf;
    uint32_t n = len / 2;
    while (n) {
        uint32_t c = n > 512 ? 512 : n;
        for (uint32_t i = 0; i < c; i++) tmp[i] = (int16_t)((in[i] * v) / 127);
        spk_push(tmp, c * 2);
        in += c; n -= c;
    }
}

static void a2d_cb(esp_a2d_cb_event_t ev, esp_a2d_cb_param_t *param)
{
    switch (ev) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "A2DP connection state %d", param->conn_stat.state);
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) { a2dp_playing = false; a2dp_conn = false; }
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) { a2dp_conn = true; save_last_peer(param->conn_stat.remote_bda); }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        a2dp_playing = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        ESP_LOGI(TAG, "A2DP audio %s", a2dp_playing ? "started" : "suspended");
        if (a2dp_playing && !sco_on) spk_want = a2dp_rate;
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            uint8_t sf = param->audio_cfg.mcc.cie.sbc_info.samp_freq;
            a2dp_rate = (sf & ESP_A2D_SBC_CIE_SF_48K) ? 48000 :
                        (sf & ESP_A2D_SBC_CIE_SF_44K) ? 44100 :
                        (sf & ESP_A2D_SBC_CIE_SF_32K) ? 32000 : 16000;
            ESP_LOGI(TAG, "A2DP SBC %lu Hz", (unsigned long)a2dp_rate);
            if (!sco_on) spk_want = a2dp_rate;
        }
        break;
    default:
        break;
    }
}

/* ---------- AVRCP: absolute volume from the PC ---------- */
static void avrc_ct_cb(esp_avrc_ct_cb_event_t ev, esp_avrc_ct_cb_param_t *param) { (void)ev; (void)param; }

static void avrc_tg_cb(esp_avrc_tg_cb_event_t ev, esp_avrc_tg_cb_param_t *param)
{
    switch (ev) {
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        avrc_vol = param->set_abs_vol.volume & 0x7f;
        ESP_LOGI(TAG, "Volume %d/127", avrc_vol);
        break;
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            esp_avrc_rn_param_t rn = { .volume = avrc_vol };
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn);
        }
        break;
    default:
        break;
    }
}

/* ---------- HFP audio callbacks (HCI data path, 16-bit PCM) ---------- */
static void hf_incoming_cb(const uint8_t *buf, uint32_t len)
{
    /* mono 16-bit -> stereo for the amp */
    static int16_t st[512];
    const int16_t *in = (const int16_t *)buf;
    uint32_t n = len / 2;
    int32_t v = hf_spk_vol;
    while (n) {
        uint32_t c = n > 256 ? 256 : n;
        for (uint32_t i = 0; i < c; i++) { int16_t s = (int16_t)((in[i] * v) / 15); st[2*i] = s; st[2*i+1] = s; }
        spk_push(st, c * 4);
        in += c; n -= c;
    }
}

static uint32_t hf_outgoing_cb(uint8_t *buf, uint32_t len)
{
    uint32_t done = 0;
    while (done < len) {
        size_t got = 0;
        uint8_t *p = xRingbufferReceiveUpTo(rb, &got, 0, len - done);
        if (!p) break;
        memcpy(buf + done, p, got);
        vRingbufferReturnItem(rb, p);
        done += got;
    }
    if (done < len) memset(buf + done, 0, len - done);  /* pad silence */
    return len;
}

/* ---------- HFP events ---------- */
static void hf_cb(esp_hf_client_cb_event_t ev, esp_hf_client_cb_param_t *param)
{
    switch (ev) {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "HFP connection state %d", param->conn_stat.state);
        hfp_conn = (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED);
        if (hfp_conn) save_last_peer(param->conn_stat.remote_bda);
        if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED)
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED)
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        break;
    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "Audio state %d", param->audio_stat.state);
        if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            want_rate = 16000; sco_on = true; spk_want = 16000;
            ESP_LOGI(TAG, "SCO up: mSBC 16 kHz");
        } else if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
            want_rate = 8000; sco_on = true; spk_want = 8000;
            ESP_LOGI(TAG, "SCO up: CVSD 8 kHz");
        } else if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            sco_on = false; spk_want = a2dp_rate;
        }
        break;
    case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
        if (param->volume_control.type == ESP_HF_VOLUME_CONTROL_TARGET_SPK) {
            hf_spk_vol = param->volume_control.volume;
            ESP_LOGI(TAG, "Call speaker volume %d/15", hf_spk_vol);
        }
        break;
    default:
        break;
    }
}

/* ---------- GAP (auto-accept pairing) ---------- */
static void gap_cb(esp_bt_gap_cb_event_t ev, esp_bt_gap_cb_param_t *param)
{
    switch (ev) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "Pairing %s", param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "OK" : "failed");
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    }
    default:
        break;
    }
}

/* ---------- Classic BT HID keyboard (Enter button on GPIO13) ---------- */
#define BTN_GPIO 13
static const uint8_t kbd_desc[] = {
    0x05,0x01,0x09,0x06,0xA1,0x01,0x05,0x07,0x19,0xE0,0x29,0xE7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x05,0x75,0x01,
    0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x01,0x95,0x06,
    0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,0x19,0x00,0x29,0x65,0x81,0x00,0xC0 };
static esp_hidd_app_param_t hid_app;
static esp_hidd_qos_param_t hid_qos;
static volatile bool hid_conn = false;
static uint8_t kbd_rep[8];

static void hidd_cb(esp_hidd_cb_event_t ev, esp_hidd_cb_param_t *param)
{
    switch (ev) {
    case ESP_HIDD_INIT_EVT:
        if (param->init.status == ESP_HIDD_SUCCESS) esp_bt_hid_device_register_app(&hid_app, &hid_qos, &hid_qos);
        break;
    case ESP_HIDD_REGISTER_APP_EVT:
        ESP_LOGI(TAG, "HID keyboard registered (status %d)", param->register_app.status);
        break;
    case ESP_HIDD_OPEN_EVT:
        if (param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTED) { hid_conn = true; ESP_LOGI(TAG, "HID keyboard connected"); save_last_peer(param->open.bd_addr); }
        break;
    case ESP_HIDD_CLOSE_EVT:
        if (param->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED) { hid_conn = false; ESP_LOGI(TAG, "HID keyboard disconnected"); }
        break;
    case ESP_HIDD_GET_REPORT_EVT:
        esp_bt_hid_device_send_report(param->get_report.report_type, 0, sizeof(kbd_rep), kbd_rep);
        break;
    case ESP_HIDD_SEND_REPORT_EVT:
        if (param->send_report.status != ESP_HIDD_SUCCESS) ESP_LOGW(TAG, "HID send failed %d", param->send_report.reason);
        break;
    default: break;
    }
}

static void send_rep(void)
{
    if (hid_conn) esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0, 8, kbd_rep);
}

#define BTN2_GPIO 12   /* MTDI strap: do not hold during reset/power-up */
#define BTN3_GPIO 4    /* Ctrl+Shift+Space button */
/* Mouse-wheel encoder drives Alt+Tab (replaces the GPIO4 button) */
#define ENC_A_GPIO 18
#define ENC_B_GPIO 19
#define ENC_SW_GPIO 21          /* optional wheel click: confirm / release Alt now */
#define ENC_STEPS_PER_DETENT 2  /* quadrature transitions per click; try 2 if it needs two clicks per step */
#define ALTTAB_RELEASE_MS 1000  /* auto-confirm: Alt released 1 s after the wheel stops */
static volatile int enc_acc = 0;
static volatile uint8_t enc_prev = 0;
static portMUX_TYPE enc_mux = portMUX_INITIALIZER_UNLOCKED;
static void enc_isr(void *arg)
{
    static const int8_t tbl[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
    uint8_t s = (gpio_get_level(ENC_A_GPIO) << 1) | gpio_get_level(ENC_B_GPIO);
    portENTER_CRITICAL_ISR(&enc_mux);
    enc_acc -= tbl[(enc_prev << 2) | s];   /* sign flipped: forward = next window */ enc_prev = s;
    portEXIT_CRITICAL_ISR(&enc_mux);
}
static void tap_tab(bool back)
{
    if (back) kbd_rep[0] |= 0x02;           /* Left Shift for reverse */
    kbd_rep[3] = 0x2B; send_rep();
    vTaskDelay(pdMS_TO_TICKS(20));
    kbd_rep[3] = 0x00; kbd_rep[0] &= ~0x02; send_rep();
}
static void btn_task(void *arg)
{
    gpio_config_t io = { .pin_bit_mask = (1ULL << BTN_GPIO) | (1ULL << BTN2_GPIO) | (1ULL << ENC_SW_GPIO) | (1ULL << BTN3_GPIO), .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&io);
    gpio_config_t eio = { .pin_bit_mask = (1ULL << ENC_A_GPIO) | (1ULL << ENC_B_GPIO), .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_ANYEDGE };
    gpio_config(&eio);
    enc_prev = (gpio_get_level(ENC_A_GPIO) << 1) | gpio_get_level(ENC_B_GPIO);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(ENC_A_GPIO, enc_isr, NULL);
    gpio_isr_handler_add(ENC_B_GPIO, enc_isr, NULL);
    const int pins[4] = { BTN_GPIO, BTN2_GPIO, ENC_SW_GPIO, BTN3_GPIO };
    int stable[4] = {1, 1, 1, 1}, last[4] = {1, 1, 1, 1}, cnt[4] = {0, 0, 0, 0};
    bool alt_on = false; int alt_idle_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        for (int i = 0; i < 4; i++) {
            int v = gpio_get_level(pins[i]);
            if (v != last[i]) { last[i] = v; cnt[i] = 0; continue; }
            if (v == stable[i] || ++cnt[i] < 2) continue;   /* ~20 ms stable */
            stable[i] = v;
            if (i == 0 && v == 0 && alt_on) {     /* Enter button confirms the switcher pick */
                ESP_LOGI(TAG, "Enter: confirm Alt+Tab");
                kbd_rep[0] &= ~0x04; send_rep(); alt_on = false;
            } else if (i == 0 && v == 0) {        /* Enter tap, modifiers preserved */
                ESP_LOGI(TAG, "Button: Enter%s", hid_conn ? "" : " (HID not connected)");
                kbd_rep[2] = 0x28; send_rep();
                vTaskDelay(pdMS_TO_TICKS(20));
                kbd_rep[2] = 0x00; send_rep();
            } else if (i == 1) {                  /* Ctrl+Win held while down */
                if (v == 0) kbd_rep[0] |= 0x09; else kbd_rep[0] &= ~0x09;
                ESP_LOGI(TAG, "Button: Ctrl+Win %s%s", v == 0 ? "down" : "up", hid_conn ? "" : " (HID not connected)");
                send_rep();
            } else if (i == 2 && v == 0 && alt_on) {   /* wheel click: pick window now */
                ESP_LOGI(TAG, "Wheel click: release Alt");
                kbd_rep[0] &= ~0x04; send_rep(); alt_on = false;
            } else if (i == 3) {                  /* GPIO4: Ctrl+Shift+Space held while down */
                if (v == 0) { kbd_rep[0] |= 0x03; kbd_rep[4] = 0x2C; } else { kbd_rep[0] &= ~0x03; kbd_rep[4] = 0x00; }
                ESP_LOGI(TAG, "Button: Ctrl+Shift+Space %s%s", v == 0 ? "down" : "up", hid_conn ? "" : " (HID not connected)");
                send_rep();
            }
        }
        int n;
        portENTER_CRITICAL(&enc_mux);
        n = enc_acc / ENC_STEPS_PER_DETENT; enc_acc -= n * ENC_STEPS_PER_DETENT;
        portEXIT_CRITICAL(&enc_mux);
        if (n != 0) {
            ESP_LOGI(TAG, "Wheel: %d%s", n, hid_conn ? "" : " (HID not connected)");
            if (!alt_on) { kbd_rep[0] |= 0x04; send_rep(); alt_on = true; vTaskDelay(pdMS_TO_TICKS(20)); }
            for (int k = 0; k < (n > 0 ? n : -n); k++) tap_tab(n < 0);
            alt_idle_ms = 0;
        } else if (alt_on) {
            alt_idle_ms += 10;
            if (alt_idle_ms >= ALTTAB_RELEASE_MS) { kbd_rep[0] &= ~0x04; send_rep(); alt_on = false; }
        }
    }
}

/* ---------- Bond logging + auto-reconnect to last host ---------- */
#define BDA_FMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define BDA_ARG(a) a[0], a[1], a[2], a[3], a[4], a[5]
static esp_bd_addr_t rc_peer;

static void save_last_peer(const uint8_t *bda)
{
    nvs_handle_t h;
    uint8_t old[6]; size_t len = 6;
    if (nvs_open("btmic", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "last_peer", old, &len) != ESP_OK || len != 6 || memcmp(old, bda, 6)) {
        nvs_set_blob(h, "last_peer", bda, 6); nvs_commit(h);
        ESP_LOGI(TAG, "Saved last peer " BDA_FMT, BDA_ARG(bda));
    }
    nvs_close(h);
}

static bool pick_peer(void)
{
    int n = esp_bt_gap_get_bond_device_num();
    ESP_LOGI(TAG, "Bonded devices: %d", n);
    if (n <= 0) return false;
    esp_bd_addr_t list[8]; int cnt = n > 8 ? 8 : n;
    esp_bt_gap_get_bond_device_list(&cnt, list);
    for (int i = 0; i < cnt; i++) ESP_LOGI(TAG, "  bond[%d] " BDA_FMT, i, BDA_ARG(list[i]));
    uint8_t last[6]; size_t len = 6; nvs_handle_t h; bool have = false;
    if (nvs_open("btmic", NVS_READONLY, &h) == ESP_OK) {
        have = nvs_get_blob(h, "last_peer", last, &len) == ESP_OK && len == 6;
        nvs_close(h);
    }
    for (int i = 0; have && i < cnt; i++)
        if (!memcmp(list[i], last, 6)) { memcpy(rc_peer, last, 6); return true; }
    memcpy(rc_peer, list[cnt - 1], 6);   /* fallback: a bonded device */
    return true;
}

static void reconnect_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!pick_peer()) { ESP_LOGI(TAG, "No bonds; waiting for pairing"); vTaskDelete(NULL); }
    ESP_LOGI(TAG, "Auto-reconnect to " BDA_FMT, BDA_ARG(rc_peer));
    for (int t = 0; t < 12; t++) {            /* ~60 s */
        if (hfp_conn && a2dp_conn && hid_conn) break;
        ESP_LOGI(TAG, "Reconnect attempt %d (HFP %d A2DP %d HID %d)", t + 1, hfp_conn, a2dp_conn, hid_conn);
        if (!hfp_conn) esp_hf_client_connect(rc_peer);
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (!a2dp_conn) esp_a2d_sink_connect(rc_peer);
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (!hid_conn) esp_bt_hid_device_connect(rc_peer);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "Auto-reconnect done (HFP %d A2DP %d HID %d)", hfp_conn, a2dp_conn, hid_conn);
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    rb = xRingbufferCreate(16000 * 2 / 4, RINGBUF_TYPE_BYTEBUF);  /* ~250 ms @16k */
    i2s_setup();
    spk_rb = xRingbufferCreate(SPK_RB_SIZE, RINGBUF_TYPE_BYTEBUF);
    spk_setup();

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));  /* classic-only */
    esp_bt_controller_config_t bc = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bc));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    esp_bluedroid_config_t bd = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bd.ssp_en = true;
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd));
#else
    ESP_ERROR_CHECK(esp_bluedroid_init());
#endif
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_gap_register_callback(gap_cb);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    esp_bt_gap_set_device_name(DEVICE_NAME);
#else
    esp_bt_dev_set_device_name(DEVICE_NAME);
#endif
    esp_bt_sp_param_t pt = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;          /* Just Works */
    esp_bt_gap_set_security_param(pt, &iocap, sizeof(iocap));
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
    esp_bt_gap_set_pin(pin_type, 4, pin);

    esp_bt_cod_t cod = {0};
    cod.major = ESP_BT_COD_MAJOR_DEV_AV;
#if CONFIG_COD_PERIPHERAL
    cod.major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    cod.minor = ESP_BT_COD_MINOR_PERIPHERAL_KEYBOARD;
#else
    cod.minor = 0x01;                                    /* wearable headset */
#endif
    cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_TELEPHONY |
                  ESP_BT_COD_SRVC_RENDERING | ESP_BT_COD_SRVC_CAPTURING;
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);

    esp_hf_client_register_callback(hf_cb);
    ESP_ERROR_CHECK(esp_hf_client_init());
    esp_hf_client_register_data_callback(hf_incoming_cb, hf_outgoing_cb);

    /* AVRCP (must init before A2DP) */
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    esp_avrc_ct_register_callback(avrc_ct_cb);
    ESP_ERROR_CHECK(esp_avrc_tg_init());
    esp_avrc_tg_register_callback(avrc_tg_cb);
    esp_avrc_rn_evt_cap_mask_t evs = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evs, ESP_AVRC_RN_VOLUME_CHANGE);
    esp_avrc_tg_set_rn_evt_cap(&evs);

    /* A2DP sink */
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_sink_register_data_callback(a2d_data_cb);
    ESP_ERROR_CHECK(esp_a2d_sink_init());

    /* Classic HID keyboard on the same link */
    hid_app.name = "ESP32-Mic Keyboard"; hid_app.description = "Enter button"; hid_app.provider = "ESP32";
    hid_app.subclass = ESP_HID_CLASS_KBD; hid_app.desc_list = (uint8_t *)kbd_desc; hid_app.desc_list_len = sizeof(kbd_desc);
    memset(&hid_qos, 0, sizeof(hid_qos));
    esp_bt_hid_device_register_callback(hidd_cb);
    ESP_ERROR_CHECK(esp_bt_hid_device_init());
    xTaskCreate(btn_task, "btn", 3072, NULL, 5, NULL);

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    xTaskCreatePinnedToCore(spk_task, "spk", 3072, NULL, 9, NULL, 1);
    xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 10, NULL, 1);
    ESP_LOGI(TAG, "Ready. Pair \"%s\" from Windows Bluetooth settings.", DEVICE_NAME);
    xTaskCreate(reconnect_task, "reconn", 3072, NULL, 4, NULL);
}
