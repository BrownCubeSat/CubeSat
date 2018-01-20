/*
 * persistent_storage.c
 *
 * Created: 12/3/2017 23:42:50
 *  Author: mcken
 */ 

#include "persistent_storage.h"

/* mutex for locking the cache on reads / writes - ensures cache is always synced from reader's perspective */
StaticSemaphore_t _cache_mutex_d;
SemaphoreHandle_t cache_mutex;

/* SPI master and slave handles */
struct spi_module spi_master_instance;
struct spi_slave_inst mram1_slave;
struct spi_slave_inst mram2_slave;

void write_state_to_storage_unsafe(void);
uint32_t get_current_timestamp_safety(bool safe);

/************************************************************************/
/* memory interface / init functions									*/
/************************************************************************/ 

void init_persistent_storage(void) {
	cache_mutex = xSemaphoreCreateMutexStatic(&_cache_mutex_d);
	mram_initialize_master(&spi_master_instance, MRAM_SPI_BAUD);
	mram_initialize_slave(&mram1_slave, P_MRAM1_CS);
	mram_initialize_slave(&mram2_slave, P_MRAM2_CS);
}

// wrapper for reading from MRAM; error checking
bool storage_read_bytes(int mram_num, uint8_t *data, int num_bytes, uint16_t address) {
	if (mram_num == 1) {
		return log_if_error(ELOC_MRAM_READ, 
			mram_read_bytes(&spi_master_instance, &mram1_slave, data, num_bytes, address),
			true); // priority
	} else if (mram_num == 2) {
		return log_if_error(ELOC_MRAM_READ,
			mram_read_bytes(&spi_master_instance, &mram1_slave, data, num_bytes, address),
			true); // priority
	} else {
		configASSERT(false);
		return false;
	}
}

// wrapper for writing to MRAM; error checking
bool storage_write_bytes(int mram_num, uint8_t *data, int num_bytes, uint16_t address) {
	if (mram_num == 1) {
		return log_if_error(ELOC_MRAM_WRITE,
			mram_write_bytes(&spi_master_instance, &mram1_slave, data, num_bytes, address),
			true); // priority
	} else if (mram_num == 2) {
		return log_if_error(ELOC_MRAM_WRITE,
			mram_write_bytes(&spi_master_instance, &mram1_slave, data, num_bytes, address),
			true); // priority
	} else {
		configASSERT(false);
		return false;
	}
}

/* read state from storage into cache */
void read_state_from_storage(void) {
	xSemaphoreTake(cache_mutex, CACHE_MUTEX_WAIT_TIME_TICKS);
	
	#ifdef XPLAINED
		// defaults when no MRAM available
		cached_state.secs_since_launch = 0;
		cached_state.sat_state = INITIAL; // signifies initial boot
		cached_state.reboot_count = 0;
		cached_state.sat_event_history;
	#else
		storage_read_bytes(1, (uint8_t*) &cached_state.secs_since_launch,	4,		STORAGE_SECS_SINCE_LAUNCH_ADDR);
		storage_read_bytes(1, &cached_state.reboot_count,					1,		STORAGE_REBOOT_CNT_ADDR);
		storage_read_bytes(1, (uint8_t*) &cached_state.sat_state,			1,		STORAGE_SAT_STATE_ADDR);
		storage_read_bytes(1, (uint8_t*) &cached_state.sat_event_history,	1,		STORAGE_SAT_EVENT_HIST_ADDR);
	#endif
	
	xSemaphoreGive(cache_mutex);
}

void increment_reboot_count(void) {
	//xSemaphoreTake(cache_mutex, CACHE_MUTEX_WAIT_TIME_TICKS); TODO
	cached_state.reboot_count++;
	write_state_to_storage_unsafe(); // have the mutex so is safe
	//xSemaphoreGive(cache_mutex);
}

