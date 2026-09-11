# Laboratorio 1: Control de LEDs RGB 

## ¿De qué trata este laboratorio?

En este laboratorio nos enfocamos en el control y manejo de periféricos de salida utilizando el microcontrolador **ESP32-S2** (placa Kaluga-1). El objetivo principal es interactuar con componentes de iluminación —específicamente LEDs direccionables RGB— para aprender a enviar señales digitales de control y manejar temporizaciones o efectos visuales básicos desde el firmware.

Para lograr esto, el proyecto depende de un controlador o *driver* especializado (`led_strip`) que emite los pulsos necesarios para encender los LEDs con diferentes colores e intensidades.

---

## El problema de compatibilidad: Transición de v4.4 a v5.x

Inicialmente, este proyecto fue estructurado sobre la versión **ESP-IDF v4.4.7**. En esa versión del framework, Espressif incluía el código de la librería `led_strip` como una carpeta local integrada dentro del propio sistema de ejemplos (`examples/common_components/led_strip`). 

Al actualizar nuestro entorno de trabajo a **ESP-IDF v5.3**, el proyecto dejó de compilar arrojando el siguiente error:

> `CMake Error: Directory specified in EXTRA_COMPONENT_DIRS doesn't exist: .../examples/common_components/led_strip`

### ¿Por qué ocurrió esto?
En ESP-IDF v5.x, Espressif cambió la arquitectura del entorno:
1. **Eliminó la carpeta interna:** Ya no existe esa ruta local de componentes dentro del framework.
2. **Implementó el Component Manager:** Ahora las librerías externas se gestionan mediante un sistema de paquetes dinámico (similar a `npm` en JavaScript o `pip` en Python), descargando los componentes directamente desde el registro oficial de Espressif en internet durante la fase de compilación.

---

## Solución aplicada: Modernización del Proyecto

Para solucionar este error de raíz y hacer que el laboratorio compile correctamente en versiones modernas de ESP-IDF, realizamos una refactorización de la configuración en dos pasos:

### 1. Limpieza de rutas antiguas (`CMakeLists.txt`)
Accedemos al archivo `CMakeLists.txt` ubicado en la raíz principal de la carpeta del proyecto. Allí eliminamos (o comentamos) la variable `EXTRA_COMPONENT_DIRS`, la cual obligaba al compilador a buscar la librería en la ruta vieja de la v4.4:

```cmake
# Eliminamos o comentamos la ruta obsoleta:
# set(EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/examples/common_components/led_strip")