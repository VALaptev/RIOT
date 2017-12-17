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
 * @file		umdk-range.c
 * @brief       umdk-range module implementation
 * @author      Dmitry Golik <info@unwds.com>
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/* define is autogenerated, do not change */
#undef _UMDK_MID_
#define _UMDK_MID_ UNWDS_RANGE_MODULE_ID

/* define is autogenerated, do not change */
#undef _UMDK_NAME_
#define _UMDK_NAME_ "range"

#include "periph/gpio.h"

#include "board.h"

#include "ultrasoundrange.h"

#include "unwds-common.h"
#include "umdk-range.h"
#include "unwds-gpio.h"

#include "thread.h"
#include "rtctimers.h"

static ultrasoundrange_t dev;

static uwnds_cb_t *callback;

static kernel_pid_t timer_pid;

static msg_t timer_msg = {};
static rtctimer_t timer;

static bool is_polled = false;

static struct {
	uint8_t is_valid;
	uint8_t publish_period_min;
	uint8_t i2c_dev;

    uint16_t transmit_pulses;
    uint16_t silencing_pulses;
    uint16_t period_us;
    uint16_t idle_period_us;
    // uint16_t period_subus;
    uint16_t chirp;
    uint16_t duty;
    uint16_t duty2;
} ultrasoundrange_config;

static bool init_sensor(void) {
    
	printf("[umdk-" _UMDK_NAME_ "] Initializing ultrasound distance meter as opt3001\n");

	bool o = ultrasoundrange_init(&dev) == 0;

	dev.transmit_pulses = ultrasoundrange_config.transmit_pulses;
        dev.silencing_pulses = ultrasoundrange_config.silencing_pulses;
        dev.period_us = ultrasoundrange_config.period_us;
        // dev.period_subus = ultrasoundrange_config.period_subus;
        dev.chirp = ultrasoundrange_config.chirp;
        dev.idle_period_us = ultrasoundrange_config.idle_period_us;
        dev.duty = ultrasoundrange_config.duty;
        dev.duty2 = ultrasoundrange_config.duty2;
        dev.verbose = false;        
	return o;

}

static void prepare_result(module_data_t *data) {
	ultrasoundrange_measure_t measure = {};
	ultrasoundrange_measure(&dev, &measure);
    
    int range;
    range = measure.range;
    
	printf("[umdk-" _UMDK_NAME_ "] Echo distance %d mm\n", range);

    if (data) {
        data->length = 1 + sizeof(range); /* Additional byte for module ID */

        data->data[0] = _UMDK_MID_;

        /* Copy measurements into response */
        memcpy(data->data + 1, (uint8_t *) &range, sizeof(range));
    }
}

static void *timer_thread(void *arg) {
    msg_t msg;
    msg_t msg_queue[4];
    msg_init_queue(msg_queue, 4);
    
    printf("[umdk-" _UMDK_NAME_ "] Periodic publisher thread started");

    while (1) {
        msg_receive(&msg);

        rtctimers_remove(&timer);

        module_data_t data = {};
        data.as_ack = is_polled;
        is_polled = false;

        prepare_result(&data);

        /* Notify the application */
        // callback(&data); 

        /* Restart after delay */
        rtctimers_set_msg(&timer, /* 60 * */ ultrasoundrange_config.publish_period_min, &timer_msg, timer_pid);
    }

    return NULL;
}

static void reset_config(void) {
    return;
}

static void init_config(void) {
	reset_config();

	if (!unwds_read_nvram_config(_UMDK_MID_, (uint8_t *) &ultrasoundrange_config, sizeof(ultrasoundrange_config)))
		return;

	if ((ultrasoundrange_config.is_valid == 0xFF) || (ultrasoundrange_config.is_valid == 0)) {
		reset_config();
		return;
	}

	if (ultrasoundrange_config.i2c_dev >= I2C_NUMOF) {
		reset_config();
		return;
	}
}

static inline void save_config(void) {
	ultrasoundrange_config.is_valid = 1;
	unwds_write_nvram_config(_UMDK_MID_, (uint8_t *) &ultrasoundrange_config, sizeof(ultrasoundrange_config));
}

