/*
 * informe.c — Clase 7 (Sistemas Embebidos, UCU 2026)
 *
 * El mismo archivo en camara_pantalla y en camara_nube.
 */

#include <inttypes.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "informe.h"

/* Lo que llevaba cada tarea en el informe anterior, para restar. */
#define MAX_TAREAS      32

typedef struct {
    TaskHandle_t        tarea;
    configRUN_TIME_COUNTER_TYPE cpu;
} anterior_t;

static anterior_t s_anterior[MAX_TAREAS];
static UBaseType_t s_n_anterior;
static configRUN_TIME_COUNTER_TYPE s_total_anterior;

static const char *nombre_estado(eTaskState s)
{
    switch (s) {
    case eRunning:   return "EJECUTANDO";
    case eReady:     return "LISTA";
    case eBlocked:   return "BLOQUEADA";
    case eSuspended: return "SUSPENDIDA";
    case eDeleted:   return "BORRADA";
    default:         return "?";
    }
}

static configRUN_TIME_COUNTER_TYPE cpu_anterior(TaskHandle_t t)
{
    for (UBaseType_t i = 0; i < s_n_anterior; i++) {
        if (s_anterior[i].tarea == t) {
            return s_anterior[i].cpu;
        }
    }
    return 0;                   /* tarea nueva: todo su tiempo es de ahora */
}

void informe_tareas(const char *tag)
{
    const UBaseType_t capacidad = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *t = malloc(capacidad * sizeof(TaskStatus_t));
    if (t == NULL) {
        return;
    }
    configRUN_TIME_COUNTER_TYPE total = 0;
    const UBaseType_t n = uxTaskGetSystemState(t, capacidad, &total);

    /* Inserción, de mayor a menor prioridad: son una veintena. */
    for (UBaseType_t i = 1; i < n; i++) {
        const TaskStatus_t x = t[i];
        UBaseType_t j = i;
        while (j > 0 && t[j - 1].uxCurrentPriority < x.uxCurrentPriority) {
            t[j] = t[j - 1];
            j--;
        }
        t[j] = x;
    }

    /* El contador cuenta µs de esp_timer; en un solo núcleo, la suma de las
     * tareas es el tiempo transcurrido. Lo que se imprime es la parte de los
     * últimos segundos, no la media desde el arranque. */
    const configRUN_TIME_COUNTER_TYPE intervalo = total - s_total_anterior;

    ESP_LOGI(tag, "  %-12s %4s  %-11s %10s  %5s", "tarea", "prio", "estado",
             "pila libre", "CPU");
    for (UBaseType_t i = 0; i < n; i++) {
        const configRUN_TIME_COUNTER_TYPE uso =
            t[i].ulRunTimeCounter - cpu_anterior(t[i].xHandle);
        const uint32_t pct_x10 = (intervalo > 0)
            ? (uint32_t)((uint64_t)uso * 1000 / intervalo) : 0;
        ESP_LOGI(tag, "  %-12s %4u  %-11s %8" PRIu32 " B  %3" PRIu32 ",%" PRIu32 " %%",
                 t[i].pcTaskName, (unsigned)t[i].uxCurrentPriority,
                 nombre_estado(t[i].eCurrentState),
                 (uint32_t)t[i].usStackHighWaterMark,
                 pct_x10 / 10, pct_x10 % 10);
    }

    s_n_anterior = 0;
    for (UBaseType_t i = 0; i < n && i < MAX_TAREAS; i++) {
        s_anterior[s_n_anterior].tarea = t[i].xHandle;
        s_anterior[s_n_anterior].cpu = t[i].ulRunTimeCounter;
        s_n_anterior++;
    }
    s_total_anterior = total;
    free(t);
}

void informe_memoria(const char *tag)
{
    ESP_LOGI(tag, "  SRAM interna: libre %u B, minimo %u B, bloque mayor %u B, "
                  "apta para DMA %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    ESP_LOGI(tag, "  PSRAM:        libre %u B, minimo %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}
