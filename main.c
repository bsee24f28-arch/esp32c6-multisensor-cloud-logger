#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include <errno.h>

/* ---- NEW: WiFi + HTTP client + NVS (needed for cloud upload) ---- */
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"

static const char *TAG = "MAIN";

/* ================= ULTRASONIC (HC-SR04) ================= */
#define ULTRA_TRIG_PIN GPIO_NUM_22
#define ULTRA_ECHO_PIN GPIO_NUM_23

/* ================= MPU6050 (I2C) ================= */
#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_SDA_IO   GPIO_NUM_6
#define I2C_MASTER_SCL_IO   GPIO_NUM_7
#define I2C_MASTER_FREQ_HZ  400000
#define I2C_MASTER_TIMEOUT_MS 1000

#define MPU6050_ADDR        0x68
#define MPU6050_REG_PWR_MGMT_1   0x6B
#define MPU6050_REG_ACCEL_XOUT_H 0x3B

/* ================= SD CARD (SPI) ================= */
#define SD_MOSI_PIN  GPIO_NUM_18
#define SD_MISO_PIN  GPIO_NUM_19
#define SD_SCK_PIN   GPIO_NUM_20
#define SD_CS_PIN    GPIO_NUM_21
#define SD_SPI_HOST  SPI2_HOST
#define MOUNT_POINT  "/sdcard"

/* ================= WIFI ================= */
#define WIFI_SSID           "your-wifi"
#define WIFI_PASS           "passsward"
#define WIFI_MAX_RETRY      5
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

/* ================= THINGSPEAK ================= */
#define THINGSPEAK_WRITE_API_KEY "your-APIKEY"
#define THINGSPEAK_URL            "http://api.thingspeak.com/update"
#define THINGSPEAK_MIN_INTERVAL_S 15   /* free tier: 1 update per 15s min */

/* ================= STATUS LED ================= */
/* Not used by any other peripheral above (22,23,6,7,18,19,20,21 are taken).
   If your board wires an LED elsewhere, just change this one line. */
#define STATUS_LED_PIN GPIO_NUM_2

static sdmmc_card_t *sd_card = NULL;

/* ---------------------------------------------------------------- */
/*  STATUS LED                                                       */
/* ---------------------------------------------------------------- */
static void status_led_init(void)
{
    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << STATUS_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_config);
    gpio_set_level(STATUS_LED_PIN, 0);
    ESP_LOGI(TAG, "Status LED initialized (GPIO%d)", STATUS_LED_PIN);
}

