/* Laboratorio 1 - Control de LED RGB direccionable y ciclo de colores */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

static const char *TAG = "LAB_LED_RGB";

#define BLINK_GPIO CONFIG_BLINK_GPIO

static uint8_t s_led_state = 0;
static uint8_t color_index = 0;
static uint32_t cycle_counter = 0;

#ifdef CONFIG_BLINK_LED_RMT
static led_strip_handle_t led_strip;

typedef struct {
    const char *name;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

/* Secuencia de colores (Intensidad regulada a 30/255) */
static const rgb_color_t colors[] = {
    {"ROJO",    30,  0,  0},
    {"VERDE",    0, 30,  0},
    {"AZUL",     0,  0, 30},
    {"AMARILLO",30, 30,  0},
    {"CIAN",     0, 30, 30},
    {"MAGENTA", 30,  0, 30},
    {"BLANCO",  30, 30, 30}
};

#define TOTAL_COLORS (sizeof(colors) / sizeof(rgb_color_t))

static void blink_led(void)
{
    if (s_led_state) {
        rgb_color_t c = colors[color_index];
        
        /* Aplicar color actual en el píxel 0 */
        led_strip_set_pixel(led_strip, 0, c.r, c.g, c.b);
        led_strip_refresh(led_strip);
        
        ESP_LOGI(TAG, "[Ciclo #%lu] Estado: ENCENDIDO | Color: %s (R:%d G:%d B:%d)", 
                 cycle_counter, c.name, c.r, c.g, c.b);
        
        /* Avanzar al siguiente color para el próximo ciclo */
        color_index = (color_index + 1) % TOTAL_COLORS;
        cycle_counter++;
    } else {
        /* Apagar el LED */
        led_strip_clear(led_strip);
        ESP_LOGI(TAG, "Estado: APAGADO");
    }
}

static void configure_led(void)
{
    ESP_LOGI(TAG, "Configurando LED RGB direccionable en GPIO %d...", BLINK_GPIO);
    
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1,
    };
    
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
}

#endif

void app_main(void)
{
    configure_led();

    while (1) {
        blink_led();
        s_led_state = !s_led_state;
        vTaskDelay(CONFIG_BLINK_PERIOD / portTICK_PERIOD_MS);
    }
}