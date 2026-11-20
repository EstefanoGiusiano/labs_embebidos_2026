/*
 * camara.h — Clase 7 (Sistemas Embebidos, UCU 2026)
 *
 * La ESP-LyraP-CAM (sensor OV2640) sobre la Kaluga-1. Lo usan los dos
 * ejemplos de la clase (camara_pantalla y camara_nube): es el mismo archivo
 * en los dos, sin cambios.
 *
 * Este módulo solo sabe CONECTAR la cámara: qué pin es cada señal en cada
 * revisión de la placa, con qué reloj, en qué formato y dónde van los
 * cuadros. Para capturar se usa directamente la API del driver, que es corta:
 *
 *     camera_fb_t *fb = esp_camera_fb_get();    espera un cuadro completo
 *     ...                                       fb->buf, fb->len: el JPEG
 *     esp_camera_fb_return(fb);                 el buffer vuelve al driver
 *
 * Entre las dos llamadas el buffer es NUESTRO y el driver no puede escribir
 * en él. Si no se devuelve, el driver se queda sin buffers y deja de capturar.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_camera.h"
#include "esp_err.h"

/* La revisión de la placa PRINCIPAL, la que dice la serigrafía junto al
 * módulo: "ESP32-S2-Kaluga-1 V1.2" o "V1.3". Cambian dos pines de datos. */
typedef enum {
    CAMARA_KALUGA_V1_2 = 0,     /* D0 y D1 en GPIO46 y GPIO45               */
    CAMARA_KALUGA_V1_3,         /* D0 y D1 en GPIO36 y GPIO37               */
} camara_kit_t;

typedef struct {
    camara_kit_t kit;
    framesize_t  tamano;        /* FRAMESIZE_QVGA, _VGA, _SVGA... _UXGA       */
    int          calidad;       /* JPEG de 0 a 63: MENOS es MEJOR y más grande */
    bool         voltear;       /* volteo vertical                            */
    bool         espejo;        /* espejo horizontal                          */
    bool         patron;        /* barras de color generadas por el sensor    */
} camara_config_t;

/* Conecta la cámara, la reconoce por SCCB, reserva los dos buffers de cuadro
 * en PSRAM y arranca la captura continua. Devuelve el error del driver si el
 * sensor no contesta: el llamante decide si sigue sin cámara. */
esp_err_t camara_iniciar(const camara_config_t *cfg);

/* El nombre del sensor detectado ("OV2640"), o "ninguno". */
const char *camara_sensor(void);

/* Bytes de cada buffer de cuadro: el JPEG más grande que cabe. */
size_t camara_tamano_buffer(void);

/* Ancho y alto, en píxeles, del tamaño configurado. */
uint16_t camara_ancho(void);
uint16_t camara_alto(void);

/* Instante de captura de un cuadro, en µs desde el arranque (esp_timer). */
static inline int64_t camara_marca_us(const camera_fb_t *fb)
{
    return (int64_t)fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec;
}
