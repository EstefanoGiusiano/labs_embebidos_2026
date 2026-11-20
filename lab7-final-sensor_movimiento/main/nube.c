




/*
 * nube.c — Clase 7 (Sistemas Embebidos, UCU 2026)
 *
 * La subida de una foto en dos pedidos HTTPS:
 *
 *   1. POST a script.google.com con el JSON y la foto en base64. Se escribe
 *      por trozos: la foto en base64 nunca está entera en memoria.
 *   2. GET a la dirección del 302, en script.googleusercontent.com, que
 *      devuelve lo que contestó el script: {"ok":true,...} o {"ok":false,...}.
 *
 * Los dos pedidos van cifrados con TLS, y en los dos se comprueba que el
 * servidor es quien dice ser: su certificado tiene que encadenar con alguna
 * de las autoridades del paquete que trae ESP-IDF (esp_crt_bundle). Para
 * los servidores de Google hace falta además, en sdkconfig,
 * CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y: Google manda su
 * raíz firmada también por una autoridad antigua que ya no está en el
 * paquete, y sin esa opción la verificación falla ("No matching trusted
 * root certificate found").
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "nube.h"

static const char *TAG = "nube";

/* Plazo de cada operación de red: conectar, escribir un trozo, esperar la
 * respuesta. El script tarda en contestar lo que tarde Drive en guardar la
 * foto: normalmente uno o dos segundos, a veces bastante más. */
#define PLAZO_MS            20000

/* La foto sale en trozos de 3 KB, que en base64 son 4 KB: múltiplo de 3 para
 * que cada trozo se codifique solo, sin el '=' de relleno salvo en el último. */
#define TROZO_BYTES         3072
#define TROZO_BASE64        (TROZO_BYTES / 3 * 4)

#define RESPUESTA_MAX       512         /* el JSON del script, de sobra */
#define URL_MAX             1024        /* la del 302 ronda los 300 caracteres */

#define HOST_RESPUESTA      "https://script.googleusercontent.com/"

static nube_config_t s_cfg;

/* Un solo buffer para el base64 y otro para la URL de la respuesta: nube_subir
 * no es reentrante. 5 KB en SRAM interna, y no en la pila de la tarea. */
static unsigned char s_base64[TROZO_BASE64 + 1];
static char s_url_respuesta[URL_MAX];

/* ======================================================================== */
/*  Utilidades                                                               */
/* ======================================================================== */

static uint32_t ms_desde(int64_t t0_us)
{
    return (uint32_t)((esp_timer_get_time() - t0_us) / 1000);
}

/* Lo que se escribe antes y después de la foto en el JSON. */
static int prefijo_json(char *dst, size_t n, const nube_foto_t *f)
{
    return snprintf(dst, n,
                    "{\"token\":\"%s\",\"dispositivo\":\"%s\",\"arranque\":\"%s\","
                    "\"numero\":%" PRIu32 ",\"edad_ms\":%" PRIu32 ","
                    "\"motivo\":\"%s\",\"rssi\":%d,\"temp_x100\":%" PRId32 ","
                    "\"ancho\":%u,\"alto\":%u,\"jpeg\":\"",
                    s_cfg.token, s_cfg.dispositivo, s_cfg.arranque, f->numero,
                    f->edad_ms, f->motivo, f->rssi, f->temp_x100, f->ancho,
                    f->alto);
}

static const char SUFIJO_JSON[] = "\"}";

size_t nube_bytes_a_enviar(const nube_foto_t *foto)
{
    char prefijo[320];
    const int n = prefijo_json(prefijo, sizeof(prefijo), foto);
    /* 4 caracteres por cada 3 bytes, redondeando hacia arriba: el último
     * grupo se completa con '='. Un JPEG de 40 000 B son 53 336 caracteres. */
    const size_t base64 = 4 * ((foto->largo + 2) / 3);
    return (size_t)n + base64 + strlen(SUFIJO_JSON);
}

/* Copia en dst el valor de "clave":"valor" de un JSON plano como el que
 * devuelve el script. Solo caracteres imprimibles: lo que llega de la red se
 * trata como no confiable, también si es de nuestro propio script. */
static bool valor_json(const char *json, const char *clave, char *dst, size_t n)
{
    char patron[32];
    snprintf(patron, sizeof(patron), "\"%s\":\"", clave);
    const char *p = strstr(json, patron);
    if (p == NULL || n == 0) {
        return false;
    }
    p += strlen(patron);
    size_t i = 0;
    while (*p != '\0' && *p != '"' && i + 1 < n) {
        dst[i++] = (*p >= 0x20 && *p < 0x7F) ? *p : '?';
        p++;
    }
    dst[i] = '\0';
    return *p == '"';
}