// deep comparison of sturcts because thier bit organization may differ
bool compare_sat_event_history(satellite_history_batch* history1, satellite_history_batch* history2) {
	bool result;
	result = result && history1->antenna_deployed == history2->antenna_deployed;
	result = result && history1->first_flash == history2->first_flash;
	result = result && history1->lifepo_b1_charged == history2->lifepo_b1_charged;
	result = result && history1->lifepo_b2_charged == history2->lifepo_b2_charged;
	result = result && history1->lion_1_charged == history2->lion_1_charged;
	result = result && history1->lion_2_charged == history2->lion_2_charged;
	return result;
}

/* must be called with cache mutex locked to be accurate, not throw errors, etc. 
 (allows us not to need recursive mutexes when called in this file) */
void write_state_to_storage_unsafe(void) {
	// keep track of the old timestamp value in case the write fails and we have to reset
	uint32_t prev_cached_secs_since_launch = cached_state.secs_since_launch;
	cached_state.secs_since_launch = get_current_timestamp_safety(false); // unsafe, we have the mutex
	cached_state.sat_state = get_sat_state();
	// reboot count is only incremented on startup and is written through cache
	// sat_event_history is written through when changed
	
	// set write time right before writing
	uint32_t prev_last_data_write_ms = last_data_write_ms;
	last_data_write_ms = xTaskGetTickCount() / portTICK_PERIOD_MS;
	
	storage_write_bytes(1, (uint8_t*) &cached_state.secs_since_launch,	4,		STORAGE_SECS_SINCE_LAUNCH_ADDR);
	storage_write_bytes(1, (uint8_t*) &cached_state.reboot_count,		1,		STORAGE_REBOOT_CNT_ADDR);
	storage_write_bytes(1, (uint8_t*) &cached_state.sat_state,			1,		STORAGE_SAT_STATE_ADDR);
	storage_write_bytes(1, (uint8_t*) &cached_state.sat_event_history,	1,		STORAGE_SAT_EVENT_HIST_ADDR);
	// NOTE: we don't write out the bootloader or program memory hash TODO: do we REALLY not want to write it out?
	
	// read it right back to confirm validity
	uint32_t temp_secs_since_launch;
	uint8_t temp_reboot_count, temp_sat_state;
	satellite_history_batch temp_sat_event_history; // fits in one
	
	storage_read_bytes(1, (uint8_t*) &temp_secs_since_launch,	4,		STORAGE_SECS_SINCE_LAUNCH_ADDR);
	storage_read_bytes(1, &temp_reboot_count,					1,		STORAGE_REBOOT_CNT_ADDR);
	storage_read_bytes(1, &temp_sat_state,						1,		STORAGE_SAT_STATE_ADDR);
	storage_read_bytes(1, (uint8_t*) &temp_sat_event_history,	1,		STORAGE_SAT_EVENT_HIST_ADDR);
	
	// log error if the stored data was not consistent with what was just written
	// note we have the mutex so no one should be able to write to these
	// while we were reading / are comparing them
	if (temp_secs_since_launch != cached_state.secs_since_launch || 
		temp_reboot_count != cached_state.reboot_count ||
		temp_sat_state != cached_state.sat_state || 
		!compare_sat_event_history(&temp_sat_event_history, &cached_state.sat_event_history)) {
	
		log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_INCONSISTENT_DATA, true);
		
		// in particular, if it was the secs_since_launch that was inconsistent,
		// go off the old value in the MRAM
		if (temp_secs_since_launch != cached_state.secs_since_launch &&
		temp_secs_since_launch < cached_state.secs_since_launch) {
			last_data_write_ms = prev_last_data_write_ms;
			cached_state.secs_since_launch = prev_cached_secs_since_launch;
		}
	}
}

/* updates state to current satellite state (any that isn't written through the cache)
   and then writes to storage */
void write_state_to_storage(void) {
	//xSemaphoreTake(cache_mutex, CACHE_MUTEX_WAIT_TIME_TICKS); TODO
	write_state_to_storage_unsafe();
	//xSemaphoreGive(cache_mutex);
}

/* Updates the sat_event_history if the given value is true, but ONLY 
   sets them to TRUE, not to FALSE; if the passed in value is FALSE, 
   the original value (TRUE or FALSE) is retained. */
