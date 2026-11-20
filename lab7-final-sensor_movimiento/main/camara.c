/*
 * camara.c — Clase 7 (Sistemas Embebidos, UCU 2026)
 *
 * La ESP-LyraP-CAM sobre la Kaluga-1. El mismo archivo, sin cambios, en
 * camara_pantalla y en camara_nube.
 *
 * Las señales de la interfaz DVP y dónde están (esquemático de la placa
 * principal v1.2, hoja 3, y board_kaluga_v1_2.h / _v1_3.h de Espressif):
 *
 *     XCLK   GPIO1   el reloj que le DAMOS al sensor (20 MHz)    switch T1
 *     VSYNC  GPIO2   empieza un cuadro                           switch T2
 *     HREF   GPIO3   hay una línea válida en los datos           switch T3
 *     SIOC   GPIO7   SCCB (un I2C): reloj                        switch T7
 *     SIOD   GPIO8   SCCB: datos                                 switch T8
 *     PCLK   GPIO33  el sensor marca cada byte con este reloj
 *     D7..D2 GPIO38 21 40 39 42 41
 *     D1, D0 GPIO45 46 en la v1.2      GPIO37 36 en la v1.3
 *
 * Tres consecuencias que no avisa ningún error:
 *
 *   - GPIO1, 2, 3, 7 y 8 llegan al conector de la cámara a través de los
 *     microinterruptores T1, T2, T3, T7 y T8: tienen que estar en ON.
 *   - En la v1.2, D1 y D0 caen en GPIO45 y GPIO46, que son pines de
 *     ARRANQUE. Con la cámara montada, flashear puede fallar ("Wrong boot
 *     mode") y el botón Reset puede no arrancar el programa: es el problema
 *     conocido que la v1.3 arregló moviendo esos dos pines. Se flashea sin la
 *     cámara; después, se apaga, se monta y se enciende.
 *   - GPIO45 es también el LED RGB: con el jumper puesto, el LED recibe los
 *     datos de la cámara. En esta clase el jumper va QUITADO.
 *
 * Los pines RESET y PWDN del sensor no llegan al ESP32-S2: la placa los fija
 * con resistencias (RESET en alto, PWDN en bajo), así que el sensor está
 * siempre encendido y el driver lo reinicia por SCCB.
 */

#include <inttypes.h>
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "camara.h"

static const char *TAG = "camara";

/* 20 MHz es el valor que usan Espressif y el driver para el OV2640. El
 * sensor multiplica este reloj por dentro; PCLK, el que acompaña a los
 * datos, sale de ahí. */
#define CAMARA_XCLK_HZ      (20 * 1000 * 1000)

/* Dos buffers de cuadro: mientras el programa usa uno, el driver llena el
 * otro. Con uno solo, la cámara se detiene cada vez que el programa trabaja. */
#define CAMARA_BUFFERS      2

static size_t   s_tamano_buffer;
static uint16_t s_ancho;
static uint16_t s_alto;
static const char *s_nombre = "ninguno";

esp_err_t camara_iniciar(const camara_config_t *cfg)
{
    if (cfg == NULL || cfg->tamano >= FRAMESIZE_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Sin PSRAM no hay dónde poner los cuadros: un JPEG VGA ya ocupa más
     * que todo lo que queda libre en la SRAM interna con WiFi. */
    if (esp_psram_get_size() == 0) {
        ESP_LOGE(TAG, "no hay PSRAM: revisa CONFIG_SPIRAM en sdkconfig");
        return ESP_ERR_NO_MEM;
    }

    s_ancho = resolution[cfg->tamano].width;
    s_alto  = resolution[cfg->tamano].height;

    /* El JPEG no tiene un tamaño fijo: depende de la escena y de la calidad.
     * El driver reserva por defecto ancho*alto/5; aquí se deja algo más de
     * margen, ancho*alto/4, porque un cuadro que no cabe se descarta entero
     * (en la consola: "cam_hal: FB-OVF"). En VGA son 75 KB por buffer. */
    s_tamano_buffer = (size_t)s_ancho * s_alto / 4;

    const bool v1_2 = (cfg->kit == CAMARA_KALUGA_V1_2);

    const camera_config_t c = {
        .pin_pwdn  = -1,                    /* fijado por la placa         */
        .pin_reset = -1,                    /* idem: reinicio por SCCB     */
        .pin_xclk  = 1,
        .pin_sccb_sda = 8,
        .pin_sccb_scl = 7,

        .pin_d7 = 38,
        .pin_d6 = 21,
        .pin_d5 = 40,
        .pin_d4 = 39,
        .pin_d3 = 42,
        .pin_d2 = 41,
        .pin_d1 = v1_2 ? 45 : 37,
        .pin_d0 = v1_2 ? 46 : 36,
        .pin_vsync = 2,
        .pin_href  = 3,
        .pin_pclk  = 33,

        .xclk_freq_hz = CAMARA_XCLK_HZ,
        .ledc_timer   = LEDC_TIMER_0,       /* XCLK sale de un LEDC         */
        .ledc_channel = LEDC_CHANNEL_0,

        /* JPEG: el SENSOR comprime. Por los cables y por la memoria pasa un
         * cuadro de decenas de KB en vez de los 600 KB de un VGA en RGB565. */
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = cfg->tamano,
        .jpeg_quality = cfg->calidad,
        .jpeg_buffer_size = s_tamano_buffer,

        .fb_count    = CAMARA_BUFFERS,
        .fb_location = CAMERA_FB_IN_PSRAM,
        /* LATEST: el driver captura sin parar y siempre ofrece el cuadro más
         * nuevo. Con WHEN_EMPTY, el primer cuadro que se pide puede tener
         * segundos: el que se capturó cuando el buffer quedó libre. */
        .grab_mode   = CAMERA_GRAB_LATEST,
    };

    ESP_LOGI(TAG, "placa principal %s: D1 y D0 en GPIO%d y GPIO%d",
             v1_2 ? "v1.2" : "v1.3", c.pin_d1, c.pin_d0);

    const esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "la camara no arranca (%s). Revisa: placa de camara "
                      "bien encajada, T1 T2 T3 T7 T8 en ON, placa de audio "
                      "quitada, y la revision de la placa (KALUGA_V1_x)",
                 esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    const camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    s_nombre = (info != NULL) ? info->name : "desconocido";

    /* Orientación: dependen de cómo está montado el sensor y de cómo se
     * mire la pantalla. Se corrigen aquí, en el sensor, sin gastar CPU. */
    s->set_vflip(s, cfg->voltear ? 1 : 0);
    s->set_hmirror(s, cfg->espejo ? 1 : 0);

    /* Las barras de color las genera el propio sensor: si se ven bien, todo
     * el camino sensor -> DVP -> DMA -> PSRAM -> JPEG -> pantalla funciona,
     * con independencia de la luz y del enfoque. */
    s->set_colorbar(s, cfg->patron ? 1 : 0);

    ESP_LOGI(TAG, "sensor %s (PID 0x%02x): %ux%u JPEG, calidad %d, "
                  "%d buffers de %u B en PSRAM, XCLK %d MHz",
             s_nombre, s->id.PID, s_ancho, s_alto, cfg->calidad,
             CAMARA_BUFFERS, (unsigned)s_tamano_buffer,
             CAMARA_XCLK_HZ / 1000000);
    return ESP_OK;
}

const char *camara_sensor(void)
{
    return s_nombre;
}

size_t camara_tamano_buffer(void)
{
    return s_tamano_buffer;
}

uint16_t camara_ancho(void)
{
    return s_ancho;
}

uint16_t camara_alto(void)
{
    return s_alto;
}
