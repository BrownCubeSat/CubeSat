/*
 * persistent_storage.c
 *
 * Created: 12/3/2017 23:42:50
 *  Author: mcken
 */

#include "persistent_storage.h"

// super simple mutex used to sync the two operations required
// to modify the cached data responsible for computing get_current_timestamp()
// (true if fields are being modified, false otherwise)
// note: this only works because it's only being written by one thread at a time
// (except in rare cases), and because read/stores are atomic in single-processor ARM
bool cache_time_fields_minimutex = false;

/* SPI master and slave handles */
struct spi_module spi_master_instance;
struct spi_slave_inst mram1_slave;
struct spi_slave_inst mram2_slave;

void write_state_to_storage(void);
void write_state_to_storage_safety(bool safe);
void cached_state_sync_redundancy(void);
uint32_t get_current_timestamp_safety(bool safe);
bool compare_sat_event_history(satellite_history_batch* history1, satellite_history_batch* history2);
bool compare_persistent_charging_data(persistent_charging_data_t* data1, persistent_charging_data_t* data2);

/************************************************************************/
/* memory interface / init functions									*/
/************************************************************************/

void init_persistent_storage(void) {
	memset(&cached_state, 0, sizeof(cached_state)); // to avoid undefined behavior if someone accidentally uses it

	mram_spi_cache_mutex = xSemaphoreCreateMutexStatic(&_mram_spi_cache_mutex_d);

	mram_initialize_master(&spi_master_instance, MRAM_SPI_BAUD);
	mram_initialize_slave(&mram1_slave, P_MRAM1_CS);
	mram_initialize_slave(&mram2_slave, P_MRAM2_CS);
}

// Returns the length of the longest subsequence of the same byte in data,
// stopping at size "length." Returns 1 if no character matches the first,
// and 0 if len was 0.
// NOTE: this is use in bootloader too so if there's a bug fix it there too!!
size_t longest_same_seq_len(uint8_t* data, size_t len) {
	uint8_t same_byte = data[0];
	size_t longest_seq_len = 0;
	size_t cur_seq_len = 0;
	// always look at the first byte as an easy way of returning 0 by default
	for (size_t i = 0; i < len; i++) {
		if (data[i] == same_byte) {
			cur_seq_len++;
		} else {
			same_byte = data[i];
			cur_seq_len = 1;
		}
		
		if (cur_seq_len > longest_seq_len) {
			longest_seq_len = cur_seq_len;
		}
	}
	return longest_seq_len;
}

