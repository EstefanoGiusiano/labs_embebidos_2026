| Supported Targets | ESP32-S2 |
| ----------------- | -------- |

# nodo_mqtt — Ejemplo 2 y base de la App 6 (Clase 6)

La **ESP32-S2-Kaluga-1** como **nodo IoT**: mide la temperatura de su chip, la publica por **MQTT** en un broker público y obedece las órdenes que le llegan del **panel web**, que es otro terminal del mismo broker. Las dos cosas a la vez, sin que una espere a la otra, con las tareas y los mecanismos de la Clase 4.

```
   [sensor p6] ──cola de muestras──▶ [publicador p4] ──publish──▶ broker
    cada N s;      120 huecos =        espera WiFi Y MQTT          │  ▲
    nunca espera   10 min sin red      (grupo de eventos)          │  │
                                                                   │  │
   [mqtt_task p5] ◀── órdenes del panel ───────────────────────────┘  │
    manejador: copia y encola                                         │
       └── cola de órdenes ──▶ [comandos p3] ── eco, estado nuevo ────┘

   [conexion p3]  (red_wifi.c) reconexión WiFi con espera creciente
   [interfaz p2]  LED: estado de la conexión
   [timer]        informe de tareas y contadores cada 30 s
```

Lo que hay que ver funcionando:

- el **sensor no espera nunca a la red**: mide con su periodo aunque el WiFi esté caído; entre él y la red hay una **cola** que aguanta diez minutos de corte;
- la única tarea que se bloquea en la red es `publicador`, y lo hace **dentro de su propio tiempo**: `esp_mqtt_client_publish()` puede tardar segundos;
- las tareas no preguntan «¿hay conexión?»: esperan **los dos bits** `EV_WIFI_OK` y `EV_MQTT_OK` de un grupo de eventos;
- el **manejador de eventos MQTT** solo copia la orden y la encola: mientras corre, el cliente MQTT entero está parado;
- la **última voluntad** y los **mensajes retenidos** hacen que el panel sepa, sin preguntar, qué placas están vivas y cuál fue su último valor.

## Hardware

- Solo la **ESP32-S2-Kaluga-1**, con el **jumper RGB** colocado (LED en GPIO45). No hace falta ninguna placa de extensión.
- El **sensor de temperatura es el interno del ESP32-S2**: mide la temperatura **del chip**, no la del aire. Con la radio transmitiendo, el chip se calienta: el número sube varios grados respecto del ambiente, y es normal.
- ⚠️ **Kit v1.2:** si la placa de audio sigue montada, desmóntala para flashear.

## Configuración

Arriba del todo de [`main/nodo_mqtt_main.c`](main/nodo_mqtt_main.c). La red, igual que en el ejemplo 1 (nombre, clave y **una** línea de seguridad descomentada; ver [el README del ejemplo 1](../wifi_estacion/README.md)). Y además, el broker y la raíz de los temas:

```c
/* --- El broker MQTT: descomenta UNO. Son públicos y gratuitos; el panel web
 *     tiene que apuntar al MISMO broker. --------------------------------- */
#define MQTT_BROKER     "mqtt://broker.hivemq.com:1883"
//#define MQTT_BROKER   "mqtt://broker.emqx.io:1883"
//#define MQTT_BROKER   "mqtts://broker.hivemq.com:8883"  /* con TLS: Clase 7 */

#define MQTT_RAIZ       "ucu-se2026"
```

- **`mqtts://`** cifra con TLS y comprueba el certificado del broker contra las autoridades públicas que trae ESP-IDF. Es la opción si la red bloquea el 1883 (el ejemplo 1 lo dice). Cuesta memoria y tiempo de conexión: mídelo en el informe.
- **`MQTT_RAIZ`**: si tu grupo quiere sus propios temas, cámbiala aquí **y** en el panel (o abre el panel con `?raiz=...`).
- ⚠️ **La clave del WiFi queda escrita en el código.** Vuelve a poner `"clave-de-tu-red"` antes de cada *commit*.

