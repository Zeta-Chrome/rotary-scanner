#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define GPIO_INT 2
#define GPIO_MOSI 12
#define GPIO_MISO 13
#define GPIO_SCLK 15
#define GPIO_CS 14

void app_main()
{
	esp_err_t ret;
	spi_device_handle_t handle;

	//Configuration for the SPI bus
	spi_bus_config_t buscfg = { .mosi_io_num = GPIO_MOSI,
								.miso_io_num = GPIO_MISO,
								.sclk_io_num = GPIO_SCLK,
								.quadwp_io_num = -1,
								.quadhd_io_num = -1 };

	//Configuration for the SPI device on the other side of the bus
	spi_device_interface_config_t devcfg = {
		.command_bits = 0,
		.address_bits = 0,
		.dummy_bits = 0,
		.clock_speed_hz = 5000000,
		.duty_cycle_pos = 128, //50% duty cycle
		.mode = 0,
		.spics_io_num = GPIO_CS,
		.cs_ena_posttrans =
			3, //Keep the CS low 3 cycles after transaction, to stop slave from missing the last bit when CS has less propagation delay than CLK
		.queue_size = 3
	};
}
