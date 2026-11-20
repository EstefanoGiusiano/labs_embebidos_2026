/*
 * camara_nube — App 7 (Clase 7, Sistemas Embebidos, UCU 2026)
 *
 * Sensor de movimiento y fotos en la nube. La cámara del ejemplo 1,
 * conectada: cada foto se guarda en Google Drive, a través de un Apps Script,
 * por HTTPS, y queda anotada en una hoja de cálculo con su motivo, el RSSI y
 * la temperatura del chip. Cada foto guardada, y las medidas cada pocos
 * segundos, se avisan por MQTT a un panel.
 *
 *   [cam_task p23]  (driver) cuadros JPEG en PSRAM, sin parar
 *        │ esp_camera_fb_get()
 *        ▼
 *   [visor p2] ──foto: copia en PSRAM──▶ cola de fotos ──▶ [subida p5] ──HTTPS──▶ Apps Script
 *    el visor en pantalla y el detector  4 huecos           espera EV_WIFI_OK;       ├ Drive
 *    de movimiento (1/8, 80x60).                            reintenta con espera     └ Hoja
 *    Tres disparos piden una foto:                          creciente                │
 *      BOOT, el temporizador y                                       aviso de foto   │
 *      el movimiento                                                  por MQTT ◀─────┘
 *
 *   [conexion p3]  (red_wifi.c) reconexión WiFi con espera creciente
 *   [aviso p3]     arranca MQTT con la IP; RSSI y temperatura cada AVISO_MEDIDAS_S
 *   [mqtt_task p4] el cliente MQTT
 *   [timer]        informe cada 30 s; una foto cada FOTO_CADA_S si no es 0
 *
 * Hardware: el del ejemplo 1 (placa principal + cámara + pantalla, sin placa
 * de audio y sin el jumper RGB) y una red WiFi de 2,4 GHz con Internet.
 * La nube: el Apps Script de ../apps_script/, instalado con tu cuenta.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/temperature_sensor.h"
#include "jpeg_decoder.h"
#include "mqtt_client.h"
#include "camara.h"
#include "imagen.h"
#include "informe.h"
#include "nube.h"
#include "pantalla.h"
#include "red_wifi.h"

/* ======================================================================== */
/*  CONFIGURACIÓN: editar antes de compilar                                  */
/* ======================================================================== */

/* --- La red WiFi, como en la Clase 6 ------------------------------------ */
#define WIFI_SSID       "Apto302"
#define WIFI_CLAVE      "26224069"

/* Seguridad de la red: descomenta UNA sola línea. Es el MÍNIMO que se acepta. */
//#define WIFI_SEGURIDAD_WPA        /* WPA  (TKIP): routers muy antiguos   */
#define WIFI_SEGURIDAD_WPA2         /* WPA2 (AES):  la gran mayoría        */
//#define WIFI_SEGURIDAD_WPA3       /* WPA3 (SAE):  redes WPA3 o WPA2/WPA3 */

/* --- La nube: lo que dio el Apps Script al instalarlo -------------------- */
/* La URL de la implementación, la que termina en /exec. */
#define NUBE_URL        "https://script.google.com/macros/s/AKfycbwfmqtYWdBkXhC5uj1Z--x9QPCUSVFxa9DPTXwR71oDK7r__ocxXT4sc_Qvk4PbNr7-/exec"

/* El token que imprimió configurar(). Es la llave de tu Drive: como la clave
 * del WiFi, no se publica ni se sube al repositorio. */
#define NUBE_TOKEN      "2e5bacf7d3be4f74b9c6754146aeebc6"

/* --- La placa principal y la pantalla, como en el ejemplo 1 -------------- */
//#define KALUGA_V1_2                 /* "ESP32-S2-Kaluga-1 V1.2"            */
#define KALUGA_V1_3

#define PANTALLA_CON_ST7789         /* LCD32 v1.1 (kit v1.2): la habitual  */
//#define PANTALLA_CON_ILI9341

/* --- La foto ------------------------------------------------------------- */
#define FOTO_TAMANO         FRAMESIZE_QVGA   /* 640x480: unos 30-60 KB      */
#define FOTO_CALIDAD        12              /* 0-63: MENOS es MEJOR        */
#define CAMARA_VOLTEAR      true
#define CAMARA_ESPEJO       false

/* --- Los avisos por MQTT, como en la Clase 6 ----------------------------- */
/* El panel tiene que apuntar al MISMO broker y a la MISMA raíz. */
#define MQTT_BROKER     "mqtt://broker.hivemq.com:1883"
//#define MQTT_BROKER   "mqtt://broker.emqx.io:1883"
#define MQTT_RAIZ       "ucu-se2026"
#define AVISO_MEDIDAS_S     10      /* cada cuántos s van RSSI y temperatura */

/* --- Los tres disparadores de foto ---------------------------------------
 *
 *   BOOT         siempre: el antirrebote es el único freno.
 *   temporizador una foto cada FOTO_CADA_S s (de 60 a 180 s); 0 lo apaga. Si
 *                hubo otra foto hace menos de FOTO_TIEMPO_MUERTO_S, se omite.
 *   movimiento   el visor compara cuadros a 1/8 (80x60). Después de una foto,
 *                de cualquier motivo, se ignora durante MOVIMIENTO_TIEMPO_MUERTO_S:
 *                una persona que se mueve no llena la cola de fotos. */
#define FOTO_CADA_S                 40
#define FOTO_TIEMPO_MUERTO_S        10

#define MOVIMIENTO_ACTIVO           1       /* 0: sin detector (para medir el visor) */
#define MOVIMIENTO_TIEMPO_MUERTO_S  20
#define MOVIMIENTO_UMBRAL_PIXEL     24      /* cuánto cambia un píxel (0-255)        */
#define MOVIMIENTO_AREA_X10         20      /* % del cuadro que cambia, en décimas   */
#define MOVIMIENTO_CUADROS          2       /* cuadros seguidos por encima del área  */

/* ======================================================================== */

#if (defined(WIFI_SEGURIDAD_WPA) + defined(WIFI_SEGURIDAD_WPA2) + \
     defined(WIFI_SEGURIDAD_WPA3)) != 1
#error "Descomenta exactamente una: WIFI_SEGURIDAD_WPA, _WPA2 o _WPA3"
#endif
#if (defined(KALUGA_V1_2) + defined(KALUGA_V1_3)) != 1
#error "Descomenta exactamente una: KALUGA_V1_2 o KALUGA_V1_3"
#endif
#if (defined(PANTALLA_CON_ST7789) + defined(PANTALLA_CON_ILI9341)) != 1
#error "Descomenta exactamente una: PANTALLA_CON_ST7789 o PANTALLA_CON_ILI9341"
#endif
#if FOTO_CADA_S < 0 || (FOTO_CADA_S > 0 && FOTO_CADA_S <= FOTO_TIEMPO_MUERTO_S)
#error "FOTO_CADA_S: 0 (apagada) o mayor que FOTO_TIEMPO_MUERTO_S"
#endif

#if defined(WIFI_SEGURIDAD_WPA)
#define WIFI_SEGURIDAD      RED_WPA
#elif defined(WIFI_SEGURIDAD_WPA2)
#define WIFI_SEGURIDAD      RED_WPA2
#else
#define WIFI_SEGURIDAD      RED_WPA3
#endif
#ifdef KALUGA_V1_2
#define KIT_CAMARA          CAMARA_KALUGA_V1_2
#else
#define KIT_CAMARA          CAMARA_KALUGA_V1_3
#endif
#ifdef PANTALLA_CON_ST7789
#define CONTROLADOR         PANTALLA_ST7789
#else
#define CONTROLADOR         PANTALLA_ILI9341
#endif

