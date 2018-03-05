/*
 * persistent_storage.h
 *
 * Created: 11/16/2017 23:39:44
 *  Author: mcken
 */ 

#ifndef PERSISTENT_STORAGE_H
#define PERSISTENT_STORAGE_H

#include <global.h>
#include "Sensor_Structs.h"
#include "equistack.h"
#include "../processor_drivers/MRAM_Commands.h"

/* addressing constants */
#define STORAGE_SECS_SINCE_LAUNCH_ADDR			20
#define STORAGE_REBOOT_CNT_ADDR					30
#define STORAGE_SAT_STATE_ADDR					34
#define STORAGE_SAT_EVENT_HIST_ADDR				38
#define STORAGE_PROG_MEM_REWRITTEN_ADDR			42
#define STORAGE_RADIO_REVIVE_TIMESTAMP_ADDR		46
#define STORAGE_PERSISTENT_CHARGING_DATA_ADDR	50
#define STORAGE_PROG_MEMORY_ADDR				60
#define STORAGE_ERR_NUM_ADDR					175080
#define STORAGE_ERR_LIST_ADDR					175084

// maximum size of a single MRAM "field," used for global buffers
#define STORAGE_MAX_FIELD_SIZE				400 // error list

// note: this is the NUMBER of stored errors; the bytes taken up is this times sizeof(sat_error_t)
#define MAX_STORED_ERRORS					ERROR_STACK_MAX
#define ORBITAL_PERIOD_S					5580 // s; 93 mins
#define MRAM_SPI_MUTEX_WAIT_TIME_TICKS		((TickType_t) 1000 / portTICK_PERIOD_MS) // ms

/* battery-specific state cache (put here for #include reasons) */
typedef struct persistent_charging_data_t {
	int8_t li_caused_reboot;
} persistent_charging_data_t;

/************************************************************************/
/* STATE CACHE                                                          */
/* NOTE: this cached state is configured to match the ACTUAL state that 
 the satellite code EXPECTS is stored in the MRAM. This is a tradeoff;
 it means that this state may not persist if something is going wrong
 with the MRAM, so the satellite (and those on the ground) may be 
 'deceived' by the impression that this state will persist. However,
 the important thing is that they will NOT be deceived about the 
 satellite state. In essence, the cache will represent what has actually
 happened, and we just hope like heck that the MRAM will reflect that,
 instead of holding the MRAM as the ground truth. 
 In sum, this is why we ignore concurrency issues with an updated cache
 being readable before the state could be written, etc.					*/
/* Also, this is the one bit of data memory (RAM) that we want to be 
"RAD safe" because it gets written to RAD safe memory and is therefore 
persistent. That is, a bit flip here is permanent, while elsewhere
the watchdog will hopefully reset if something goes wrong and we'll be 
on a clean slate.
To provide redundancy, we simply keep 3 copies of this struct in RAM
(slightly dispersed as you'll see below so they're not quite in the same
region; though a large swath of bit flips is incredibly unlikely),
and we simply have two vote against any one, assuming (justifiably) that
the chance of all three being different is minuscule. 
NOTE: this one is the one we refer to, but before using it the 
cached_state_correct() method is (and should be) called to apply 
the voting, and after writing to it call cached_state_sync_redundancy().
Finally, note that this is not guaranteed to be rad safe to readers
in the period between writes, only to be corrected before MRAM writes
(which occur every PERSISTENT_DATA_BACKUP_TASK_FREQ = 10s) */
/************************************************************************/
struct persistent_data {
	uint32_t secs_since_launch; // note: most recent one stored in MRAM, not current timestamp
	uint8_t reboot_count;
	sat_state_t sat_state; // most recent known state
	satellite_history_batch sat_event_history;
	uint8_t prog_mem_rewritten; // actually a bool; only written by bootloader (one in sat event history follows that paradigm)
	uint32_t radio_revive_timestamp;
	persistent_charging_data_t persistent_charging_data;
	
} cached_state;

/* 
	mutex for locking SPI lines and MRAM drivers 
	Also is responsible for locking against multiple writes to the cache,
	such that changes to all three redundant caches appear atomic 
	(nobody misinterprets an intentional change to the cache as a bit flip)
*/
StaticSemaphore_t _mram_spi_cache_mutex_d;
SemaphoreHandle_t mram_spi_cache_mutex;

// dispersed!
struct persistent_data cached_state_2;

// variable to be updated on each data read so we know how current the MRAM
// data is (only for computing timestamps) 
// (measured relative to start of current RTOS tick count)
uint32_t last_data_write_ms;

// dispersed!
struct persistent_data cached_state_3;

/* memory interface / action functions */
void init_persistent_storage(void);
void read_state_from_storage(void);
void write_state_to_storage(void);
void write_state_to_storage_emergency(bool from_isr);
void cached_state_correct_errors(void);

bool increment_reboot_count(void);
bool set_radio_revive_timestamp(uint32_t radio_revive_timestamp);
void set_persistent_charging_data_unsafe(persistent_charging_data_t data);
bool update_sat_event_history(uint8_t antenna_deployed,
								uint8_t lion_1_charged,
								uint8_t lion_2_charged,
								uint8_t lifepo_b1_charged,
								uint8_t lifepo_b2_charged,
								uint8_t first_flash,
								uint8_t prog_mem_rewritten);

/* functions to get components of cached state */
uint32_t					cache_get_secs_since_launch(void);
uint8_t						cache_get_reboot_count(void);
sat_state_t					cache_get_sat_state(void);
satellite_history_batch		cache_get_sat_event_history(void);
bool						cache_get_prog_mem_rewritten(void);
uint32_t					cache_get_radio_revive_timestamp(void);
persistent_charging_data_t	cache_get_persistent_charging_data(void);

/* functions which require reading from MRAM (bypass cache) */
void populate_error_stacks(equistack* error_stack);

/* helper functions using cached state */
uint32_t get_current_timestamp(void);
uint64_t get_current_timestamp_ms(void);
uint16_t get_orbits_since_launch(void);
bool passed_orbit_fraction(uint8_t* prev_orbit_fraction, uint8_t orbit_fraction_denominator);

/* maintenance helpers */
void write_custom_state(void);

/* utility functions (for testing elsewhere) */
size_t longest_same_seq_len(uint8_t* data, size_t len);

#endif