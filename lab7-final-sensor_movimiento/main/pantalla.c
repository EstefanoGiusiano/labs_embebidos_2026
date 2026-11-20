/*
 * pantalla — implementación sobre esp_lcd (Clases 5 y 7, Sistemas Embebidos, UCU)
 *
 * Conexión de la ESP-LyraP-LCD32 sobre la Kaluga-1, igual en la v1.2 y en la
 * v1.3:
 *
 *      CLK   GPIO15      DC    GPIO13      RST   GPIO16
 *      MOSI  GPIO9       CS    GPIO11      BK    GPIO6  (ver abajo)
 *
 * Ninguno de estos pines lo usa la cámara: las dos placas funcionan juntas.
 *
 * El bus es SPI2 a propósito. En el ESP32-S2 el SPI3 es el único que sirve de
 * DMA al ADC y al DAC (ver el ejemplo `analogico` de la Clase 5).
 *
 * La retroiluminación (BK) depende de la revisión de la placa de pantalla:
 *
 *   - LCD32 v1.1 (kit v1.2): la gobierna el GPIO6 y enciende con el pin en
 *     BAJO. Una resistencia de la propia placa lo mantiene en alto mientras
 *     nadie lo maneje, así que sin esto el panel dibuja pero no se ve.
 *   - LCD32 v1.2 (kit v1.3): está encendida por hardware y el GPIO6 ya no
 *     llega a la pantalla.
 *
 * El GPIO6 se pone en bajo en los dos casos: en la v1.1 hace falta y en la
 * v1.2 no cambia nada en la pantalla.
 */

#include "pantalla.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LCD_HOST            SPI2_HOST
#define LCD_CLK             15
#define LCD_MOSI            9
#define LCD_DC              13
#define LCD_RST             16
#define LCD_CS              11
#define LCD_BK              6       /* -1: no tocar el pin */
#define LCD_RELOJ_HZ        (40 * 1000 * 1000)

/* Banda de trabajo: se dibuja en trozos de 16 filas como mucho. Un buffer de
 * pantalla entera serían 320x240x2 = 150 KB, más de lo que hay libre en la
 * SRAM interna. Con 320x16 bastan 10 KB, y el DMA los manda en una sola
 * transferencia. Es el compromiso clásico entre memoria y número de
 * transacciones. */
#define BANDA_FILAS         16
#define BUFFER_PIXELES      (PANTALLA_ANCHO * BANDA_FILAS)

static const char *TAG = "pantalla";

static esp_lcd_panel_handle_t s_panel;
static pantalla_controlador_t s_controlador;
static uint16_t *s_buf;          /* apto para DMA, en SRAM interna */
static SemaphoreHandle_t s_sem_enviado;   /* lo da la interrupción del SPI */

/* --- Fuente de 5x7, columna a columna, bit menos significativo arriba -----
 *
 * 42 glifos: espacio, cinco signos, diez dígitos y las 26 mayúsculas. Es la
 * fuente clásica de las pantallas de caracteres; cada glifo son 5 bytes, así
 * que la tabla entera ocupa 210 bytes de flash. */