#define BOTON_BOOT          GPIO_NUM_0
#define ANTIRREBOTE_MS      300

/* --- Prioridades ---------------------------------------------------------
 *
 * La pila de red de ESP-IDF va por encima de todo lo nuestro (wifi 23,
 * sys_evt 20, tcpip 18). El driver de la cámara también: cam_task 23.
 *
 *   5  subida   casi siempre bloqueada en la red; cuando le toca la CPU
 *               (base64, el saludo TLS) es poco rato y no debe esperar al visor
 *   4  mqtt_task el cliente MQTT: por debajo de subida, que durante el saludo
 *               TLS lo atrasa un par de segundos (el keepalive es de 30 s)
 *   3  aviso    (aviso.c) casi siempre dormida: lee RSSI y temperatura y publica
 *   3  conexion (red_wifi.c) solo decide cuándo reintentar el WiFi
 *   2  visor    decodificar JPEG es CPU pura: abajo del todo, para que use
 *               lo que sobra y no lo que necesitan los demás
 */
#define PRIO_SUBIDA         5
#define PRIO_MQTT           4
#define PRIO_AVISO          3
#define PRIO_VISOR          2

/* En bytes. La de subida es grande: el saludo TLS (mbedTLS) corre en la
 * tarea que llama a esp_http_client. Se ajustan con el informe. */
#define PILA_SUBIDA         8192
#define PILA_VISOR          4096

#define PAUSA_VISOR_MS      5
#define FOTO_EN_PANTALLA_MS 2000

/* --- La cola de fotos ------------------------------------------------------
 * Cada hueco es un puntero a una foto en PSRAM: 4 fotos VGA son unos 200 KB.
 * Con la cola llena, la foto NUEVA se descarta y se avisa en pantalla: quien
 * pulsó el botón lo ve, y las que ya estaban esperando no se pierden. */
#define COLA_FOTOS_LARGO    4

/* --- Reintentos ------------------------------------------------------------ */
#define ESPERA_MIN_MS       2000
#define ESPERA_MAX_MS       60000
#define INTENTOS_SERVIDOR   5       /* errores del servidor, por foto        */

#define INFORME_PERIODO_MS  30000

/* Quién pide la foto: bits de la notificación al visor. */
#define AVISO_BOTON         (1u << 0)
#define AVISO_MOVIMIENTO    (1u << 1)
#define AVISO_PERIODICA     (1u << 2)

enum { MOTIVO_BOTON = 0, MOTIVO_MOVIMIENTO, MOTIVO_PERIODICA, MOTIVOS };
static const char *const NOMBRE_MOTIVO[MOTIVOS] = { "boton", "movimiento", "periodica" };

#define NEGRO               0x0000
#define BLANCO              0xFFFF

static const char *TAG = "camara_nube";

/* ======================================================================== */
/*  Tipos                                                                    */
/* ======================================================================== */

/* Lo que viaja por la cola, POR COPIA: el puntero y los datos de la foto.
 * La memoria del JPEG es de quien tiene la foto en la mano: del visor hasta
 * que la encola, de "subida" desde que la saca. Quien la tiene, la libera. */
typedef struct {
    uint8_t *jpeg;
    size_t   largo;
    int64_t  captura_us;
    uint32_t numero;
    uint16_t ancho;
    uint16_t alto;
    const char *motivo;         /* un literal de NOMBRE_MOTIVO               */
    int      rssi;              /* dBm al capturar                           */
    int32_t  temp_x100;         /* centésimas de °C del chip al capturar     */
} foto_t;

/* Lo que se cuenta y lo que enseña la barra de estado. Lo escriben el visor
 * y "subida"; se toca con el mutex. */
typedef struct {
    uint32_t tomadas;
    uint32_t cola_llena;        /* descartadas al tomarlas                  */
    uint32_t sin_memoria;
    uint32_t subidas;
    uint32_t repetidas;         /* el script ya la tenía: no se guardó dos veces */
    uint32_t rechazadas;        /* la nube dijo que no, o demasiados errores */
    uint32_t reintentos;
    uint64_t bytes;             /* enviados en las subidas buenas           */
    uint32_t subida_ms_max;     /* de empezar el POST a tener la respuesta  */
    uint64_t subida_ms_suma;
    uint32_t extremo_ms_max;    /* de la captura a tenerla en la nube       */
    uint32_t por_motivo[MOTIVOS];   /* fotos tomadas por cada disparador     */
    uint32_t coincidencias;     /* dos disparos a la vez: una sola foto      */
    uint32_t periodicas_omitidas;   /* el temporizador cayó en tiempo muerto */
    uint32_t mov_detectados;    /* veces que el detector dijo "cambió"       */
    uint32_t mov_omitidos;      /* ...y cayeron en tiempo muerto             */
    uint32_t mov_pico_x10;      /* mayor % de píxeles cambiados, en décimas  */
    /* El visor desde el informe anterior (se pone a cero al imprimir): */
    uint32_t v_cuadros;
    uint64_t v_us_total, v_us_espera, v_us_copia, v_us_decod, v_us_mov,
             v_us_pantalla;
    bool     en_mano;           /* "subida" tiene una foto (subiendo o esperando
                                 * para reintentar)                          */
    bool     subiendo;          /* dentro de nube_subir() ahora mismo        */
    char     ultimo[24];        /* para la pantalla: "OK 2.7 S", "ERROR RED" */
} estado_t;

/* ======================================================================== */
/*  Objetos del kernel y estado                                              */
/* ======================================================================== */

static EventGroupHandle_t s_eventos;        /* EV_WIFI_OK (de red_wifi.c)   */
static QueueHandle_t      s_cola_fotos;     /* visor -> subida              */
static SemaphoreHandle_t  s_mutex_estado;
static TaskHandle_t       s_tarea_visor;
static TimerHandle_t      s_timer_informe;
static TimerHandle_t      s_timer_foto;

static estado_t s_estado = { .ultimo = "LISTA" };
static bool     s_nube_lista;
#if MOVIMIENTO_ACTIVO
static bool     s_mov_listo;
#endif

static uint8_t *s_jpeg;                     /* el cuadro del visor          */
static volatile uint32_t s_pulsacion_ms;    /* cuándo se pidió la foto      */
static volatile uint32_t s_foto_ms;         /* cuándo empezó la última foto */
static volatile bool     s_hubo_foto;
static int64_t  s_periodica_us;             /* captura de la última periódica */

static char s_id[24];                       /* "kaluga-0ab2c3", de la MAC   */
static char s_arranque[12];                 /* al azar en cada arranque     */

/* ======================================================================== */
/*  Utilidades                                                               */
/* ======================================================================== */

static inline uint32_t us_a_ms(int64_t us)
{
    return (uint32_t)(us / 1000);
}

static void anotar(const char *ultimo)
{
    xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
    strlcpy(s_estado.ultimo, ultimo, sizeof(s_estado.ultimo));
    xSemaphoreGive(s_mutex_estado);
}

static void barra_de_estado(imagen_t *img, const char *texto)
{
    const int escala = (img->ancho >= 240) ? 2 : 1;
    const int alto = 8 * escala + 4;
    const int y = img->alto - alto;

    pantalla_rect_en_imagen(img->pixeles, img->ancho, img->alto,
                            0, y, img->ancho, alto, NEGRO);
    pantalla_texto_en_imagen(img->pixeles, img->ancho, img->alto,
                             4, y + 2, texto, escala, BLANCO, NEGRO);
}