// wrapper for reading a field from MRAM
// handles RAIDing, error checking and correction, and field duplication
// returns whether accurate data should be expected in data (whether error checks worked out)
bool storage_read_field_unsafe(uint8_t *data, int num_bytes, uint32_t address) {
	// data is used as mram1_data1
	static uint8_t mram1_data2[STORAGE_MAX_FIELD_SIZE];
	static uint8_t mram2_data1[STORAGE_MAX_FIELD_SIZE];
	static uint8_t mram2_data2[STORAGE_MAX_FIELD_SIZE];
	// TODO: ^^^^^ can we find a way to use just two of these 
	
	// read both duplicates from MRAM1
	bool success_mram1_data1 = !log_if_error(ELOC_MRAM1_READ,
		mram_read_bytes(&spi_master_instance, &mram1_slave, data, num_bytes, address),
		true); // priority
	bool success_mram1_data2 = !log_if_error(ELOC_MRAM1_READ,
		mram_read_bytes(&spi_master_instance, &mram1_slave, mram1_data2, num_bytes, 
		address + num_bytes), true); // priority

	bool success_mram2_data1 = !log_if_error(ELOC_MRAM2_READ,
		mram_read_bytes(&spi_master_instance, &mram2_slave, mram2_data1, num_bytes, address),
		true); // priority
	bool success_mram2_data2 = !log_if_error(ELOC_MRAM2_READ,
		mram_read_bytes(&spi_master_instance, &mram2_slave, mram2_data2, num_bytes, 
		address + num_bytes), true); // priority
		
	// helpful constants
	bool success_mram1 = success_mram1_data1 && success_mram1_data2;
	bool success_mram2 = success_mram2_data1 && success_mram2_data2;
	bool mram1_data_matches = memcmp(data,		  mram1_data2, num_bytes) == 0;
	bool mram2_data_matches = memcmp(mram2_data1, mram2_data2, num_bytes) == 0;
	
	/* if both sets of data match, do an additional comparison between them to determine our confidence */
	if (mram1_data_matches && mram2_data_matches) {
		bool mrams_match = memcmp(data, mram2_data1, num_bytes) == 0;
		if (mrams_match) {
			// return data in data
			return true;
		} else {
			log_error(ELOC_MRAM_READ, ECODE_INCONSISTENT_DATA, true);
			// if one has failed, definitely take the other one
			if (!success_mram2) {
				// return data in data
				return false;
			}
			if (!success_mram1) {
				memcpy(data, mram2_data1, num_bytes);
				return false;
			}
			
			// if both are okay but still didn't match, take the one with the shortest sequence of shared bytes
			// (if one has failed, it's likely all 0xFF's or 0x00's and therefore "matches")
			// note that if the data is 1 byte long this essentially defaults to mram1
			size_t mram1_same_seq_len = longest_same_seq_len(data,		num_bytes);
			size_t mram2_same_seq_len = longest_same_seq_len(mram2_data1, num_bytes);
			
			// do some additional error logging of long same sequences
			if (num_bytes > 2 && mram1_same_seq_len == num_bytes) {
				log_error(ELOC_MRAM1_READ, ECODE_ALL_SAME_VAL, true);
			}
			if (num_bytes > 2 && mram2_same_seq_len == num_bytes) {
				log_error(ELOC_MRAM2_READ, ECODE_ALL_SAME_VAL, true);
			}
			
			if (mram1_same_seq_len <= mram2_same_seq_len) { // mram1 has a shorter stream
				// return data in data
				return false;
			} else {
				memcpy(data, mram2_data2, num_bytes);
				return false;
			}
		}
	}

	/* if only one of the two sets of data matches, return the other one (but log error) 
	   (we could that the status codes from that MRAM were okay, because 0xff's from
	   a bad MRAM or SPI driver would match, but that would not be detected by the status
	   codes from the driver in most cases, so it's more likely that MRAM2 failed
	   and MRAM1 happened to have an insignificant bad status code (the only code that
	   can really occur with the SPI driver that might mean something is an overflow--
	   and the MRAM's wouldn't likely match if only one of them overflowed)) */
	if (mram1_data_matches && !mram2_data_matches) {
		log_error(ELOC_MRAM2_READ, ECODE_INCONSISTENT_DATA, true);
		return success_mram1;
	}
	if (!mram1_data_matches && mram2_data_matches) {
		log_error(ELOC_MRAM1_READ, ECODE_INCONSISTENT_DATA, true);
		// need to copy over (which data # shouldn't matter)
		memcpy(data, mram2_data1, num_bytes);
		return success_mram2;
	}
	
	/* if neither set of data matches, check for a cross-match and use it if available 
	 (also require that status codes are good for reason above) */
	/*
	1_1 _ 2_1
	    X  
	1_2 _ 2_2
	*/
	if (!mram1_data_matches && !mram2_data_matches) {
		log_error(ELOC_MRAM1_READ, ECODE_INCONSISTENT_DATA, true);
		log_error(ELOC_MRAM2_READ, ECODE_INCONSISTENT_DATA, true);
		
		if (memcmp(data, mram2_data1, num_bytes) == 0 
			&& success_mram1_data1 && success_mram2_data1) {
			// return data in data
			return true;
		}
		else if (memcmp(data, mram2_data2, num_bytes) == 0
			&& success_mram1_data1 && success_mram2_data2) {
			// return data in data
			return true;
		}
		else if (memcmp(mram1_data2, mram2_data1, num_bytes) == 0
			&& success_mram1_data2 && success_mram2_data1) {
			memcpy(data, mram1_data2, num_bytes);
			return true;
		}
		else if (memcmp(mram1_data2, mram2_data2, num_bytes) == 0
			&& success_mram1_data2 && success_mram2_data2) {
			memcpy(data, mram1_data2, num_bytes);
			return true;
		} else {
			log_error(ELOC_MRAM1_READ, ECODE_BAD_DATA, true); // just really bad 
			// we could try and compare data without caring about status codes, 
			// but this case is so unlikely and hard to recover from we determined
			// it's not worth it 
			return false;
		}
	}
}

// wrapper for writing a field to MRAM
// handles RAIDing, error checking, and field duplication
bool storage_write_field_unsafe(uint8_t *data, int num_bytes, uint32_t address) {
	bool success1 = !log_if_error(ELOC_MRAM1_WRITE,
		mram_write_bytes(&spi_master_instance, &mram1_slave, data, num_bytes, address),
			true); // priority
	bool success2 = !log_if_error(ELOC_MRAM1_WRITE,
		mram_write_bytes(&spi_master_instance, &mram1_slave, data, num_bytes, 
			address + num_bytes), true); // priority	
	
	bool success3 = !log_if_error(ELOC_MRAM2_WRITE,
		mram_write_bytes(&spi_master_instance, &mram2_slave, data, num_bytes, address),
			true); // priority
	return success1 && success2 && success3 && !log_if_error(ELOC_MRAM2_WRITE,
		mram_write_bytes(&spi_master_instance, &mram2_slave, data, num_bytes, 
			address + num_bytes), true); // priority
}

