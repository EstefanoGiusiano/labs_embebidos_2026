#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/touch_pad.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "TOUCH_LAB";

// ASIGNACIÓN TOUCH PADS (ESP32-S2)
#define TOUCH_PAD_PWR    TOUCH_PAD_NUM1 // GPIO1 -> Botón 1
#define TOUCH_PAD_COLOR  TOUCH_PAD_NUM2 // GPIO2 -> Botón 2
#define TOUCH_PAD_BRIGHT TOUCH_PAD_NUM3 // GPIO3 -> Botón 3

// SALIDAS FÍSICAS GPIO PARA LEDS
#define LED_PWR_GPIO    GPIO_NUM_15 // Salida LED Encendido/Apagado
#define LED_COLOR_GPIO  GPIO_NUM_16 // Salida LED Cambio de Estado/Color
#define LED_BRIGHT_GPIO GPIO_NUM_17 // Salida LED Brillo/Nivel

typedef enum {
    TOUCH_STATUS_INACTIVE = 0,
    TOUCH_STATUS_ACTIVE = 1
} touch_status_t;

typedef struct {
    touch_pad_t pad_num;
    touch_status_t status;
    int64_t timestamp_us;
} touch_event_t;

static QueueHandle_t touch_evt_queue = NULL;

// Variables de estado del sistema
static bool system_on = true;
static int color_index = 0;
static const char* colors[] = {"Rojo", "Verde", "Azul", "Blanco"};
static int brightness = 100;

static int64_t press_start_time[TOUCH_PAD_MAX] = {0};

// Función para actualizar los estados de los LEDs Físicos
static void update_physical_leds(void) {
    if (!system_on) {
        gpio_set_level(LED_PWR_GPIO, 0);
        gpio_set_level(LED_COLOR_GPIO, 0);
        gpio_set_level(LED_BRIGHT_GPIO, 0);
        return;
    }

    // LED de encendido activo
    gpio_set_level(LED_PWR_GPIO, 1);

    // Parpadeos/activación visual según selección de color y brillo
    gpio_set_level(LED_COLOR_GPIO, (color_index % 2)); // Alterna estado lógico
    gpio_set_level(LED_BRIGHT_GPIO, (brightness > 50) ? 1 : 0);
}

// ISR: Disparo por hardware
static void IRAM_ATTR touch_isr_handler(void *arg) {
    int64_t now = esp_timer_get_time();
    uint32_t pad_intr = touch_pad_get_status();
    touch_pad_clear_status();

    touch_pad_t pads[3] = {TOUCH_PAD_PWR, TOUCH_PAD_COLOR, TOUCH_PAD_BRIGHT};
    
    for (int i = 0; i < 3; i++) {
        if ((pad_intr >> pads[i]) & 0x01) {
            touch_event_t evt;
            evt.pad_num = pads[i];
            evt.timestamp_us = now;
            
            uint32_t val = 0, thresh = 0;
            touch_pad_read_raw_data(pads[i], &val);
            touch_pad_get_thresh(pads[i], &thresh);

            evt.status = (val >= thresh) ? TOUCH_STATUS_ACTIVE : TOUCH_STATUS_INACTIVE;

            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xQueueSendFromISR(touch_evt_queue, &evt, &xHigherPriorityTaskWoken);
            if (xHigherPriorityTaskWoken) {
                portYIELD_FROM_ISR();
            }
        }
    }
}

