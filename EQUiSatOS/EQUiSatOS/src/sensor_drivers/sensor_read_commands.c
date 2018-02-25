/*
 * sensor_read_commands.c
 *
 * Created: 11/1/2016 8:18:16 PM
 *  Author: jleiken
 */

#include "sensor_read_commands.h"

static uint8_t IR_ADDS[6] = {
	IR_FLASH,
	IR_SIDE1,
	IR_SIDE2,
	IR_RBF,
	IR_ACCESS,
	IR_TOP1
};

static uint8_t IR_ELOCS[6] = {
	ELOC_IR_FLASH,
	ELOC_IR_SIDE1,
	ELOC_IR_SIDE2,
	ELOC_IR_RBF,
	ELOC_IR_ACCESS,
	ELOC_IR_TOP1
};

static uint8_t TEMP_ELOCS[8] = {
	ELOC_TEMP_LF_1,
	ELOC_TEMP_LF_2,
	ELOC_TEMP_L_1,
	ELOC_TEMP_L_2,
	ELOC_TEMP_LED_1,
	ELOC_TEMP_LED_2,
	ELOC_TEMP_LED_3,
	ELOC_TEMP_LED_4
};

static uint8_t PD_ELOCS[6] = {
	ELOC_PD_FLASH,
	ELOC_PD_SIDE1,
	ELOC_PD_SIDE2,
	ELOC_PD_RBF,
	ELOC_PD_ACCESS,
	ELOC_PD_TOP1,
};

static struct adc_module adc_instance; // global is allowed because we always lock the processor ADC

void init_sensor_read_commands(void) {
	i2c_mutex = xSemaphoreCreateMutexStatic(&_i2c_mutex_d);
	irpow_mutex = xSemaphoreCreateMutexStatic(&_irpow_mutex_d);
	processor_adc_mutex = xSemaphoreCreateMutexStatic(&_processor_adc_mutex_d);
}

/************************************************************************/
/* HELPERS                                                              */
/************************************************************************/
/* NOTE: the "batch" value passed into these functions are generally arrays, so are passed by reference */

void read_ad7991_ctrlbrd_unsafe(ad7991_ctrlbrd_batch batch);

static inline uint8_t truncate_16t(uint16_t src) {
	return src >> 8;
}

static void log_if_out_of_bounds(uint16_t reading, uint low, uint high, uint8_t eloc, bool priority) {
	if (reading <= low) {
		log_error(eloc, ECODE_READING_LOW, priority);
	} else if (reading >= high) {
		log_error(eloc, ECODE_READING_HIGH, priority);
	}
}

// note: processor ADC is locked externally to these methods for speed and for particular edge cases
static void commands_read_adc_mV(uint16_t* dest, int pin, uint8_t eloc,
	uint low_bound, uint high_bound, bool priority) {
	status_code_genare_t sc = configure_adc(&adc_instance, pin);
	log_if_error(eloc, sc, priority);
	sc = read_adc_mV(adc_instance, dest);
	log_if_error(eloc, sc, priority);
	log_if_out_of_bounds(*dest, low_bound, high_bound, eloc, priority);
}

static void commands_read_adc_mV_truncate(uint8_t* dest, int pin, uint8_t eloc, uint low_bound, uint high_bound, bool priority) {
	uint16_t read;
	status_code_genare_t sc = configure_adc(&adc_instance, pin);
	log_if_error(eloc, sc, priority);
	sc = read_adc_mV(adc_instance, &read);
	log_if_error(eloc, sc, priority);
	log_if_out_of_bounds(read, low_bound, high_bound, eloc, priority);
	*dest = truncate_16t(read);
}

bool _set_5v_enable(bool on) {
	// note: to avoid chance of deadlock, any locks
	// of the i2c bus / processor adc mutex must be above this
	if (hardware_state_mutex_take())
	{
		set_output(on, P_5V_EN);
		get_hw_states()->rail_5v_enabled = on;

		hardware_state_mutex_give();
		return true;
	} else {
		log_error(ELOC_5V_REF, ECODE_HW_STATE_MUTEX_TIMEOUT, true);
		return false;
	}
}