/* read state from storage into cache */
void read_state_from_storage(void) {
	if (xSemaphoreTake(mram_spi_cache_mutex, MRAM_SPI_MUTEX_WAIT_TIME_TICKS))
	{
		#ifdef XPLAINED
			// defaults when no MRAM available
			cached_state.secs_since_launch = 0;
			cached_state.sat_state = INITIAL; // signifies initial boot
			cached_state.reboot_count = 0;
			memset(cached_state.sat_event_history, 0, sizeof(satellite_history_batch))
			cached_state.prog_mem_rewritten = false;
			cached_state.radio_revive_timestamp = 0;
			cached_state.persistent_charging_data.li_caused_reboot = -1;
		#else
			storage_read_field_unsafe((uint8_t*) &cached_state.secs_since_launch,	4,		STORAGE_SECS_SINCE_LAUNCH_ADDR);
			storage_read_field_unsafe(&cached_state.reboot_count,					1,		STORAGE_REBOOT_CNT_ADDR);
			storage_read_field_unsafe((uint8_t*) &cached_state.sat_state,			1,		STORAGE_SAT_STATE_ADDR);
			storage_read_field_unsafe((uint8_t*) &cached_state.sat_event_history,	1,		STORAGE_SAT_EVENT_HIST_ADDR);
			storage_read_field_unsafe((uint8_t*) &cached_state.prog_mem_rewritten,	1,		STORAGE_PROG_MEM_REWRITTEN_ADDR);
			storage_read_field_unsafe((uint8_t*) &cached_state.radio_revive_timestamp,	4,	STORAGE_RADIO_REVIVE_TIMESTAMP_ADDR);
			storage_read_field_unsafe((uint8_t*) &cached_state.persistent_charging_data,1,	STORAGE_PERSISTENT_CHARGING_DATA_ADDR);
		#endif
		
		xSemaphoreGive(mram_spi_cache_mutex);
	} else {
		log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_SPI_MUTEX_TIMEOUT, true);
	}
	// write to redundancy for (probably) the first time
	cached_state_sync_redundancy();
}

// writes error stack data to mram, and confirms it was written correctly if told to
bool storage_write_check_errors_unsafe(equistack* stack, bool confirm) {
	// (move these (big) buffers off stack)
	static sat_error_t error_buf[ERROR_STACK_MAX]; 
	static sat_error_t temp_error_buf[ERROR_STACK_MAX];
	
	uint8_t num_errors = stack->cur_size;
	if (num_errors >= ERROR_STACK_MAX) {
		// watch for radiation bit flips, because this could overwrite part of the MRAM
		log_error(ELOC_MRAM_WRITE, ECODE_OUT_OF_BOUNDS, true);
		num_errors = ERROR_STACK_MAX;
	}
	
	bool got_mutex = true;
	if (!xSemaphoreTake(stack->mutex, (TickType_t) EQUISTACK_MUTEX_WAIT_TIME_TICKS)) {
		// log error, but continue on because we're just reading
		log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_EQUISTACK_MUTEX_TIMEOUT, true);
		got_mutex = false;
	}
	{
		// populate buffer with current errors (thread-safely)
		for (int i = 0; i < num_errors; i++) {
			sat_error_t* err = (sat_error_t*) equistack_Get_Unsafe(stack, i);
			error_buf[i] = *err;
		}
	}
	if (got_mutex) xSemaphoreGive(stack->mutex);
	
	// write size and error data to storage
	storage_write_field_unsafe(&num_errors,	1, STORAGE_ERR_NUM_ADDR);
	if (num_errors > 0) {
		storage_write_field_unsafe((uint8_t*) error_buf,
			num_errors * sizeof(sat_error_t), STORAGE_ERR_LIST_ADDR);
	}
	
	if (confirm) {
		// check if stored # of errors matches
		uint8_t temp_num_errors;
		storage_read_field_unsafe(&temp_num_errors,	1, STORAGE_ERR_NUM_ADDR);
		if (temp_num_errors != num_errors) {
			return false;
		}
	
		// check if actual stored errors match (if necessary)
		if (num_errors > 0) {
			storage_read_field_unsafe((uint8_t*) temp_error_buf,
				num_errors * sizeof(sat_error_t), STORAGE_ERR_LIST_ADDR);
			if (memcmp(error_buf, temp_error_buf, num_errors * sizeof(sat_error_t)) != 0) {
				return false;
			}
		}
	}
	return true;
}