// Tarea consumidora de eventos FreeRTOS
static void touch_task(void *pvParameters) {
    touch_event_t evt;
    while (1) {
        if (xQueueReceive(touch_evt_queue, &evt, portMAX_DELAY)) {
            int64_t processed_time = esp_timer_get_time();
            int64_t latency = processed_time - evt.timestamp_us;
            
            const char* status_str = (evt.status == TOUCH_STATUS_ACTIVE) ? "ACTIVE" : "INACTIVE";
            
            ESP_LOGI(TAG, "CANAL: TOUCH_PAD_%d | ESTADO: %s | LATENCIA ISR->TASK: %" PRIu64 " us", 
                     evt.pad_num, status_str, latency);

            if (evt.status == TOUCH_STATUS_ACTIVE) {
                press_start_time[evt.pad_num] = evt.timestamp_us;
            } else {
                int64_t duration_ms = (evt.timestamp_us - press_start_time[evt.pad_num]) / 1000;
                bool is_long_press = duration_ms > 1000;

                switch (evt.pad_num) {
                    case TOUCH_PAD_PWR:
                        if (is_long_press) {
                            system_on = false;
                            ESP_LOGI(TAG, "  -> [BOTON 1] Pulsación LARGA (%" PRIu64 " ms): Apagado de emergencia", duration_ms);
                        } else {
                            system_on = !system_on;
                            ESP_LOGI(TAG, "  -> [BOTON 1] Pulsación CORTA: Sistema %s", system_on ? "ON" : "OFF");
                        }
                        break;

                    case TOUCH_PAD_COLOR:
                        if (system_on) {
                            color_index = (color_index + 1) % 4;
                            ESP_LOGI(TAG, "  -> [BOTON 2] Color cambiado a: %s", colors[color_index]);
                        } else {
                            ESP_LOGW(TAG, "  -> [BOTON 2] Ignorado (Sistema OFF)");
                        }
                        break;

                    case TOUCH_PAD_BRIGHT:
                        if (system_on) {
                            brightness = (brightness >= 100) ? 25 : brightness + 25;
                            ESP_LOGI(TAG, "  -> [BOTON 3] Brillo cambiado a: %d%%", brightness);
                        } else {
                            ESP_LOGW(TAG, "  -> [BOTON 3] Ignorado (Sistema OFF)");
                        }
                        break;

                    default:
                        break;
                }

                // Cambiar el nivel físico de los pines
                update_physical_leds();
            }
        }
    }
}

void app_main(void) {
    // Configurar Pines GPIO de Salida para los LEDs
    gpio_reset_pin(LED_PWR_GPIO);
    gpio_set_direction(LED_PWR_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(LED_COLOR_GPIO);
    gpio_set_direction(LED_COLOR_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(LED_BRIGHT_GPIO);
    gpio_set_direction(LED_BRIGHT_GPIO, GPIO_MODE_OUTPUT);

    update_physical_leds();

    // inicializamos el  Touch Pad  en esta parte 
    ESP_ERROR_CHECK(touch_pad_init());
    ESP_ERROR_CHECK(touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER));
    ESP_ERROR_CHECK(touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V));

    ESP_ERROR_CHECK(touch_pad_config(TOUCH_PAD_PWR));
    ESP_ERROR_CHECK(touch_pad_config(TOUCH_PAD_COLOR));
    ESP_ERROR_CHECK(touch_pad_config(TOUCH_PAD_BRIGHT));

    ESP_ERROR_CHECK(touch_pad_fsm_start());
    vTaskDelay(pdMS_TO_TICKS(150));

    // Calibración desde Benchmark
    uint32_t val1 = 0, val2 = 0, val3 = 0;
    touch_pad_read_benchmark(TOUCH_PAD_PWR, &val1);
    touch_pad_read_benchmark(TOUCH_PAD_COLOR, &val2);
    touch_pad_read_benchmark(TOUCH_PAD_BRIGHT, &val3);

    if (val1 == 0) val1 = 1000;
    if (val2 == 0) val2 = 1000;
    if (val3 == 0) val3 = 1000;

    ESP_ERROR_CHECK(touch_pad_set_thresh(TOUCH_PAD_PWR, (uint32_t)(val1 * 1.15)));
    ESP_ERROR_CHECK(touch_pad_set_thresh(TOUCH_PAD_COLOR, (uint32_t)(val2 * 1.15)));
    ESP_ERROR_CHECK(touch_pad_set_thresh(TOUCH_PAD_BRIGHT, (uint32_t)(val3 * 1.15)));

    // FreeRTOS
    touch_evt_queue = xQueueCreate(10, sizeof(touch_event_t));
    xTaskCreate(touch_task, "touch_task", 3072, NULL, 5, NULL);

    // Interrupciones
    ESP_ERROR_CHECK(touch_pad_isr_register(touch_isr_handler, NULL, TOUCH_PAD_INTR_MASK_ALL));
    ESP_ERROR_CHECK(touch_pad_intr_enable(TOUCH_PAD_INTR_MASK_ALL));

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "Sistema y salidas GPIO listos | Benchmarks: P1=%" PRIu32 ", P2=%" PRIu32 ", P3=%" PRIu32, val1, val2, val3);
    ESP_LOGI(TAG, "==================================================");
}