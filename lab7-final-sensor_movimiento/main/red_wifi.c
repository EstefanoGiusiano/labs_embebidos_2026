/*
 * red_wifi.c — Clase 6 (Sistemas Embebidos, UCU 2026)
 *
 * La Kaluga-1 como ESTACIÓN WiFi, con reconexión automática. El mismo
 * archivo, sin cambios, en wifi_estacion y en nodo_mqtt.
 *
 * Quién hace qué:
 *
 *   [sys_evt p20]  manejador de eventos del sistema (al_evento)
 *        │  anota el motivo, pone o borra EV_WIFI_OK y AVISA. Nunca espera:
 *        │  esa tarea reparte TODOS los eventos del sistema.
 *        │  notificación
 *        ▼
 *   [conexion p3]  la tarea de este módulo
 *        llama a esp_wifi_connect(), se bloquea hasta el siguiente aviso,
 *        deja pasar una espera que se dobla en cada fallo (1 s, 2 s, 4 s...
 *        hasta 60 s) y vuelve a intentarlo.
 *
 * El driver WiFi no reconecta solo: si nadie vuelve a llamar a
 * esp_wifi_connect(), una placa que pierde la red se queda sin ella.
 */

#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "red_wifi.h"

/* Prioridad 3: la tarea solo decide CUÁNDO reintentar. El trabajo de verdad
 * lo hacen las tareas de ESP-IDF (wifi p23, tcpip p18, sys_evt p20). */
#define PRIO_CONEXION       3
#define PILA_CONEXION       3072

#define ESPERA_MIN_MS       1000
#define ESPERA_MAX_MS       60000

static const char *TAG = "red_wifi";

static EventGroupHandle_t s_eventos;
static SemaphoreHandle_t  s_mutex;     /* protege s_estado y s_hubo_ip      */
static TaskHandle_t       s_tarea;     /* destino de los avisos de caída    */
static red_estado_t  s_estado;
static bool               s_hubo_ip;   /* hubo IP desde el último intento   */
static int64_t            s_inicio_us; /* cuándo se llamó a red_wifi_conectar */

/* ======================================================================== */
/*  Textos para el registro                                                  */
/* ======================================================================== */

const char *red_wifi_motivo(uint8_t motivo)
{
    switch (motivo) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRE: la autenticacion caduco";
    case WIFI_REASON_AUTH_LEAVE:
        return "AUTH_LEAVE: el punto de acceso cerro la sesion";
    case WIFI_REASON_ASSOC_LEAVE:
        return "ASSOC_LEAVE: desconexion pedida";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT: casi siempre, la clave esta mal";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT: se dejo de oir al punto de acceso";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND: no se ve la red (nombre mal escrito? 5 GHz?)";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL: autenticacion rechazada (con WPA3, la clave)";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL: el punto de acceso rechazo la asociacion";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL: fallo la conexion";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_AP_FOUND_W_COMPATIBLE_SECURITY: la red usa otra seguridad";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: la red es menos segura que lo pedido";
    default:
        return "otro: ver wifi_err_reason_t en esp_wifi_types_generic.h";
    }
}

const char *red_wifi_nombre_seguridad(wifi_auth_mode_t modo)
{
    switch (modo) {
    case WIFI_AUTH_OPEN:                 return "abierta";
    case WIFI_AUTH_WEP:                  return "WEP";
    case WIFI_AUTH_WPA_PSK:              return "WPA";
    case WIFI_AUTH_WPA2_PSK:             return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:         return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:             return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:        return "WPA2/WPA3";
    case WIFI_AUTH_WPA_ENTERPRISE:       return "WPA-Enterprise";
    case WIFI_AUTH_ENTERPRISE:           return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_ENTERPRISE:      return "WPA3-Enterprise";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "WPA2/WPA3-Ent";
    case WIFI_AUTH_WPA3_ENT_192:         return "WPA3-Ent-192";
    case WIFI_AUTH_OWE:                  return "OWE";
    case WIFI_AUTH_WAPI_PSK:             return "WAPI";
    default:                             return "?";
    }
}

/* ======================================================================== */
/*  Manejador de eventos:  corre en la tarea sys_evt (prioridad 20)          */
/* ======================================================================== */

/* Regla de oro: aquí no se espera. Ni vTaskDelay(), ni un reintento con
 * pausa, ni un printf largo de más. Mientras este manejador corre, ningún
 * otro evento del sistema avanza: tampoco el de "ya tienes IP". Se anota, se
 * avisa a quien corresponda y se vuelve. */