static void set_period (int period) {
    rtctimers_remove(&timer);

    ultrasoundrange_config.publish_period_min = period;
	save_config();

	/* Don't restart timer if new period is zero */
	if (ultrasoundrange_config.publish_period_min) {
        rtctimers_set_msg(&timer, /* 60 * */ ultrasoundrange_config.publish_period_min, &timer_msg, timer_pid);
		printf("[umdk-" _UMDK_NAME_ "] Period set to %d minute (s)\n", ultrasoundrange_config.publish_period_min);
    } else {
        printf("[umdk-" _UMDK_NAME_ "] Timer stopped");
    }
}

int umdk_range_shell_cmd(int argc, char **argv) {
    if (argc == 1) {
        puts ("range - actually ultrasound rangefinder");
        puts ("range get - get results now");
        puts ("range send - get and send results now");
        puts ("range period <N> - set period to N minutes");
        puts ("range time <N> - set ultrasound period to N microseconds");
        // printf ("range sub <N> - set sub-microsecond addendum to period (in us/%d)\n", UZ_SUBUS_DIVISOR);
        puts ("range chirp <N> - set decrement of period per cycle");
        puts ("range number <N> - set number of US generation periods");
        puts ("range idle <N> - set ultrasound idle time to N microseconds");
        puts ("range number2 <N> - set number of US silencing periods");
        puts ("range duty <N> - set generation duty cycle in percents");
        puts ("range duty2 <N> - set silencing duty cycle in percents");
        puts ("range reset - reset settings to default\n");

        puts ("Current settings:");
        printf("period (publish_period_min): %d\n", ultrasoundrange_config.publish_period_min);
        printf("number (transmit_pulses): %d\n", ultrasoundrange_config.transmit_pulses);
        printf("time (period_us): %d\n", ultrasoundrange_config.period_us);
        // printf("sub (period_subus): %d\n", ultrasoundrange_config.period_subus);
        printf("chirp (chirp): %d\n", ultrasoundrange_config.chirp);
        printf("duty (duty): %d\n", ultrasoundrange_config.duty);
        printf("idle (idle_period_us): %d\n", ultrasoundrange_config.idle_period_us);
        printf("number2 (silencing_pulses): %d\n", ultrasoundrange_config.silencing_pulses);
        printf("duty2 (duty2): %d\n", ultrasoundrange_config.duty2);
        // printf(": %d\n", ultrasoundrange_config.);
        return 0;
    }
/*	uint8_t is_valid;
	uint8_t publish_period_min;
	uint8_t i2c_dev;

    uint16_t transmit_pulses;
    uint16_t silencing_pulses;
    uint16_t period_us;
    uint16_t idle_period_us;
    uint16_t period_subus;
*/

    
    char *cmd = argv[1];
	
    if (strcmp(cmd, "get") == 0) {
        prepare_result(NULL);
    }
    
    if (strcmp(cmd, "send") == 0) {
		/* Send signal to publisher thread */
		msg_send(&timer_msg, timer_pid);
    }
    
    if (strcmp(cmd, "period") == 0) {
        int val = atoi(argv[2]);
        set_period(val);
    }
    
    if (strcmp(cmd, "time") == 0) {
        int val = atoi(argv[2]);
        ultrasoundrange_config . period_us = val;
        dev            . period_us = val;
        save_config();
    }
    
    /*if (strcmp(cmd, "sub") == 0) {
        int val = atoi(argv[2]);
        dev            . period_subus = val;
    }*/
    
    if (strcmp(cmd, "chirp") == 0) {
        int val = atoi(argv[2]);
        ultrasoundrange_config . chirp = val;
        dev            . chirp = val;
    }
    
    if (strcmp(cmd, "number") == 0) {
        int val = atoi(argv[2]);
        ultrasoundrange_config . transmit_pulses = val;
        dev            . transmit_pulses = val;
        save_config();
    }
    
    if (strcmp(cmd, "number2") == 0) {
        int val = atoi(argv[2]);
        ultrasoundrange_config . silencing_pulses = val;
        dev            . silencing_pulses = val;
        save_config();
    }
    
    if (strcmp(cmd, "idle") == 0) {
        int val = atoi(argv[2]);
        ultrasoundrange_config . idle_period_us = val;
        dev            . idle_period_us = val;
        save_config();
    }
    
    if (strcmp(cmd, "duty") == 0) {
        int val = atoi(argv[2]);
        ultrasoundrange_config . duty = val;
        dev            . duty = val;
        save_config();
    }
    
    if (strcmp(cmd, "duty2") == 0) {
        int val = atoi(argv[2]);
        ultrasoundrange_config . duty2 = val;
        dev            . duty2 = val;
        save_config();
    }
    
    if (strcmp(cmd, "verbose") == 0) {
        int val = atoi(argv[2]);
        // ultrasoundrange_config . verbose = val;
        dev            . verbose = val;
        save_config();
    }
    
    if (strcmp(cmd, "reset") == 0) {
        reset_config();
        save_config();
    }
    
    if (strcmp(cmd, "sin") == 0) {
        printf("x = array((");
        for (unsigned int x = 0; x < 65536; x += 128) {
        // for (int x = -32768; x < 32768; x += 128) {
            // printf("%d, ", x);
            printf("%d, ", x * 65536);
        }
        printf("))\n");
        printf("sin_x = ");
        for (unsigned int x = 0; x < 65536; x += 128) {
            printf("%d, ", fast_sin(x * 65536));
        }
        printf("\n");
        printf("cos_x = ");
        for (unsigned int x = 0; x < 65536; x += 128) {
            printf("%d, ", fast_cos(x * 65536));
        }
        printf("\n");
        /*printf("cos_x1 = ");
        for (int x = 0; x < 65536; x += 128) {
            printf("%d, ", _fast_cos_1(x));
        }
        printf("\n");*/
        /*printf("atan_xt = array((");
        for (int x = 0; x < 65536; x += 128) {
            printf("%d, ", _atan_t7(x));
        }
        printf("))\n");
        printf("atan_xp = array((");
        for (int x = 0; x < 65536; x += 128) {
            printf("%d, ", _atan_p4(x));
        }
        printf("))\n");
        printf("atan_x3 = array((");
        // for (int x = 0; x < 65536; x += 128) {
        for (int x = -32768; x < 32768; x += 128) {
            printf("%d, ", fast_atan2(x, 8192));
        }
        printf("))\n");*/
    }
    if (strcmp(cmd, "atan") == 0) {
        int y = atoi(argv[2]);
        int x = atoi(argv[3]);
        printf("atan2 (y, x) = %d\n", fast_atan2(y, x));
    }
    return 1;
}

