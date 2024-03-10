#include <stdio.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "sdkconfig.h"
#include "driver/uart.h"
#include "nimble/ble.h"

char *TAG = "BLE-Server";
uint8_t ble_addr_type;
void ble_app_advertise(void);
static uint16_t response_char_attr_handle;
bool lightIsOn = false;

#define RESPONSE_CHAR_UUID 0x1ABC

void send_ble_response(uint16_t conn_handle, uint16_t attr_handle, const char *response)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(response, strlen(response));
    int rc = ble_gattc_notify_custom(conn_handle, attr_handle, om);
    printf("Sending notification %d", strlen(response));

    if (rc != 0)
    {
        printf("Error sending notification: %d", rc);
    }
}

static int response_char_access_cb(uint16_t conn_handle,
                                   uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt,
                                   void *arg)
{
    printf("response_char_access_cb start");
    // This example is for a read operation; adjust as necessary for your use case.
    switch (ctxt->op)
    {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        printf("Read request on response characteristic");
        // You can provide a static value or update this value as needed.
        // For instance, "ACK" message if it's just to indicate readiness.
        const char *response = "ACK";
        int rc = os_mbuf_append(ctxt->om, response, strlen(response));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    // Add case for BLE_GATT_ACCESS_OP_WRITE_CHR if your characteristic should be writable.
    default:
        assert(0);
        return BLE_ATT_ERR_UNLIKELY;
    }
}

// Write data to ESP32 defined as server
static int device_write(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // printf("Data from the client: %.*s\n", ctxt->om->om_len, ctxt->om->om_data);
    char *data = (char *)ctxt->om->om_data;
    size_t data_len = ctxt->om->om_len;

    printf("%d\n", strcmp(data, (char *)"LIGHT ON"));
    if (strncmp(data, "LIGHTON", data_len) == 0 && data_len == strlen("LIGHTON"))
    {
        printf("LIGHT ON %d\n", gpio_get_level(GPIO_NUM_18));
        lightIsOn = !lightIsOn;
        gpio_set_level(GPIO_NUM_18, lightIsOn ? 1 : 0);
        send_ble_response(conn_handle, response_char_attr_handle, lightIsOn ? "LIGHTON" : "LIGHTOFF");
    }
    else if (strcmp(data, (char *)"LIGHT OFF\0") == 0)
    {
        printf("LIGHT OFF\n");
    }
    else if (strcmp(data, (char *)"FAN ON\0") == 0)
    {
        printf("FAN ON\n");
    }
    else if (strcmp(data, (char *)"FAN OFF\0") == 0)
    {
        printf("FAN OFF\n");
    }
    else
    {
        // printf("Data from the client: %.*s\n", ctxt->om->om_len, ctxt->om->om_data);
        printf("Data from the client: ");
        for (size_t i = 0; i < data_len; i++)
        {
            putchar(data[i]);
        }
        printf("\n");
    }

    return 0;
}

// Read data from ESP32 defined as server
static int device_read(uint16_t con_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    os_mbuf_append(ctxt->om, "Data from the server", strlen("Data from the server"));
    return 0;
}

// Array of pointers to other service definitions
// UUID - Universal Unique Identifier
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID16_DECLARE(0x180), // Define UUID for device type
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = BLE_UUID16_DECLARE(0xFEF4), // Define UUID for reading
          .flags = BLE_GATT_CHR_F_READ,
          .access_cb = device_read},
         {.uuid = BLE_UUID16_DECLARE(0xDEAD), // Define UUID for writing
          .flags = BLE_GATT_CHR_F_WRITE,
          .access_cb = device_write},
         {
             .uuid = BLE_UUID16_DECLARE(RESPONSE_CHAR_UUID),
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &response_char_attr_handle,
             .access_cb = response_char_access_cb,
         },
         {0}}},
    {0}};

// BLE event handling
static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
    // Advertise if connected
    case BLE_GAP_EVENT_CONNECT:
        printf("GAP_BLE GAP EVENT CONNECT %s", event->connect.status == 0 ? "OK!" : "FAILED!");
        if (event->connect.status != 0)
        {
            ble_app_advertise();
        }
        break;
    // Advertise again after completion of the event
    case BLE_GAP_EVENT_DISCONNECT:
        printf("GAP_BLE GAP EVENT DISCONNECTED");
        ble_app_advertise();
        lightIsOn = false;
        gpio_set_level(GPIO_NUM_18, 0);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        printf("GAP_BLE GAP EVENT");
        ble_app_advertise();
        break;
    default:
        break;
    }
    return 0;
}

// Define the BLE connection
void ble_app_advertise(void)
{
    // GAP - device name definition
    struct ble_hs_adv_fields fields;
    const char *device_name;
    memset(&fields, 0, sizeof(fields));
    device_name = ble_svc_gap_device_name(); // Read the BLE device name
    fields.name = (uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    // GAP - device connectivity definition
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; // connectable or non-connectable
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // discoverable or non-discoverable
    ble_gap_adv_start(ble_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
}

// The application
void ble_app_on_sync(void)
{
    ble_hs_id_infer_auto(0, &ble_addr_type); // Determines the best address type automatically
    ble_app_advertise();                     // Define the BLE connection
}

// The infinite task
void host_task(void *param)
{
    nimble_port_run(); // This function will return only when nimble_port_stop() is executed
}

void app_main()
{
    gpio_set_direction(GPIO_NUM_18, GPIO_MODE_OUTPUT);

    uart_set_baudrate(UART_NUM_0, 115200);
    nvs_flash_init(); // 1 - Initialize NVS flash using
    // esp_nimble_hci_and_controller_init();      // 2 - Initialize ESP controller
    nimble_port_init();                   // 3 - Initialize the host stack
    ble_svc_gap_device_name_set(TAG);     // 4 - Initialize NimBLE configuration - server name
    ble_svc_gap_init();                   // 4 - Initialize NimBLE configuration - gap service
    ble_svc_gatt_init();                  // 4 - Initialize NimBLE configuration - gatt service
    ble_gatts_count_cfg(gatt_svcs);       // 4 - Initialize NimBLE configuration - config gatt services
    ble_gatts_add_svcs(gatt_svcs);        // 4 - Initialize NimBLE configuration - queues gatt services.
    ble_hs_cfg.sync_cb = ble_app_on_sync; // 5 - Initialize application
    nimble_port_freertos_init(host_task); // 6 - Run the thread
}