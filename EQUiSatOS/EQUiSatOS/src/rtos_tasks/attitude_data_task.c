/*
 * antenna_data_task.c
 *
 * Created: 9/21/2017 20:44:50
 *  Author: mcken
 */ 

#include "rtos_tasks.h"

void attitude_data_task(void *pvParameters)
{
	// delay to offset task relative to others, then start
	vTaskDelay(ATTITUDE_DATA_TASK_FREQ_OFFSET);
	TickType_t prev_wake_time = xTaskGetTickCount();
	
	attitude_data_t *current_struct = (attitude_data_t*) equistack_Initial_Stage(&attitude_readings_equistack);
	
	// variable for timing data reads (which may include task suspensions)
	TickType_t time_before_data_read;
	
	// variable for keeping track of our current progress through an orbit
	// (= numerator of (x / ATTITUDE_DATA_LOGS_PER_ORBIT) of an orbit)
	// we set this to the max because we want it to think we've wrapped around
	// an orbit on boot (log immediately)
	uint8_t prev_orbit_fraction = ATTITUDE_DATA_LOGS_PER_ORBIT; 
	
	init_task_state(ATTITUDE_DATA_TASK); // suspend or run on boot
	
	for ( ;; )
	{
		vTaskDelayUntil( &prev_wake_time, ATTITUDE_DATA_TASK_FREQ / portTICK_PERIOD_MS);
		
		// report to watchdog
		report_task_running(ATTITUDE_DATA_TASK);
		
		// set start timestamp
		current_struct->timestamp = get_current_timestamp();

		// time the data reading to make sure it doesn't exceed a maximum
		time_before_data_read = xTaskGetTickCount() / portTICK_PERIOD_MS;
		
		// read all sensors first
		read_ir_object_temps_batch(	current_struct->ir_obj_temps_data);
		read_pdiode_batch(			&(current_struct->pdiode_data));
		read_accel_batch(			current_struct->accelerometer_data	[0]);
		read_gyro_batch(			current_struct->gyro_data);
		read_magnetometer_batch(	current_struct->magnetometer_data	[0]);
		
		// delay a bit and take a second batch of readings to allow rate measurements
		// TODO: may want to error if this is off (by a tighter bound than ATTITUDE_DATA_MAX_READ_TIME)
		vTaskDelay(FLASH_ACTIVATE_TASK_FREQ / portTICK_PERIOD_MS);
		read_accel_batch(			current_struct->accelerometer_data	[1]);
		read_magnetometer_batch(	current_struct->magnetometer_data	[1]);
		
		// if we were suspended in some period between start of this packet and here, DON'T add it
		// and go on to rewrite the current one
		TickType_t data_read_time = (xTaskGetTickCount() / portTICK_PERIOD_MS) - time_before_data_read;
		if (data_read_time <= ATTITUDE_DATA_MAX_READ_TIME) {
			if (passed_orbit_fraction(&prev_orbit_fraction, ATTITUDE_DATA_LOGS_PER_ORBIT)) {
				// validate previous stored value in stack, getting back the next staged address we can start adding to
				current_struct = (attitude_data_t*) equistack_Stage(&attitude_readings_equistack);
			}
		} else {
			// log error if the data read took too long
			log_error(ELOC_ATTITUDE_DATA, ECODE_EXCESSIVE_SUSPENSION, false);
		}
	}
	// delete this task if it ever breaks out
	vTaskDelete( NULL );
}