## Los temas y lo que viaja por ellos

El identificador de la placa sale de su dirección MAC: `kaluga-` y los tres últimos bytes en hexadecimal. Se imprime al arrancar.

| Tema | Quién publica | QoS | Retenido | Contenido |
|---|---|:-:|:-:|---|
| `ucu-se2026/<ID>/temperatura` | La placa | 0 | Sí | `{"t":41.37,"rssi":-58,"seq":120,"edad":0}` |
| `ucu-se2026/<ID>/estado` | La placa, y el broker por ella | 1 | Sí | `{"estado":"en_linea","ip":"192.168.1.37","periodo":5,"version":"clase6-1.0"}` o, como **última voluntad**, `{"estado":"desconectado"}` |
| `ucu-se2026/<ID>/comando` | El panel | 1 | **No** | `periodo=N`, `identificar`, `ping=XYZ` |
| `ucu-se2026/<ID>/eco` | La placa | 0 | No | El testigo `XYZ` de un ping |

- **`t`** va con **punto** decimal y dos decimales: es el formato del JSON, no el de la consola. Sale de centésimas enteras sin pasar por `printf("%f")`.
- **`seq`** cuenta las medidas desde el arranque. El panel ve los **huecos** (muestras perdidas) y los **reinicios** (la secuencia vuelve a cero).
- **`edad`** son los milisegundos que la muestra esperó en la cola antes de salir: 0 con la red bien, minutos para las que salen al volver la red. Con ese número el panel dibuja cada punto en el instante en que se **midió**, no en el que llegó.
- **Una orden nunca se publica retenida.** El broker se la entregaría a la placa en cada reconexión; por eso la placa descarta las que llegan así.

| Orden | Qué hace | Se rechaza si… |
|---|---|---|
| `periodo=N` | Segundos entre medidas, de **1 a 600**. Despierta al sensor y republica el estado, que es la **confirmación** | `N` fuera de rango, con signos o letras de más |
| `identificar` | El LED parpadea en blanco **5 s**: ¿cuál de las veinte es la mía? | Cualquier otro texto |
| `ping=XYZ` | Publica `XYZ` en `.../eco`: el panel mide la ida y vuelta | `XYZ` vacío, de más de 32 caracteres o fuera de `[A-Za-z0-9_-]` |

Cualquiera puede publicar en un broker público, órdenes incluidas. Por eso la placa comprueba cada orden **entera** antes de aplicarla, **descarta las que llegan retenidas** y cuenta todas las que rechaza.

## Las tareas

| Tarea | Prio | De quién | Espera en | Por qué esa prioridad |
|---|:-:|---|---|---|
| **wifi** | 23 | ESP-IDF | — | El driver de la radio |
| **esp_timer** | 22 | ESP-IDF | — | Temporizadores de alta resolución |
| **sys_evt** | 20 | ESP-IDF | Su cola | Reparte los eventos WiFi e IP |
| **tcpip** | 18 | ESP-IDF | Su buzón | La pila TCP/IP; todo lo que usa la red va **por debajo** |
| **sensor** | 6 | Programa | Notificación con plazo | Periódica y cortísima: mide, encola y se bloquea |
| **mqtt_task** | 5 | Componente MQTT | El socket | El cliente MQTT; su manejador de eventos corre aquí |
| **publicador** | 4 | Programa | Los dos bits; la cola | Puede quedarse segundos en `publish()`: no molesta, nadie la espera |
| **comandos** | 3 | Programa | Cola de órdenes | Responde a una persona, pero la red ya tarda cientos de ms |
| **conexion** | 3 | `red_wifi.c` | Notificación | Solo decide cuándo reintentar |
| **interfaz** | 2 | Programa | Notificación con plazo | El LED |