static void marco(imagen_t *img, int grosor)
{
    uint16_t *p = img->pixeles;
    const int w = img->ancho;
    const int h = img->alto;
    pantalla_rect_en_imagen(p, w, h, 0, 0, w, grosor, BLANCO);
    pantalla_rect_en_imagen(p, w, h, 0, h - grosor, w, grosor, BLANCO);
    pantalla_rect_en_imagen(p, w, h, 0, 0, grosor, h, BLANCO);
    pantalla_rect_en_imagen(p, w, h, w - grosor, 0, grosor, h, BLANCO);
}

/* La línea del visor: red, fotos pendientes y lo último que pasó. */
static void texto_de_estado(char *dst, size_t n)
{
    const bool red = (xEventGroupGetBits(s_eventos) & EV_WIFI_OK) != 0;

    xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
    const unsigned pendientes = (unsigned)uxQueueMessagesWaiting(s_cola_fotos)
                                + (s_estado.en_mano ? 1u : 0u);
    char ultimo[sizeof(s_estado.ultimo)];
    strlcpy(ultimo, s_estado.subiendo ? "SUBIENDO" : s_estado.ultimo, sizeof(ultimo));
    xSemaphoreGive(s_mutex_estado);

    snprintf(dst, n, "%s COLA %u %s", red ? "RED" : "SIN RED", pendientes, ultimo);
}

/* ======================================================================== */
/*  Medidas: RSSI y temperatura del chip                                     */
/* ======================================================================== */

static temperature_sensor_handle_t s_tsens;

static esp_err_t medidas_iniciar(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
    if (err != ESP_OK) {
        return err;
    }
    return temperature_sensor_enable(s_tsens);
}

/* RSSI en dBm (0 si no hay red) y temperatura en centésimas de °C
 * (4137 = 41,37 °C). false, con ceros, si el sensor de temperatura falló. */
static bool medidas_leer(int *rssi, int32_t *temp_x100)
{
    *rssi = 0;
    *temp_x100 = 0;

    int r = 0;
    if (esp_wifi_sta_get_rssi(&r) == ESP_OK) {
        *rssi = r;
    }

    float celsius = 0.0f;
    if (s_tsens == NULL ||
        temperature_sensor_get_celsius(s_tsens, &celsius) != ESP_OK) {
        return false;
    }
    /* Del float a centésimas AQUÍ, redondeando: el resto no ve un float. */
    *temp_x100 = (int32_t)(celsius * 100.0f + (celsius >= 0.0f ? 0.5f : -0.5f));
    return true;
}

/* ======================================================================== */
/*  Detector de movimiento                                                   */
/* ======================================================================== */

/* Por diferencia de cuadros, barato: el JPEG del visor se decodifica a 1/8
 * (un VGA da 80 x 60), y a esa escala TJpgDec solo usa la media (coeficiente
 * DC) de cada bloque de 8x8, sin la transformada completa.
 *
 *   JPEG del visor ──esp_jpeg a 1/8──▶ 80x60 en gris ──resta──▶ cuadro anterior
 *   se quita el cambio de brillo de TODA la imagen (auto-exposición, una luz)
 *   y se cuentan los píxeles que cambiaron más que MOVIMIENTO_UMBRAL_PIXEL: si
 *   son más de MOVIMIENTO_AREA_X10 durante MOVIMIENTO_CUADROS cuadros seguidos,
 *   la escena cambió. */
#if MOVIMIENTO_ACTIVO

typedef struct {
    uint16_t cambiados;         /* píxeles que cambiaron en este cuadro      */
    uint16_t total;             /* píxeles comparados (4800 en un VGA)       */
    int16_t  brillo;            /* cambio de brillo medio que se quitó       */
} movimiento_medida_t;

/* La memoria de trabajo de TJpgDec (ver imagen.c): 4 KB, alineada a 4. */
static uint8_t s_mov_trabajo[4096] __attribute__((aligned(4)));

static uint16_t *s_mov_rgb;        /* salida a 1/8, RGB565                   */
static uint8_t  *s_mov_gris;       /* el cuadro actual en gris               */
static uint8_t  *s_mov_previo;     /* el cuadro anterior en gris             */
static size_t    s_mov_n;          /* píxeles de cada uno                    */
static int       s_mov_ancho;
static int       s_mov_alto;
static bool      s_mov_hay_previo;
static uint8_t   s_mov_seguidos;

/* ancho y alto del JPEG de la cámara: de ahí sale el tamaño a 1/8. Reserva
 * unos 20 KB en PSRAM. */
