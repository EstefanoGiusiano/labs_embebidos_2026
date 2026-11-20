/*
 * informe.h — Clase 7 (Sistemas Embebidos, UCU 2026)
 *
 * Lo que se imprime en el informe periódico de los dos ejemplos: todas las
 * tareas, con el porcentaje de CPU que usó cada una, y la memoria libre de
 * cada clase. Es el mismo archivo en camara_pantalla y en camara_nube.
 *
 * Hace falta en sdkconfig:
 *   CONFIG_FREERTOS_USE_TRACE_FACILITY=y        uxTaskGetSystemState()
 *   CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y   el tiempo de CPU por tarea
 */
#pragma once

/* Todas las tareas, de mayor a menor prioridad: estado, pila libre y % de
 * CPU desde la llamada anterior (desde el arranque, la primera vez). */
void informe_tareas(const char *tag);

/* SRAM interna (libre, mínimo histórico, bloque más grande, apta para DMA)
 * y PSRAM (libre y mínimo histórico). */
void informe_memoria(const char *tag);
