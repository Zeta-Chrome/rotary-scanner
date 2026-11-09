#include "esp_log.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "freertos/idf_additions.h"
#include "portmacro.h"

#define RCV_HOST SPI2_HOST
#define GPIO_HANDSHAKE 2
#define GPIO_MOSI 23
#define GPIO_MISO 19
#define GPIO_SCLK 18
#define GPIO_CS 5

#define FRAME_SZ (320 * 240 * 2)
#define CAPTURE_CMD 0x55
#define FLASH_ON_CMD 0xAA
#define FLASH_OFF_CMD 0x33

void cam_capture_img(char *fb)
{
	vTaskDelay(50);
	memset(fb, 0xAA, FRAME_SZ);
}

void cam_on_flash()
{
}

void cam_off_flash()
{
}

void app_main()
{
	esp_err_t ret;

	//Configuration for the SPI bus
	spi_bus_config_t bus_cfg = {
		.mosi_io_num = GPIO_MOSI,
		.miso_io_num = GPIO_MISO,
		.sclk_io_num = GPIO_SCLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
	};

	//Configuration for the SPI slave interface
	spi_slave_interface_config_t slv_cfg = { .mode = 0,
											 .spics_io_num = GPIO_CS,
											 .queue_size = 3,
											 .flags = 0,
											 .post_setup_cb = NULL,
											 .post_trans_cb = NULL };

	//Configuration for the handshake line
	gpio_config_t io_conf = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = BIT64(GPIO_HANDSHAKE),
	};
	gpio_config(&io_conf);

	//Enable pull-ups on SPI lines so we don't detect rogue pulses when no master is connected.
	gpio_set_pull_mode(GPIO_MOSI, GPIO_PULLUP_ONLY);
	gpio_set_pull_mode(GPIO_SCLK, GPIO_PULLUP_ONLY);
	gpio_set_pull_mode(GPIO_CS, GPIO_PULLUP_ONLY);

	//Initialize SPI slave interface
	ret = spi_slave_initialize(RCV_HOST, &bus_cfg, &slv_cfg, SPI_DMA_CH_AUTO);
	assert(ret == ESP_OK);

	char *send_buf = spi_bus_dma_memory_alloc(RCV_HOST, FRAME_SZ, 0);
	char *recv_buf = spi_bus_dma_memory_alloc(RCV_HOST, 1, 0);
	assert(send_buf && recv_buf);
	spi_slave_transaction_t t = { 0 };

	while (1) {
		gpio_set_level(GPIO_HANDSHAKE, 0);

		// Set buffer as 0
		*recv_buf = 0;
		t.length = 1;
		t.tx_buffer = NULL;
		t.rx_buffer = recv_buf;

		ret = spi_slave_transmit(RCV_HOST, &t, portMAX_DELAY);

		switch (*recv_buf) {
		case CAPTURE_CMD:
			printf("Capture Image command\n");
			cam_capture_img(send_buf);
			break;
		case FLASH_ON_CMD:
			printf("Flash ON command\n");
			cam_on_flash();
			continue;
		case FLASH_OFF_CMD:
			printf("Flash OFF command\n");
			cam_off_flash();
			continue;
		default:
			printf("Invalid command recieved\n");
			continue;
		}

		// Let the RPI know image is ready
		gpio_set_level(GPIO_HANDSHAKE, 1);

		t.length = FRAME_SZ;
		t.tx_buffer = send_buf;
		t.rx_buffer = NULL;

		ret = spi_slave_transmit(RCV_HOST, &t, portMAX_DELAY);
	}
}