static esp_err_t movimiento_iniciar(uint16_t ancho, uint16_t alto)
{
    s_mov_n = (size_t)((ancho + 7) / 8) * ((alto + 7) / 8);
    s_mov_rgb = heap_caps_malloc(s_mov_n * sizeof(uint16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mov_gris = heap_caps_malloc(2 * s_mov_n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_mov_rgb == NULL || s_mov_gris == NULL) {
        free(s_mov_rgb);
        free(s_mov_gris);
        s_mov_rgb = NULL;
        s_mov_gris = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_mov_previo = s_mov_gris + s_mov_n;
    ESP_LOGI(TAG, "detector listo: %u px a 1/8, pixel > %d, area > %d,%d %%, "
                  "%d cuadro(s) seguido(s)", (unsigned)s_mov_n,
             MOVIMIENTO_UMBRAL_PIXEL, MOVIMIENTO_AREA_X10 / 10,
             MOVIMIENTO_AREA_X10 % 10, MOVIMIENTO_CUADROS);
    return ESP_OK;
}

/* Olvida el cuadro anterior: al volver de una foto, la escena pudo cambiar
 * mientras el visor estaba parado. */
static void movimiento_reiniciar(void)
{
    s_mov_hay_previo = false;
    s_mov_seguidos = 0;
}

/* RGB565 (sin intercambiar bytes) a luminancia de 8 bits. */
static inline uint8_t a_gris(uint16_t p)
{
    const uint32_t r = ((p >> 11) & 0x1F) << 3;
    const uint32_t g = ((p >> 5) & 0x3F) << 2;
    const uint32_t b = (p & 0x1F) << 3;
    return (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
}

/* Analiza un cuadro. true cuando la escena cambió durante los cuadros
 * seguidos que pide la configuración. El primer cuadro después de
 * movimiento_reiniciar() solo sirve de referencia. */
static bool movimiento_analizar(const uint8_t *jpeg, size_t largo,
                                movimiento_medida_t *medida)
{
    *medida = (movimiento_medida_t){ 0 };
    if (s_mov_rgb == NULL || jpeg == NULL || largo == 0) {
        return false;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = largo,
        .outbuf = (uint8_t *)s_mov_rgb,
        .outbuf_size = s_mov_n * sizeof(uint16_t),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_1_8,
        .advanced.working_buffer = s_mov_trabajo,
        .advanced.working_buffer_size = sizeof(s_mov_trabajo),
    };
    esp_jpeg_image_output_t salida;
    if (esp_jpeg_decode(&cfg, &salida) != ESP_OK) {
        return false;
    }
    const size_t n = (size_t)salida.width * salida.height;
    if (n != s_mov_n) {
        return false;
    }
    if (salida.width != s_mov_ancho || salida.height != s_mov_alto) {
        s_mov_ancho = salida.width;
        s_mov_alto = salida.height;
        movimiento_reiniciar();
    }

    int32_t suma_dif = 0;
    for (size_t i = 0; i < n; i++) {
        s_mov_gris[i] = a_gris(s_mov_rgb[i]);
        suma_dif += (int32_t)s_mov_gris[i] - (int32_t)s_mov_previo[i];
    }
    if (!s_mov_hay_previo) {
        for (size_t i = 0; i < n; i++) {
            s_mov_previo[i] = s_mov_gris[i];
        }
        s_mov_hay_previo = true;
        return false;
    }

    const int32_t brillo = suma_dif / (int32_t)n;
    uint32_t cambiados = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t d = (int32_t)s_mov_gris[i] - (int32_t)s_mov_previo[i] - brillo;
        if (abs(d) > MOVIMIENTO_UMBRAL_PIXEL) {
            cambiados++;
        }
        s_mov_previo[i] = s_mov_gris[i];
    }

    medida->cambiados = (uint16_t)cambiados;
    medida->total = (uint16_t)n;
    medida->brillo = (int16_t)brillo;
    const bool supera = cambiados * 1000u > (uint32_t)MOVIMIENTO_AREA_X10 * n;
    s_mov_seguidos = supera ? (uint8_t)((s_mov_seguidos < 255) ? s_mov_seguidos + 1 : 255) : 0;
    if (s_mov_seguidos >= MOVIMIENTO_CUADROS) {
        s_mov_seguidos = 0;
        return true;
    }
    return false;
}

#endif /* MOVIMIENTO_ACTIVO */

/* ======================================================================== */
/*  Avisos por MQTT al panel                                                 */
/* ======================================================================== */

/* La placa nunca manda una foto por aquí (son decenas de KB y el broker es
 * público): manda DATOS, y el panel enseña la foto desde Drive.
 *
 *   <MQTT_RAIZ>/<ID>/medidas  QoS 0, retenido  {"rssi":-58,"t":41.37,"seq":12}
 *                             cada AVISO_MEDIDAS_S: RSSI y temperatura en el tiempo
 *   <MQTT_RAIZ>/<ID>/foto     QoS 1, retenido  una por foto YA GUARDADA en Drive
 *   <MQTT_RAIZ>/<ID>/estado   QoS 1, retenido  en_linea; "desconectado" es la
 *                             última voluntad
 *
 * La tarea "aviso" espera el WiFi, arranca el cliente MQTT y publica las
 * medidas. Los avisos de foto se ENCOLAN en el cliente: la tarea de subida no
 * espera al broker. */

/* Bit del grupo de eventos: hay conexión con el broker. EV_WIFI_OK es BIT0. */
#define EV_MQTT_OK          BIT1

#define VERSION_APP         "app7-1.0"
#define KEEPALIVE_S         30
#define PILA_AVISO          3072
#define PILA_MQTT           6144
#define ESTADO_DESCONECTADO "{\"estado\":\"desconectado\"}"

typedef struct {
    uint32_t    numero;         /* foto n.º desde el arranque                */
    const char *motivo;         /* "boton", "movimiento" o "periodica"       */
    int         rssi;           /* dBm al sacar la foto                      */
    int32_t     temp_x100;      /* centésimas de °C del chip al sacar la foto */
    const char *archivo;        /* nombre del archivo en Drive               */
    const char *drive_id;       /* identificador del archivo en Drive        */
    uint32_t    ms_subida;      /* POST + respuesta                          */
    uint32_t    ms_captura;     /* de la captura a tenerla en la nube        */
    size_t      bytes;          /* del POST                                  */
    bool        repetida;
} aviso_foto_t;

static esp_mqtt_client_handle_t s_mqtt;
static volatile uint32_t        s_avisos_encolados;
static volatile uint32_t        s_avisos_perdidos;

/* El cliente MQTT guarda punteros al identificador y al tema de la última
 * voluntad: tienen que vivir todo el programa. */
static char s_tema_medidas[64];
static char s_tema_foto[64];
static char s_tema_estado[64];

/* Centésimas a texto con PUNTO decimal (formato JSON). */
static void x100_a_texto(char *dst, size_t n, int32_t x100)
{
    const char *signo = (x100 < 0) ? "-" : "";
    const int32_t a = (x100 < 0) ? -x100 : x100;
    snprintf(dst, n, "%s%" PRId32 ".%02" PRId32, signo, a / 100, a % 100);
}

/* Un nombre de archivo o un identificador de Drive va dentro de un JSON sin
 * escapar: solo [A-Za-z0-9_.-], lo demás se vuelve '_'. */
static void limpiar(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src != NULL && src[i] != '\0' && i + 1 < n; i++) {
        const char c = src[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        dst[i] = ok ? c : '_';
    }
    dst[i] = '\0';
}

static void publicar_estado_en_linea(void)
{
    red_estado_t w;
    red_wifi_estado(&w);
    char json[96];
    snprintf(json, sizeof(json),
             "{\"estado\":\"en_linea\",\"ip\":\"" IPSTR "\",\"version\":\"%s\"}",
             IP2STR(&w.ip), VERSION_APP);
    /* Dentro del manejador de eventos solo se encola: publicar bloquearía al
     * cliente que está ejecutando este manejador. */
    esp_mqtt_client_enqueue(s_mqtt, s_tema_estado, json, 0, 1, 1, true);
}

static void al_evento_mqtt(void *arg, esp_event_base_t base, int32_t id, void *datos)
{
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[mqtt] conectado a %s como %s", MQTT_BROKER, s_id);
        publicar_estado_en_linea();
        xEventGroupSetBits(s_eventos, EV_MQTT_OK);
        break;
    case MQTT_EVENT_DISCONNECTED:
        xEventGroupClearBits(s_eventos, EV_MQTT_OK);
        ESP_LOGW(TAG, "[mqtt] desconectado; el cliente reintenta solo");
        break;
    default:
        break;
    }
}

static void tarea_aviso(void *arg)
{
    /* Arrancar el cliente con la primera IP: antes, su primer intento
     * fallaría y el siguiente tardaría el tiempo de reconexión. */
    xEventGroupWaitBits(s_eventos, EV_WIFI_OK, pdFALSE, pdTRUE, portMAX_DELAY);
    if (esp_mqtt_client_start(s_mqtt) != ESP_OK) {
        ESP_LOGE(TAG, "no arranca el cliente MQTT");
        vTaskDelete(NULL);
    }

    uint32_t secuencia = 0;
    TickType_t proximo = xTaskGetTickCount();
    while (1) {
        xTaskDelayUntil(&proximo, pdMS_TO_TICKS(AVISO_MEDIDAS_S * 1000));
        if (!(xEventGroupGetBits(s_eventos) & EV_MQTT_OK)) {
            continue;
        }

        int rssi = 0;
        int32_t temp_x100 = 0;
        const bool temp_ok = medidas_leer(&rssi, &temp_x100);
        char t[16] = "null";
        if (temp_ok) {
            x100_a_texto(t, sizeof(t), temp_x100);
        }
        char json[80];
        snprintf(json, sizeof(json), "{\"rssi\":%d,\"t\":%s,\"seq\":%" PRIu32 "}",
                 rssi, t, secuencia++);
        /* QoS 0 y retenido, como la temperatura de la Clase 6: una medida
         * perdida se reemplaza en un periodo; la última queda para quien
         * se conecte tarde. */
        esp_mqtt_client_publish(s_mqtt, s_tema_medidas, json, 0, 0, 1);
    }
}

/* Después de identidad(): necesita s_id. */
static esp_err_t aviso_iniciar(void)
{
    snprintf(s_tema_medidas, sizeof(s_tema_medidas), "%s/%s/medidas", MQTT_RAIZ, s_id);
    snprintf(s_tema_foto, sizeof(s_tema_foto), "%s/%s/foto", MQTT_RAIZ, s_id);
    snprintf(s_tema_estado, sizeof(s_tema_estado), "%s/%s/estado", MQTT_RAIZ, s_id);

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = MQTT_BROKER,
        .credentials.client_id = s_id,
        .session = {
            .keepalive = KEEPALIVE_S,
            .last_will = {
                .topic = s_tema_estado,
                .msg = ESTADO_DESCONECTADO,
                .qos = 1,
                .retain = 1,
            },
        },
        .task = {
            .priority = PRIO_MQTT,
            .stack_size = PILA_MQTT,
        },
    };
    s_mqtt = esp_mqtt_client_init(&mc);
    if (s_mqtt == NULL) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY,
                                                   al_evento_mqtt, NULL);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(tarea_aviso, "aviso", PILA_AVISO, NULL, PRIO_AVISO,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "avisos a %s, cada %d s: %s", MQTT_BROKER, AVISO_MEDIDAS_S,
             s_tema_medidas);
    return ESP_OK;
}

