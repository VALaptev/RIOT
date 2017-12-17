/*
 * Copyright (C) 2016 Unwired Devices [info@unwds.com]
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup
 * @ingroup
 * @brief
 * @{
 * @file		umdk-adc.c
 * @brief       umdk-adc module implementation
 * @author      Eugene Ponomarev
 * @author      Oleg Artamonov
 */

#ifdef __cplusplus
extern "C" {
#endif

/* define is autogenerated, do not change */
#undef _UMDK_MID_
#define _UMDK_MID_ UNWDS_ADC_MODULE_ID

/* define is autogenerated, do not change */
#undef _UMDK_NAME_
#define _UMDK_NAME_ "adc"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "periph/gpio.h"
#include "periph/adc.h"
#include "board.h"

#include "unwds-common.h"

#include "umdk-adc.h"

#include "thread.h"
#include "rtctimers.h"

static uwnds_cb_t *callback;

static kernel_pid_t timer_pid;

static msg_t timer_msg = {};
static rtctimer_t timer;

static bool is_polled = false;

static struct {
	uint8_t publish_period_min;
	uint32_t adc_lines_enabled;
} adc_config;

static void reset_config(void) {
	adc_config.publish_period_min = UMDK_ADC_PUBLISH_PERIOD_MIN;

	for (int i = 0; i < ADC_NUMOF; i++) {
		adc_config.adc_lines_enabled |= (1 << i);
	}
}

static void init_config(void) {
	reset_config();

	if (!unwds_read_nvram_config(_UMDK_MID_, (uint8_t *) &adc_config, sizeof(adc_config))) {
        reset_config();
    }
        
	printf("[umdk-" _UMDK_NAME_ "] Publish period: %d min\n", adc_config.publish_period_min);
}

static inline void save_config(void) {
	unwds_write_nvram_config(_UMDK_MID_, (uint8_t *) &adc_config, sizeof(adc_config));
}

static void init_adc(void)
{
    int i = 0;

    for (i = 0; i < ADC_NUMOF; i++) {
        if (adc_config.adc_lines_enabled & (1 << i)) {        
            if (adc_init(ADC_LINE(i)) < 0) {
                printf("[umdk-" _UMDK_NAME_ "] Failed to initialize adc line #%d\n", i + 1);
                continue;
            }
        }
    }
}

static void prepare_result(module_data_t *buf)
{
    int i;

    uint16_t samples[ADC_NUMOF] = {};

    for (i = 0; i < ADC_NUMOF; i++) {
        if (!(adc_config.adc_lines_enabled & (1 << i))) {
            samples[i] = 0xFFFF;
        } else {
            adc_init(ADC_LINE(i));
            samples[i] = adc_sample(ADC_LINE(i), UMDK_ADC_ADC_RESOLUTION);
        }
    }
    
    /* VDD scaling */
    if (UMDK_ADC_CONVERT_TO_MILLIVOLTS) {
        samples[ADC_VREF_INDEX] = adc_sample(ADC_LINE(ADC_VREF_INDEX), UMDK_ADC_ADC_RESOLUTION);
    }
	
	if (UMDK_ADC_CONVERT_TO_MILLIVOLTS) {
		/* Calculate Vdd */
		uint32_t full_scale = 0;
		
		switch (UMDK_ADC_ADC_RESOLUTION) {
			case ADC_RES_12BIT:
				full_scale = 4095;
				break;
			case ADC_RES_10BIT:
				full_scale = 1023;
				break;
			case ADC_RES_8BIT:
				full_scale = 255;
				break;
			case ADC_RES_6BIT:
				full_scale = 63;
				break;
			default:
				puts("[umdk-" _UMDK_NAME_ "] Unsupported ADC resolution, aborting.");
				return;
				break; 
		}
		
		for (i = 0; i < ADC_NUMOF; i++) {
			if ((i != ADC_VREF_INDEX) && (i != ADC_TEMPERATURE_INDEX) && (samples[i] != 0xFFFF)) {
				samples[i] = (uint32_t)(samples[i] * samples[ADC_VREF_INDEX]) / full_scale;
			}
		}
	}
	
	for (i = 0; i < ADC_NUMOF; i++) {
        if (samples[i] != 0xFFFF) {
            printf("[umdk-" _UMDK_NAME_ "] Reading line #%d: %d", i + 1, samples[i]);
            if (UMDK_ADC_CONVERT_TO_MILLIVOLTS) {
                if (i == ADC_TEMPERATURE_INDEX) {
                    puts(" C");
                }
                else {
                    puts(" mV");
                }
            }
            else {
                puts(" ");
            }
        }
    }

    if (buf) {
        buf->data[0] = _UMDK_MID_;
        memcpy(buf->data + 1, (uint8_t *) &samples, sizeof(samples));
        buf->length = sizeof(samples) + 1; /* Additional byte for module ID */
    }
}

static void *timer_thread(void *arg)
{
    msg_t msg;
    msg_t msg_queue[4];
    msg_init_queue(msg_queue, 4);

    puts("[umdk-" _UMDK_NAME_ "] Periodic publisher thread started");

    while (1) {
        msg_receive(&msg);

        module_data_t data = {};
        data.as_ack = is_polled;
        is_polled = false;

        prepare_result(&data);

        /* Notify the application */
        callback(&data);

        /* Restart after delay */
        rtctimers_set_msg(&timer, 60 * adc_config.publish_period_min, &timer_msg, timer_pid);
    }

    return NULL;
}

static void set_period (int period) {
    adc_config.publish_period_min = period;
    save_config();

    /* Don't restart timer if new period is zero */
    if (adc_config.publish_period_min) {
        rtctimers_set_msg(&timer, 60 * adc_config.publish_period_min, &timer_msg, timer_pid);
        printf("[umdk-" _UMDK_NAME_ "] Period set to %d minutes\n", adc_config.publish_period_min);
    } else {
        puts("[umdk-" _UMDK_NAME_ "] Timer stopped");
    }
}

int umdk_adc_shell_cmd(int argc, char **argv) {
    if (argc == 1) {
        puts (_UMDK_NAME_ " get - get results now");
        puts (_UMDK_NAME_ " send - get and send results now");
        puts (_UMDK_NAME_ " period <N> - set period to N minutes");
        puts (_UMDK_NAME_ " lines <A B C D> - enable specific ADC lines");
        puts (_UMDK_NAME_ " cfg - view current ADC configuration");
        puts (_UMDK_NAME_ " reset - reset settings to default");
        return 0;
    }
    
    char *cmd = argv[1];
	
    if (strcmp(cmd, "get") == 0) {
        prepare_result(NULL);
    }
    
    if (strcmp(cmd, "send") == 0) {
		/* Send signal to publisher thread */
		msg_send(&timer_msg, timer_pid);
    }
    
    if (strcmp(cmd, "period") == 0) {
        char *val = argv[2];
        set_period(atoi(val));
    }
    
    if (strcmp(cmd, "reset") == 0) {
        reset_config();
        save_config();
    }
    
    if (strcmp(cmd, "cfg") == 0) {
        int i = 0;
        printf("[umdk-" _UMDK_NAME_ "] Period: %d min\n", adc_config.publish_period_min);
        for (i = 0; i < 32; i++) {
            if (adc_config.adc_lines_enabled & (1 << i)) {
                printf("[umdk-" _UMDK_NAME_ "] Line #%d enabled\n", i+1);
            }
        }
    }
    
    if (strcmp(cmd, "lines") == 0) {
        int i = 0;
        int line = 0;
        adc_config.adc_lines_enabled = 0;
        for (i = 2; i < argc; i++) {
            line = strtol(argv[i], NULL, 10);
            if ((line != 0) & (line <= ADC_NUMOF)) {
                adc_config.adc_lines_enabled |= 1 << (line - 1);
                printf("[umdk-" _UMDK_NAME_ "] Line #%d enabled\n", line);
            }
        }
        save_config();
    }
    
    return 1;
}

void umdk_adc_init(uint32_t *non_gpio_pin_map, uwnds_cb_t *event_callback)
{
    (void) non_gpio_pin_map;

    callback = event_callback;
    init_config();

    init_adc();

    /* Create handler thread */
    char *stack = (char *) allocate_stack(UMDK_ADC_STACK_SIZE);
    if (!stack) {
    	puts("[umdk-" _UMDK_NAME_ "] unable to allocate memory. Is too many modules enabled?");
    	return;
    }

    unwds_add_shell_command( _UMDK_NAME_, "type '" _UMDK_NAME_ "' for commands list", umdk_adc_shell_cmd);

    timer_pid = thread_create(stack, UMDK_ADC_STACK_SIZE, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, timer_thread, NULL, "ADC thread");

    /* Start publishing timer */
    rtctimers_set_msg(&timer, 60 * adc_config.publish_period_min, &timer_msg, timer_pid);
}

static void reply_ok(module_data_t *reply)
{
    reply->length = 2;
    reply->data[0] = _UMDK_MID_;
    reply->data[1] = UMDK_ADC_REPLY_OK;
}

static void reply_fail(module_data_t *reply)
{
    reply->length = 2;
    reply->data[0] = _UMDK_MID_;
    reply->data[1] = UMDK_ADC_REPLY_FAIL;
}

bool umdk_adc_cmd(module_data_t *cmd, module_data_t *reply)
{
    if (cmd->length < 1) {
    	reply_fail(reply);
        return true;
    }

    umdk_adc_cmd_t c = cmd->data[0];
    switch (c) {
        case UMDK_ADC_CMD_SET_PERIOD: {
            if (cmd->length != 2) {
                reply_fail(reply);
                break;
            }

            uint8_t period = cmd->data[1];
            set_period(period);

            reply_ok(reply);
            break;
        }

        case UMDK_ADC_CMD_POLL:
        	is_polled = true;

            /* Send signal to publisher thread */
            msg_send(&timer_msg, timer_pid);

            return false; /* Don't reply */

            break;

        case UMDK_ADC_SET_LINES: {
            adc_config.adc_lines_enabled = cmd->data[1];
            
            int i = 0;
            for (i = 0; i < ADC_NUMOF; i++) {
                if (adc_config.adc_lines_enabled & (1 << i))
                	printf("[umdk-" _UMDK_NAME_ "] Line #%d enabled\n", i+1);
            }
            
            save_config();

            /* Re-initialize ADC lines */
            init_adc();

            reply_ok(reply);
            break;
        }

        default:
            reply_fail(reply);
            break;
    }

    return true;
}

#ifdef __cplusplus
}
#endif