static const uint8_t FUENTE[42][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 },   /* espacio */
    { 0x08, 0x08, 0x08, 0x08, 0x08 },   /* -       */
    { 0x00, 0x60, 0x60, 0x00, 0x00 },   /* .       */
    { 0x00, 0x36, 0x36, 0x00, 0x00 },   /* :       */
    { 0x3E, 0x51, 0x49, 0x45, 0x3E },   /* 0 */
    { 0x00, 0x42, 0x7F, 0x40, 0x00 },   /* 1 */
    { 0x42, 0x61, 0x51, 0x49, 0x46 },   /* 2 */
    { 0x21, 0x41, 0x45, 0x4B, 0x31 },   /* 3 */
    { 0x18, 0x14, 0x12, 0x7F, 0x10 },   /* 4 */
    { 0x27, 0x45, 0x45, 0x45, 0x39 },   /* 5 */
    { 0x3C, 0x4A, 0x49, 0x49, 0x30 },   /* 6 */
    { 0x01, 0x71, 0x09, 0x05, 0x03 },   /* 7 */
    { 0x36, 0x49, 0x49, 0x49, 0x36 },   /* 8 */
    { 0x06, 0x49, 0x49, 0x29, 0x1E },   /* 9 */
    { 0x7E, 0x11, 0x11, 0x11, 0x7E },   /* A */
    { 0x7F, 0x49, 0x49, 0x49, 0x36 },   /* B */
    { 0x3E, 0x41, 0x41, 0x41, 0x22 },   /* C */
    { 0x7F, 0x41, 0x41, 0x22, 0x1C },   /* D */
    { 0x7F, 0x49, 0x49, 0x49, 0x41 },   /* E */
    { 0x7F, 0x09, 0x09, 0x01, 0x01 },   /* F */
    { 0x3E, 0x41, 0x49, 0x49, 0x7A },   /* G */
    { 0x7F, 0x08, 0x08, 0x08, 0x7F },   /* H */
    { 0x00, 0x41, 0x7F, 0x41, 0x00 },   /* I */
    { 0x20, 0x40, 0x41, 0x3F, 0x01 },   /* J */
    { 0x7F, 0x08, 0x14, 0x22, 0x41 },   /* K */
    { 0x7F, 0x40, 0x40, 0x40, 0x40 },   /* L */
    { 0x7F, 0x02, 0x0C, 0x02, 0x7F },   /* M */
    { 0x7F, 0x04, 0x08, 0x10, 0x7F },   /* N */
    { 0x3E, 0x41, 0x41, 0x41, 0x3E },   /* O */
    { 0x7F, 0x09, 0x09, 0x09, 0x06 },   /* P */
    { 0x3E, 0x41, 0x51, 0x21, 0x5E },   /* Q */
    { 0x7F, 0x09, 0x19, 0x29, 0x46 },   /* R */
    { 0x46, 0x49, 0x49, 0x49, 0x31 },   /* S */
    { 0x01, 0x01, 0x7F, 0x01, 0x01 },   /* T */
    { 0x3F, 0x40, 0x40, 0x40, 0x3F },   /* U */
    { 0x1F, 0x20, 0x40, 0x20, 0x1F },   /* V */
    { 0x7F, 0x20, 0x18, 0x20, 0x7F },   /* W */
    { 0x63, 0x14, 0x08, 0x14, 0x63 },   /* X */
    { 0x03, 0x04, 0x78, 0x04, 0x03 },   /* Y */
    { 0x61, 0x51, 0x49, 0x45, 0x43 },   /* Z */
    { 0x20, 0x10, 0x08, 0x04, 0x02 },   /* /       */
    { 0x23, 0x13, 0x08, 0x64, 0x62 },   /* %       */
};

static int indice_glifo(char c)
{
    if (c >= '0' && c <= '9') { return 4 + (c - '0'); }
    if (c >= 'A' && c <= 'Z') { return 14 + (c - 'A'); }
    if (c >= 'a' && c <= 'z') { return 14 + (c - 'a'); }
    switch (c) {
    case '-': return 1;
    case '.': return 2;
    case ',': return 2;
    case ':': return 3;
    case '/': return 40;
    case '%': return 41;
    default:  return 0;             /* cualquier otra cosa, un espacio */
    }
}

/* Escribe un carácter de (6*escala) x (8*escala) píxeles en dst, una matriz
 * cuyas filas miden `paso` píxeles. Es la única función que sabe leer la
 * fuente: la usan el texto en pantalla y el texto dentro de una imagen. */
static void componer_caracter(uint16_t *dst, int paso, char c, int escala,
                              uint16_t frente, uint16_t fondo)
{
    const uint8_t *glifo = FUENTE[indice_glifo(c)];
    const int ancho_car = 6 * escala;       /* 5 de glifo + 1 de separación */
    const int alto_car = 8 * escala;

    for (int fy = 0; fy < alto_car; fy++) {
        for (int fx = 0; fx < ancho_car; fx++) {
            const int col = fx / escala;
            const int fil = fy / escala;
            const bool tinta = (col < 5) && (fil < 7)
                               && ((glifo[col] >> fil) & 1);
            dst[fy * paso + fx] = tinta ? frente : fondo;
        }
    }
}

/* ---------------------------------------------------------------- envío -- */

/* Contexto de interrupción: el SPI acaba de mandar el último byte de un
 * bloque de color. Solo da el semáforo; el patrón de siempre. */