/* 
	Writes cached state to MRAM
	NOTE: the SPI mutex MUST be held if called with safe == false - otherwise all of MRAM can be corrupted 
	Also serves to correct any errors in stack space for cached state
*/
void write_state_to_storage_safety(bool safe) {
	if (!safe || xSemaphoreTake(mram_spi_cache_mutex, MRAM_SPI_MUTEX_WAIT_TIME_TICKS))
	{
		// always do this (every PERSISTENT_DATA_BACKUP_TASK_FREQ ms),
		// plus we need to do it before every cached_state update
		cached_state_correct_errors();
	
		cached_state.sat_state = get_sat_state();
		// reboot count is only incremented on startup and is written through cache
		// other fields are written through when changed

		// (variables for read results)
		uint32_t temp_secs_since_launch;
		uint8_t temp_reboot_count, temp_sat_state;
		satellite_history_batch temp_sat_event_history;
		uint8_t temp_prog_mem_rewritten;
		uint32_t temp_radio_revive_timestamp;
		persistent_charging_data_t temp_persistent_charging_data;
		bool errors_write_confirmed = false;

		// set write time right before writing
		// (keep track of the old timestamp value in case the write fails and we have to reset)
		uint32_t prev_cached_secs_since_launch = cached_state.secs_since_launch;
		uint32_t prev_last_data_write_ms = last_data_write_ms;
	
		// quickly set time fields mutex while we're doing this so timestamp doesn't jump forward
		uint32_t cur_timestamp = get_current_timestamp(); // grab before because "takes" mutex
		cache_time_fields_minimutex = true;
		cached_state.secs_since_launch = cur_timestamp;
		last_data_write_ms = xTaskGetTickCount() / portTICK_PERIOD_MS;
		cache_time_fields_minimutex = false;
		cached_state_sync_redundancy();
	
		// actually perform writes
		storage_write_field_unsafe((uint8_t*) &cached_state.secs_since_launch,	4,		STORAGE_SECS_SINCE_LAUNCH_ADDR);
		storage_write_field_unsafe((uint8_t*) &cached_state.reboot_count,		1,		STORAGE_REBOOT_CNT_ADDR);
		storage_write_field_unsafe((uint8_t*) &cached_state.sat_state,			1,		STORAGE_SAT_STATE_ADDR);
		storage_write_field_unsafe((uint8_t*) &cached_state.sat_event_history,	1,		STORAGE_SAT_EVENT_HIST_ADDR);
		storage_write_field_unsafe((uint8_t*) &cached_state.prog_mem_rewritten,	1,		STORAGE_PROG_MEM_REWRITTEN_ADDR);
		storage_write_field_unsafe((uint8_t*) &cached_state.radio_revive_timestamp,4,	STORAGE_RADIO_REVIVE_TIMESTAMP_ADDR);
		storage_write_field_unsafe((uint8_t*) &cached_state.persistent_charging_data,1,	STORAGE_PERSISTENT_CHARGING_DATA_ADDR);
		errors_write_confirmed = storage_write_check_errors_unsafe(&error_equistack, true);

		// read it right back to confirm validity
		storage_read_field_unsafe((uint8_t*) &temp_secs_since_launch,	4,		STORAGE_SECS_SINCE_LAUNCH_ADDR);
		storage_read_field_unsafe(&temp_reboot_count,					1,		STORAGE_REBOOT_CNT_ADDR);
		storage_read_field_unsafe(&temp_sat_state,						1,		STORAGE_SAT_STATE_ADDR);
		storage_read_field_unsafe((uint8_t*) &temp_sat_event_history,	1,		STORAGE_SAT_EVENT_HIST_ADDR);
		storage_read_field_unsafe(&temp_prog_mem_rewritten,				1,		STORAGE_PROG_MEM_REWRITTEN_ADDR);
		storage_read_field_unsafe((uint8_t*) &temp_radio_revive_timestamp,	4,	STORAGE_RADIO_REVIVE_TIMESTAMP_ADDR);
		storage_read_field_unsafe((uint8_t*) &temp_persistent_charging_data,1,	STORAGE_PERSISTENT_CHARGING_DATA_ADDR);
		
		// log error if the stored data was not consistent with what was just written
		// note we have the mutex so no one should be able to write to these
		// while we were reading / are comparing them
		if (temp_secs_since_launch != cached_state.secs_since_launch 
			|| temp_reboot_count != cached_state.reboot_count 
			|| temp_sat_state != cached_state.sat_state 
			|| !compare_sat_event_history(&temp_sat_event_history, &cached_state.sat_event_history) 
			|| temp_prog_mem_rewritten != cached_state.prog_mem_rewritten
			|| temp_radio_revive_timestamp != cached_state.radio_revive_timestamp
			|| !compare_persistent_charging_data(&temp_persistent_charging_data, &cached_state.persistent_charging_data)
			|| !errors_write_confirmed) {

			log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_INCONSISTENT_DATA, true);

			// in particular, if it was the secs_since_launch that was inconsistent,
			// go off the old value in the MRAM
			if (temp_secs_since_launch != cached_state.secs_since_launch &&
			temp_secs_since_launch < cached_state.secs_since_launch) { // TODO: why the greater than condition??
				// grab time field mutex
				cache_time_fields_minimutex = true;
				last_data_write_ms = prev_last_data_write_ms;
				cached_state.secs_since_launch = prev_cached_secs_since_launch;
				cache_time_fields_minimutex = false;
			}
		}
		
		// apply changes to cached state
		cached_state_sync_redundancy();
		
		if (safe) xSemaphoreGive(mram_spi_cache_mutex);
		
	} else {
		log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_SPI_MUTEX_TIMEOUT, true);
	}
}