void update_sat_event_history(uint8_t antenna_deployed,
								uint8_t lion_1_charged,
								uint8_t lion_2_charged,
								uint8_t lifepo_b1_charged,
								uint8_t lifepo_b2_charged,
								uint8_t first_flash) {
										
	//xSemaphoreTake(cache_mutex, CACHE_MUTEX_WAIT_TIME_TICKS); TODO
	
	if (antenna_deployed)
		cached_state.sat_event_history.antenna_deployed = antenna_deployed;
	if (lion_1_charged)
		cached_state.sat_event_history.lion_1_charged = lion_1_charged;
	if (lion_2_charged)
		cached_state.sat_event_history.lion_2_charged = lion_2_charged;
	if (lifepo_b1_charged)
		cached_state.sat_event_history.lifepo_b1_charged = lifepo_b1_charged;
	if (lifepo_b2_charged)
		cached_state.sat_event_history.lifepo_b2_charged = lifepo_b2_charged;
	if (first_flash)
		cached_state.sat_event_history.first_flash = first_flash;
	
	write_state_to_storage_unsafe(); // we already have the mutex, so its safe
	
	//xSemaphoreGive(cache_mutex);
}

/************************************************************************/
/* helper functions using cached state		                            */
/************************************************************************/

/*
 * Current timestamp in seconds since boot, with an accuracy of +/- the
 * data write frequency (a reboot could happen at any point in that period
 * due to a watchdog reset). Segment since reboot is accurate to ms.
 */
uint32_t get_current_timestamp(void) {
	return get_current_timestamp_safety(true);
}
uint32_t get_current_timestamp_safety(bool wait_on_write) {
	return ((xTaskGetTickCount() / portTICK_PERIOD_MS - last_data_write_ms) / 1000) 
		 + cache_get_secs_since_launch(wait_on_write);
}

/* Current timestamp in ms since boot, with the above described (low) accuracy */
uint64_t get_current_timestamp_ms(void) {
	return (xTaskGetTickCount() / portTICK_PERIOD_MS) - last_data_write_ms 
			+ (1000 * cache_get_secs_since_launch(true));
}

/* returns truncated number or orbits since first boot */
uint16_t get_orbits_since_launch(void) {
	return get_current_timestamp() / ORBITAL_PERIOD_S;
}

/** 
 * Returns whether we're currently at or above 
 * (*prev_orbit_fraction / orbit_fraction_denominator) percent through an orbit,
 * where prev_orbit_fraction is the last known orbit fraction (set by this function)
 * and 1 / orbit_fraction_denominator is a fraction ("bucket") to divide an orbit by such that
 * this function will return true after each such fraction of orbital time passes.
 * This function is designed specifically to be used to time actions according
 * to fractions of the current orbit, and ensures this function will return true
 * orbit_fraction_denominator times during an orbit as long as it is called at least 
 * that many times during the orbit.
 *
 * Test cases (with implicit argument of current orbit percentage):
 *		at_orbit_fraction(0, 2, .1) = false
 *		at_orbit_fraction(0, 2, .1) = false
 */
bool at_orbit_fraction(uint8_t* prev_orbit_fraction, uint8_t orbit_fraction_denominator) {
	#ifdef TESTING_SPEEDUP
		return true;
	#else 
		// first, we scale up by the denominator to bring our integer precision up to the 
		// fractional (bucket) size. Thus, we will truncate all bits that determine how
		// far we are inside a fractional bucket, and it will give us only the current one
		// we're in
		// TODO: Will the calculations stay in the 32 bit registers? I.e. will they overflow?
		// (use 64 bit for now to be safe)
		uint64_t cur_orbit_fraction = (get_current_timestamp() * orbit_fraction_denominator) / 
										(ORBITAL_PERIOD_S * orbit_fraction_denominator);
									
		// strictly not equal to (really greater than) because we only want 
		// this to return true on a CHANGE, 
		// i.e. when the fraction moves from one "bucket" or 
		// fraction component to the next we set prev_orbit_fraction 
		// so that we wait the fractional amount before returning true again
		if (cur_orbit_fraction != *prev_orbit_fraction) {
			*prev_orbit_fraction = cur_orbit_fraction;
			return true;
		}
		return false;
	#endif
}