static bool IRAM_ATTR al_terminar_envio(esp_lcd_panel_io_handle_t io,
                                        esp_lcd_panel_io_event_data_t *ev,
                                        void *ctx)
{
    BaseType_t hp_task_woken = pdFALSE;

    xSemaphoreGiveFromISR(s_sem_enviado, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

/* Manda a la pantalla el rectángulo que hay compuesto en s_buf y ESPERA a que
 * el SPI termine.
 *
 * La función de dibujo de esp_lcd no espera: encola la transferencia y
 * vuelve, y el DMA sigue leyendo s_buf después. Volver a escribir en el
 * buffer para el trozo siguiente sin haber esperado mezclaría los dos
 * dibujos, y tampoco daría ningún error. La tarea no espera dando vueltas: se
 * bloquea en un semáforo que da la interrupción del SPI, y mientras tanto la
 * CPU es de las demás. Es el patrón ISR -> semáforo -> tarea de la Clase 4. */
static void enviar(int x0, int y0, int x1, int y1)
{
    xSemaphoreTake(s_sem_enviado, 0);           /* por si quedó dado de antes */
    if (esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, s_buf) != ESP_OK) {
        return;
    }
    /* El plazo es solo una red de seguridad: los 10 KB de la banda más grande
     * tardan 2 ms a 40 MHz. */
    xSemaphoreTake(s_sem_enviado, pdMS_TO_TICKS(100));
}

/* ------------------------------------------------------------- arranque -- */

bool pantalla_iniciar(pantalla_controlador_t controlador)
{
    s_controlador = controlador;

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_CLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BUFFER_PIXELES * (int)sizeof(uint16_t) + 8,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo abrir el SPI2");
        return false;
    }

    s_sem_enviado = xSemaphoreCreateBinary();
    if (s_sem_enviado == NULL) {
        ESP_LOGE(TAG, "sin memoria para el semaforo de envio");
        return false;
    }

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_RELOJ_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = al_terminar_envio,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io) != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo crear el canal de la pantalla");
        return false;
    }

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    const esp_err_t err = (controlador == PANTALLA_ILI9341)
        ? esp_lcd_new_panel_ili9341(io, &panel_cfg, &s_panel)
        : esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo crear el panel");
        s_panel = NULL;
        return false;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* Sin inversión de color: con ella la LCD32 v1.1 salía en negativo. Si
     * en tu panel la imagen sale con el fondo claro y las letras oscuras,
     * en vez de fondo oscuro y letras claras, pon true. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
    /* De 240x320 vertical a 320x240 apaisado: intercambiar ejes y espejar.
     *
     * Los dos argumentos de mirror son los ejes del panel en vertical, no los
     * de la imagen apaisada. Con los ejes intercambiados el primero (x)
     * voltea la imagen de arriba abajo y el segundo (y), de izquierda a
     * derecha. Con (true, false) la LCD32 v1.1 mostraba todo como en un
     * espejo.
     *
     * Si en tu panel el TEXTO sale en espejo, cambia uno de los dos; si sale
     * bien pero cabeza abajo, cambia los dos. La orientación de la FOTO se
     * corrige aparte, en el sensor (CAMARA_VOLTEAR y CAMARA_ESPEJO). */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* El buffer de dibujo tiene que ser accesible por DMA: SRAM interna. */
    s_buf = heap_caps_malloc(BUFFER_PIXELES * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "sin memoria para el buffer de dibujo");
        s_panel = NULL;
        return false;
    }

#if LCD_BK >= 0
    /* La retroiluminación, lo último: en BAJO enciende (ver la cabecera). */
    const gpio_config_t bk = {
        .pin_bit_mask = 1ULL << LCD_BK,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bk));
    ESP_ERROR_CHECK(gpio_set_level(LCD_BK, 0));
#endif

    ESP_LOGI(TAG, "panel %s listo, %dx%d, SPI2 a %d MHz",
             pantalla_controlador(), PANTALLA_ANCHO, PANTALLA_ALTO,
             LCD_RELOJ_HZ / 1000000);
    return true;
}

bool pantalla_disponible(void)
{
    return s_panel != NULL;
}

const char *pantalla_controlador(void)
{
    return (s_controlador == PANTALLA_ILI9341) ? "ILI9341" : "ST7789";
}

/* --------------------------------------------------------------- dibujo -- */

void pantalla_rect(int x, int y, int ancho, int alto, uint16_t color)
{
    if (s_panel == NULL) {
        return;
    }
    /* Recorte contra los bordes: dibujar fuera de la pantalla no es un error
     * del panel, es un error nuestro, y aquí se absorbe en silencio. */
    if (x < 0) { ancho += x; x = 0; }
    if (y < 0) { alto += y;  y = 0; }
    if (x + ancho > PANTALLA_ANCHO) { ancho = PANTALLA_ANCHO - x; }
    if (y + alto > PANTALLA_ALTO)   { alto = PANTALLA_ALTO - y; }
    if (ancho <= 0 || alto <= 0) {
        return;
    }

    const int filas = (BUFFER_PIXELES / ancho) < BANDA_FILAS
                          ? (BUFFER_PIXELES / ancho) : BANDA_FILAS;
    for (int i = 0; i < ancho * filas; i++) {
        s_buf[i] = color;
    }

    for (int fila = 0; fila < alto; fila += filas) {
        const int n = (alto - fila < filas) ? (alto - fila) : filas;
        enviar(x, y + fila, x + ancho, y + fila + n);
    }
}

