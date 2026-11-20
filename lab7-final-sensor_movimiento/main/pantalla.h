/*
 * pantalla — dibujo mínimo sobre la TFT del ESP-LyraP-LCD32 (Clases 5 y 7)
 *
 * Un envoltorio muy delgado sobre esp_lcd: rectángulos, texto e imágenes.
 * No hay librería gráfica, no hay LVGL y no hay buffer de pantalla completo
 * en la SRAM interna: todo sale por una banda de 16 filas.
 *
 * La versión de la Clase 7 añade lo que hace falta para una cámara:
 *
 *   - pantalla_imagen(): manda una imagen RGB565 que puede estar en PSRAM;
 *   - pantalla_texto_en_imagen() y pantalla_rect_en_imagen(): escriben
 *     DENTRO de una imagen en memoria, para componer la barra de estado
 *     sobre la foto antes de mandarla, sin parpadeo;
 *   - el controlador se elige al arrancar, no al compilar: no hace falta
 *     ninguna opción -D con los botones de VS Code.
 *
 * Es el mismo archivo en camara_pantalla y en camara_nube.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* La ESP-LyraP-LCD32 es un panel de 240x320. Lo usamos apaisado. */
#define PANTALLA_ANCHO      320
#define PANTALLA_ALTO       240

typedef enum {
    PANTALLA_ST7789 = 0,        /* LCD32 v1.1 (kit v1.2): el habitual        */
    PANTALLA_ILI9341,           /* algunas series de la LCD32 v1.2           */
} pantalla_controlador_t;

/* Color de 16 bits, 5 rojo + 6 verde + 5 azul.
 *
 * Los bytes van INVERTIDOS a propósito: el panel espera cada palabra de 16
 * bits con el byte más significativo primero, y el ESP32-S2 es little endian.
 * Invertir aquí, una vez, sale más barato que pedirle al driver que lo haga
 * en cada transferencia — y funciona igual con ST7789 que con ILI9341.
 *
 * Las imágenes que se mandan con pantalla_imagen() tienen que venir en ese
 * mismo orden: el decodificador JPEG lo hace con swap_color_bytes = 1. */
static inline uint16_t pantalla_color(uint8_t r, uint8_t g, uint8_t b)
{
    const uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

/* Arranca el panel. Devuelve false si el panel no se pudo iniciar: el
 * llamante sigue funcionando por consola y las demás funciones no hacen
 * nada. */
bool pantalla_iniciar(pantalla_controlador_t controlador);

bool pantalla_disponible(void);

/* Nombre del controlador, para el log de arranque. */
const char *pantalla_controlador(void);

void pantalla_limpiar(uint16_t color);

/* Rectángulo relleno. Recorta solo contra los bordes de la pantalla. */
void pantalla_rect(int x, int y, int ancho, int alto, uint16_t color);

/* Texto en una fuente de 5x7 escalada por un entero. Solo dígitos, mayúsculas
 * y unos pocos signos (- . , : / %): las minúsculas salen en mayúsculas y
 * cualquier otra cosa, como un espacio. */
void pantalla_texto(int x, int y, const char *texto, int escala,
                    uint16_t frente, uint16_t fondo);

/* Ancho en píxeles que ocupará ese texto, para poder centrarlo. */
int pantalla_ancho_texto(const char *texto, int escala);

/* Manda a la pantalla una imagen RGB565 de ancho x alto píxeles, fila a fila
 * y sin huecos, con la esquina superior izquierda en (x, y). La imagen puede
 * estar en PSRAM: se copia por bandas al buffer interno apto para DMA. Lo que
 * cae fuera de la pantalla se recorta. */
void pantalla_imagen(int x, int y, int ancho, int alto, const uint16_t *pixeles);

/* Lo mismo que pantalla_rect() y pantalla_texto(), pero sobre una imagen en
 * memoria de ancho_img x alto_img píxeles. No toca la pantalla. */
void pantalla_rect_en_imagen(uint16_t *img, int ancho_img, int alto_img,
                             int x, int y, int ancho, int alto, uint16_t color);
void pantalla_texto_en_imagen(uint16_t *img, int ancho_img, int alto_img,
                              int x, int y, const char *texto, int escala,
                              uint16_t frente, uint16_t fondo);