> ¿Por qué `sensor` está **por encima** de `mqtt_task`? Porque su trabajo dura microsegundos y se vuelve a bloquear: nunca le quita a nadie más que eso. Y si estuviera por debajo, una ráfaga de tráfico MQTT podría retrasar la medida. Es el mismo razonamiento que el táctil de `audio_vu` en la Clase 5.

## Colores del LED

| Color | Significa |
|---|---|
| **Ámbar** | Arrancando |
| **Rojo** | Sin WiFi |
| **Azul** | Con WiFi, sin broker |
| **Verde tenue**, con un destello en cada publicación | Conectado y publicando |
| **Blanco** parpadeando | Orden `identificar`, durante 5 s |

## Estructura

```
nodo_mqtt/
├── CMakeLists.txt
├── sdkconfig.defaults        Flash de 4 MB, partición grande, tick de 1 ms, informe
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml     espressif/mqtt y espressif/led_strip
│   ├── red_wifi.c / .h       La conexión WiFi (el mismo archivo que wifi_estacion)
│   └── nodo_mqtt_main.c      Configuración, tareas, MQTT, órdenes e informe
└── README.md   (este archivo)
```

> **Desde ESP-IDF v6 el cliente MQTT no viene dentro del IDF**: es el componente `espressif/mqtt` del registro, como `led_strip`. El primer *build* lo descarga, y necesita Internet.

## Cómo compilar, flashear y ver los datos

1. Compila y flashea como siempre (botones de VS Code o `idf.py build flash monitor`). No hace falta ninguna opción `-D`.
2. Abre el panel: [`../panel_web/index.html`](../panel_web/index.html), con doble clic. Se conecta solo a HiveMQ.
3. Tu placa aparece con el ID que imprimió al arrancar. Pulsa **Identificar** y mira cuál parpadea.

Para ver los mensajes **sin el panel**, sirve cualquier cliente MQTT: el cliente web de HiveMQ (*MQTT Browser Client*, en su página del broker público: host `broker.hivemq.com`, puerto `8884`, SSL activado), suscrito a `ucu-se2026/#`.

## Salida (ilustrativa)

Los tiempos y las direcciones son de ejemplo; las pilas libres y el heap se dejan en `...` porque son lo que tienes que medir. Este ejemplo **no se ha ejecutado todavía en una placa**.

```
I (318) nodo_mqtt: === nodo_mqtt: la Kaluga-1 como nodo IoT ===
I (402) red_wifi: WiFi listo: red "MiCasa", minimo WPA2, personal (clave)
I (404) nodo_mqtt: ID de esta placa: kaluga-0ab2c3
I (405) nodo_mqtt:   publica en  ucu-se2026/kaluga-0ab2c3/temperatura
I (406) nodo_mqtt:   escucha en  ucu-se2026/kaluga-0ab2c3/comando
I (2734) red_wifi: asociada al punto de acceso: canal 6, seguridad WPA3; pidiendo IP por DHCP
I (3781) red_wifi: IP 192.168.1.37  mascara 255.255.255.0  puerta de enlace 192.168.1.1
I (3790) nodo_mqtt: Arranque completo. app_main termina; las tareas siguen.
I (4398) nodo_mqtt: [mqtt] conectado a mqtt://broker.hivemq.com:1883 como kaluga-0ab2c3, 617 ms despues de la IP
I (30412) nodo_mqtt: --- Informe de kaluga-0ab2c3 (cada 30 s) ------------------------
I (30413) nodo_mqtt:   tarea        prio  estado      pila libre
I (30414) nodo_mqtt:   wifi           23  BLOQUEADA   ...
I (30415) nodo_mqtt:   esp_timer      22  BLOQUEADA   ...
I (30416) nodo_mqtt:   sys_evt        20  BLOQUEADA   ...
I (30417) nodo_mqtt:   tcpip          18  BLOQUEADA   ...
I (30418) nodo_mqtt:   sensor          6  BLOQUEADA   ...
I (30419) nodo_mqtt:   mqtt_task       5  BLOQUEADA   ...
I (30420) nodo_mqtt:   publicador      4  BLOQUEADA   ...
I (30421) nodo_mqtt:   comandos        3  BLOQUEADA   ...
I (30422) nodo_mqtt:   conexion        3  BLOQUEADA   ...
I (30423) nodo_mqtt:   interfaz        2  BLOQUEADA   ...
I (30424) nodo_mqtt:   Tmr Svc         1  EJECUTANDO  ...
I (30425) nodo_mqtt:   IDLE            0  LISTA       ...
I (30426) nodo_mqtt:   WiFi SI, MQTT SI;  conexiones WiFi 1, desconexiones 0, conexiones MQTT 1
I (30427) nodo_mqtt:   arranque: IP a los 3379 ms, broker 617 ms despues de la IP
I (30428) nodo_mqtt:   medidas 7, publicadas 7, en cola 0/120, perdidas 0, devueltas 0
I (30429) nodo_mqtt:   ordenes aceptadas 0, rechazadas 0;  periodo 5 s;  publish mas lento ... us
I (30430) nodo_mqtt:   heap libre: ...
I (61203) nodo_mqtt: [comandos] identificar: LED en blanco 5 s
I (75410) nodo_mqtt: [comandos] periodo = 2 s
```