/* Lo que contestó el script. Los errores que dependen de la foto o de la
 * configuración no se arreglan reintentando; los del propio Google, sí. */
static nube_resultado_t interpretar_json(const char *json, nube_informe_t *inf)
{
    if (strstr(json, "\"ok\":true") != NULL) {
        if (!valor_json(json, "nombre", inf->nombre, sizeof(inf->nombre))) {
            strlcpy(inf->nombre, "?", sizeof(inf->nombre));
        }
        if (!valor_json(json, "id", inf->id, sizeof(inf->id))) {
            strlcpy(inf->id, "?", sizeof(inf->id));
        }
        inf->repetida = (strstr(json, "\"repetida\":true") != NULL);
        return NUBE_OK;
    }
    if (strstr(json, "\"ok\":false") == NULL) {
        snprintf(inf->error, sizeof(inf->error), "respuesta que no es del script: %.40s", json);
        return NUBE_ERROR_SERVIDOR;
    }

    char motivo[48] = "?";
    valor_json(json, "error", motivo, sizeof(motivo));
    snprintf(inf->error, sizeof(inf->error), "el script dice: %s", motivo);

    /* "interno": una excepción dentro del script (Drive o la hoja fallaron);
     * "limite": demasiadas fotos en este minuto; "ocupado": otra subida tenía
     * el cerrojo del script. Los tres pasan solos. */
    if (strcmp(motivo, "interno") == 0 || strcmp(motivo, "limite") == 0 ||
        strcmp(motivo, "ocupado") == 0) {
        return NUBE_ERROR_SERVIDOR;
    }
    return NUBE_RECHAZADA;          /* token, formato, tamano, no_es_jpeg... */
}

/* ======================================================================== */
/*  Paso 1: el POST con la foto                                              */
/* ======================================================================== */

/* Las cabeceras de la RESPUESTA se leen así, con un manejador de eventos:
 * esp_http_client llama a esta función con cada cabecera completa.
 *
 * Las dos funciones que parecen servir no sirven. esp_http_client_get_header()
 * lee las cabeceras del PEDIDO, no las de la respuesta. Y
 * esp_http_client_get_url(), después de esp_http_client_set_redirection(),
 * reconstruye la URL como esquema://host:puerto/ruta: sin la consulta (lo que
 * va detrás del '?') y con el puerto escrito. La dirección de la respuesta de
 * Google es justamente su consulta: ?user_content_key=... */
static esp_err_t al_evento_http(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_HEADER && ev->header_key != NULL &&
        ev->header_value != NULL && strcasecmp(ev->header_key, "Location") == 0) {
        strlcpy(s_url_respuesta, ev->header_value, sizeof(s_url_respuesta));
    }
    return ESP_OK;
}

/* Escribe todo el buffer; esp_http_client_write devuelve lo escrito o < 0. */
static bool escribir(esp_http_client_handle_t c, const void *datos, size_t n)
{
    return esp_http_client_write(c, (const char *)datos, (int)n) == (int)n;
}

