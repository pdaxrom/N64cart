#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/structs/systick.h"
#include "n64.h"
#include "rgb_led.h"

#if defined(PICO_DEFAULT_LED_PIN) && (PICO_LED_WS2812 == 1)

static uint32_t T0;
static uint32_t T1;
static uint32_t pwm_buffer[8 * 3 + 1];
static int pwm_dma_chan = -1;

static void rgb2pwm(uint32_t rgb)
{
    uint32_t grb = ((rgb << 16) & 0xff000000) | (rgb & 0x00ff0000) | ((rgb & 0x000000ff) << 8);
    for (int i = 0; i < (8 * 3); i++) {
        if (grb & 0x80000000) {
            pwm_buffer[i] = T1 << 16;
        } else {
            pwm_buffer[i] = T0 << 16;
        }
        grb <<= 1;
    }
    pwm_buffer[3 * 8] = 0;
}

void set_rgb_led(uint32_t rgb)
{
    rgb2pwm(rgb);
    dma_channel_wait_for_finish_blocking(pwm_dma_chan);
    dma_channel_set_read_addr(pwm_dma_chan, pwm_buffer, true);
}

void init_rgb_led(void)
{
    gpio_set_function(PICO_DEFAULT_LED_PIN, GPIO_FUNC_PWM);
    int led_pwm_slice_num = pwm_gpio_to_slice_num(PICO_DEFAULT_LED_PIN);

    float clk = clock_get_hz(clk_sys);
    uint32_t pwm_wrap = 71;

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, clk / 800000.f / (float)pwm_wrap);
    pwm_config_set_wrap(&config, pwm_wrap);
    pwm_init(led_pwm_slice_num, &config, true);

    T0 = pwm_wrap * 33 / 100;
    T1 = pwm_wrap - T0;

    rgb2pwm(0x000000);

    // Setup DMA channel to drive the PWM
    pwm_dma_chan = dma_claim_unused_channel(true);

    dma_channel_config pwm_dma_chan_config = dma_channel_get_default_config(pwm_dma_chan);
    // Transfers 32-bits at a time, increment read address so we pick up a new fade value each
    // time, don't increment writes address so we always transfer to the same PWM register.
    channel_config_set_transfer_data_size(&pwm_dma_chan_config, DMA_SIZE_32);
    channel_config_set_read_increment(&pwm_dma_chan_config, true);
    channel_config_set_write_increment(&pwm_dma_chan_config, false);
    // Transfer when PWM slice that is connected to the LED asks for a new value
    channel_config_set_dreq(&pwm_dma_chan_config, DREQ_PWM_WRAP0 + led_pwm_slice_num);

    // Setup the channel and set it going
    dma_channel_configure(
                pwm_dma_chan,
                &pwm_dma_chan_config,
                &pwm_hw->slice[led_pwm_slice_num].cc, // Write to PWM counter compare
                pwm_buffer,
                3 * 8 + 1,
                true
                );
}

#else

void set_rgb_led(uint32_t rgb)
{
}

void init_rgb_led(void)
{
}

#endif