static void status_led_blink(int times, int on_ms, int off_ms)
{
    for (int i = 0; i < times; i++) {
        gpio_set_level(STATUS_LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        gpio_set_level(STATUS_LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

/* ---------------------------------------------------------------- */
/*  WIFI                                                              */
/* ---------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Retrying WiFi connection (%d/%d)...", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                          &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s ...", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully.");
        return true;
    } else {
        ESP_LOGE(TAG, "WiFi connection failed after %d retries.", WIFI_MAX_RETRY);
        return false;
    }
}

/* ---------------------------------------------------------------- */
/*  THINGSPEAK UPLOAD                                                */
/* ---------------------------------------------------------------- */
static bool thingspeak_upload(float distance,
                               float ax, float ay, float az,
                               float gx, float gy, float gz)
{
    char url[300];
    snprintf(url, sizeof(url),
             "%s?api_key=%s&field1=%.2f&field2=%.2f&field3=%.2f"
             "&field4=%.2f&field5=%.2f&field6=%.2f&field7=%.2f",
             THINGSPEAK_URL, THINGSPEAK_WRITE_API_KEY,
             distance, ax, ay, az, gx, gy, gz);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    bool success = false;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "ThingSpeak upload -> HTTP %d, len=%d", status, content_length);
        if (status == 200) {
            success = true;
        } else {
            ESP_LOGE(TAG, "ThingSpeak rejected the update (check API key / rate limit)");
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return success;
}

/* ---------------------------------------------------------------- */
/*  SENSORS (unchanged from your original)                           */
/* ---------------------------------------------------------------- */
static void ultrasonic_init(void)
{
    gpio_config_t trig_config = {
        .pin_bit_mask = (1ULL << ULTRA_TRIG_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&trig_config);
    gpio_set_level(ULTRA_TRIG_PIN, 0);

    gpio_config_t echo_config = {
        .pin_bit_mask = (1ULL << ULTRA_ECHO_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&echo_config);

    ESP_LOGI(TAG, "Ultrasonic sensor initialized (TRIG=GPIO22, ECHO=GPIO23)");
}

static float ultrasonic_read_distance(void)
{
    gpio_set_level(ULTRA_TRIG_PIN, 0);
    esp_rom_delay_us(2);

    gpio_set_level(ULTRA_TRIG_PIN, 1);
    esp_rom_delay_us(10);
    gpio_set_level(ULTRA_TRIG_PIN, 0);

    int64_t wait_start = esp_timer_get_time();
    while (gpio_get_level(ULTRA_ECHO_PIN) == 0) {
        if ((esp_timer_get_time() - wait_start) > 50000) {
            return -1.0f;
        }
    }

    int64_t echo_start = esp_timer_get_time();
    while (gpio_get_level(ULTRA_ECHO_PIN) == 1) {
        if ((esp_timer_get_time() - echo_start) > 50000) {
            return -1.0f;
        }
    }

    int64_t echo_duration = esp_timer_get_time() - echo_start;
    float distance = (echo_duration * 0.0343f) / 2.0f;
    return distance;
}

static esp_err_t i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return err;
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

static esp_err_t mpu6050_register_write(uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_write_to_device(I2C_MASTER_NUM, MPU6050_ADDR, write_buf,
                                       sizeof(write_buf),
                                       I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}

static esp_err_t mpu6050_register_read(uint8_t reg_addr, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(I2C_MASTER_NUM, MPU6050_ADDR, &reg_addr, 1,
                                         data, len,
                                         I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}

static esp_err_t mpu6050_init(void)
{
    return mpu6050_register_write(MPU6050_REG_PWR_MGMT_1, 0x00);
}

typedef struct {
    float accel_x, accel_y, accel_z;
    float gyro_x, gyro_y, gyro_z;
} mpu6050_data_t;

static esp_err_t mpu6050_read(mpu6050_data_t *out)
{
    uint8_t raw[14];
    esp_err_t err = mpu6050_register_read(MPU6050_REG_ACCEL_XOUT_H, raw, sizeof(raw));
    if (err != ESP_OK) return err;

    int16_t ax = (raw[0] << 8) | raw[1];
    int16_t ay = (raw[2] << 8) | raw[3];
    int16_t az = (raw[4] << 8) | raw[5];
    int16_t gx = (raw[8] << 8) | raw[9];
    int16_t gy = (raw[10] << 8) | raw[11];
    int16_t gz = (raw[12] << 8) | raw[13];

    out->accel_x = ax / 16384.0f;
    out->accel_y = ay / 16384.0f;
    out->accel_z = az / 16384.0f;
    out->gyro_x = gx / 131.0f;
    out->gyro_y = gy / 131.0f;
    out->gyro_z = gz / 131.0f;

    return ESP_OK;
}

static esp_err_t sd_card_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus for SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = SD_SPI_HOST;

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. Card may need formatting.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    sdmmc_card_print_info(stdout, sd_card);
    return ESP_OK;
}

static void sd_log_line(const char *line)
{
    FILE *f = fopen(MOUNT_POINT "/log.csv", "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open log.csv: %s", strerror(errno));
        return;
    }
    fprintf(f, "%s\n", line);
    fclose(f);
    ESP_LOGI(TAG, "Logged to SD card successfully");
}

/* ---------------------------------------------------------------- */
/*  MAIN                                                              */
/* ---------------------------------------------------------------- */
void app_main(void)
{
    /* NVS is required by the WiFi driver to store calibration data */
   
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    status_led_init();
    ultrasonic_init();

    ESP_ERROR_CHECK(i2c_master_init());
    esp_err_t mpu_status = mpu6050_init();
    if (mpu_status != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 init failed: %s", esp_err_to_name(mpu_status));
    } else {
        ESP_LOGI(TAG, "MPU6050 initialized (SDA=GPIO6, SCL=GPIO7)");
    }

    esp_err_t sd_status = sd_card_init();
    if (sd_status != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed, logging to SD will be skipped");
    } else {
        sd_log_line("timestamp_us,distance_cm,accel_x,accel_y,accel_z,gyro_x,gyro_y,gyro_z");
    }

    bool wifi_ok = wifi_init_sta();
    if (wifi_ok) {
        status_led_blink(2, 150, 150); /* two quick blinks = WiFi connected */
    }

    int seconds_since_upload = THINGSPEAK_MIN_INTERVAL_S; /* upload on first loop */

    while (1) {
        float distance = ultrasonic_read_distance();
        mpu6050_data_t imu = {0};
        esp_err_t imu_err = mpu6050_read(&imu);

        if (distance >= 0.0f) {
            ESP_LOGI(TAG, "Distance: %.2f cm", distance);
        } else {
            ESP_LOGE(TAG, "Ultrasonic reading timeout!");
        }

        if (imu_err == ESP_OK) {
            ESP_LOGI(TAG, "Accel(g): X=%.2f Y=%.2f Z=%.2f  Gyro(dps): X=%.2f Y=%.2f Z=%.2f",
                     imu.accel_x, imu.accel_y, imu.accel_z,
                     imu.gyro_x, imu.gyro_y, imu.gyro_z);
        } else {
            ESP_LOGE(TAG, "MPU6050 read failed: %s", esp_err_to_name(imu_err));
        }

        if (sd_status == ESP_OK) {
            char line[160];
            snprintf(line, sizeof(line), "%lld,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f",
                     (long long)esp_timer_get_time(),
                     distance,
                     imu.accel_x, imu.accel_y, imu.accel_z,
                     imu.gyro_x, imu.gyro_y, imu.gyro_z);
            sd_log_line(line);
        }

        /* Only push to the cloud every THINGSPEAK_MIN_INTERVAL_S seconds,
           even though we log to SD every second. ThingSpeak's free tier
           will reject faster updates than this. */
        if (wifi_ok && seconds_since_upload >= THINGSPEAK_MIN_INTERVAL_S) {
            bool uploaded = thingspeak_upload(distance,
                                               imu.accel_x, imu.accel_y, imu.accel_z,
                                               imu.gyro_x, imu.gyro_y, imu.gyro_z);
            if (uploaded) {
                status_led_blink(1, 300, 0); /* one solid blink = data uploaded */
            }
            seconds_since_upload = 0;
        }
        seconds_since_upload++;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}