static nube_resultado_t enviar_foto(const nube_foto_t *f, nube_informe_t *inf)
{
    char prefijo[320];
    const int n_prefijo = prefijo_json(prefijo, sizeof(prefijo), f);
    if (n_prefijo <= 0 || n_prefijo >= (int)sizeof(prefijo)) {
        strlcpy(inf->error, "identificador o token demasiado largos", sizeof(inf->error));
        return NUBE_RECHAZADA;
    }
    inf->bytes = nube_bytes_a_enviar(f);

    const esp_http_client_config_t cfg = {
        .url = s_cfg.url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = PLAZO_MS,
        /* El certificado del servidor se comprueba contra las autoridades
         * públicas que trae ESP-IDF. Sin esto, TLS cifraría igual, pero con
         * cualquiera que se hiciera pasar por Google. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* El 302 lo tratamos nosotros. Si el cliente lo siguiera solo,
         * repetiría el POST, foto incluida, contra la dirección de la
         * respuesta: esp_http_client conserva el método al redirigir. */
        .disable_auto_redirect = true,
        .buffer_size = 1024,        /* cabeceras de la respuesta de Google */
        .keep_alive_enable = false,
        .event_handler = al_evento_http,    /* para leer la cabecera Location */
    };
    s_url_respuesta[0] = '\0';
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        strlcpy(inf->error, "sin memoria para el cliente HTTP", sizeof(inf->error));
        return NUBE_ERROR_SERVIDOR;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");

    nube_resultado_t r = NUBE_ERROR_RED;
    const int64_t t0 = esp_timer_get_time();

    /* open() conecta (DNS, TCP y el saludo TLS) y manda las cabeceras, con
     * Content-Length: la longitud se sabe antes de codificar la foto. */
    esp_err_t err = esp_http_client_open(c, (int)inf->bytes);
    if (err != ESP_OK) {
        snprintf(inf->error, sizeof(inf->error), "no conecta con el script: %s",
                 esp_err_to_name(err));
        goto fin;
    }

    bool ok = escribir(c, prefijo, (size_t)n_prefijo);
    for (size_t i = 0; ok && i < f->largo; i += TROZO_BYTES) {
        const size_t n = (f->largo - i < TROZO_BYTES) ? f->largo - i : TROZO_BYTES;
        size_t n_base64 = 0;
        if (mbedtls_base64_encode(s_base64, sizeof(s_base64), &n_base64,
                                  f->jpeg + i, n) != 0) {
            ok = false;
            break;
        }
        ok = escribir(c, s_base64, n_base64);
    }
    ok = ok && escribir(c, SUFIJO_JSON, strlen(SUFIJO_JSON));
    if (!ok) {
        strlcpy(inf->error, "se corto la conexion mandando la foto", sizeof(inf->error));
        goto fin;
    }

    /* Ahora sí, la respuesta: el script corre mientras esperamos aquí. */
    if (esp_http_client_fetch_headers(c) < 0) {
        strlcpy(inf->error, "sin respuesta del script (plazo agotado?)", sizeof(inf->error));
        goto fin;
    }
    inf->http = esp_http_client_get_status_code(c);
    inf->ms_envio = ms_desde(t0);

    switch (inf->http) {
    case 302:
    case 303:
        /* La dirección de la respuesta llegó en la cabecera Location, y
         * al_evento_http() la copió en s_url_respuesta. */
        if (s_url_respuesta[0] == '\0') {
            strlcpy(inf->error, "302 sin direccion de respuesta", sizeof(inf->error));
            r = NUBE_ERROR_SERVIDOR;
        } else if (strlen(s_url_respuesta) >= sizeof(s_url_respuesta) - 1) {
            strlcpy(inf->error, "302 con una direccion demasiado larga", sizeof(inf->error));
            r = NUBE_ERROR_SERVIDOR;
        } else if (strncmp(s_url_respuesta, HOST_RESPUESTA, strlen(HOST_RESPUESTA)) == 0) {
            r = NUBE_OK;            /* el script corrió: falta leer qué dijo */
        } else if (strstr(s_url_respuesta, "accounts.google.com") != NULL) {
            strlcpy(inf->error, "el script pide iniciar sesion: implementalo "
                                "con acceso 'Cualquier usuario'", sizeof(inf->error));
            r = NUBE_RECHAZADA;
        } else {
            snprintf(inf->error, sizeof(inf->error), "redireccion inesperada: %.50s",
                     s_url_respuesta);
            r = NUBE_ERROR_SERVIDOR;
        }
        break;
    case 200: {
        /* Google no contesta así a un script que termina bien. Si llega un
         * 200, casi siempre es una página de error en HTML. */
        char cuerpo[RESPUESTA_MAX];
        const int n = esp_http_client_read_response(c, cuerpo, sizeof(cuerpo) - 1);
        cuerpo[(n > 0) ? n : 0] = '\0';
        r = interpretar_json(cuerpo, inf);
        break;
    }
    case 404:
        strlcpy(inf->error, "404: la URL del script no existe (NUBE_URL?)",
                sizeof(inf->error));
        r = NUBE_RECHAZADA;
        break;
    case 401:
    case 403:
        snprintf(inf->error, sizeof(inf->error), "%d: sin permiso para usar el script",
                 inf->http);
        r = NUBE_RECHAZADA;
        break;
    case 413:
        strlcpy(inf->error, "413: la foto es demasiado grande", sizeof(inf->error));
        r = NUBE_RECHAZADA;
        break;
    default:
        snprintf(inf->error, sizeof(inf->error), "respuesta HTTP %d", inf->http);
        r = (inf->http >= 400 && inf->http < 500 && inf->http != 408 && inf->http != 429)
                ? NUBE_RECHAZADA : NUBE_ERROR_SERVIDOR;
        break;
    }

fin:
    if (inf->ms_envio == 0) {
        inf->ms_envio = ms_desde(t0);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return r;
}

/* ======================================================================== */
/*  Paso 2: el GET de la respuesta                                           */
/* ======================================================================== */

static nube_resultado_t leer_respuesta(nube_informe_t *inf)
{
    char cuerpo[RESPUESTA_MAX];
    const esp_http_client_config_t cfg = {
        .url = s_url_respuesta,
        .method = HTTP_METHOD_GET,
        .timeout_ms = PLAZO_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        strlcpy(inf->error, "sin memoria para el cliente HTTP", sizeof(inf->error));
        return NUBE_ERROR_SERVIDOR;
    }

    nube_resultado_t r = NUBE_ERROR_RED;
    const int64_t t0 = esp_timer_get_time();

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        snprintf(inf->error, sizeof(inf->error), "no conecta para leer la respuesta: %s",
                 esp_err_to_name(err));
        goto fin;
    }
    if (esp_http_client_fetch_headers(c) < 0) {
        strlcpy(inf->error, "sin respuesta al pedir el resultado", sizeof(inf->error));
        goto fin;
    }
    inf->http = esp_http_client_get_status_code(c);
    if (inf->http != 200) {
        snprintf(inf->error, sizeof(inf->error), "el resultado llego con HTTP %d",
                 inf->http);
        r = NUBE_ERROR_SERVIDOR;
        goto fin;
    }

    const int n = esp_http_client_read_response(c, cuerpo, sizeof(cuerpo) - 1);
    if (n <= 0) {
        strlcpy(inf->error, "resultado vacio", sizeof(inf->error));
        r = NUBE_ERROR_SERVIDOR;
        goto fin;
    }
    cuerpo[n] = '\0';
    ESP_LOGI(TAG, "respuesta: %s", cuerpo);
    r = interpretar_json(cuerpo, inf);

fin:
    inf->ms_respuesta = ms_desde(t0);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return r;
}