static void al_evento(void *arg, esp_event_base_t base, int32_t id, void *datos)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *c = datos;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_estado.canal = c->channel;
        s_estado.seguridad = c->authmode;
        xSemaphoreGive(s_mutex);

        ESP_LOGI(TAG, "asociada al punto de acceso: canal %u, seguridad %s; "
                      "pidiendo IP por DHCP", c->channel,
                 red_wifi_nombre_seguridad(c->authmode));

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = datos;

        xEventGroupClearBits(s_eventos, EV_WIFI_OK);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_estado.conectada = false;
        s_estado.desconexiones++;
        s_estado.ultimo_motivo = d->reason;
        xSemaphoreGive(s_mutex);

        ESP_LOGW(TAG, "sin conexion: motivo %u, %s", d->reason,
                 red_wifi_motivo(d->reason));

        /* La espera y el reintento son cosa de la tarea "conexion". El
         * puntero se comprueba: avisar a una tarea que todavía no existe es
         * un assert y un reinicio (lo vimos en la Clase 5). */
        if (s_tarea != NULL) {
            xTaskNotifyGive(s_tarea);
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = datos;
        const int64_t ahora = esp_timer_get_time();

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_estado.conectada = true;
        s_estado.conexiones++;
        s_estado.ip = e->ip_info.ip;
        s_estado.marca_ip_us = ahora;
        s_estado.espera_ms = ESPERA_MIN_MS;
        if (s_estado.conexiones == 1) {
            s_estado.ms_hasta_ip = (uint32_t)((ahora - s_inicio_us) / 1000);
        }
        s_hubo_ip = true;
        xSemaphoreGive(s_mutex);

        ESP_LOGI(TAG, "IP " IPSTR "  mascara " IPSTR "  puerta de enlace " IPSTR,
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.netmask),
                 IP2STR(&e->ip_info.gw));

        /* Y ahora sí: todas las tareas bloqueadas en este bit despiertan. */
        xEventGroupSetBits(s_eventos, EV_WIFI_OK);

    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        /* Sigue asociada, pero el DHCP no renovó la dirección. No hay que
         * reconectar: si la IP vuelve, llegará otro IP_EVENT_STA_GOT_IP. */
        xEventGroupClearBits(s_eventos, EV_WIFI_OK);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_estado.conectada = false;
        xSemaphoreGive(s_mutex);

        ESP_LOGW(TAG, "se perdio la IP; se espera a que el DHCP la devuelva");
    }
}

/* ======================================================================== */
/*  Tarea "conexion":  conecta, espera el aviso de caída y reintenta          */
/* ======================================================================== */

static void tarea_conexion(void *arg)
{
    uint32_t espera_ms = ESPERA_MIN_MS;

    while (1) {
        /* Un aviso que haya quedado de antes es viejo: no hay ningún intento
         * en marcha que lo haya podido producir. */
        (void)ulTaskNotifyTake(pdTRUE, 0);

        const esp_err_t err = esp_wifi_connect();
        if (err == ESP_OK) {
            /* BLOQUEADA hasta que el manejador avise de una desconexión. Con
             * la red funcionando, la tarea se queda aquí horas sin gastar
             * CPU. Un intento fallido también termina aquí: el driver avisa
             * con WIFI_EVENT_STA_DISCONNECTED y el motivo. */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            /* Si la llamada falla de entrada, el driver no generará ningún
             * evento: esperar el aviso sería bloquearse para siempre. */
            ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        }

        /* Si hubo IP desde el intento anterior, esto es una caída nueva y la
         * espera vuelve al mínimo. Si no, es otro intento fallido. */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_hubo_ip) {
            espera_ms = ESPERA_MIN_MS;
            s_hubo_ip = false;
        }
        s_estado.espera_ms = espera_ms;
        xSemaphoreGive(s_mutex);

        /* Un poco de azar, hasta un 25 % más: si se cae el router del aula,
         * las veinte placas no vuelven a llamar en el mismo milisegundo. */
        const uint32_t azar = esp_random() % (espera_ms / 4 + 1);
        ESP_LOGI(TAG, "reintento en %" PRIu32 " ms", espera_ms + azar);
        vTaskDelay(pdMS_TO_TICKS(espera_ms + azar));

        /* Espera exponencial: cada fallo seguido, el doble, hasta un tope. */
        espera_ms = (espera_ms >= ESPERA_MAX_MS / 2) ? ESPERA_MAX_MS
                                                     : espera_ms * 2;
    }
}