/* external function(s) */
void write_state_to_storage(void) {
	write_state_to_storage_safety(true);
}

/* Writes cached state to MRAM, but doesn't confirm it was correct. 
   Can also be used from an ISR if from_isr is true */
void write_state_to_storage_emergency(bool from_isr) {
	bool got_mutex;
	if (from_isr) {
		got_mutex = xSemaphoreTakeFromISR(mram_spi_cache_mutex, NULL);
	} else {
		got_mutex = xSemaphoreTake(mram_spi_cache_mutex, MRAM_SPI_MUTEX_WAIT_TIME_TICKS);
	}
	
	if (got_mutex)
	{
		cached_state_correct_errors();
		uint32_t cur_timestamp = get_current_timestamp(); // grab before because "takes" mutex
		cache_time_fields_minimutex = true;
		cached_state.secs_since_launch = cur_timestamp;
		last_data_write_ms = xTaskGetTickCount() / portTICK_PERIOD_MS;
		cache_time_fields_minimutex = false;
		cached_state_sync_redundancy();
		
		storage_write_field_unsafe((uint8_t*) &cached_state.secs_since_launch,	4,		STORAGE_SECS_SINCE_LAUNCH_ADDR);
		storage_write_field_unsafe((uint8_t*) &cached_state.reboot_count,		1,		STORAGE_REBOOT_CNT_ADDR);
		storage_write_field_unsafe((uint8_t*) &cached_state.sat_state,			1,		STORAGE_SAT_STATE_ADDR);
		storage_write_field_unsafe((uint8_t*) &cached_state.sat_event_history,	1,		STORAGE_SAT_EVENT_HIST_ADDR);
		storage_write_field_unsafe((uint8_t*) &cached_state.prog_mem_rewritten,	1,		STORAGE_PROG_MEM_REWRITTEN_ADDR);
		storage_write_field_unsafe((uint8_t*) &cached_state.radio_revive_timestamp,4,	STORAGE_RADIO_REVIVE_TIMESTAMP_ADDR);
		storage_write_field_unsafe((uint8_t*) &cached_state.persistent_charging_data,1,	STORAGE_PERSISTENT_CHARGING_DATA_ADDR);
		storage_write_check_errors_unsafe(&error_equistack, false);
		
		if (from_isr) {
			xSemaphoreGiveFromISR(mram_spi_cache_mutex, NULL);
		} else {
			xSemaphoreGive(mram_spi_cache_mutex);
		}
	}
}

/************************************************************************/
/* Cached state redundancy                                              */
/************************************************************************/

// Compares the state of the three redundant cached state buffers,
// and corrects any errors it sees via a two vs. one vote 
// (double-corruption is extremely unlikely)
void cached_state_correct_errors(void) {
	bool state1_matches_state2 = memcmp(&cached_state,	&cached_state_2, sizeof(struct persistent_data)) == 0;
	bool state1_matches_state3 = memcmp(&cached_state,	&cached_state_3, sizeof(struct persistent_data)) == 0;
	bool state2_matches_state3 = memcmp(&cached_state_2,&cached_state_3, sizeof(struct persistent_data)) == 0;
	
	if (state1_matches_state2 && state1_matches_state3 && state2_matches_state3) {
		// 1 == 2 == 3 == 1; all match so nothing to do!
		return;
	} else if (state1_matches_state2) {
		// 1 == 2 =/= 3 =/= 1; 1 matches 2 => 3 is wrong
		memcpy(&cached_state_3, &cached_state, sizeof(struct persistent_data));
		log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_CORRUPTED, false);
		configASSERT(false); // only reason should be radiation corruption
	} else if (state1_matches_state3) {
		// 1 =/= 2 =/= 3 == 1; 1 matches 3 => 2 is wrong
		memcpy(&cached_state_2, &cached_state, sizeof(struct persistent_data));
		log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_CORRUPTED, false);
		configASSERT(false); // only reason should be radiation corruption
	} else if (state2_matches_state3) {
		// 1 =/= 2 == 3 =/= 1; 2 matches 3 => 1 is wrong
		memcpy(&cached_state, &cached_state_2, sizeof(struct persistent_data));
		log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_CORRUPTED, false);
		configASSERT(false); // only reason should be radiation corruption
	} else {
		// there's not much we can do (this is extremely unlikely), so just take the cached state
		log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_CORRUPTED, true);
		configASSERT(false); // only reason should be radiation corruption
	}
}