/* Encola el aviso de una foto ya guardada. false si no cupo. */
static bool aviso_foto(const aviso_foto_t *f)
{
    if (s_mqtt == NULL) {
        return false;
    }
    char archivo[100];
    char drive[68];
    char t[16];
    limpiar(archivo, sizeof(archivo), f->archivo);
    limpiar(drive, sizeof(drive), f->drive_id);
    x100_a_texto(t, sizeof(t), f->temp_x100);

    char json[400];
    const int n = snprintf(json, sizeof(json),
        "{\"n\":%" PRIu32 ",\"motivo\":\"%s\",\"rssi\":%d,\"t\":%s,"
        "\"archivo\":\"%s\",\"drive\":\"%s\",\"ms_subida\":%" PRIu32
        ",\"ms_captura\":%" PRIu32 ",\"bytes\":%u,\"repetida\":%s}",
        f->numero, f->motivo, f->rssi, t, archivo, drive, f->ms_subida,
        f->ms_captura, (unsigned)f->bytes, f->repetida ? "true" : "false");
    if (n <= 0 || n >= (int)sizeof(json)) {
        s_avisos_perdidos++;
        return false;
    }
    /* QoS 1 y retenido: un aviso de foto no se debe perder, y el panel que
     * se abre tarde ve la última. enqueue() copia y vuelve. */
    if (esp_mqtt_client_enqueue(s_mqtt, s_tema_foto, json, 0, 1, 1, true) < 0) {
        s_avisos_perdidos++;
        return false;
    }
    s_avisos_encolados++;
    return true;
}

/* ======================================================================== */
/*  El botón BOOT y la foto periódica                                        */
/* ======================================================================== */

/* ¿Hubo una foto, de cualquier motivo, hace menos de `plazo_s`? */
static bool en_tiempo_muerto(uint32_t plazo_s)
{
    const uint32_t ahora_ms = us_a_ms(esp_timer_get_time());
    return s_hubo_foto && (uint32_t)(ahora_ms - s_foto_ms) < plazo_s * 1000u;
}

static void IRAM_ATTR al_pulsar_boot(void *arg)
{
    const uint32_t ahora_ms = (uint32_t)(esp_timer_get_time() / 1000);
    static uint32_t anterior_ms;

    if ((uint32_t)(ahora_ms - anterior_ms) < ANTIRREBOTE_MS) {
        return;
    }
    anterior_ms = ahora_ms;
    s_pulsacion_ms = ahora_ms;

    BaseType_t despertar = pdFALSE;
    xTaskNotifyFromISR(s_tarea_visor, AVISO_BOTON, eSetBits, &despertar);
    portYIELD_FROM_ISR(despertar);
}

/* Corre en "Tmr Svc". Si acaba de salir otra foto, esta se omite y se cuenta:
 * dos fotos casi iguales con segundos de diferencia no aportan nada. */
static void foto_periodica(TimerHandle_t timer)
{
    if (en_tiempo_muerto(FOTO_TIEMPO_MUERTO_S)) {
        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        s_estado.periodicas_omitidas++;
        xSemaphoreGive(s_mutex_estado);
        return;
    }
    s_pulsacion_ms = (uint32_t)(esp_timer_get_time() / 1000);
    xTaskNotify(s_tarea_visor, AVISO_PERIODICA, eSetBits);
}

static void configurar_boton(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOTON_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    /* El servicio ya lo instaló el driver de la cámara: ESP_ERR_INVALID_STATE
     * es lo esperado. */
    const esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(BOTON_BOOT, al_pulsar_boot, NULL));
}

/* ======================================================================== */
/*  La foto: del driver a la cola                                            */
/* ======================================================================== */

static void tomar_foto(uint32_t pedidos)
{
    const uint32_t pulsacion_ms = s_pulsacion_ms;

    /* Si piden a la vez varios disparadores, sale UNA foto y manda el motivo
     * más "humano": el botón, luego el movimiento, luego el reloj. */
    const int motivo = (pedidos & AVISO_BOTON)       ? MOTIVO_BOTON
                     : (pedidos & AVISO_MOVIMIENTO)  ? MOTIVO_MOVIMIENTO
                                                     : MOTIVO_PERIODICA;
    /* El tiempo muerto cuenta desde que se ACEPTA la foto, no desde que sale:
     * un disparo que llega mientras se toma ya cae dentro. */
    s_foto_ms = us_a_ms(esp_timer_get_time());
    s_hubo_foto = true;

    /* El primer cuadro que empezó DESPUÉS de pedir la foto (ejemplo 1). */
    camera_fb_t *fb = esp_camera_fb_get();
    for (int intento = 0; fb != NULL && intento < 4; intento++) {
        if ((int32_t)(us_a_ms(camara_marca_us(fb)) - pulsacion_ms) >= 0) {
            break;
        }
        esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
    }
    if (fb == NULL) {
        ESP_LOGW(TAG, "[foto] la camara no entrego ningun cuadro");
        return;
    }

    int rssi = 0;
    int32_t temp_x100 = 0;
    (void)medidas_leer(&rssi, &temp_x100);

    /* La copia, del tamaño exacto de ESTE JPEG: cada foto ocupa lo que
     * ocupa. El buffer del driver vuelve enseguida. */
    foto_t f = {
        .jpeg = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM),
        .largo = fb->len,
        .captura_us = camara_marca_us(fb),
        .ancho = (uint16_t)fb->width,
        .alto = (uint16_t)fb->height,
        .motivo = NOMBRE_MOTIVO[motivo],
        .rssi = rssi,
        .temp_x100 = temp_x100,
    };
    if (f.jpeg != NULL) {
        memcpy(f.jpeg, fb->buf, fb->len);
    }
    esp_camera_fb_return(fb);

    xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
    f.numero = ++s_estado.tomadas;
    s_estado.por_motivo[motivo]++;
    if (__builtin_popcount(pedidos) > 1) {
        s_estado.coincidencias++;
    }
    if (f.jpeg == NULL) {
        s_estado.sin_memoria++;
    }
    xSemaphoreGive(s_mutex_estado);

    if (f.jpeg == NULL) {
        ESP_LOGW(TAG, "[foto %" PRIu32 "] sin PSRAM para guardarla (%u B)",
                 f.numero, (unsigned)f.largo);
        anotar("SIN MEMORIA");
        return;
    }

    /* Se decodifica ANTES de encolarla: una vez en la cola, la foto es de
     * "subida", que la puede liberar en cualquier momento. */
    imagen_t img;
    const esp_err_t err_img = imagen_decodificar(f.jpeg, f.largo, &img);

    const char *destino;
    if (!s_nube_lista) {
        destino = "SIN NUBE";
        free(f.jpeg);
    } else if (xQueueSend(s_cola_fotos, &f, 0) == pdTRUE) {
        destino = "EN COLA";
    } else {
        destino = "COLA LLENA";
        free(f.jpeg);
        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        s_estado.cola_llena++;
        xSemaphoreGive(s_mutex_estado);
    }
    /* A partir de aquí f.jpeg ya no es nuestro: no se toca. */

    ESP_LOGI(TAG, "[foto %" PRIu32 "] %s, RSSI %d dBm, chip %" PRId32 ",%02" PRId32
                  " C, %u B, %ux%u: %s%s", f.numero, f.motivo, f.rssi,
             f.temp_x100 / 100, (f.temp_x100 < 0 ? -f.temp_x100 : f.temp_x100) % 100,
             (unsigned)f.largo, f.ancho, f.alto, destino,
             (__builtin_popcount(pedidos) > 1) ? " (coincidieron varios disparos)" : "");
    if (motivo == MOTIVO_PERIODICA) {
        if (s_periodica_us != 0) {
            ESP_LOGI(TAG, "[foto %" PRIu32 "] periodica: %" PRIu32 " ms desde la "
                          "periodica anterior", f.numero,
                     us_a_ms(f.captura_us - s_periodica_us));
        }
        s_periodica_us = f.captura_us;
    }

    if (err_img == ESP_OK) {
        char texto[40];
        snprintf(texto, sizeof(texto), "FOTO %" PRIu32 " %s", f.numero, destino);
        marco(&img, 4);
        barra_de_estado(&img, texto);
        imagen_mostrar(&img);
    }
}

