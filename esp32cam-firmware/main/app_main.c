#include "esp_log.h"
#include "cam_pins.h"
#include "esp_camera.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "portmacro.h"

#define RCV_HOST SPI2_HOST
#define GPIO_HANDSHAKE 2
#define GPIO_MOSI 23
#define GPIO_MISO 19
#define GPIO_SCLK 18
#define GPIO_CS 5
#define GPIO_FLASH 4

#define FRAME_WD 320
#define FRAME_HT 240
#define FRAME_CH 2
#define FRAME_SZ (FRAME_WD * FRAME_HT * FRAME_CH)

#define CAPTURE_CMD 0x55
#define FLASH_ON_CMD 0xAA
#define FLASH_OFF_CMD 0x33

#define FB_QUEUE_SIZE 1

bool esp32cam_init(pixformat_t format, size_t fb_count,
				   camera_grab_mode_t grab_mode)
{
	camera_config_t config;
	config.ledc_channel = LEDC_CHANNEL_0;
	config.ledc_timer = LEDC_TIMER_0;
	config.pin_d0 = CAM_PIN_D0;
	config.pin_d1 = CAM_PIN_D1;
	config.pin_d2 = CAM_PIN_D2;
	config.pin_d3 = CAM_PIN_D3;
	config.pin_d4 = CAM_PIN_D4;
	config.pin_d5 = CAM_PIN_D5;
	config.pin_d6 = CAM_PIN_D6;
	config.pin_d7 = CAM_PIN_D7;
	config.pin_vsync = CAM_PIN_VSYNC;
	config.pin_href = CAM_PIN_HREF;
	config.pin_pclk = CAM_PIN_PCLK;
	config.pin_xclk = CAM_PIN_XCLK;
	config.pin_sccb_sda = CAM_PIN_SIOD;
	config.pin_sccb_scl = CAM_PIN_SIOC;
	config.pin_pwdn = CAM_PIN_PWDN;
	config.pin_reset = CAM_PIN_RESET;
	config.xclk_freq_hz = 12000000;
	config.grab_mode = grab_mode;
	config.pixel_format = format;
	config.frame_size = FRAMESIZE_QVGA;
	config.jpeg_quality = 0;
	config.fb_count = fb_count;
	config.fb_location = CAMERA_FB_IN_PSRAM;
	config.grab_mode = grab_mode;

	if (esp_camera_init(&config) != ESP_OK)
		return false;

	ESP_LOGI("CAMERA", "Camera initialized successfully with %zu buffers",
			 fb_count);

	return true;
}

bool esp32cam_capture_frame(char *frame)
{
	camera_fb_t *fb = esp_camera_fb_get();
	if (!fb)
		return false;

	memcpy(frame, fb->buf, fb->len);
	esp_camera_fb_return(fb);
	return true;
}

void esp32cam_on_flash()
{
	gpio_set_level(GPIO_FLASH, 1);
}

void esp32cam_off_flash()
{
	gpio_set_level(GPIO_FLASH, 0);
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

	//Configuration for the handshake line
	gpio_config_t flash_conf = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = BIT64(GPIO_FLASH),
	};
	gpio_config(&flash_conf);

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

	// Init camera
	esp32cam_init(PIXFORMAT_YUV422, FB_QUEUE_SIZE, CAMERA_GRAB_LATEST);

	while (1) {
		gpio_set_level(GPIO_HANDSHAKE, 0);

		// Set buffer as 0
		*recv_buf = 0;
		t.length = 8;
		t.tx_buffer = NULL;
		t.rx_buffer = recv_buf;

		ret = spi_slave_transmit(RCV_HOST, &t, portMAX_DELAY);

		switch (*recv_buf) {
		case CAPTURE_CMD:
			printf("Capture Image command\n");
			if (esp32cam_capture_frame(send_buf))
				continue;
			break;
		case FLASH_ON_CMD:
			printf("Flash ON command\n");
			esp32cam_on_flash();
			continue;
		case FLASH_OFF_CMD:
			printf("Flash OFF command\n");
			esp32cam_off_flash();
			continue;
		default:
			printf("Invalid command recieved\n");
			continue;
		}

		// Let the RPI know image is ready
		gpio_set_level(GPIO_HANDSHAKE, 1);

		// Transmit the frame
		t.length = FRAME_SZ * 8;
		t.tx_buffer = send_buf;
		t.rx_buffer = NULL;

		ret = spi_slave_transmit(RCV_HOST, &t, portMAX_DELAY);
	}
}