/* ======================================================================== */
/*  API                                                                      */
/* ======================================================================== */

esp_err_t red_wifi_iniciar(const red_config_t *cfg, EventGroupHandle_t eventos)
{
    if (cfg == NULL || eventos == NULL || cfg->ssid == NULL ||
        cfg->clave == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t n_ssid = strlen(cfg->ssid);
    const size_t n_clave = strlen(cfg->clave);

    /* Errores de configuración: mejor un mensaje claro ahora que un
     * "HANDSHAKE_TIMEOUT" incomprensible dentro de diez segundos. */
    if (n_ssid == 0 || n_ssid > 32) {
        ESP_LOGE(TAG, "WIFI_SSID tiene que tener entre 1 y 32 caracteres");
        return ESP_ERR_INVALID_ARG;
    }
    if (n_clave < 8 || n_clave > 64) {
        ESP_LOGE(TAG, "una clave WPA tiene entre 8 y 63 caracteres (o 64 "
                      "hexadecimales); la escrita tiene %u", (unsigned)n_clave);
        return ESP_ERR_INVALID_ARG;
    }

    s_eventos = eventos;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 1. NVS. El driver guarda ahí la calibración de la radio; sin ella,
     *    esp_wifi_init() falla. Si la partición viene de otro programa con
     *    otro formato, se borra y se empieza de cero. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 2. La pila TCP/IP (lwIP). Crea la tarea tcpip, prioridad 18. */
    ESP_ERROR_CHECK(esp_netif_init());

    /* 3. El bucle de eventos por defecto. Crea la tarea sys_evt, prio 20. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 4. La interfaz de red de la estación, con su cliente DHCP. */
    esp_netif_create_default_wifi_sta();

    /* 5. El driver WiFi. Crea la tarea wifi, prioridad 23. */
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    /* La configuración vive en el .c del programa: que el driver no la
     * copie además a la flash, donde sobreviviría a un cambio de programa. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, al_evento, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, al_evento, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP, al_evento, NULL, NULL));

    /* 6. La configuración de la estación. */
    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, cfg->ssid, n_ssid);

    /* Escaneo de todos los canales y, si hay varios puntos de acceso con el
     * mismo nombre (una universidad, una oficina), el de mejor señal. */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    /* Red de clave compartida: la clave va en la configuración, y el umbral
     * es el modo MÍNIMO que se acepta. Una red menos segura que el umbral se
     * descarta con el motivo NO_AP_FOUND_IN_AUTHMODE_THRESHOLD. */
    static const wifi_auth_mode_t UMBRAL[] = {
        [RED_WPA]  = WIFI_AUTH_WPA_PSK,
        [RED_WPA2] = WIFI_AUTH_WPA2_PSK,
        [RED_WPA3] = WIFI_AUTH_WPA3_PSK,
    };
    memcpy(wc.sta.password, cfg->clave, n_clave);
    wc.sta.threshold.authmode = UMBRAL[cfg->seguridad];

    if (cfg->seguridad == RED_WPA3) {
        /* WPA3 exige tramas de gestión protegidas (PMF): con "required", la
         * placa ni siquiera intenta asociarse a una red que no las ofrece. */
        wc.sta.pmf_cfg.required = true;
        /* Los dos métodos con los que SAE deriva la clave. */
        wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    /* 7. Arranca la radio. No conecta: eso empieza en red_wifi_conectar(). */
    ESP_ERROR_CHECK(esp_wifi_start());

    static const char *NOMBRE_SEG[] = { "WPA", "WPA2", "WPA3" };
    ESP_LOGI(TAG, "WiFi listo: red \"%s\", minimo %s", cfg->ssid,
             NOMBRE_SEG[cfg->seguridad]);
    return ESP_OK;
}

esp_err_t red_wifi_conectar(void)
{
    if (s_mutex == NULL || s_tarea != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_inicio_us = esp_timer_get_time();

    /* xTaskCreate() escribe s_tarea ANTES de que la tarea nueva pueda correr,
     * así que el manejador ya la encuentra cuando llega el primer aviso. */
    if (xTaskCreate(tarea_conexion, "conexion", PILA_CONEXION, NULL,
                    PRIO_CONEXION, &s_tarea) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void red_wifi_estado(red_estado_t *e)
{
    if (s_mutex == NULL) {                  /* antes de red_wifi_iniciar() */
        memset(e, 0, sizeof(*e));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *e = s_estado;
    xSemaphoreGive(s_mutex);
}