/* ======================================================================== */
/*  API                                                                      */
/* ======================================================================== */

/* Un token de configurar() son 32 caracteres hexadecimales. Aquí se acepta
 * cualquier secreto de 16 a 64 caracteres [A-Za-z0-9_-]: va dentro del JSON
 * tal cual, sin escapar. */
static bool token_valido(const char *t)
{
    size_t n = 0;
    for (; t[n] != '\0'; n++) {
        const char c = t[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return n >= 16 && n <= 64;
}

esp_err_t nube_iniciar(const nube_config_t *cfg)
{
    if (cfg == NULL || cfg->url == NULL || cfg->token == NULL ||
        cfg->dispositivo == NULL || cfg->arranque == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(cfg->url, "https://script.google.com/macros/s/", 35) != 0 ||
        strstr(cfg->url, "/exec") == NULL) {
        ESP_LOGE(TAG, "NUBE_URL tiene que ser la URL de la implementacion "
                      "del Apps Script: https://script.google.com/macros/s/.../exec");
        return ESP_ERR_INVALID_ARG;
    }
    if (!token_valido(cfg->token)) {
        ESP_LOGE(TAG, "NUBE_TOKEN: copia el token que imprimio configurar() "
                      "en el registro del Apps Script (32 caracteres)");
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    ESP_LOGI(TAG, "subidas a %.60s...", cfg->url);
    return ESP_OK;
}

nube_resultado_t nube_subir(const nube_foto_t *foto, nube_informe_t *inf)
{
    memset(inf, 0, sizeof(*inf));
    if (s_cfg.url == NULL || foto == NULL || foto->jpeg == NULL || foto->largo == 0) {
        strlcpy(inf->error, "nube_iniciar() no se llamo, o la foto esta vacia",
                sizeof(inf->error));
        return NUBE_RECHAZADA;
    }

    ESP_LOGI(TAG, "foto %" PRIu32 ": POST de %u B (JPEG de %u B en base64)",
             foto->numero, (unsigned)nube_bytes_a_enviar(foto),
             (unsigned)foto->largo);

    nube_resultado_t r = enviar_foto(foto, inf);
    if (r != NUBE_OK) {
        return r;
    }
    ESP_LOGI(TAG, "foto %" PRIu32 ": %d en %" PRIu32 " ms; leyendo el resultado",
             foto->numero, inf->http, inf->ms_envio);
    return leer_respuesta(inf);
}

const char *nube_nombre_resultado(nube_resultado_t r)
{
    switch (r) {
    case NUBE_OK:             return "OK";
    case NUBE_ERROR_RED:      return "ERROR DE RED";
    case NUBE_ERROR_SERVIDOR: return "ERROR DEL SERVIDOR";
    case NUBE_RECHAZADA:      return "RECHAZADA";
    default:                  return "?";
    }
}