/* ======================================================================== */
/*  TAREA visor                                                              */
/* ======================================================================== */

static void tarea_visor(void *arg)
{
    uint32_t pedidos = 0;               /* disparadores que esperan su foto */

    while (1) {
        /* Como en el ejemplo 1: «avisos» solo vale si se devolvió pdTRUE. */
        uint32_t avisos = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &avisos, 0) == pdTRUE) {
            pedidos |= avisos;
        }
        if (pedidos != 0) {
            tomar_foto(pedidos);
            /* En pantalla hasta el plazo, o hasta el siguiente disparo. */
            pedidos = (xTaskNotifyWait(0, UINT32_MAX, &avisos,
                                       pdMS_TO_TICKS(FOTO_EN_PANTALLA_MS)) == pdTRUE)
                          ? avisos : 0;
#if MOVIMIENTO_ACTIVO
            movimiento_reiniciar();     /* la escena pudo cambiar entretanto */
#endif
            continue;
        }

        /* El desglose de cada cuadro, en µs: esperar el cuadro de la cámara,
         * copiarlo, decodificarlo para la pantalla, el detector, la pantalla
         * y la pausa. */
        const int64_t t0 = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const int64_t t1 = esp_timer_get_time();
        const size_t largo = fb->len;
        memcpy(s_jpeg, fb->buf, largo);
        esp_camera_fb_return(fb);
        const int64_t t2 = esp_timer_get_time();

        imagen_t img;
        const bool decodificada = (imagen_decodificar(s_jpeg, largo, &img) == ESP_OK);
        const int64_t t3 = esp_timer_get_time();

#if MOVIMIENTO_ACTIVO
        if (s_mov_listo) {
            movimiento_medida_t med;
            const bool cambio = movimiento_analizar(s_jpeg, largo, &med);
            const uint32_t pico_x10 = (med.total > 0)
                ? (uint32_t)med.cambiados * 1000u / med.total : 0;
            bool disparar = false;

            xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
            if (pico_x10 > s_estado.mov_pico_x10) {
                s_estado.mov_pico_x10 = pico_x10;
            }
            if (cambio) {
                s_estado.mov_detectados++;
                if (en_tiempo_muerto(MOVIMIENTO_TIEMPO_MUERTO_S)) {
                    s_estado.mov_omitidos++;
                } else {
                    disparar = true;
                }
            }
            xSemaphoreGive(s_mutex_estado);

            if (cambio) {
                ESP_LOGI(TAG, "[movimiento] cambio: %u de %u px (%" PRIu32 ",%" PRIu32
                              " %%), brillo %d%s", med.cambiados, med.total,
                         pico_x10 / 10, pico_x10 % 10, med.brillo,
                         disparar ? "" : "; en tiempo muerto, no se saca foto");
            }
            if (disparar) {
                s_pulsacion_ms = us_a_ms(esp_timer_get_time());
                pedidos |= AVISO_MOVIMIENTO;
            }
        }
#endif
        const int64_t t4 = esp_timer_get_time();

        if (decodificada) {
            char texto[40];
            texto_de_estado(texto, sizeof(texto));
            barra_de_estado(&img, texto);
            imagen_mostrar(&img);
        }
        const int64_t t5 = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(PAUSA_VISOR_MS));
        const int64_t t6 = esp_timer_get_time();

        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        s_estado.v_cuadros++;
        s_estado.v_us_total += (uint64_t)(t6 - t0);
        s_estado.v_us_espera += (uint64_t)(t1 - t0);
        s_estado.v_us_copia += (uint64_t)(t2 - t1);
        s_estado.v_us_decod += (uint64_t)(t3 - t2);
        s_estado.v_us_mov += (uint64_t)(t4 - t3);
        s_estado.v_us_pantalla += (uint64_t)(t5 - t4);
        xSemaphoreGive(s_mutex_estado);
    }
}

/* ======================================================================== */
/*  TAREA subida:  de la cola a la nube                                      */
/* ======================================================================== */

/* El texto corto de la pantalla para cada resultado. */
static void resumen(nube_resultado_t r, const nube_informe_t *inf, char *dst, size_t n)
{
    if (r == NUBE_OK) {
        const uint32_t ms = inf->ms_envio + inf->ms_respuesta;
        snprintf(dst, n, "OK %" PRIu32 ".%" PRIu32 " S", ms / 1000, (ms % 1000) / 100);
    } else if (r == NUBE_ERROR_RED) {
        strlcpy(dst, "ERROR RED", n);
    } else if (r == NUBE_ERROR_SERVIDOR) {
        strlcpy(dst, "ERROR NUBE", n);
    } else if (strstr(inf->error, "token") != NULL) {
        strlcpy(dst, "TOKEN MAL", n);
    } else if (strstr(inf->error, "404") != NULL) {
        strlcpy(dst, "URL MAL", n);
    } else if (strstr(inf->error, "sesion") != NULL) {
        strlcpy(dst, "ACCESO MAL", n);
    } else {
        strlcpy(dst, "RECHAZADA", n);
    }
}