// emergency function to turn on IR power if it happens not to be on
// note: this will only turn on IR power if we're NOT in low power mode;
// we determined that the risk of
void _enable_ir_pow_if_necessary(void) {
	// note: doesn't take irpow_mutex because that's only used in low power
	// (and this is an emergency function)
	bool is_enabled = get_output(P_IR_PWR_CMD);
	if (!is_enabled && !low_power_active()) { // TODO: really not in low power?
		set_output(true, P_IR_PWR_CMD);
		vTaskDelay(IR_WAKE_DELAY);
	}
}

void verify_regulators_unsafe(void) {
	struct hw_states* states;
	uint16_t low3v6RefBound, high3v6RefBound, low3v6SnsBound, high3v6SnsBound;
	ad7991_ctrlbrd_batch batch;

	// only lock hardware state mutex while needed to act on state,
	// but long enough to ensure the state doesn't change in the middle of checking it
	if (hardware_state_mutex_take())
	{
		read_ad7991_ctrlbrd_unsafe(batch);

		states = get_hw_states();
		low3v6RefBound = states->radio_powered ? B_3V6_REF_ON_LOW : B_3V6_REF_OFF_LOW;
		high3v6RefBound = states->radio_powered ? B_3V6_REF_ON_HIGH : B_3V6_REF_OFF_HIGH;
		low3v6SnsBound = states->radio_powered ? B_3V6_SNS_ON_LOW : B_3V6_SNS_OFF_LOW;
		high3v6SnsBound = states->radio_powered ? B_3V6_SNS_ON_HIGH : B_3V6_SNS_OFF_HIGH;

		hardware_state_mutex_give();
	} else {
		log_error(ELOC_VERIFY_REGS, ECODE_HW_STATE_MUTEX_TIMEOUT, true);
		return;
	}

	// 5V regulator state is technically locked by both i2c_irpow_mutex and processor_adc_mutex
	uint16_t low5vRefBound = states->rail_5v_enabled ? B_5VREF_ON_LOW : B_5VREF_OFF_LOW;
	uint16_t high5vRefBound = states->rail_5v_enabled ? B_5VREF_ON_HIGH : B_5VREF_OFF_HIGH;

	// 3V6_REF is index 0
	log_if_out_of_bounds(batch[0], low3v6RefBound, high3v6RefBound, ELOC_AD7991_CBRD_3V6_REF, true);
	// 3V6_SNS is index 1
	log_if_out_of_bounds(batch[1], low3v6SnsBound, high3v6SnsBound, ELOC_AD7991_CBRD_3V6_SNS, true);
	// 5VREF is index 2
	log_if_out_of_bounds(batch[2], low5vRefBound, high5vRefBound, ELOC_AD7991_CBRD_5V_REF, true);
	// 3V3REF current is index 3
	log_if_out_of_bounds(batch[3], B_3V3_REF_LOW, B_3V3_REF_HIGH, ELOC_AD7991_CBRD_3V3_REF, true);
}

void verify_regulators(void) {
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		verify_regulators_unsafe();
		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_AD7991_BBRD_L2_SNS, ECODE_I2C_MUTEX_TIMEOUT, true);
	}
}

/************************************************************************/
/* SENSOR BATCH READING FUNCTIONS                                       */
/************************************************************************/

void read_ir_object_temps_batch(ir_object_temps_batch batch) {
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		for (int i = 0; i < 6; i ++) {
			uint16_t obj;
			status_code_genare_t sc = MLX90614_read_all_obj(IR_ADDS[i], &obj);
			log_if_error(IR_ELOCS[i], sc, false);
			log_if_out_of_bounds(obj, B_IR_OBJ_LOW, B_IR_OBJ_HIGH, IR_ELOCS[i], false);
			batch[i] = obj;
		}

		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_IR_TOP1, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(batch, 0, sizeof(ir_object_temps_batch));
	}
}

void read_ir_ambient_temps_batch(ir_ambient_temps_batch batch) {
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		for (int i = 0; i < 6; i++) {
			uint16_t amb;
			status_code_genare_t sc = MLX90614_read2ByteValue(IR_ADDS[i], AMBIENT, &amb);
			log_if_error(IR_ELOCS[i], sc, false);
			log_if_out_of_bounds(amb, B_IR_AMB_LOW, B_IR_AMB_HIGH, IR_ELOCS[i], false);
			batch[i] = truncate_16t(amb);
		}

		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_IR_FLASH, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(batch, 0, sizeof(ir_ambient_temps_batch));
	}
}

