/*
 * microfono — nivel del micrófono de la Kaluga-1 (ES8311 por I2S + I2C).
 *
 * Es lo único del programa que sabe de audio. El resto solo ve un entero:
 * el nivel RMS del último instante, en centésimas de dBFS (-4230 = -42,30 dBFS;
 * 0 dBFS es la escala completa del conversor, -90 dBFS es silencio).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Valor de "no hay lectura": el micrófono no arrancó o la lectura falló. */
#define MIC_SIN_DATO                INT32_MIN

#define MIC_GANANCIA_INICIAL_DB     24
#define MIC_GANANCIA_MAX_DB         42
#define MIC_GANANCIA_PASO_DB        6      /* el ES8311 sube de 6 en 6 dB */

/* Arranca I2C, I2S y el códec. Si falla, el nodo sigue sin micrófono. */
esp_err_t microfono_iniciar(int ganancia_db);

/* Mide unos 220 ms (bloquea la tarea que llama, no la CPU). Devuelve
 * centésimas de dBFS o MIC_SIN_DATO. */
int32_t microfono_leer_db_x100(void);

/* Cambia la ganancia de entrada. true solo si el códec la aplicó. */
bool microfono_poner_ganancia(int ganancia_db);