static void tarea_subida(void *arg)
{
    foto_t f;

    while (xQueueReceive(s_cola_fotos, &f, portMAX_DELAY) == pdTRUE) {
        uint32_t espera_ms = ESPERA_MIN_MS;
        int errores_servidor = 0;
        bool terminada = false;

        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        s_estado.en_mano = true;
        xSemaphoreGive(s_mutex_estado);

        while (!terminada) {
            /* Sin red no se intenta: se espera el bit, sin gastar CPU. */
            xEventGroupWaitBits(s_eventos, EV_WIFI_OK, pdFALSE, pdTRUE, portMAX_DELAY);

            xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
            s_estado.subiendo = true;
            xSemaphoreGive(s_mutex_estado);

            const nube_foto_t nf = {
                .jpeg = f.jpeg,
                .largo = f.largo,
                .ancho = f.ancho,
                .alto = f.alto,
                .numero = f.numero,
                /* La «edad» de la Clase 6: la nube resta este número a su
                 * propia hora para saber cuándo se sacó la foto. */
                .edad_ms = us_a_ms(esp_timer_get_time() - f.captura_us),
                .motivo = f.motivo,
                .rssi = f.rssi,
                .temp_x100 = f.temp_x100,
            };
            nube_informe_t inf;
            const nube_resultado_t r = nube_subir(&nf, &inf);

            char corto[24];
            resumen(r, &inf, corto, sizeof(corto));
            const uint32_t subida_ms = inf.ms_envio + inf.ms_respuesta;
            const uint32_t extremo_ms = us_a_ms(esp_timer_get_time() - f.captura_us);

            xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
            s_estado.subiendo = false;
            strlcpy(s_estado.ultimo, corto, sizeof(s_estado.ultimo));
            if (r == NUBE_OK) {
                s_estado.subidas++;
                s_estado.repetidas += inf.repetida ? 1 : 0;
                s_estado.bytes += inf.bytes;
                s_estado.subida_ms_suma += subida_ms;
                if (subida_ms > s_estado.subida_ms_max) {
                    s_estado.subida_ms_max = subida_ms;
                }
                if (extremo_ms > s_estado.extremo_ms_max) {
                    s_estado.extremo_ms_max = extremo_ms;
                }
            }
            xSemaphoreGive(s_mutex_estado);

            if (r == NUBE_OK) {
                ESP_LOGI(TAG, "[subida] foto %" PRIu32 " en Drive: %s (%" PRIu32
                              " ms + %" PRIu32 " ms, %u B enviados)%s",
                         f.numero, inf.nombre, inf.ms_envio, inf.ms_respuesta,
                         (unsigned)inf.bytes,
                         inf.repetida ? "; ya estaba de un intento anterior" : "");
                /* Con la foto YA guardada, el aviso al panel: se encola y la
                 * subida no espera al broker. */
                const aviso_foto_t av = {
                    .numero = f.numero,
                    .motivo = f.motivo,
                    .rssi = f.rssi,
                    .temp_x100 = f.temp_x100,
                    .archivo = inf.nombre,
                    .drive_id = inf.id,
                    .ms_subida = subida_ms,
                    .ms_captura = extremo_ms,
                    .bytes = inf.bytes,
                    .repetida = inf.repetida,
                };
                if (!aviso_foto(&av)) {
                    ESP_LOGW(TAG, "[subida] foto %" PRIu32 ": el aviso MQTT no cupo",
                             f.numero);
                }
                terminada = true;
            } else if (r == NUBE_RECHAZADA) {
                ESP_LOGE(TAG, "[subida] foto %" PRIu32 " rechazada, se descarta: %s",
                         f.numero, inf.error);
                terminada = true;
            } else if (r == NUBE_ERROR_SERVIDOR && ++errores_servidor >= INTENTOS_SERVIDOR) {
                ESP_LOGE(TAG, "[subida] foto %" PRIu32 ": %d errores del servidor "
                              "seguidos, se descarta: %s", f.numero, errores_servidor,
                         inf.error);
                terminada = true;
            } else {
                /* Reintento con espera creciente y azar, como el WiFi de la
                 * Clase 6: veinte placas no vuelven a llamar a la vez. */
                const uint32_t azar = esp_random() % (espera_ms / 4 + 1);
                ESP_LOGW(TAG, "[subida] foto %" PRIu32 ": %s (%s); otra vez en %"
                              PRIu32 " ms", f.numero, nube_nombre_resultado(r),
                         inf.error, espera_ms + azar);
                xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
                s_estado.reintentos++;
                xSemaphoreGive(s_mutex_estado);
                vTaskDelay(pdMS_TO_TICKS(espera_ms + azar));
                espera_ms = (espera_ms >= ESPERA_MAX_MS / 2) ? ESPERA_MAX_MS
                                                             : espera_ms * 2;
            }

            if (terminada && r != NUBE_OK) {
                xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
                s_estado.rechazadas++;
                xSemaphoreGive(s_mutex_estado);
            }
        }

        free(f.jpeg);                       /* la foto era nuestra */
        xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
        s_estado.en_mano = false;
        xSemaphoreGive(s_mutex_estado);
    }
}

/* ======================================================================== */
/*  TEMPORIZADOR SOFTWARE:  informe                                          */
/* ======================================================================== */

static void informe(TimerHandle_t timer)
{
    xSemaphoreTake(s_mutex_estado, portMAX_DELAY);
    const estado_t e = s_estado;
    /* Lo del visor y el pico del detector valen para el periodo que acaba. */
    s_estado.v_cuadros = 0;
    s_estado.v_us_total = s_estado.v_us_espera = s_estado.v_us_copia = 0;
    s_estado.v_us_decod = s_estado.v_us_mov = s_estado.v_us_pantalla = 0;
    s_estado.mov_pico_x10 = 0;
    xSemaphoreGive(s_mutex_estado);

    red_estado_t w;
    red_wifi_estado(&w);
    const uint32_t avisos_ok = s_avisos_encolados;
    const uint32_t avisos_perdidos = s_avisos_perdidos;
    const EventBits_t bits = xEventGroupGetBits(s_eventos);

    ESP_LOGI(TAG, "--- Informe de %s (cada %d s) -------------------------",
             s_id, INFORME_PERIODO_MS / 1000);
    informe_tareas(TAG);
    informe_memoria(TAG);
    ESP_LOGI(TAG, "  WiFi %s, MQTT %s;  conexiones WiFi %" PRIu32 ", desconexiones %"
                  PRIu32 ";  avisos de foto %" PRIu32 " (perdidos %" PRIu32 ")",
             w.conectada ? "SI" : "no", (bits & EV_MQTT_OK) ? "SI" : "no",
             w.conexiones, w.desconexiones, avisos_ok, avisos_perdidos);
    if (e.v_cuadros > 0 && e.v_us_total > 0) {
        /* Todo en décimas de ms por cuadro; "pausa" es lo que queda. */
        const uint64_t n100 = (uint64_t)e.v_cuadros * 100;
        const uint64_t suma = e.v_us_espera + e.v_us_copia + e.v_us_decod +
                              e.v_us_mov + e.v_us_pantalla;
        const uint32_t fps_x10 = (uint32_t)((uint64_t)e.v_cuadros * 10000000ULL /
                                            e.v_us_total);
        ESP_LOGI(TAG, "  visor: %" PRIu32 ",%" PRIu32 " cuadros/s (%" PRIu32
                      " cuadros) con detector %s;  por cuadro, en ms: espera de la "
                      "camara %" PRIu32 ",%" PRIu32 ", copia %" PRIu32 ",%" PRIu32
                      ", decodificar %" PRIu32 ",%" PRIu32 ", detector %" PRIu32
                      ",%" PRIu32 ", pantalla %" PRIu32 ",%" PRIu32 ", pausa %"
                      PRIu32 ",%" PRIu32,
                 fps_x10 / 10, fps_x10 % 10, e.v_cuadros,
                 MOVIMIENTO_ACTIVO ? "SI" : "no",
                 (uint32_t)(e.v_us_espera / n100) / 10, (uint32_t)(e.v_us_espera / n100) % 10,
                 (uint32_t)(e.v_us_copia / n100) / 10, (uint32_t)(e.v_us_copia / n100) % 10,
                 (uint32_t)(e.v_us_decod / n100) / 10, (uint32_t)(e.v_us_decod / n100) % 10,
                 (uint32_t)(e.v_us_mov / n100) / 10, (uint32_t)(e.v_us_mov / n100) % 10,
                 (uint32_t)(e.v_us_pantalla / n100) / 10,
                 (uint32_t)(e.v_us_pantalla / n100) % 10,
                 (uint32_t)((e.v_us_total - suma) / n100) / 10,
                 (uint32_t)((e.v_us_total - suma) / n100) % 10);
    }
    ESP_LOGI(TAG, "  disparos: boton %" PRIu32 ", movimiento %" PRIu32 ", periodica %"
                  PRIu32 ";  coincidencias %" PRIu32 ", periodicas omitidas %" PRIu32
                  ";  detector: %" PRIu32 " cambios (%" PRIu32 " en tiempo muerto), "
                  "pico %" PRIu32 ",%" PRIu32 " %% del cuadro",
             e.por_motivo[MOTIVO_BOTON], e.por_motivo[MOTIVO_MOVIMIENTO],
             e.por_motivo[MOTIVO_PERIODICA], e.coincidencias, e.periodicas_omitidas,
             e.mov_detectados, e.mov_omitidos, e.mov_pico_x10 / 10,
             e.mov_pico_x10 % 10);
    ESP_LOGI(TAG, "  fotos: tomadas %" PRIu32 ", subidas %" PRIu32 " (repetidas %"
                  PRIu32 "), en cola %u/%d, cola llena %" PRIu32 ", sin memoria %"
                  PRIu32 ", rechazadas %" PRIu32 ", reintentos %" PRIu32,
             e.tomadas, e.subidas, e.repetidas,
             (unsigned)uxQueueMessagesWaiting(s_cola_fotos), COLA_FOTOS_LARGO,
             e.cola_llena, e.sin_memoria, e.rechazadas, e.reintentos);
    if (e.subidas > 0) {
        ESP_LOGI(TAG, "  subida: media %" PRIu32 " ms, maxima %" PRIu32 " ms;  "
                      "de la captura a la nube, maximo %" PRIu32 " ms;  %" PRIu32
                      " KB enviados",
                 (uint32_t)(e.subida_ms_suma / e.subidas), e.subida_ms_max,
                 e.extremo_ms_max, (uint32_t)(e.bytes / 1024));
    }
}