// TODO: make sure this all looks okay
void read_lion_volts_batch(lion_volts_batch batch) {
	uint16_t val_1_precise;
	uint16_t val_2_precise;

	// locks and releases processor_adc_mutex
	read_lion_volts_precise(&val_1_precise, &val_2_precise);
	batch[0] = truncate_16t(val_1_precise);
	batch[1] = truncate_16t(val_2_precise);
}

void read_lion_volts_precise(uint16_t* val_1, uint16_t* val_2) {
	if (xSemaphoreTake(processor_adc_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		commands_read_adc_mV(val_1, P_AI_L1_REF, ELOC_L1_REF, B_L_VOLT_LOW, B_L_VOLT_HIGH, true);
		commands_read_adc_mV(val_2, P_AI_L2_REF, ELOC_L2_REF, B_L_VOLT_LOW, B_L_VOLT_HIGH, true);

		xSemaphoreGive(processor_adc_mutex);
	} else {
		log_error(ELOC_L1_REF, ECODE_PROC_ADC_MUTEX_TIMEOUT, true);
		memset(val_1, 0, sizeof(uint16_t));
		memset(val_2, 0, sizeof(uint16_t));
	}
}

void read_ad7991_batbrd(lion_current_batch batch1, panelref_lref_batch batch2) {
	uint16_t results[4];
	status_code_genare_t sc;
	uint16_t low_limit, high_limit;

	// (we need to lock i2c_irpow_mutex before hardware_state_mutex to avoid deadlock)
	//if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS)) {
	if (true) {
		_enable_ir_pow_if_necessary();
		// only lock hardware state mutex while needed to act on state,
		// but long enough to ensure the state doesn't change in the middle of checking it
		//if (hardware_state_mutex_take()) {
		if (true) {
			sc = AD7991_read_all_mV(results, AD7991_BATBRD);
			log_if_error(ELOC_AD7991_BBRD, sc, true);

			struct hw_states* states = get_hw_states();
			if (states->antenna_deploying || states->radio_transmitting) {
				low_limit = B_L_CUR_REG_LOW;
				high_limit = B_L_CUR_REG_HIGH;
			} else {
				low_limit = B_L_CUR_HIGH_LOW;
				high_limit = B_L_CUR_HIGH_HIGH;
			}

			//hardware_state_mutex_give();
		} else {
			log_error(ELOC_AD7991_BBRD, ECODE_HW_STATE_MUTEX_TIMEOUT, true);
			memset(batch1, 0, sizeof(lion_current_batch));
			memset(batch2, 0, sizeof(panelref_lref_batch));
			xSemaphoreGive(i2c_mutex); // outer mutex
			return;
		}
		//xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_AD7991_BBRD, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(batch1, 0, sizeof(lion_current_batch));
		memset(batch2, 0, sizeof(panelref_lref_batch));
		return;
	}

	// results[0] = L2_SNS
	batch1[1] = truncate_16t(results[0]);
	log_if_out_of_bounds(results[0], low_limit, high_limit, ELOC_AD7991_BBRD_L2_SNS, true);
	// results[1] = L1_SNS
	batch1[0] = truncate_16t(results[1]);
	log_if_out_of_bounds(results[1], low_limit, high_limit, ELOC_AD7991_BBRD_L1_SNS, true);

	// results[2] = L_REF
	batch2[1] = truncate_16t(results[2]);
	log_if_out_of_bounds(results[2], B_LREF_LOW, B_LREF_HIGH, ELOC_AD7991_BBRD_L1_SNS, true);
	// results[3] = PANELREF
	batch2[0] = truncate_16t(results[3]);
	log_if_out_of_bounds(results[3], B_PANELREF_LOW, B_PANELREF_HIGH, ELOC_AD7991_BBRD_L2_SNS, true);
}

// TODO: why is ad7991_ctrlbrd required outside here??
// unsafe version required for verify_regulators_unsafe
void read_ad7991_ctrlbrd_unsafe(ad7991_ctrlbrd_batch batch) {
	status_code_genare_t sc = AD7991_read_all_mV(batch, AD7991_CTRLBRD);
	log_if_error(ELOC_AD7991_CBRD, sc, false);
}

void read_ad7991_ctrlbrd(ad7991_ctrlbrd_batch batch) {
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		read_ad7991_ctrlbrd_unsafe(batch);
		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_AD7991_CBRD, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(batch, 0, sizeof(ad7991_ctrlbrd_batch));
	}
}

