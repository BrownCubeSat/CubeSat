/*
 * MRAM_Commands.h
 *
 * Created: 4/10/2016 1:59:16 PM
 *  Author: Gustavo
 */ 

#ifndef MRAM_COMMANDS_H_
#define MRAM_COMMANDS_H_

#include <stdbool.h>
#include <spi.h>

// NOTE: looking at initialize_master in mram.c, the PADX must correspond to the PADX on these
// signals, but the mux setting (a letter) DOES NOT have to correspond with the function of
// these pin's muxes
#define P_SPI_MOSI			PINMUX_PA18C_SERCOM1_PAD2 // corresponds to signal SI
#define P_SPI_MISO			PINMUX_PA16C_SERCOM1_PAD0 // corresponds to signal SO
#define P_SPI_SCK			PINMUX_PA19C_SERCOM1_PAD3
#define MRAM_SPI_SERCOM		SERCOM1
#define MRAM_SPI_BAUD		10000000

#define P_MRAM1_CS			PIN_PA17
#define P_MRAM2_CS			PIN_PB16

extern const uint8_t NUM_CONTROL_BYTES, READ_COMMAND, WRITE_COMMAND, ENABLE_COMMAND;

/************************************************************************/
/* Initialize the master, the baudrate should be inside the proper range*/
/************************************************************************/
uint8_t mram_initialize_master(struct spi_module *spi_master_instance, uint32_t baudrate);

/************************************************************************/
/* Initialize the slave, just pass the pointer                          */
/************************************************************************/
uint8_t mram_initialize_slave(struct spi_slave_inst *slave, int ss_pin);

/************************************************************************/
/* Given master, slave number of bytes and initial address, the content */
/* will be read to data                                                 */
/************************************************************************/
status_code_genare_t mram_read_bytes(struct spi_module *spi_master_instance, struct spi_slave_inst *slave, uint8_t *data, int num_bytes, uint16_t address);

/************************************************************************/
/* Given master, slave number of bytes and initial address, the content */
/* of data will be written                                              */
/************************************************************************/
status_code_genare_t mram_write_bytes(struct spi_module *spi_master_instance, struct spi_slave_inst *slave, uint8_t *data, int num_bytes, uint16_t address);

/************************************************************************/
/* Given master and slave, the content of the MRAM's status register	*/
/* will be written				                                        */
/************************************************************************/
status_code_genare_t mram_read_status_register(struct spi_module *spi_master_instance, struct spi_slave_inst *slave, uint8_t *reg_out);

#endif /* MRAM_COMMANDS_H_ */