La primera medida se toma **antes** de que haya red: espera en la cola y sale con `edad` de unos 4 s en cuanto conecta el broker. Por eso, a los 30 s, `publicadas` ya es igual que `medidas`.

## Detalles de diseño que conviene mirar en el código

- **La cola es el amortiguador entre dos relojes**: el del sensor, que es fijo, y el de la red, que no lo es. 120 huecos de 24 bytes son 2,8 KB de RAM y diez minutos de corte con el periodo de 5 s.
- **Con la cola llena se tira la muestra más antigua**, no la nueva: importa más saber qué pasa ahora. `publicador` no puede colarse entre el `xQueueReceive()` y el `xQueueSend()` que lo hacen: tiene menos prioridad que `sensor`, hay un solo núcleo y ninguna de las dos llamadas se bloquea.
- **Si `publish()` falla, la muestra vuelve al frente** de la cola (`xQueueSendToFront`): la serie no se desordena y no se pierde nada.
- **La espera del sensor se puede cortar.** En vez de `vTaskDelay()`, una notificación con plazo: si el panel cambia el periodo de 600 s a 2 s, el sensor mide en el acto en lugar de terminar de esperar diez minutos.
- **El manejador MQTT solo copia y encola.** Comprueba que el mensaje llegó entero y es corto, lo copia (el tema y los datos **no** terminan en `'\0'`) y lo deja en la cola de órdenes. Si se interpretara ahí, una orden lenta pararía al cliente MQTT entero.
- **Las órdenes retenidas se descartan.** El broker guarda un retenido y lo entrega en cada suscripción: una orden retenida se ejecutaría en cada reconexión, para siempre. El panel nunca retiene órdenes, así que una que llega retenida la escribió otro. El manejador solo anota el indicador; la decisión la toma `comandos`, que es quien escribe en el registro.
- **Suscribirse en cada conexión.** Con sesión limpia, el broker olvida las suscripciones al desconectarse: la suscripción a `.../comando` se repite en cada `MQTT_EVENT_CONNECTED`, no una vez en el arranque.
- **Nunca se imprime ni se publica con el mutex tomado.** Se copia el estado, se suelta, y después se formatea o se publica. Un `publish()` puede tardar segundos: con el mutex tomado, bloquearía a todas las tareas que lo esperan.
- **Reintento del broker con azar.** 10 s más un azar de hasta 5 s, distinto en cada placa: los brokers públicos limitan las conexiones seguidas desde una misma IP, y un aula entera sale por una sola IP.
- **Identificador único por placa.** Dos clientes con el mismo identificador se echan mutuamente del broker en bucle; por eso sale de la MAC.

