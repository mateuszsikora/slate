/*
 * Slate — minimal CH422G output driver on ESP-IDF's current I2C master API.
 */

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/i2c_master.h"
#include "esp_check.h"

#include "slate_ch422g.h"

static const char *TAG = "slate_ch422g";

/* CH422G selects registers with separate 7-bit I2C addresses. Slate uses only
 * WR-SET and the eight push-pull outputs in WR-IO. */
#define CH422G_ADDR_WR_SET (0x48 >> 1)
#define CH422G_ADDR_WR_IO  (0x70 >> 1)

#define CH422G_WR_SET_IO_OE UINT8_C(1)
#define CH422G_OUTPUTS_RESET UINT8_C(0xFF)

#define CH422G_I2C_HZ         400000
#define CH422G_I2C_TIMEOUT_MS 10
#define CH422G_LOCK_TIMEOUT_MS 100

struct slate_ch422g {
    i2c_master_dev_handle_t set_device;
    i2c_master_dev_handle_t output_device;
    SemaphoreHandle_t lock;
    uint8_t outputs;
};

static esp_err_t add_device(i2c_master_bus_handle_t bus, uint16_t address,
                            i2c_master_dev_handle_t *out_device)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = CH422G_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &config, out_device);
}

static esp_err_t write_byte(i2c_master_dev_handle_t device, uint8_t value)
{
    return i2c_master_transmit(device, &value, sizeof(value), CH422G_I2C_TIMEOUT_MS);
}

static void remove_device(i2c_master_dev_handle_t *device)
{
    if (*device) {
        i2c_master_bus_rm_device(*device);
        *device = NULL;
    }
}

esp_err_t slate_ch422g_new(i2c_master_bus_handle_t bus, slate_ch422g_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(bus && out_handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    *out_handle = NULL;

    slate_ch422g_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "allocate driver");

    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = add_device(bus, CH422G_ADDR_WR_SET, &handle->set_device);
    if (err == ESP_OK) {
        err = add_device(bus, CH422G_ADDR_WR_IO, &handle->output_device);
    }
    if (err == ESP_OK) {
        err = write_byte(handle->set_device, CH422G_WR_SET_IO_OE);
    }
    if (err == ESP_OK) {
        err = write_byte(handle->output_device, CH422G_OUTPUTS_RESET);
    }
    if (err != ESP_OK) {
        remove_device(&handle->output_device);
        remove_device(&handle->set_device);
        vSemaphoreDelete(handle->lock);
        free(handle);
        return err;
    }

    handle->outputs = CH422G_OUTPUTS_RESET;
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t slate_ch422g_set_level(slate_ch422g_handle_t handle, uint8_t pin_mask, bool high)
{
    ESP_RETURN_ON_FALSE(handle && pin_mask, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    if (xSemaphoreTake(handle->lock, pdMS_TO_TICKS(CH422G_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint8_t next = high ? (uint8_t) (handle->outputs | pin_mask)
                              : (uint8_t) (handle->outputs & (uint8_t) ~pin_mask);
    esp_err_t err = ESP_OK;
    if (next != handle->outputs) {
        err = write_byte(handle->output_device, next);
        if (err == ESP_OK) {
            handle->outputs = next;
        }
    }

    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t slate_ch422g_del(slate_ch422g_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    if (handle->output_device) {
        result = i2c_master_bus_rm_device(handle->output_device);
        handle->output_device = NULL;
    }
    if (handle->set_device) {
        esp_err_t err = i2c_master_bus_rm_device(handle->set_device);
        if (result == ESP_OK) {
            result = err;
        }
        handle->set_device = NULL;
    }

    vSemaphoreDelete(handle->lock);
    free(handle);
    return result;
}