// Propagate change(s) in the primary cached state to the backup
// buffers. Must be called after every cache write
void cached_state_sync_redundancy(void) {
	memcpy(&cached_state_2, &cached_state, sizeof(struct persistent_data));
	memcpy(&cached_state_3, &cached_state, sizeof(struct persistent_data));
}

/************************************************************************/
/* External state write functions                                       */
/************************************************************************/

bool increment_reboot_count(void) {
	if (xSemaphoreTake(mram_spi_cache_mutex, MRAM_SPI_MUTEX_WAIT_TIME_TICKS)) {
		cached_state_correct_errors();
		cached_state.reboot_count++;
		cached_state_sync_redundancy();
		write_state_to_storage_safety(false);
		
		xSemaphoreGive(mram_spi_cache_mutex);
		return true;
		
	} else {
		// TODO: maybe increment reboot count anyways (and update sync redundancy)
		log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_SPI_MUTEX_TIMEOUT, true);
		return false;
	}
}

bool set_radio_revive_timestamp(uint32_t radio_revive_timestamp) {
	if (xSemaphoreTake(mram_spi_cache_mutex, MRAM_SPI_MUTEX_WAIT_TIME_TICKS)) {
		cached_state_correct_errors();
		cached_state.radio_revive_timestamp = radio_revive_timestamp;
		cached_state_sync_redundancy();
		write_state_to_storage_safety(false);
		
		xSemaphoreGive(mram_spi_cache_mutex);
		return true;
		
	} else {
		// TODO: maybe set timestamp anyways (and update sync redundancy)
		log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_SPI_MUTEX_TIMEOUT, true);
		return false;
	}
}

// will be called with scheduler suspended (and mutex taken), so no concurrency worries
void set_persistent_charging_data_unsafe(persistent_charging_data_t data) {
	cached_state_correct_errors();
	cached_state.persistent_charging_data = data;
	cached_state_sync_redundancy();
	write_state_to_storage_safety(false); // unsafe, make sure to have mutex above!
}

/* Updates the sat_event_history if the given value is true, but ONLY
   sets them to TRUE, not to FALSE; if the passed in value is FALSE,
   the original value (TRUE or FALSE) is retained. 
   Should really be called periodically for these crucial things */
bool update_sat_event_history(uint8_t antenna_deployed,
								uint8_t lion_1_charged,
								uint8_t lion_2_charged,
								uint8_t lifepo_b1_charged,
								uint8_t lifepo_b2_charged,
								uint8_t first_flash,
								uint8_t prog_mem_rewritten) {

	if (xSemaphoreTake(mram_spi_cache_mutex, MRAM_SPI_MUTEX_WAIT_TIME_TICKS)) {
		cached_state_correct_errors();
	
		if (antenna_deployed)
			cached_state.sat_event_history.antenna_deployed = true;
		if (lion_1_charged)
			cached_state.sat_event_history.lion_1_charged = true;
		if (lion_2_charged)
			cached_state.sat_event_history.lion_2_charged = true;
		if (lifepo_b1_charged)
			cached_state.sat_event_history.lifepo_b1_charged = true;
		if (lifepo_b2_charged)
			cached_state.sat_event_history.lifepo_b2_charged = true;
		if (first_flash)
			cached_state.sat_event_history.first_flash = true;
		if (prog_mem_rewritten)
			cached_state.sat_event_history.prog_mem_rewritten = true;

		cached_state_sync_redundancy();
		write_state_to_storage();
		
		xSemaphoreGive(mram_spi_cache_mutex);
		return true;
		
	} else {
		// TODO: maybe update state anyways (and update sync redundancy)?
		log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_SPI_MUTEX_TIMEOUT, true);
		return false;
	}
}


/************************************************************************/
// functions to get components of cached state
// NOTE: use of mutexes here is necessary because we're using a single
// MRAM chip which is a single-reader single-writer shared resource.
/************************************************************************/

uint32_t cache_get_secs_since_launch() {
	// note: not necessary to take minimutex because people
	// don't have access to last_write_time_ms
	return cached_state.secs_since_launch;
}

uint8_t cache_get_reboot_count() {
	return cached_state.reboot_count;
}

/* returns satellite state at last reboot */
sat_state_t cache_get_sat_state() {
	return cached_state.sat_state;
}

satellite_history_batch cache_get_sat_event_history() {
	return cached_state.sat_event_history;
}

bool cache_get_prog_mem_rewritten(void) {
	return cached_state.prog_mem_rewritten;
}

uint32_t cache_get_radio_revive_timestamp(void) {
	return cached_state.radio_revive_timestamp;
}