## Cosas para probar

| Experimento | Qué observar |
|---|---|
| Desenchufar la placa | El panel la marca **desconectada** al cabo de un rato: el broker publica la última voluntad cuando pasa 1,5 × keepalive (45 s) sin oírla |
| Apagar el router dos minutos | La cola sube (24 muestras); al volver salen en ráfaga con su `edad`, el panel las coloca en su sitio y no marca huecos |
| Bajar `COLA_MUESTRAS_LARGO` a 10 y cortar la red dos minutos | `perdidas` crece en el informe y el panel marca **huecos** |
| Cambiar el periodo desde el panel | La consola dice `periodo = 2 s` y el panel muestra la confirmación |
| Ping desde el panel | La ida y vuelta completa; compara la red de casa con el teléfono como punto de acceso |
| Publicar `periodo=1` **retenido** en `.../comando` desde otro cliente | La placa la descarta: `orden retenida descartada`. Quita esa comprobación y reinicia: la orden se aplica otra vez en **cada** reconexión. Bórrala publicando un mensaje vacío retenido en el mismo tema |
| Pasar a `mqtts://broker.hivemq.com:8883` | El informe: cuánto baja el heap libre y cuánto sube «broker X ms después de la IP» |
| Llamar a `esp_mqtt_client_publish()` desde `tarea_sensor` y cortar el WiFi | `medidas` deja de avanzar mientras la red no responde: el sensor quedó atado al reloj de la red |
| Un `vTaskDelay(pdMS_TO_TICKS(2000))` en `recibir_comando()` | Las órdenes y los ecos se retrasan, y el resto del cliente también |

## Solución de problemas

| Síntoma | Solución |
|---|---|
| LED **rojo** | Sin WiFi: el ejemplo 1 dice por qué (nombre, clave, seguridad, 5 GHz) |
| LED **azul**, `[mqtt] error de red: errno ...` | No se llega al broker: puerto bloqueado (prueba `mqtts://` o la prueba del ejemplo 1) o sin salida a Internet |
| `el broker rechazo la conexion: codigo 5` en HiveMQ | Demasiadas conexiones seguidas desde tu IP, o dos placas con el mismo ID. Espera un minuto antes de reintentar |
| `codigo 3, servidor no disponible` | El broker está saturado o te limita: cambia de broker en la placa **y** en el panel |
| El panel no muestra tu placa | ¿Mismo broker? ¿Misma raíz? ¿Un filtro escrito? Mira el registro de mensajes del panel |
| La placa sale «sin estado» | El estado retenido todavía no llegó; espera unos segundos |
| `[comandos] orden rechazada` | Formato exacto: `periodo=N` (1 a 600), `identificar`, `ping=XYZ` |
| `A stack overflow in task X` | Sube su pila y ajústala con la columna «pila libre» del informe |

## Referencias

- [../../detalles/mqtt-terminales-y-broker.md](../../detalles/mqtt-terminales-y-broker.md) · [../../detalles/red-y-rtos.md](../../detalles/red-y-rtos.md) · [../../detalles/seguridad-iot-introduccion.md](../../detalles/seguridad-iot-introduccion.md)
- [../wifi_estacion/](../wifi_estacion/) (la conexión WiFi) · [../panel_web/](../panel_web/) (el otro terminal)
- Espressif, *ESP-MQTT* (componente `espressif/mqtt`): `mqtt_client.h` documenta qué funciones bloquean y desde dónde se pueden llamar.
- OASIS, *MQTT Version 5.0*, §1–3: [../../../bibliografia/OASIS - MQTT v5.0 (OASIS Standard).pdf](../../../bibliografia/)
- [../../../clase-4/detalle-cola-semaforo-o-mutex.md](../../../clase-4/detalle-cola-semaforo-o-mutex.md): cola, buzón, mutex y notificación.
