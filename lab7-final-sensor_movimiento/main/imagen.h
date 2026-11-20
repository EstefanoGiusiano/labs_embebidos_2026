/*
 * imagen.h — Clase 7 (Sistemas Embebidos, UCU 2026)
 *
 * Del JPEG a los píxeles de la pantalla. Es el mismo archivo en
 * camara_pantalla y en camara_nube.
 *
 *     JPEG (decenas de KB)  ──esp_jpeg──▶  RGB565 en PSRAM  ──▶  pantalla
 *                            a 1/1, 1/2,      hasta 320x240,
 *                            1/4 o 1/8        150 KB
 *
 * El decodificador (TJpgDec, dentro del componente espressif/esp_jpeg) puede
 * reducir la imagen MIENTRAS la decodifica, a la mitad, a un cuarto o a un
 * octavo. Este módulo elige la escala más grande con la que la foto entera
 * cabe en la pantalla: un VGA (640x480) sale a la mitad, justo 320x240.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t *pixeles;      /* RGB565 en el orden de bytes del panel        */
    int       ancho;        /* ya reducida: cabe en 320x240                 */
    int       alto;
    int       divisor;      /* 1, 2, 4 u 8                                  */
} imagen_t;

/* Reserva el buffer de la imagen (150 KB, en PSRAM). Una sola vez. */
esp_err_t imagen_iniciar(void);

/* Decodifica un JPEG en el buffer de la imagen. El buffer es UNO solo: la
 * imagen anterior se pierde. ESP_ERR_INVALID_SIZE si la foto no cabe ni a
 * 1/8; ESP_FAIL si los datos no son un JPEG que el decodificador entienda. */
esp_err_t imagen_decodificar(const uint8_t *jpeg, size_t largo, imagen_t *img);

/* Decodifica a un divisor explícito (1, 2, 4 u 8). Para VGA, divisor 8 da
 * 80x60 y se usa para el detector de movimiento. */
esp_err_t imagen_decodificar_escala(const uint8_t *jpeg, size_t largo,
                                    int divisor, imagen_t *img);

/* Manda la imagen a la pantalla, centrada. Si cambió de tamaño desde la vez
 * anterior, borra antes la pantalla para que no queden restos en el marco. */
void imagen_mostrar(const imagen_t *img);