persistent_charging_data_t cache_get_persistent_charging_data(void) {
	return cached_state.persistent_charging_data;
}

/************************************************************************/
/* functions which require reading from MRAM (bypass cache)				*/
/************************************************************************/
void populate_error_stacks(equistack* error_stack) {
	// take big buffers off stack
	static sat_error_t error_buf[ERROR_STACK_MAX];
	
	if (xSemaphoreTake(mram_spi_cache_mutex, MRAM_SPI_MUTEX_WAIT_TIME_TICKS))
	{
		// read in errors from MRAM
		uint8_t num_stored_errors;
		storage_read_field_unsafe(&num_stored_errors,	1, STORAGE_ERR_NUM_ADDR);
		// make sure number of errors is in a reasonable bound (note we're using a uint)
		// it may be a larger issue if this is wrong, but read in errors anyway (we
		// wouldn't want to miss anything - but we'll add this error at the end so we
		// see that this happened)
		bool error_num_too_long = false;
		if (num_stored_errors >= ERROR_STACK_MAX) {
			error_num_too_long = true;
			num_stored_errors = ERROR_STACK_MAX;
		}
		
		// special case; we can't read in 0 bytes (invalid arg)
		if (num_stored_errors > 0) {
			storage_read_field_unsafe((uint8_t*) error_buf,
			num_stored_errors * sizeof(sat_error_t), STORAGE_ERR_LIST_ADDR);

			// read all errors that we have stored in MRAM in
			for (int i = 0; i < num_stored_errors; i++) {
				equistack_Push(error_stack, &(error_buf[i]));
			}
		}
		
		if (error_num_too_long) {
			// log this after we've populated, making sure it's priority
			// so it overwrites any garbage errors we may have gotten
			log_error(ELOC_MRAM_READ, ECODE_OUT_OF_BOUNDS, true);
		}
		
		xSemaphoreGive(mram_spi_cache_mutex);
	} else {
		log_error(ELOC_CACHED_PERSISTENT_STATE, ECODE_SPI_MUTEX_TIMEOUT, true);
	}
}


/************************************************************************/
/* Struct compare functions                                              */
/************************************************************************/

/* deep comparison of structs because their bit organization may differ */
bool compare_sat_event_history(satellite_history_batch* history1, satellite_history_batch* history2) {
	bool result = true;
	result = result && history1->antenna_deployed == history2->antenna_deployed;
	result = result && history1->first_flash == history2->first_flash;
	result = result && history1->lifepo_b1_charged == history2->lifepo_b1_charged;
	result = result && history1->lifepo_b2_charged == history2->lifepo_b2_charged;
	result = result && history1->lion_1_charged == history2->lion_1_charged;
	result = result && history1->lion_2_charged == history2->lion_2_charged;
	result = result && history1->prog_mem_rewritten == history2->prog_mem_rewritten;
	return result;
}
bool compare_persistent_charging_data(persistent_charging_data_t* data1, persistent_charging_data_t* data2) {
	return data1->li_caused_reboot == data1->li_caused_reboot;
}

/************************************************************************/
/* helper functions using cached state		                            */
/************************************************************************/

/*
 * Current timestamp in seconds since boot, with an accuracy of +/- the
 * data write task frequency (a reboot could happen at any point in that period
 * due to a watchdog reset). Segment since reboot is accurate to ms.
 */
uint32_t get_current_timestamp(void) {
	while (cache_time_fields_minimutex) {}; // wait on time field mutex
	return ((xTaskGetTickCount() / portTICK_PERIOD_MS - last_data_write_ms) / 1000)
		 + cache_get_secs_since_launch();
}

