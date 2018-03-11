/*
 * antenna_pwm.c
 *
 * Created: 12/2/17 8:58:23 PM
 *  Author: jacobleiken
 */ 

#include "antenna_pwm.h"

static int curren_pwm_pin = 1;
static int current_on_cycle = PWM_PERIOD / 2;

// not for flight
static void try_pwm_deploy_basic(int pin, int pin_mux, int ms, int p_ant) {
	for (int i = 8; i < 16; i++) {
		configure_pwm(pin, pin_mux, p_ant);
		enable_pwm(i);
		delay_ms(ms);
		disable_pwm();
	}
}

bool antenna_did_deploy(void) {
	// if we fail to get the mutex, continue on and mess with anything reading sensors
	// (we don't care about sensors much relative to antenna deployment)
	bool got_mutex = false;
	if (xSemaphoreTake(processor_adc_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS)) {
		got_mutex = true;		
	} else {
		log_error(ELOC_ANTENNA_DEPLOY, ECODE_PROC_ADC_MUTEX_TIMEOUT, true);
	}
	
	_set_5v_enable_unsafe(true);
	bool did_deploy = get_input(P_DET_RTN);
	_set_5v_enable_unsafe(false);
	
	if (got_mutex) xSemaphoreGive(processor_adc_mutex);
	return did_deploy;
}

void pwm_test(void) {
	try_pwm_deploy(P_ANT_DRV1, P_ANT_DRV1_MUX, PWM_LENGTH_MS, 1);
	try_pwm_deploy_basic(P_ANT_DRV2, P_ANT_DRV2_MUX, PWM_LENGTH_MS, 2);
	try_pwm_deploy_basic(P_ANT_DRV3, P_ANT_DRV3_MUX, PWM_LENGTH_MS, 3);
}

void try_pwm_deploy(long pin, long pin_mux, int ms, uint8_t p_ant) {
	configure_pwm(pin, pin_mux, p_ant);
	
	hardware_state_mutex_take();
	enable_pwm(current_on_cycle);
	get_hw_states()->antenna_deploying = true;
	hardware_state_mutex_give();
	
	//delay_ms(ms); // for testing only
	vTaskDelay(ms / portTICK_PERIOD_MS);
		
	// read current (both just in case) so we can shut it down if we need
	// TODO: depend on pin
	uint16_t li1, li2, lf1, lf2, lf3, lf4;
	read_lion_current_precise(&li1, &li2);
	read_lifepo_current_precise(&lf1, &lf2, &lf3, &lf4);
	
	hardware_state_mutex_take();
	disable_pwm();
	get_hw_states()->antenna_deploying = false;
	hardware_state_mutex_give();
		
	bool can_cont = true;
	if (p_ant == 1) {
		print("PWM was on LiON\nCurrent on 1: %d\nCurrent on 2: %d\n", li1, li2);
		// TODO: if current is too low log an error
		if (li1 > PWM_MAX_CUR || li2 > PWM_MAX_CUR) {
			can_cont = false;
		}
	} else {
		uint16_t bank1 = lf1 + lf2;
		print("PWM was on LiFePO4\nCurrent on bank 1: %d\n", bank1);
		if (bank1 > PWM_MAX_CUR) {
			can_cont = false;
		}
	}
	if (can_cont) {
		// increment for next call
		current_on_cycle++;
		// it shouldn't be on too much, so if it's at 14 switch to the next pin
		if (current_on_cycle >= (PWM_PERIOD - 2)) {
			current_on_cycle = PWM_PERIOD / 2;
			curren_pwm_pin++;
			// now if the pin is past 3, set it back to 1
			if (curren_pwm_pin > 3) {
				curren_pwm_pin = 1;
			}
		}
	} else {
		current_on_cycle = PWM_PERIOD / 2;
		curren_pwm_pin++;
		// now if the pin is past 3, set it back to 1
		if (curren_pwm_pin > 3) {
			curren_pwm_pin = 1;
		}
	}
}

int get_current_pwm_pin(void) {
	return curren_pwm_pin;
}