/************************************************************************/
// functions to get components of cached state   
// NOTE: use of mutexes here is not to prevent race conditions, etc., but
// to ensure the final read value is the most recent value and matches
// what is in MRAM, etc.	                   
// The wait_on_write parameter can be used to configure these mutexes, 
// which will essentially halt any reads if we're in the process
// of writing to the MRAM
/************************************************************************/

uint32_t cache_get_secs_since_launch(bool wait_on_write) {
	uint32_t secs_since_launch;
	//if (wait_on_write) xSemaphoreTake(cache_mutex, CACHE_MUTEX_WAIT_TIME_TICKS); TODO
	secs_since_launch = cached_state.secs_since_launch;
	//if (wait_on_write) xSemaphoreGive(cache_mutex);
	return secs_since_launch;
}

uint8_t cache_get_reboot_count(bool wait_on_write) {
	uint8_t reboot_count;
	//if (wait_on_write) xSemaphoreTake(cache_mutex, CACHE_MUTEX_WAIT_TIME_TICKS); TODO
	reboot_count = cached_state.reboot_count;
	//if (wait_on_write) xSemaphoreGive(cache_mutex);
	return reboot_count;
}

/* returns satellite state at last reboot */
sat_state_t cache_get_sat_state(bool wait_on_write) {
	sat_state_t sat_state;
	//if (wait_on_write) xSemaphoreTake(cache_mutex, CACHE_MUTEX_WAIT_TIME_TICKS); TODO
	sat_state = cached_state.sat_state;
	//if (wait_on_write) xSemaphoreGive(cache_mutex);
	return sat_state;
}

satellite_history_batch* cache_get_sat_event_history(bool wait_on_write) {
	satellite_history_batch* sat_event_history;
	//xSemaphoreTake(cache_mutex, CACHE_MUTEX_WAIT_TIME_TICKS); TODO
	sat_event_history = &(cached_state.sat_event_history);
	//xSemaphoreGive(cache_mutex);
	return sat_event_history;
}

/************************************************************************/
/* functions which require reading from MRAM (bypass cache)				*/
/************************************************************************/
void populate_error_stacks(equistack* priority_errors, equistack* normal_errors) {
	//xSemaphoreTake(cache_mutex, CACHE_MUTEX_WAIT_TIME_TICKS); TODO
	
	// read in errors from MRAM
	uint8_t num_stored_priority_errors;
	uint8_t num_stored_normal_errors;
	sat_error_t priority_error_buf[PRIORITY_ERROR_STACK_MAX];
	sat_error_t normal_error_buf[NORMAL_ERROR_STACK_MAX];
	storage_read_bytes(1, &num_stored_priority_errors,	1, STORAGE_PRIORITY_ERR_NUM_ADDR);
	storage_read_bytes(1, &num_stored_normal_errors,	1, STORAGE_NORMAL_ERR_NUM_ADDR);
	storage_read_bytes(1, (uint8_t*) priority_error_buf, 
		PRIORITY_ERROR_STACK_MAX * sizeof(sat_error_t), STORAGE_PRIORITY_LIST_ADDR);
	storage_read_bytes(1, (uint8_t*) normal_error_buf,
		NORMAL_ERROR_STACK_MAX * sizeof(sat_error_t), STORAGE_NORMAL_LIST_ADDR);

	// read all errors that we have stored in MRAM in
	for (int i = 0; i < num_stored_priority_errors; i++) {
		equistack_Push(priority_errors, &(priority_error_buf[i]));
	}
	for (int i = 0; i < num_stored_normal_errors; i++) {
		equistack_Push(normal_errors, &(normal_error_buf[i]));
	}
	
	//xSemaphoreGive(cache_mutex);
}
