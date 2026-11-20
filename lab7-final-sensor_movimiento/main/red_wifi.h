/*
 * red_wifi.h — Clase 6 (Sistemas Embebidos, UCU 2026)
 *
 * La Kaluga-1 como ESTACIÓN WiFi, con reconexión automática. Lo usan los dos
 * ejemplos de la clase (wifi_estacion y nodo_mqtt): es el mismo archivo en
 * los dos, sin cambios.
 *
 * Uso, en este orden:
 *
 *     red_wifi_iniciar(&cfg, eventos);   pila de red y driver, SIN conectar
 *     ...                                aquí se puede escanear
 *     red_wifi_conectar();               arranca la tarea "conexion"
 *
 * El resto del programa no pregunta "¿hay red?": espera el bit EV_WIFI_OK
 * en el grupo de eventos con xEventGroupWaitBits(). Este módulo es el ÚNICO
 * que toca ese bit; los demás bits del grupo son del programa.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi_types.h"

/* El bit del grupo de eventos que este módulo pone (con IP) y borra (sin red). */
#define EV_WIFI_OK      BIT0

/* El nivel MÍNIMO de seguridad que se acepta. Si la red ofrece algo mejor, el
 * driver lo negocia solo: con WPA2 pedido y una red WPA2/WPA3, conecta con
 * WPA3. */
typedef enum {
    RED_WPA = 0,
    RED_WPA2,
    RED_WPA3,
} red_seguridad_t;

typedef struct {
    const char       *ssid;       /* nombre de la red, como lo ve el teléfono */
    const char       *clave;      /* la clave de la red, la misma para todos  */
    red_seguridad_t   seguridad;
} red_config_t;

/* Una foto del estado, para el informe. Se copia entera bajo un mutex. */
typedef struct {
    bool             conectada;      /* asociada Y con IP                      */
    uint32_t         conexiones;     /* veces que obtuvo IP                    */
    uint32_t         desconexiones;  /* eventos de desconexión, intentos       */
                                     /* fallidos incluidos                     */
    uint8_t          ultimo_motivo;  /* wifi_err_reason_t de la última         */
    uint8_t          canal;
    wifi_auth_mode_t seguridad;      /* la negociada en la última conexión     */
    uint32_t         ms_hasta_ip;    /* de red_wifi_conectar() a la 1.ª IP     */
    uint32_t         espera_ms;      /* espera antes del próximo reintento     */
    int64_t          marca_ip_us;    /* esp_timer_get_time() de la última IP   */
    esp_ip4_addr_t   ip;
} red_estado_t;

/* Inicializa NVS, la pila TCP/IP, el bucle de eventos y el driver WiFi, y
 * carga la configuración. NO conecta. */
esp_err_t red_wifi_iniciar(const red_config_t *cfg, EventGroupHandle_t eventos);

/* Crea la tarea que conecta y reconecta. Llamar una sola vez. */
esp_err_t red_wifi_conectar(void);

/* Copia el estado actual. Se puede llamar desde cualquier tarea. */
void red_wifi_estado(red_estado_t *e);

/* Textos para el registro: un código de desconexión y un modo de seguridad. */
const char *red_wifi_motivo(uint8_t motivo);
const char *red_wifi_nombre_seguridad(wifi_auth_mode_t modo);