/* ======================================================================== */
/*  Arranque                                                                 */
/* ======================================================================== */

static void identidad(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_id, sizeof(s_id), "kaluga-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_arranque, sizeof(s_arranque), "%08" PRIx32, esp_random());
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== camara_nube: fotos de la Kaluga-1 a Google Drive ===");

    if (!pantalla_iniciar(CONTROLADOR)) {
        ESP_LOGW(TAG, "sin pantalla: el ejemplo sigue, pero solo por consola");
    }
    pantalla_limpiar(NEGRO);
    pantalla_texto(16, 16, "CAMARA NUBE", 3, BLANCO, NEGRO);
    pantalla_texto(16, 56, "INICIANDO...", 2, BLANCO, NEGRO);

    const camara_config_t cam = {
        .kit = KIT_CAMARA,
        .tamano = FOTO_TAMANO,
        .calidad = FOTO_CALIDAD,
        .voltear = CAMARA_VOLTEAR,
        .espejo = CAMARA_ESPEJO,
        .patron = false,
    };
    if (camara_iniciar(&cam) != ESP_OK) {
        pantalla_texto(16, 96, "SIN CAMARA", 2, pantalla_color(255, 80, 80), NEGRO);
        pantalla_texto(16, 124, "VER LA CONSOLA", 2, BLANCO, NEGRO);
        return;
    }
    s_jpeg = heap_caps_malloc(camara_tamano_buffer(), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(s_jpeg != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(imagen_iniciar());
    ESP_ERROR_CHECK(medidas_iniciar());
#if MOVIMIENTO_ACTIVO
    s_mov_listo = (movimiento_iniciar(camara_ancho(), camara_alto()) == ESP_OK);
    if (!s_mov_listo) {
        ESP_LOGW(TAG, "sin detector de movimiento: el ejemplo sigue sin el");
    }
#endif

    /* --- Objetos del kernel, ANTES de las tareas que los usan ------------ */
    s_eventos      = xEventGroupCreate();
    s_cola_fotos   = xQueueCreate(COLA_FOTOS_LARGO, sizeof(foto_t));
    s_mutex_estado = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK((s_eventos && s_cola_fotos && s_mutex_estado) ? ESP_OK
                                                                  : ESP_ERR_NO_MEM);

    /* --- Red, identidad y nube --------------------------------------------- */
    const red_config_t red = {
        .ssid = WIFI_SSID,
        .clave = WIFI_CLAVE,
        .seguridad = WIFI_SEGURIDAD,
    };
    ESP_ERROR_CHECK(red_wifi_iniciar(&red, s_eventos));
    identidad();
    ESP_LOGI(TAG, "ID de esta placa: %s (arranque %s)", s_id, s_arranque);

    if (aviso_iniciar() != ESP_OK) {
        ESP_LOGW(TAG, "sin avisos MQTT: las fotos se guardan igual");
    }

    const nube_config_t nube = {
        .url = NUBE_URL,
        .token = NUBE_TOKEN,
        .dispositivo = s_id,
        .arranque = s_arranque,
    };
    if (strstr(NUBE_URL, "PEGA-AQUI") != NULL || strstr(NUBE_TOKEN, "pega-aqui") != NULL) {
        /* Con los valores de ejemplo la cámara funciona igual; las fotos se
         * ven en pantalla pero no se suben. */
        ESP_LOGW(TAG, "falta configurar NUBE_URL y NUBE_TOKEN: instala el Apps "
                      "Script (ver ../apps_script/README.md); las fotos no se suben");
        s_nube_lista = false;
    } else {
        s_nube_lista = (nube_iniciar(&nube) == ESP_OK);
    }
    if (!s_nube_lista) {
        anotar("SIN NUBE");                 /* el log dice qué falta */
    }

    /* --- Las tareas -------------------------------------------------------- */
    xTaskCreate(tarea_visor, "visor", PILA_VISOR, NULL, PRIO_VISOR, &s_tarea_visor);
    ESP_ERROR_CHECK(s_tarea_visor != NULL ? ESP_OK : ESP_ERR_NO_MEM);
        if (s_nube_lista) {
        if (xTaskCreateWithCaps(tarea_subida, "subida", PILA_SUBIDA, NULL,
                                PRIO_SUBIDA, NULL,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "no se pudo crear la tarea de subida (pila de %d B en "
                          "PSRAM): las fotos no se suben", PILA_SUBIDA);
            s_nube_lista = false;
            anotar("SIN NUBE");
        }
    }
    configurar_boton();

    s_timer_informe = xTimerCreate("informe", pdMS_TO_TICKS(INFORME_PERIODO_MS),
                                   pdTRUE, NULL, informe);
    ESP_ERROR_CHECK(s_timer_informe != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTimerStart(s_timer_informe, pdMS_TO_TICKS(100)) == pdPASS
                        ? ESP_OK : ESP_FAIL);
#if FOTO_CADA_S > 0
    s_timer_foto = xTimerCreate("foto", pdMS_TO_TICKS(FOTO_CADA_S * 1000),
                                pdTRUE, NULL, foto_periodica);
    ESP_ERROR_CHECK(s_timer_foto != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTimerStart(s_timer_foto, pdMS_TO_TICKS(100)) == pdPASS
                        ? ESP_OK : ESP_FAIL);
#else
    (void)s_timer_foto;
    (void)foto_periodica;
#endif

    /* --- A conectar --------------------------------------------------------
     * La subida espera el bit EV_WIFI_OK: mientras no haya red, las fotos
     * esperan en la cola. */
    ESP_ERROR_CHECK(red_wifi_conectar());
    ESP_LOGI(TAG, "Listo: BOOT, el temporizador (%d s) y el movimiento sacan fotos. "
                  "app_main termina.", FOTO_CADA_S);
}
