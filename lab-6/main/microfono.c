#include "microfono.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

/* Pines de la Kaluga-1 v1.3 con la placa de audio ESP-LyraT-8311A. */
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     7
#define PIN_I2S_MCLK    35
#define PIN_I2S_BCLK    18
#define PIN_I2S_WS      17
#define PIN_I2S_DIN     34

#define FS_HZ           16000
#define BLOQUE_FRAMES   256          /* 16 ms a 16 kHz                        */
#define BLOQUES_STALE   6            /* 96 ms: lo que el DMA acumuló sin leer */
#define BLOQUES_MEDIDOS 8            /* 128 ms de señal para el RMS           */

static const char *TAG = "microfono";

static esp_codec_dev_handle_t s_dev;
static SemaphoreHandle_t      s_mutex;

/* Estéreo intercalado (izquierda, derecha): el micrófono entra por la
 * izquierda del ES8311. Un solo lector (tarea sensor): búfer estático. */
static int16_t s_bloque[BLOQUE_FRAMES * 2];

esp_err_t microfono_iniciar(int ganancia_db)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_handle_t i2c = NULL;
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&i2c_cfg, &i2c);
    if (err != ESP_OK) {
        return err;
    }

    i2s_chan_handle_t rx = NULL;
    const i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    err = i2s_new_channel(&chan_cfg, NULL, &rx);
    if (err != ESP_OK) {
        return err;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FS_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_I2S_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_std_mode(rx, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }

    audio_codec_i2c_cfg_t ctrl_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    audio_codec_i2s_cfg_t data_cfg = { .port = I2S_NUM_0, .rx_handle = rx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (ctrl_if == NULL || data_if == NULL || gpio_if == NULL) {
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = -1,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (codec_if == NULL) {
        return ESP_FAIL;
    }

    const esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (s_dev == NULL) {
        return ESP_FAIL;
    }

    const esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = FS_HZ,
    };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_gain(s_dev, (float)ganancia_db) != ESP_CODEC_DEV_OK) {
        s_dev = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "micrófono listo: %d Hz, ganancia %d dB", FS_HZ, ganancia_db);
    return ESP_OK;
}

int32_t microfono_leer_db_x100(void)
{
    if (s_dev == NULL) {
        return MIC_SIN_DATO;
    }
    int32_t resultado = MIC_SIN_DATO;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int64_t suma = 0;
    int64_t suma_cuad = 0;
    int64_t n = 0;
    bool ok = true;

    for (int b = 0; b < BLOQUES_STALE + BLOQUES_MEDIDOS && ok; b++) {
        ok = esp_codec_dev_read(s_dev, s_bloque, sizeof(s_bloque)) ==
             ESP_CODEC_DEV_OK;
        if (ok && b >= BLOQUES_STALE) {
            for (int i = 0; i < BLOQUE_FRAMES; i++) {
                const int32_t x = s_bloque[2 * i];
                suma += x;
                suma_cuad += (int64_t)x * x;
                n++;
            }
        }
    }
    xSemaphoreGive(s_mutex);

    if (ok && n > 0) {
        /* RMS sin la componente continua: varianza = E[x²] - E[x]². */
        const float media = (float)suma / (float)n;
        const float varianza = (float)suma_cuad / (float)n - media * media;
        float db = -90.0f;
        if (varianza >= 1.0f) {
            db = 10.0f * log10f(varianza / (32768.0f * 32768.0f));
        }
        if (db < -90.0f) {
            db = -90.0f;
        }
        resultado = (int32_t)(db * 100.0f - 0.5f);
    }
    return resultado;
}

bool microfono_poner_ganancia(int ganancia_db)
{
    if (s_dev == NULL) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool ok = esp_codec_dev_set_in_gain(s_dev, (float)ganancia_db) ==
                    ESP_CODEC_DEV_OK;
    xSemaphoreGive(s_mutex);
    return ok;
}
