#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdbool.h>

#define CONSUMER "pi02w_rgb_sw_v2"
#define CHIP_PATH "/dev/gpiochip0"

// BCM GPIO Numbers (Physical Pins 11, 13, 15, 16)
#define PIN_RED   17
#define PIN_GREEN 27
#define PIN_BLUE  22
#define PIN_SW    23

// Global request reference for the timer signal handler
struct gpiod_line_request *rgb_req = NULL;
int current_color_state = 0;

// Timer callback (Signal Handler)
void timer_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)si;
    (void)uc;

    if (rgb_req != NULL) {
        // v2 uses enum gpiod_line_value
        enum gpiod_line_value values[3] = {
            GPIOD_LINE_VALUE_INACTIVE, 
            GPIOD_LINE_VALUE_INACTIVE, 
            GPIOD_LINE_VALUE_INACTIVE
        };

        if (current_color_state == 0) {
            values[0] = GPIOD_LINE_VALUE_ACTIVE; // Red
        } else if (current_color_state == 1) {
            values[1] = GPIOD_LINE_VALUE_ACTIVE; // Green
        } else if (current_color_state == 2) {
            values[2] = GPIOD_LINE_VALUE_ACTIVE; // Blue
        }

        // Applies values to the requested lines in the order they were configured
        gpiod_line_request_set_values(rgb_req, values);

        current_color_state++;
        if (current_color_state > 2) {
            current_color_state = 0;
        }
    }
}

int main(void)
{
    struct gpiod_chip *chip = NULL;
    struct gpiod_line_settings *settings_rgb = NULL;
    struct gpiod_line_settings *settings_sw = NULL;
    struct gpiod_line_config *line_cfg_rgb = NULL;
    struct gpiod_line_config *line_cfg_sw = NULL;
    struct gpiod_request_config *req_cfg = NULL;
    struct gpiod_line_request *sw_req = NULL;

    unsigned int rgb_offsets[3] = {PIN_RED, PIN_GREEN, PIN_BLUE};
    unsigned int sw_offset[1] = {PIN_SW};

    // 1. Open the GPIO chip via path (v2 standard)
    chip = gpiod_chip_open(CHIP_PATH);
    if (chip == NULL) {
        perror("Failed to open " CHIP_PATH);
        return EXIT_FAILURE;
    }

    // 2. Configure RGB Output Settings
    settings_rgb = gpiod_line_settings_new();
    if (settings_rgb == NULL) goto cleanup;
    gpiod_line_settings_set_direction(settings_rgb, GPIOD_LINE_DIRECTION_OUTPUT);

    line_cfg_rgb = gpiod_line_config_new();
    if (line_cfg_rgb == NULL) goto cleanup;
    gpiod_line_config_add_line_settings(line_cfg_rgb, rgb_offsets, 3, settings_rgb);

    // 3. Configure Switch Input Settings with Pull-Up
    settings_sw = gpiod_line_settings_new();
    if (settings_sw == NULL) goto cleanup;
    gpiod_line_settings_set_direction(settings_sw, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(settings_sw, GPIOD_LINE_BIAS_PULL_UP);

    line_cfg_sw = gpiod_line_config_new();
    if (line_cfg_sw == NULL) goto cleanup;
    gpiod_line_config_add_line_settings(line_cfg_sw, sw_offset, 1, settings_sw);

    // 4. Create a Request Config
    req_cfg = gpiod_request_config_new();
    if (req_cfg == NULL) goto cleanup;
    gpiod_request_config_set_consumer(req_cfg, CONSUMER);

    // 5. Request the lines from the chip
    // We separate RGB and Switch requests so the timer handler only needs the RGB request
    rgb_req = gpiod_chip_request_lines(chip, req_cfg, line_cfg_rgb);
    if (rgb_req == NULL) {
        perror("Failed to request RGB lines");
        goto cleanup;
    }

    sw_req = gpiod_chip_request_lines(chip, req_cfg, line_cfg_sw);
    if (sw_req == NULL) {
        perror("Failed to request Switch line");
        goto cleanup;
    }

    // Free the configuration objects (they are no longer needed after requesting)
    gpiod_line_settings_free(settings_rgb);
    gpiod_line_settings_free(settings_sw);
    gpiod_line_config_free(line_cfg_rgb);
    gpiod_line_config_free(line_cfg_sw);
    gpiod_request_config_free(req_cfg);

    // 6. Configure POSIX Timer
    struct sigevent sev;
    struct itimerspec its;
    timer_t timerid;
    struct sigaction sa;

    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = timer_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGRTMIN, &sa, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_ptr = &timerid;
    if (timer_create(CLOCK_REALTIME, &sev, &timerid) == -1) {
        perror("timer_create");
        return EXIT_FAILURE;
    }

    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 500000000; 
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 500000000;

    if (timer_settime(timerid, 0, &its, NULL) == -1) {
        perror("timer_settime");
        return EXIT_FAILURE;
    }

    printf("v2 Timer started. Monitoring switch (Press Ctrl+C to exit)...\n");

    // 7. Main loop
    enum gpiod_line_value sw_val[1];
    
    while (true) {
        // Read the switch value
        if (gpiod_line_request_get_values(sw_req, sw_val) == 0) {
            // Because of the pull-up, INACTIVE (0) means grounded/pressed
            if (sw_val[0] == GPIOD_LINE_VALUE_INACTIVE) {
                printf("Switch is PRESSED!\n");
            }
        }
        usleep(100000); // 100ms
    }

cleanup:
    // Safe teardown if setup fails
    if (settings_rgb != NULL) gpiod_line_settings_free(settings_rgb);
    if (settings_sw != NULL) gpiod_line_settings_free(settings_sw);
    if (line_cfg_rgb != NULL) gpiod_line_config_free(line_cfg_rgb);
    if (line_cfg_sw != NULL) gpiod_line_config_free(line_cfg_sw);
    if (req_cfg != NULL) gpiod_request_config_free(req_cfg);
    if (rgb_req != NULL) gpiod_line_request_release(rgb_req);
    if (sw_req != NULL) gpiod_line_request_release(sw_req);
    if (chip != NULL) gpiod_chip_close(chip);

    return EXIT_FAILURE;
}
