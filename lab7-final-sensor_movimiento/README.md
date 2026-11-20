| Supported Targets | ESP32-S2 |
| ----------------- | -------- |

# camara_nube — Ejemplo 2 y base de la App 7 (Clase 7)

La cámara del [ejemplo 1](../camara_pantalla/), conectada: cada foto se ve en la pantalla y se **guarda en Google Drive**, a través de un **Apps Script**, por **HTTPS**. Cada foto queda además anotada en una **hoja de cálculo**: cuándo se sacó, de qué placa, cuánto ocupa y el enlace al archivo.

```
   [cam_task p23]  (driver) cuadros JPEG en PSRAM, sin parar
        │ esp_camera_fb_get()
        ▼
   [visor p2] ──foto: copia en PSRAM──▶ cola de fotos ──▶ [subida p5] ──HTTPS──▶ Apps Script
    el visor en pantalla;               4 huecos           espera EV_WIFI_OK;       ├ Drive
    BOOT o el temporizador                                 reintenta con espera     └ Hoja
    piden una foto                                         creciente

   [conexion p3]  (red_wifi.c) reconexión WiFi con espera creciente
   [timer]        informe cada 30 s; una foto cada FOTO_CADA_S si no es 0
```

Lo que hay que ver funcionando:

- el **visor no espera nunca a la red**: entre la foto y la nube hay una **cola** de 4 fotos en PSRAM, y una tarea propia que se bloquea en la red;
- la foto viaja **cifrada y a un servidor comprobado**: TLS, con el certificado de Google verificado contra las autoridades que trae ESP-IDF;
- la placa **no tiene credenciales de Google**: solo conoce la URL del script y un **token** que el script comprueba;
- si la red se cae, las fotos **esperan**; si la respuesta se pierde, la placa **reintenta** y la nube **no la guarda dos veces**;
- la **hora de captura** la pone la nube con su reloj: la placa solo dice cuánto **esperó** la foto (la `edad` de la Clase 6).

## Hardware

