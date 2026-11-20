/*
 * imagen.c — Clase 7 (Sistemas Embebidos, UCU 2026)
 *
 * Del JPEG a la pantalla. El mismo archivo en camara_pantalla y en
 * camara_nube.
 */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "jpeg_decoder.h"
#include "imagen.h"
#include "pantalla.h"

static const char *TAG = "imagen";

#define PIXELES_PANTALLA    (PANTALLA_ANCHO * PANTALLA_ALTO)

/* La memoria de trabajo de TJpgDec: las tablas de Huffman y de cuantización
 * del JPEG, y los bloques de 8x8 que está decodificando. Lo que ocupa depende
 * del submuestreo de color: unos 3,0 KB para un JPEG 4:2:2, como los del
 * OV2640, y unos 3,5 KB para uno 4:2:0. Los 3100 B que usan por defecto
 * esp_jpeg y el driver de la cámara no alcanzan para el segundo: 4 KB cubren
 * los dos. Se reserva una vez, aquí, en la SRAM interna; si no,
 * esp_jpeg_decode() haría un malloc en cada cuadro.
 *
 * TJpgDec saca de esta memoria tablas de enteros de 32 bits, así que tiene
 * que empezar en una dirección múltiplo de 4: en el ESP32-S2, leer o escribir
 * 32 bits desalineados es una excepción (LoadStoreAlignment). GCC alinea así
 * los arreglos por su cuenta, pero no si se compila optimizando el tamaño
 * (-Os): se pide explícitamente. */
static uint8_t s_trabajo[4096] __attribute__((aligned(4)));

static uint16_t *s_pixeles;     /* 320x240x2 = 150 KB, en PSRAM */
static int s_ancho_anterior;
static int s_alto_anterior;

esp_err_t imagen_iniciar(void)
{
    /* 150 KB no caben en la SRAM interna junto con el WiFi: van a la PSRAM.
     * La CPU la lee y la escribe a través de su caché, más despacio que la
     * SRAM interna pero sin que el programa tenga que hacer nada especial. */
    s_pixeles = heap_caps_malloc(PIXELES_PANTALLA * sizeof(uint16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pixeles == NULL) {
        ESP_LOGE(TAG, "sin PSRAM para la imagen (%u B)",
                 (unsigned)(PIXELES_PANTALLA * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t imagen_decodificar(const uint8_t *jpeg, size_t largo, imagen_t *img)
{
    if (s_pixeles == NULL || jpeg == NULL || largo == 0 || img == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = largo,
        .outbuf = (uint8_t *)s_pixeles,
        .outbuf_size = PIXELES_PANTALLA * sizeof(uint16_t),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags.swap_color_bytes = 1,
        .advanced.working_buffer = s_trabajo,
        .advanced.working_buffer_size = sizeof(s_trabajo),
    };

    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) {
        return ESP_FAIL;
    }

    int divisor = 1;
    esp_jpeg_image_scale_t escala = JPEG_IMAGE_SCALE_0;
    while ((info.width / divisor > PANTALLA_ANCHO ||
            info.height / divisor > PANTALLA_ALTO) && divisor < 8) {
        divisor *= 2;
        escala = (esp_jpeg_image_scale_t)(escala + 1);
    }
    if (info.width / divisor > PANTALLA_ANCHO ||
        info.height / divisor > PANTALLA_ALTO) {
        return ESP_ERR_INVALID_SIZE;
    }
    cfg.out_scale = escala;

    esp_jpeg_image_output_t salida;
    if (esp_jpeg_decode(&cfg, &salida) != ESP_OK) {
        return ESP_FAIL;
    }

    img->pixeles = s_pixeles;
    img->ancho = salida.width;
    img->alto = salida.height;
    img->divisor = divisor;
    return ESP_OK;
}

esp_err_t imagen_decodificar_escala(const uint8_t *jpeg, size_t largo,
                                    int divisor, imagen_t *img)
{
    if (s_pixeles == NULL || jpeg == NULL || largo == 0 || img == NULL ||
        (divisor != 1 && divisor != 2 && divisor != 4 && divisor != 8)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = largo,
        .outbuf = (uint8_t *)s_pixeles,
        /* El tamaño REAL del buffer: si la imagen no cabe, esp_jpeg_decode()
         * devuelve un error en vez de escribir fuera. La función jpg2rgb565()
         * del driver de la cámara le pasa UINT32_MAX y no lo comprueba. */
        .outbuf_size = PIXELES_PANTALLA * sizeof(uint16_t),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        /* Byte más significativo primero: el orden que espera el panel. */
        .flags.swap_color_bytes = 1,
        .advanced.working_buffer = s_trabajo,
        .advanced.working_buffer_size = sizeof(s_trabajo),
    };

    cfg.out_scale = (esp_jpeg_image_scale_t)(
        divisor == 1 ? 0 : divisor == 2 ? 1 : divisor == 4 ? 2 : 3);

    esp_jpeg_image_output_t salida;
    if (esp_jpeg_decode(&cfg, &salida) != ESP_OK) {
        return ESP_FAIL;
    }

    img->pixeles = s_pixeles;
    img->ancho = salida.width;
    img->alto = salida.height;
    img->divisor = divisor;
    return ESP_OK;
}

void imagen_mostrar(const imagen_t *img)
{
    if (img == NULL || img->pixeles == NULL) {
        return;
    }
    if (img->ancho != s_ancho_anterior || img->alto != s_alto_anterior) {
        pantalla_limpiar(0);                /* negro alrededor de la foto */
        s_ancho_anterior = img->ancho;
        s_alto_anterior = img->alto;
    }
    pantalla_imagen((PANTALLA_ANCHO - img->ancho) / 2,
                    (PANTALLA_ALTO - img->alto) / 2,
                    img->ancho, img->alto, img->pixeles);
}