void pantalla_limpiar(uint16_t color)
{
    pantalla_rect(0, 0, PANTALLA_ANCHO, PANTALLA_ALTO, color);
}

int pantalla_ancho_texto(const char *texto, int escala)
{
    return (int)strlen(texto) * 6 * escala;
}

void pantalla_texto(int x, int y, const char *texto, int escala,
                    uint16_t frente, uint16_t fondo)
{
    if (s_panel == NULL) {
        return;
    }
    const int ancho_car = 6 * escala;
    const int alto_car = 8 * escala;
    if (escala < 1 || ancho_car * alto_car > BUFFER_PIXELES) {
        return;                             /* escala absurda */
    }

    if (y < 0 || y + alto_car > PANTALLA_ALTO) {
        return;
    }

    for (const char *p = texto; *p != '\0'; p++, x += ancho_car) {
        if (x + ancho_car > PANTALLA_ANCHO) {
            return;                         /* se salió por la derecha */
        }
        if (x < 0) {
            continue;                       /* carácter fuera por la izquierda */
        }
        /* Se compone el carácter entero en memoria y se manda de una vez. Un
         * píxel por transferencia SPI sería órdenes de magnitud más lento: el
         * coste de una transacción domina sobre el de los datos. */
        componer_caracter(s_buf, ancho_car, *p, escala, frente, fondo);
        enviar(x, y, x + ancho_car, y + alto_car);
    }
}

void pantalla_imagen(int x, int y, int ancho, int alto, const uint16_t *pixeles)
{
    if (s_panel == NULL || pixeles == NULL || ancho <= 0 || alto <= 0) {
        return;
    }

    /* La parte visible, y dónde empieza dentro de la imagen. */
    int x0 = x, y0 = y, x1 = x + ancho, y1 = y + alto;
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > PANTALLA_ANCHO) { x1 = PANTALLA_ANCHO; }
    if (y1 > PANTALLA_ALTO)  { y1 = PANTALLA_ALTO; }
    const int w = x1 - x0;
    if (w <= 0 || y1 <= y0) {
        return;
    }

    /* Cuántas filas enteras caben en la banda: 16 con el ancho completo. */
    const int filas = BUFFER_PIXELES / w;

    for (int fy = y0; fy < y1; fy += filas) {
        const int n = (y1 - fy < filas) ? (y1 - fy) : filas;

        /* De la PSRAM a la SRAM interna, fila a fila. El DMA del SPI no lee
         * de la PSRAM sin condiciones (alineación, caché), y con esta copia
         * no hace falta saberlas: el SPI siempre lee de s_buf. Cuesta
         * leer la imagen una vez más, unos pocos ms por pantalla. */
        for (int i = 0; i < n; i++) {
            const uint16_t *origen = pixeles + (size_t)(fy + i - y) * ancho + (x0 - x);
            memcpy(&s_buf[i * w], origen, (size_t)w * sizeof(uint16_t));
        }
        enviar(x0, fy, x1, fy + n);
    }
}

/* ------------------------------------------------- dibujo en una imagen -- */

void pantalla_rect_en_imagen(uint16_t *img, int ancho_img, int alto_img,
                             int x, int y, int ancho, int alto, uint16_t color)
{
    if (img == NULL) {
        return;
    }
    if (x < 0) { ancho += x; x = 0; }
    if (y < 0) { alto += y;  y = 0; }
    if (x + ancho > ancho_img) { ancho = ancho_img - x; }
    if (y + alto > alto_img)   { alto = alto_img - y; }
    for (int fy = y; fy < y + alto; fy++) {
        uint16_t *p = img + (size_t)fy * ancho_img + x;
        for (int fx = 0; fx < ancho; fx++) {
            p[fx] = color;
        }
    }
}

void pantalla_texto_en_imagen(uint16_t *img, int ancho_img, int alto_img,
                              int x, int y, const char *texto, int escala,
                              uint16_t frente, uint16_t fondo)
{
    const int ancho_car = 6 * escala;
    const int alto_car = 8 * escala;
    if (img == NULL || escala < 1 || y < 0 || y + alto_car > alto_img) {
        return;
    }
    for (const char *p = texto; *p != '\0'; p++, x += ancho_car) {
        if (x + ancho_car > ancho_img) {
            return;
        }
        if (x < 0) {
            continue;
        }
        componer_caracter(img + (size_t)y * ancho_img + x, ancho_img, *p,
                          escala, frente, fondo);
    }
}