El del [ejemplo 1](../camara_pantalla/README.md#hardware), con el mismo procedimiento para flashear en el kit v1.2 (**sin la cámara**), y además una red WiFi de **2,4 GHz** con salida a Internet.

## Antes de compilar: la nube

La placa necesita dos datos que da el Apps Script al instalarlo: la **URL** de la implementación y el **token**. La instalación son unos diez minutos con una cuenta de Google, y está paso a paso en [`../apps_script/README.md`](../apps_script/README.md). Sin ellos el ejemplo funciona como cámara, pero las fotos no se suben (la pantalla dice `SIN NUBE`).

## Configuración

Arriba del todo de [`main/camara_nube_main.c`](main/camara_nube_main.c):

```c
#define WIFI_SSID       "nombre-de-tu-red"
#define WIFI_CLAVE      "clave-de-tu-red"
#define WIFI_SEGURIDAD_WPA2         /* una sola línea descomentada, como en la Clase 6 */

#define NUBE_URL        "https://script.google.com/macros/s/PEGA-AQUI-EL-ID/exec"
#define NUBE_TOKEN      "pega-aqui-el-token"

#define KALUGA_V1_2
#define PANTALLA_CON_ST7789

#define FOTO_TAMANO         FRAMESIZE_VGA
#define FOTO_CALIDAD        12
#define CAMARA_VOLTEAR      true
#define CAMARA_ESPEJO       false
#define FOTO_CADA_S         0
```

- **La red**, igual que en la Clase 6: nombre, clave y **una** línea de seguridad descomentada. El módulo [`red_wifi.c`](main/red_wifi.c) es el mismo archivo que en la Clase 6.
- **`NUBE_URL`**: la URL de la aplicación web, la que termina en `/exec`.
- **`NUBE_TOKEN`**: los 32 caracteres que imprime `configurar()` en el registro del script.
- **`FOTO_CADA_S`**: además del botón, una foto automática cada tantos segundos. `0` la desactiva.
- ⚠️ **La clave del WiFi y el token quedan escritos en el código.** El token es la llave de tu Drive: vuelve a poner los valores de ejemplo antes de cada *commit*. Si se publica por error, `nuevoToken()` en el script lo invalida.

## La subida, paso a paso

```
   placa ──POST https://script.google.com/macros/s/<ID>/exec─────────────▶ Google
          Content-Type: application/json   (Content-Length: se sabe antes de empezar)
          {"token":"...","dispositivo":"kaluga-0ab2c3","arranque":"5f3a9c01",
           "numero":3,"edad_ms":850,"ancho":640,"alto":480,"jpeg":"/9j/4AAQ..."}
                                                      el script corre: token, Drive, hoja
         ◀── 302  Location: https://script.googleusercontent.com/macros/echo?user_content_key=...
   placa ──GET esa dirección (de un solo uso)────────────────────────────▶ Google
         ◀── 200  {"ok":true,"nombre":"kaluga-0ab2c3_2026-10-24_09-15-32_3.jpg","id":"...","bytes":40211}
```

| Paso | Qué pasa | Por qué así |
|---|---|---|
| El POST | La foto va en **base64** dentro de un JSON | Apps Script solo entrega el cuerpo como **texto**: los bytes de un JPEG no sobrevivirían. Cuesta un 33 % más de datos |
| Por trozos | Se codifican y se mandan 3 KB cada vez | La foto en base64 nunca está entera en memoria |
| El 302 | No es un error: el script ya corrió | Google sirve **siempre** la respuesta de un script desde otra dirección, de un solo uso |
| El GET | Se lee el resultado | Ahí está `ok`, el nombre del archivo o el motivo del rechazo |

| Resultado | Ejemplos | Qué hace la placa |
|---|---|---|
| **OK** | `{"ok":true,...}` | Libera la foto; `OK 2.7 S` en pantalla |
| **Error de red** | No conecta, se corta, plazo agotado | Reintenta la misma foto: 2, 4, 8... hasta 60 s, con azar; sin red, espera el WiFi |
| **Error del servidor** | HTTP 500, una página HTML, `limite`, `interno` | Reintenta hasta 5 veces y la descarta |
| **Rechazada** | `token`, `no_es_jpeg`, 404 (URL), inicio de sesión | La descarta enseguida: insistir no lo arregla. `TOKEN MAL`, `URL MAL` o `ACCESO MAL` en pantalla |

## Las tareas

| Tarea | Prio | De quién | Espera en | Por qué esa prioridad |
|---|:-:|---|---|---|
| **wifi** | 23 | ESP-IDF | — | El driver de la radio |
| **cam_task** | 23 | Driver de la cámara | Su cola de eventos | Copia el DMA al cuadro en PSRAM |
| **esp_timer** | 22 | ESP-IDF | — | Temporizadores |
| **sys_evt** | 20 | ESP-IDF | Su cola | Los eventos WiFi e IP |
| **tcpip** | 18 | ESP-IDF | Su buzón | TCP/IP: todo lo que usa la red va por debajo |
| **subida** | 5 | Programa | La cola de fotos; el bit `EV_WIFI_OK`; el socket | Casi siempre bloqueada; cuando corre (base64, el saludo TLS) es poco rato |
| **conexion** | 3 | `red_wifi.c` | Notificación | Solo decide cuándo reintentar el WiFi |
| **visor** | 2 | Programa | Un cuadro; el SPI | Decodificar es CPU pura: usa lo que sobra |
| **Tmr Svc** | 1 | FreeRTOS | Sus temporizadores | El informe y la foto periódica |

> El **saludo TLS** es cálculo de curva elíptica en la tarea `subida`, y con prioridad 5 le quita la CPU al visor: durante el saludo, el visor se frena. Está a propósito: una subida que espera a que el visor termine tarda más, y el visor no tiene prisa.

## La barra de estado

`RED COLA 1 SUBIENDO` — tres partes: si hay red (`RED` o `SIN RED`), cuántas fotos esperan (las de la cola más la que tiene `subida` en la mano), y lo último que pasó:

| Texto | Qué pasó |
|---|---|
| `SUBIENDO` | `subida` está dentro de `nube_subir()` |
| `OK 2.7 S` | La última foto llegó; lo que tardó, POST más GET |
| `ERROR RED`, `ERROR NUBE` | La última subida falló; la foto sigue y se reintenta |
| `TOKEN MAL`, `URL MAL`, `ACCESO MAL`, `RECHAZADA` | La nube la rechazó: revisa la configuración |
| `FOTO 3 EN COLA` | La foto recién sacada, con marco blanco |
| `FOTO 9 COLA LLENA` | No cabe: se descarta la NUEVA, las que esperaban siguen |
| `SIN NUBE` | Falta configurar `NUBE_URL` y `NUBE_TOKEN` |

## Estructura

```
camara_nube/
├── CMakeLists.txt
├── sdkconfig.defaults        Lo del ejemplo 1, más TLS: el paquete de autoridades
│                             con soporte de raíces cruzadas, y mbedTLS en PSRAM
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml     espressif/esp32-camera, esp_jpeg y esp_lcd_ili9341
│   ├── camara.c / .h         } los mismos archivos que en camara_pantalla
│   ├── imagen.c / .h         }
│   ├── pantalla.c / .h       }
│   ├── informe.c / .h        }
│   ├── red_wifi.c / .h       El WiFi: el mismo archivo que en la Clase 6
│   ├── nube.c / .h           La subida: POST con la foto, el 302 y el GET del resultado
│   └── camara_nube_main.c    Configuración, visor, cola, subida e informe
└── README.md   (este archivo)
```

> **Una opción de `sdkconfig.defaults` es imprescindible:** `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`. Los servidores de Google mandan su raíz (*GTS Root R4*) también firmada por una autoridad antigua (*GlobalSign Root CA*) que ya no está en el paquete de ESP-IDF. Sin la opción, la placa busca esa autoridad, no la encuentra y corta la conexión: `esp-x509-crt-bundle: No matching trusted root certificate found`. Se comprobó con el código de ESP-IDF v6.0.2 contra `script.google.com`, con la opción y sin ella. Si el `sdkconfig` se creó antes de tener la opción, bórralo para que se regenere.

## Cómo compilar, flashear y probar

1. Instala el Apps Script y comprueba que `probar()` deja una foto en tu Drive ([`../apps_script/README.md`](../apps_script/README.md)).
2. Escribe la red, la URL y el token en el `.c`.
3. Compila y flashea **sin la cámara** (kit v1.2), apaga, encaja la cámara y enciende.
4. Espera `RED` en la barra, pulsa **BOOT** y mira: `FOTO 1 EN COLA`, luego `SUBIENDO`, luego `OK ... S`.
5. Abre la carpeta **Kaluga - fotos** en Google Drive y la hoja **Kaluga - registro de fotos**.

## Salida (ilustrativa)

Las líneas salen de los formatos del programa; los números que dependen de la placa y de la red se dejan en `...`. Este ejemplo **no se ha ejecutado todavía en una placa** (ver «Estado», abajo).

```
I (312) camara_nube: === camara_nube: fotos de la Kaluga-1 a Google Drive ===
I (624) pantalla: panel ST7789 listo, 320x240, SPI2 a 40 MHz
I (...) camara: sensor OV2640 (PID 0x26): 640x480 JPEG, calidad 12, 2 buffers de 76800 B en PSRAM, XCLK 20 MHz
I (...) red_wifi: WiFi listo: red "MiCasa", minimo WPA2
I (...) camara_nube: ID de esta placa: kaluga-0ab2c3 (arranque 5f3a9c01)
I (...) nube: subidas a https://script.google.com/macros/s/AKfycbwXXXXXXXXXXXXXXXXXX...
I (...) camara_nube: Listo: pulsa BOOT para sacar una foto. app_main termina.
I (...) red_wifi: IP 192.168.1.37  mascara 255.255.255.0  puerta de enlace 192.168.1.1
I (...) camara_nube: [foto 1] ... B, 640x480: EN COLA
I (...) nube: foto 1: POST de ... B (JPEG de ... B en base64)
I (...) nube: foto 1: 302 en ... ms; leyendo el resultado
I (...) nube: respuesta: {"ok":true,"nombre":"kaluga-0ab2c3_2026-10-24_09-15-32_1.jpg","id":"...","bytes":...}
I (...) camara_nube: [subida] foto 1 en Drive: kaluga-0ab2c3_2026-10-24_09-15-32_1.jpg (... ms + ... ms, ... B enviados)
I (...) camara_nube: --- Informe de kaluga-0ab2c3 (cada 30 s) -------------------------
I (...) camara_nube:   tarea        prio  estado      pila libre    CPU
I (...) camara_nube:   ...
I (...) camara_nube:   SRAM interna: libre ... B, minimo ... B, bloque mayor ... B, apta para DMA ... B
I (...) camara_nube:   PSRAM:        libre ... B, minimo ... B
I (...) camara_nube:   WiFi SI;  conexiones 1, desconexiones 0
I (...) camara_nube:   fotos: tomadas 1, subidas 1 (repetidas 0), en cola 0/4, cola llena 0, sin memoria 0, rechazadas 0, reintentos 0
I (...) camara_nube:   subida: media ... ms, maxima ... ms;  de la captura a la nube, maximo ... ms;  ... KB enviados
```

## Detalles de diseño que conviene mirar en el código

- **La memoria de cada foto tiene un dueño.** El visor reserva exactamente lo que ocupa ese JPEG (en PSRAM), y al encolarla la foto pasa a ser de `subida`, que la libera al terminar. Por eso la imagen para la pantalla se decodifica **antes** de encolar: después, el visor ya no puede tocarla.
- **Cola llena: se descarta la nueva.** Lo contrario que en la Clase 6, donde se tiraba la muestra más vieja. Aquí la foto nueva es la que alguien acaba de sacar y ve en pantalla `COLA LLENA`; las que esperaban ya se dieron por guardadas. No hay una política correcta en abstracto: depende de qué vale más.
- **Tres clases de fallo, tres respuestas.** Sin red se espera; con un error del servidor se reintenta unas pocas veces; con un rechazo no se insiste. Reintentar un token equivocado no lo arregla y retrasa todas las fotos de detrás.
- **Idempotencia.** Si la respuesta se pierde (plazo agotado con el script ya terminado), la placa reintenta la **misma** foto. El script la reconoce por `dispositivo`, `arranque` y `numero`, contesta `"repetida":true` y no la guarda dos veces. Es el problema del QoS 1 de la Clase 6, resuelto en la aplicación.
- **El 302, leído a mano.** `esp_http_client` no sigue el 302 por su cuenta (`disable_auto_redirect`): lo seguiría repitiendo el POST con la foto entera. La dirección del resultado se lee de la cabecera `Location` con un manejador de eventos; `esp_http_client_get_url()` no sirve, porque la reconstruye sin la consulta `?user_content_key=...`.
- **La hora de captura la pone la nube.** La placa no sincroniza el reloj (sin SNTP): manda `edad_ms`, cuánto hace que se sacó la foto, y el script resta. Funciona igual aunque la foto haya esperado diez minutos en la cola.
- **Sin credenciales de Google en la placa.** El script corre con la cuenta de su dueño; la placa solo puede pedirle «guarda esta foto», y solo con el token. Perder la placa no da acceso al Drive: se cambia el token y listo.

## Cosas para probar

| Experimento | Qué observar |
|---|---|
| Apagar el router y sacar tres fotos | `SIN RED COLA 3`; al volver la red, las tres suben en orden, y la hoja enseña cuánto esperó cada una |
| Sacar seis fotos sin red | La sexta: `COLA LLENA`. ¿Cuántas fotos caben en PSRAM con `FOTO_TAMANO` en UXGA? |
| Cambiar el token en el `.c` | `TOKEN MAL`; la foto se descarta y no se reintenta |
| `nuevoToken()` en el script, sin tocar la placa | Lo mismo: la placa quedó fuera |
| Quitar `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY` (y borrar `sdkconfig`) | `No matching trusted root certificate found` y `ERROR RED`: el TLS se corta antes del primer byte de la foto |
| `FOTO_CADA_S 60` durante una hora | La hoja: 60 filas. ¿Hubo reintentos? ¿Cuánto tarda la subida más lenta? |
| `FOTO_TAMANO` en UXGA | El POST pasa de decenas a cientos de KB: compara la subida media en el informe |
| Mirar la columna CPU durante una subida | El visor baja mientras `subida` hace el saludo TLS |

## Solución de problemas

| Síntoma | Solución |
|---|---|
| `SIN NUBE` en la barra | Faltan `NUBE_URL` y `NUBE_TOKEN`, o tienen mal formato: el registro dice cuál |
| `SIN RED` todo el rato | El WiFi: la Clase 6 ([`wifi_estacion`](../../../clase-6/ejemplos/wifi_estacion/)) dice por qué |
| `ERROR RED` y en el registro `No matching trusted root certificate found` | Falta `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`: borra el `sdkconfig` para que se regenere desde `sdkconfig.defaults` |
| `ERROR RED` con `ESP_ERR_HTTP_CONNECT` | No se llega a Google: ¿hay Internet en esa red? ¿Un portal cautivo? |
| `URL MAL` | La URL no es la de la implementación, o le falta `/exec` |
| `ACCESO MAL` | La implementación no tiene acceso «Cualquier usuario»; o la cuenta es de un dominio que no lo permite: usa una cuenta personal |
| `TOKEN MAL` | El token del `.c` no es el del script, o se cambió con `nuevoToken()` |
| `ERROR NUBE` con `limite` | Más de 30 fotos por minuto: espera, o sube `MAX_POR_MINUTO` en el script |
| `SIN MEMORIA` | No hay PSRAM para otra foto: baja `FOTO_TAMANO` o la cola |
| `A stack overflow in task subida` | El saludo TLS usa mucha pila: no bajes `PILA_SUBIDA` sin mirar el informe |

## Estado

- Compila sin *warnings* con **ESP-IDF v6.0.2** para `esp32s2` en todas las variantes de configuración (WPA, WPA2, WPA3; kit v1.2 y v1.3; ST7789 e ILI9341; foto periódica; UXGA). Con dos opciones de una misma lista descomentadas, o ninguna, se detiene con el `#error` previsto.
- **TLS contra Google, con el código real de ESP-IDF** (compilado para el PC): sin la opción de raíces cruzadas, `script.google.com`, `script.googleusercontent.com` y `www.googleapis.com` fallan con *No matching trusted root certificate found*; con ella, verifican. `nube.c`, sin cambios, mandó una foto de 40 KB a `script.google.com` (a una implementación inexistente) y clasificó bien el 404.
- **La subida completa, de punta a punta en el PC**: `nube.c` con el cliente HTTP real de ESP-IDF contra una imitación del servidor de Google que ejecuta el [`Codigo.gs`](../apps_script/Codigo.gs) real con Drive y Sheets simulados. Las fotos llegaron idénticas byte a byte, también en los bordes de los trozos de base64; funcionaron el 302, la repetida, el reintento tras un plazo agotado (una sola copia en Drive), el límite por minuto y todos los fallos (404, inicio de sesión, 500, HTML, sin `Location`, conexión cortada, sin servidor).
- **La lógica de las tareas** (`camara_nube_main.c` real con FreeRTOS en el PC y la cámara, la pantalla, la red y la nube simuladas): cola llena, orden de subida al volver la red, reintentos con espera creciente, descarte por rechazo y por errores del servidor, antirrebote del botón; sin fugas de memoria y sin quedarse con más de un cuadro del driver.
- **Nunca se ha ejecutado en una Kaluga-1**, ni contra un Apps Script de verdad: la cámara, la pantalla, los tiempos, la memoria y el comportamiento real de Google (el formato exacto del 302) están por comprobar.

## Referencias

- [../../detalles/fotos-a-la-nube.md](../../detalles/fotos-a-la-nube.md) · [../../detalles/seguridad-y-privacidad-de-las-fotos.md](../../detalles/seguridad-y-privacidad-de-las-fotos.md) · [../../detalles/imagen-en-memoria-y-en-pantalla.md](../../detalles/imagen-en-memoria-y-en-pantalla.md)
- [../camara_pantalla/](../camara_pantalla/) (la cámara y la pantalla) · [../apps_script/](../apps_script/) (el otro extremo)
- Espressif, *ESP HTTP Client* y *ESP x509 Certificate Bundle* (ESP-IDF v6.0): <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/api-reference/protocols/esp_http_client.html>
- Google, *Web Apps* y *Content Service* (Apps Script): <https://developers.google.com/apps-script/guides/web>