void umdk_range_init(uint32_t *non_gpio_pin_map, uwnds_cb_t *event_callback) {
	(void) non_gpio_pin_map;

	callback = event_callback;

	init_config();
	printf("[umdk-" _UMDK_NAME_ "] Publish period: %d min\n", ultrasoundrange_config.publish_period_min);

	if (!init_sensor()) {
		printf("[umdk-" _UMDK_NAME_ "] Unable to init sensor!");
        return;
	}

	/* Create handler thread */
	char *stack = (char *) allocate_stack(UMDK_RANGE_STACK_SIZE);
	if (!stack) {
		printf("[umdk-" _UMDK_NAME_ "] Unable to allocate memory. Are too many modules enabled?");
		return;
	}
    
    unwds_add_shell_command("range", "type 'range' for commands list", umdk_range_shell_cmd);
    
	timer_pid = thread_create(stack, UMDK_RANGE_STACK_SIZE, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, timer_thread, NULL, "range thread");

    /* Start publishing timer */
	rtctimers_set_msg(&timer, /* 60 * */ ultrasoundrange_config.publish_period_min, &timer_msg, timer_pid);
}

static void reply_fail(module_data_t *reply) {
	reply->length = 2;
	reply->data[0] = _UMDK_MID_;
	reply->data[1] = 255;
}

static void reply_ok(module_data_t *reply) {
	reply->length = 2;
	reply->data[0] = _UMDK_MID_;
	reply->data[1] = 0;
}

bool umdk_range_cmd(module_data_t *cmd, module_data_t *reply) {
	if (cmd->length < 1) {
		reply_fail(reply);
		return true;
	}

	umdk_range_cmd_t c = cmd->data[0];
	switch (c) {
	case UMDK_RANGE_CMD_SET_PERIOD: {
		if (cmd->length != 2) {
			reply_fail(reply);
			break;
		}

		uint8_t period = cmd->data[1];
		set_period(period);

		reply_ok(reply);
		break;
	}

	case UMDK_RANGE_CMD_POLL:
		is_polled = true;

		/* Send signal to publisher thread */
		msg_send(&timer_msg, timer_pid);

		return false; /* Don't reply */

	case UMDK_RANGE_CMD_INIT_SENSOR: {
		init_sensor();

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