/* Current timestamp in ms since boot, with the above described (low) accuracy */
uint64_t get_current_timestamp_ms(void) {
	while (cache_time_fields_minimutex) {}; // wait on time field mutex
	return (xTaskGetTickCount() / portTICK_PERIOD_MS) - last_data_write_ms
			+ (1000 * cache_get_secs_since_launch());
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
bool passed_orbit_fraction(uint8_t* prev_orbit_fraction, uint8_t orbit_fraction_denominator) {
	#ifdef TESTING_SPEEDUP
		return true;
	#else
		// TODO: Does this work??
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
/* Utility for setting MRAM; used to write initial state				*/
/************************************************************************/
void write_custom_state(void) {
	/*** CONFIG ***/
	uint32_t secs_since_launch =				0;
	uint8_t reboot_count =						0;
	sat_state_t sat_state =						INITIAL;
	satellite_history_batch sat_event_history;
	sat_event_history.antenna_deployed =		false;
	sat_event_history.first_flash =				false;
	sat_event_history.lifepo_b1_charged =		false;
	sat_event_history.lifepo_b2_charged =		false;
	sat_event_history.lion_1_charged =			false;
	sat_event_history.lion_2_charged =			false;
	sat_event_history.prog_mem_rewritten =		false;
	uint8_t prog_mem_rewritten =				false;
	uint32_t radio_revive_timestamp =				0;
	persistent_charging_data_t persistent_charging_data;
	persistent_charging_data.li_caused_reboot = -1;

	#define NUM_ERRS	0
	const uint8_t num_errs = NUM_ERRS;
	sat_error_t error_buf[NUM_ERRS];
// 	sat_error_t error_buf[NUM_ERRS] = {
// 		{10, 20, 40},
// 		{11, 120, 247},
// 		{1, 2, 3},
// 	};

	/*** WRITING ***/

	// set write time right before writing
	last_data_write_ms = xTaskGetTickCount() / portTICK_PERIOD_MS;

	storage_write_field_unsafe((uint8_t*) &secs_since_launch,	4,		STORAGE_SECS_SINCE_LAUNCH_ADDR);
	storage_write_field_unsafe((uint8_t*) &reboot_count,		1,		STORAGE_REBOOT_CNT_ADDR);
	storage_write_field_unsafe((uint8_t*) &sat_state,			1,		STORAGE_SAT_STATE_ADDR);
	storage_write_field_unsafe((uint8_t*) &sat_event_history,	1,		STORAGE_SAT_EVENT_HIST_ADDR);
	storage_write_field_unsafe((uint8_t*) &prog_mem_rewritten,	1,		STORAGE_PROG_MEM_REWRITTEN_ADDR);
	storage_write_field_unsafe((uint8_t*) &radio_revive_timestamp, 4,	STORAGE_RADIO_REVIVE_TIMESTAMP_ADDR);
	storage_write_field_unsafe((uint8_t*) &persistent_charging_data, 1,	STORAGE_PERSISTENT_CHARGING_DATA_ADDR);

	// write errors
	storage_write_field_unsafe((uint8_t*) &num_errs,		1, STORAGE_ERR_NUM_ADDR);
	if (num_errs > 0)
		storage_write_field_unsafe((uint8_t*) error_buf,
			num_errs * sizeof(sat_error_t), STORAGE_ERR_LIST_ADDR);

	/*** read it right back to confirm validity ***/
	uint32_t temp_secs_since_launch;
	uint8_t temp_reboot_count;
	sat_state_t temp_sat_state;
	satellite_history_batch temp_sat_event_history;
	uint8_t temp_prog_mem_rewritten;
	uint32_t temp_radio_revive_timestamp;
	persistent_charging_data_t temp_persistent_charging_data;

	uint8_t temp_num_errs;
	sat_error_t temp_error_buf[num_errs];

	storage_read_field_unsafe((uint8_t*) &temp_secs_since_launch,	4,		STORAGE_SECS_SINCE_LAUNCH_ADDR);
	storage_read_field_unsafe((uint8_t*) &temp_reboot_count,		1,		STORAGE_REBOOT_CNT_ADDR);
	storage_read_field_unsafe((uint8_t*) &temp_sat_state,			1,		STORAGE_SAT_STATE_ADDR);
	storage_read_field_unsafe((uint8_t*) &temp_sat_event_history,	1,		STORAGE_SAT_EVENT_HIST_ADDR);
	storage_read_field_unsafe((uint8_t*) &temp_prog_mem_rewritten,	1,		STORAGE_PROG_MEM_REWRITTEN_ADDR);
	storage_read_field_unsafe((uint8_t*) &temp_radio_revive_timestamp,	4,	STORAGE_RADIO_REVIVE_TIMESTAMP_ADDR);
	storage_read_field_unsafe((uint8_t*) &temp_persistent_charging_data, 1,	STORAGE_PERSISTENT_CHARGING_DATA_ADDR);

	storage_read_field_unsafe((uint8_t*) &temp_num_errs,	1, STORAGE_ERR_NUM_ADDR);
	configASSERT(temp_num_errs == num_errs);
	if (num_errs > 0)
		storage_read_field_unsafe((uint8_t*) temp_error_buf,
			num_errs * sizeof(sat_error_t), STORAGE_ERR_LIST_ADDR);

	/*** CHECKS ***/
	configASSERT(temp_secs_since_launch == secs_since_launch);
	configASSERT(temp_reboot_count == reboot_count);
	configASSERT(temp_sat_state == sat_state);
	configASSERT(compare_sat_event_history(&temp_sat_event_history, &sat_event_history));
	configASSERT(temp_prog_mem_rewritten == prog_mem_rewritten);
	configASSERT(temp_radio_revive_timestamp == radio_revive_timestamp);
	configASSERT(compare_persistent_charging_data(&temp_persistent_charging_data, &persistent_charging_data))

	configASSERT(memcmp(error_buf, temp_error_buf, num_errs * sizeof(sat_error_t)) == 0);
}