/************************************************************************/
/* FLASH-RELATED FUNCTIONS - include unsafe and safe version            */
/************************************************************************/

void en_and_read_led_temps_batch(led_temps_batch batch) {
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		if (xSemaphoreTake(processor_adc_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
		{
			_set_5v_enable(true);
			vTaskDelay(EN_5V_POWER_UP_DELAY_MS / portTICK_PERIOD_MS); // TODO: maybe don't hold the proc_adc_mutex
			verify_regulators_unsafe();
			_read_led_temps_batch_unsafe(batch);
			_set_5v_enable(false);

			xSemaphoreGive(processor_adc_mutex);
		} else {
			log_error(ELOC_TEMP_L_2, ECODE_PROC_ADC_MUTEX_TIMEOUT, true);
			memset(batch, 0, sizeof(led_temps_batch));
		}
		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_TEMP_L_2, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(batch, 0, sizeof(led_temps_batch));
	}
}

// note: only called from flash_task, and with i2c_irpow_mutex held
void _read_led_temps_batch_unsafe(led_temps_batch batch) {
	for (int i = 4; i < 8; i++) {
		uint8_t rs8;
		status_code_genare_t sc = LTC1380_channel_select(TEMP_MULTIPLEXER_I2C, i, &rs8);
		log_if_error(TEMP_ELOCS[i], sc, true);
		commands_read_adc_mV_truncate(&rs8, P_AI_TEMP_OUT, TEMP_ELOCS[i], B_LED_TEMP_LOW, B_LED_TEMP_HIGH, true);
		batch[i - 4] = rs8;
	}
}

// TODO: may be unnecessary
void en_and_read_lifepo_temps_batch(lifepo_bank_temps_batch batch) {
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		if (xSemaphoreTake(processor_adc_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
		{
			_set_5v_enable(true);
			vTaskDelay(EN_5V_POWER_UP_DELAY_MS / portTICK_PERIOD_MS); // TODO: maybe don't hold the proc_adc_mutex
			verify_regulators_unsafe();
			_read_lifepo_temps_batch_unsafe(batch);
			_set_5v_enable(false);

			xSemaphoreGive(processor_adc_mutex);
		} else {
			log_error(ELOC_TEMP_LF_1, ECODE_PROC_ADC_MUTEX_TIMEOUT, true);
			memset(batch, 0, sizeof(lifepo_bank_temps_batch));
		}
		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_TEMP_LF_1, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(batch, 0, sizeof(lifepo_bank_temps_batch));
	}
}

void _read_lifepo_temps_batch_unsafe(lifepo_bank_temps_batch batch) {
	for (int i = 0; i < 2; i++) {
		uint8_t rs8;
		status_code_genare_t sc = LTC1380_channel_select(TEMP_MULTIPLEXER_I2C, i, &rs8);
		log_if_error(TEMP_ELOCS[i], sc, true);
		// TODO: define different 'on' range?
		commands_read_adc_mV_truncate(&rs8, P_AI_TEMP_OUT, TEMP_ELOCS[i], B_L_TEMP_LOW, B_L_TEMP_HIGH, true);
		batch[i] = rs8;
	}
}

// TODO: only reason the second argument exists right now is because of system_test
void _read_lifepo_current_batch_unsafe(lifepo_current_batch batch, bool flashing_now) {
	uint low_limit, high_limit;
	if (flashing_now) {
		low_limit = B_LF_CUR_REG_LOW;
		high_limit = B_LF_CUR_REG_HIGH;
	} else {
		low_limit = B_LF_CUR_FLASH_LOW;
		high_limit = B_LF_CUR_FLASH_HIGH;
	}
	commands_read_adc_mV_truncate(&batch[0], P_AI_LFB1SNS, ELOC_LFB1SNS, low_limit, high_limit, true);
	commands_read_adc_mV_truncate(&batch[1], P_AI_LFB1OSNS, ELOC_LFB1OSNS, low_limit, high_limit, true);
	commands_read_adc_mV_truncate(&batch[2], P_AI_LFB2SNS, ELOC_LFB2SNS, low_limit, high_limit, true);
	commands_read_adc_mV_truncate(&batch[3], P_AI_LFB2OSNS, ELOC_LFB2OSNS, low_limit, high_limit, true);
}

void read_lifepo_current_batch(lifepo_current_batch batch, bool flashing_now) {
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		if (xSemaphoreTake(processor_adc_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
		{
			_read_lifepo_current_batch_unsafe(batch, flashing_now);
			
			xSemaphoreGive(processor_adc_mutex);
		} else {
			log_error(ELOC_LFB1SNS, ECODE_PROC_ADC_MUTEX_TIMEOUT, true);
			memset(batch, 0, sizeof(lifepo_current_batch));
		}
		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_LFB1SNS, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(batch, 0, sizeof(lifepo_current_batch));
	}
}


void read_lf_volts_precise_unsafe(uint16_t* val_1, uint16_t* val_2, uint16_t* val_3, uint16_t* val_4) {
	// note: lifepo voltages will not vary enough during flash to warrant a separate bound for them
	commands_read_adc_mV(val_1, P_AI_LF1REF, ELOC_LF1REF, B_LF_VOLT_LOW, B_LF_VOLT_HIGH, true);
	commands_read_adc_mV(val_2, P_AI_LF2REF, ELOC_LF2REF, B_LF_VOLT_LOW, B_LF_VOLT_HIGH, true);
	commands_read_adc_mV(val_3, P_AI_LF3REF, ELOC_LF3REF, B_LF_VOLT_LOW, B_LF_VOLT_HIGH, true);
	commands_read_adc_mV(val_4, P_AI_LF4REF, ELOC_LF4REF, B_LF_VOLT_LOW, B_LF_VOLT_HIGH, true);

	// 	b1 *= 1950;
	// 	b3 *= 1950;
	// 	b0 = (batch[0]*3870) - batch[1];
	// 	b2 = (batch[2]*3870) - batch[3];
}

void read_lf_volts_precise(uint16_t* val_1, uint16_t* val_2, uint16_t* val_3, uint16_t* val_4) {
	if (xSemaphoreTake(processor_adc_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		read_lf_volts_precise_unsafe(val_1, val_2, val_3, val_4);
		xSemaphoreGive(processor_adc_mutex);
	} else {
		log_error(ELOC_L1_REF, ECODE_PROC_ADC_MUTEX_TIMEOUT, true);
		memset(val_1, 0, sizeof(uint16_t));
		memset(val_2, 0, sizeof(uint16_t));
		memset(val_3, 0, sizeof(uint16_t));
		memset(val_4, 0, sizeof(uint16_t));
	}
}

void _read_lifepo_volts_batch_unsafe(lifepo_volts_batch batch) {
	// TODO: make sure this all looks okay
	uint16_t val_1_precise;
	uint16_t val_2_precise;
	uint16_t val_3_precise;
	uint16_t val_4_precise;

	read_lf_volts_precise_unsafe(&val_1_precise, &val_2_precise, &val_3_precise, &val_4_precise);

	batch[0] = truncate_16t(val_1_precise);
	batch[1] = truncate_16t(val_2_precise);
	batch[2] = truncate_16t(val_3_precise);
	batch[3] = truncate_16t(val_4_precise);
}

void read_lifepo_volts_batch(lifepo_volts_batch batch) {
	if (xSemaphoreTake(processor_adc_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_read_lifepo_volts_batch_unsafe(batch);
		xSemaphoreGive(processor_adc_mutex);
	} else {
		log_error(ELOC_LF1REF, ECODE_PROC_ADC_MUTEX_TIMEOUT, true);
		memset(batch, 0, sizeof(lifepo_volts_batch));
	}
}

// TODO: only reason the second argument exists right now is because of system_test
void _read_led_current_batch_unsafe(led_current_batch batch, bool flashing_now) {
	uint low_limit, high_limit;
	if (flashing_now) { // protected by i2c_irpower_mutex (and processor_adc_mutex)
		low_limit = B_LED_CUR_REG_LOW;
		high_limit = B_LED_CUR_REG_HIGH;
	} else {
		low_limit = B_LED_CUR_FLASH_LOW;
		high_limit = B_LED_CUR_FLASH_HIGH;
	}
	commands_read_adc_mV_truncate(&batch[0], P_AI_LED1SNS, ELOC_LED1SNS, low_limit, high_limit, true);
	commands_read_adc_mV_truncate(&batch[1], P_AI_LED2SNS, ELOC_LED2SNS, low_limit, high_limit, true);
	commands_read_adc_mV_truncate(&batch[2], P_AI_LED3SNS, ELOC_LED3SNS, low_limit, high_limit, true);
	commands_read_adc_mV_truncate(&batch[3], P_AI_LED4SNS, ELOC_LED4SNS, low_limit, high_limit, true);
}


void verify_flash_readings(bool flashing_now) {
	// note: if this function happens to context switch into being flashing
	// on this line, and then comes back while flashing, the flash
	// task will have the mutex so we'll wait here until it's done
	// (the passed flash state will be valid even if a flash happens)
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		if (xSemaphoreTake(processor_adc_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
		{
			uint8_t buffer[4]; // size of largest data type

			// read values to nowhere, making them check bounds for errors
			_read_led_temps_batch_unsafe(buffer);
			_read_lifepo_temps_batch_unsafe(buffer);
			_read_lifepo_current_batch_unsafe(buffer, flashing_now);
			_read_lifepo_volts_batch_unsafe(buffer);
			_read_led_current_batch_unsafe(buffer, flashing_now);

			xSemaphoreGive(processor_adc_mutex);
		} else {
			log_error(ELOC_LED1SNS, ECODE_PROC_ADC_MUTEX_TIMEOUT, true);
			// no data passed back that needs to be cleared
		}
		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_LED1SNS, ECODE_I2C_MUTEX_TIMEOUT, true);
		// no data passed back that needs to be cleared
	}
}

/************************************************************************/
/* END OF FLASH-RELATED FUNCTIONS                                       */
/************************************************************************/

void read_pdiode_batch(pdiode_batch* batch) {
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		if (xSemaphoreTake(processor_adc_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
		{
			// TODO: need to output to two bits of a uint16_t
			for (int i = 0; i < 6; i++) {
				uint8_t rs;
				// TODO: LTC1380_channel_select takes a uint8, not 16

				// TODO INFO: it looks like these (uint8_t) readings need to be
				// taken, truncated, and shifted onto the batch (i.e. (x >> 6) << (i*2))

				status_code_genare_t sc = LTC1380_channel_select(PHOTO_MULTIPLEXER_I2C, i, &rs);
				log_if_error(PD_ELOCS[i], sc, false);
				commands_read_adc_mV(&rs, P_AI_PD_OUT, PD_ELOCS[i], B_PD_LOW, B_PD_HIGH, false);
				//batch[i] = truncate_16t(rs);
			}
			xSemaphoreGive(processor_adc_mutex);
		} else {
			log_error(ELOC_PD_FLASH, ECODE_PROC_ADC_MUTEX_TIMEOUT, true);
			memset(batch, 0, sizeof(pdiode_batch));
		}
		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_PD_FLASH, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(batch, 0, sizeof(pdiode_batch));
	}
}

void en_and_read_lion_temps_batch(lion_temps_batch batch) {
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		if (xSemaphoreTake(processor_adc_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
		{
			_set_5v_enable(true);
			vTaskDelay(EN_5V_POWER_UP_DELAY_MS / portTICK_PERIOD_MS); // TODO: maybe don't hold the proc_adc_mutex
			verify_regulators_unsafe();

			for (int i = 2; i < 4; i++) {
				uint8_t rs8;
				status_code_genare_t sc = LTC1380_channel_select(TEMP_MULTIPLEXER_I2C, i, &rs8);
				log_if_error(TEMP_ELOCS[i], sc, true);
				commands_read_adc_mV_truncate(&rs8, P_AI_TEMP_OUT, TEMP_ELOCS[i], B_L_TEMP_LOW, B_L_TEMP_HIGH, true);
				batch[i - 2] = rs8;
			}

			_set_5v_enable(false);
			xSemaphoreGive(processor_adc_mutex);
		} else {
			log_error(ELOC_TEMP_L_1, ECODE_PROC_ADC_MUTEX_TIMEOUT, true);
			memset(batch, 0, sizeof(lion_temps_batch));
		}
		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_TEMP_L_1, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(batch, 0, sizeof(lion_temps_batch));
	}
}

void read_accel_batch(accelerometer_batch accel_batch) {
	int16_t rs[3];
	status_code_genare_t sc;
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		sc = MPU9250_read_acc(rs);
		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_IMU_ACC, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(accel_batch, 0, sizeof(accelerometer_batch));
		return;
	}

	log_if_error(ELOC_IMU_ACC, sc, false);
	for (int i = 0; i < 3; i++) {
		accel_batch[i] = truncate_16t(rs[i]);
	}
}

void _read_gyro_batch_unsafe(gyro_batch gyr_batch) {
	int16_t rs[3];
	status_code_genare_t sc = MPU9250_read_gyro(rs);

	log_if_error(ELOC_IMU_GYRO, sc, false);
	for (int i = 0; i < 3; i++) {
		log_if_out_of_bounds(rs[i], B_GYRO_LOW, B_GYRO_HIGH, ELOC_IMU_GYRO, false);
		gyr_batch[i] = truncate_16t(rs[i]);
	}
}

void read_gyro_batch(gyro_batch gyr_batch) {
	int16_t rs[3];
	status_code_genare_t sc;
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		sc = MPU9250_read_gyro(rs);
		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_IMU_GYRO, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(gyr_batch, 0, sizeof(gyro_batch));
		return;
	}

	log_if_error(ELOC_IMU_GYRO, sc, false);
	for (int i = 0; i < 3; i++) {
		log_if_out_of_bounds(rs[i], B_GYRO_LOW, B_GYRO_HIGH, ELOC_IMU_GYRO, false);
		gyr_batch[i] = truncate_16t(rs[i]);
	}
}

void read_magnetometer_batch(magnetometer_batch batch) {
	int16_t rs[3];
	status_code_genare_t sc;
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		//sc = MPU9250_read_mag(rs);
		sc = HMC5883L_readXYZ(rs);
		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_IMU_MAG, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(batch, 0, sizeof(magnetometer_batch));
		return;
	}

	log_if_error(ELOC_IMU_MAG, sc, false);
	for (int i = 0; i < 3; i++) {
		batch[i] = truncate_16t(rs[i]);
	}
}

void read_bat_charge_dig_sigs_batch(bat_charge_dig_sigs_batch* batch) {
	status_code_genare_t sc;
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS))
	{
		_enable_ir_pow_if_necessary();
		sc = TCA9535_init(batch);
		// zero out the places we're going to overwrite
		// see order in Message Format spreadsheet
		*batch &= 0xF3F0;
		// fill in the new values we want
		*batch |= get_input(P_L1_RUN_CHG);
		*batch |= (get_input(P_L2_RUN_CHG)<<1);
		*batch |= (get_input(P_LF_B1_RUNCHG)<<2);
		*batch |= (get_input(P_LF_B2_RUNCHG)<<3);
		*batch |= (get_input(P_L1_DISG)<<10);
		*batch |= (get_input(P_L2_DISG)<<11);

		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_TCA, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(batch, 0, sizeof(bat_charge_dig_sigs_batch));
	}
	log_if_error(ELOC_TCA, sc, true);
}

void read_proc_temp_batch(proc_temp_batch* batch) {
	if (xSemaphoreTake(i2c_mutex, HARDWARE_MUTEX_WAIT_TIME_TICKS)) {
		_enable_ir_pow_if_necessary();
		uint16_t buf;
		enum status_code sc = MPU9250_read_temp(&buf);
		log_if_error(ELOC_PROC_TEMP, sc, false);
		log_if_out_of_bounds(buf, B_PROC_TEMP_LOW, B_PROC_TEMP_HIGH, ELOC_PROC_TEMP, false);
		*batch = truncate_16t(buf);
		xSemaphoreGive(i2c_mutex);
	} else {
		log_error(ELOC_PROC_TEMP, ECODE_I2C_MUTEX_TIMEOUT, true);
		memset(batch, 0, sizeof(magnetometer_batch));
	}
}

void read_radio_temp_batch(radio_temp_batch* batch) {
	*batch = truncate_16t(get_radio_temp_cached());
}

bool read_field_from_bcds(bat_charge_dig_sigs_batch batch, bcds_conversions_t shift) {
	return (batch >> shift) & 1;
}

// void read_digital_out_batch(digital_out_batch* batch) {
// 	// yet to be defined, mapped to certain events
// }
