#include "usbd_core.h"
#include "usbd_cdc.h"

/*!< endpoint address */
#define CDC_IN_EP  0x81
#define CDC_OUT_EP 0x02
#define CDC_INT_EP 0x83

#define USBD_VID           0x36b7
#define USBD_PID           0xFFFF
#define USBD_MAX_POWER     100
#define USBD_LANGID_STRING 1033

/*!< config descriptor size */
#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)

uint8_t dma_in_ep_idx  = (CDC_IN_EP & 0x7f);
uint8_t dma_out_ep_idx = CDC_OUT_EP;

/*!< global descriptor */
static const uint8_t cdc_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, 0x02),
    USB_LANGID_INIT(USBD_LANGID_STRING),
    0x0A, USB_DESCRIPTOR_TYPE_STRING,
    'P', 0x00, 'U', 0x00, 'Y', 0x00, 'A', 0x00,
    0x1C, USB_DESCRIPTOR_TYPE_STRING,
    'P', 0x00, 'U', 0x00, 'Y', 0x00, 'A', 0x00, ' ', 0x00,
    'C', 0x00, 'D', 0x00, 'C', 0x00, ' ', 0x00, 'D', 0x00, 'E', 0x00,
    'M', 0x00, 'O', 0x00,
    0x16, USB_DESCRIPTOR_TYPE_STRING,
    '2', 0x00, '0', 0x00, '2', 0x00, '2', 0x00, '1', 0x00,
    '2', 0x00, '3', 0x00, '4', 0x00, '5', 0x00, '6', 0x00,
#ifdef CONFIG_USB_HS
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00,
#endif
    0x00
};

USB_MEM_ALIGNX uint8_t read_buffer[128];
static cdc_acm_rx_buf_t client_rx_buf = {0};

volatile bool ep_tx_busy_flag = false;
volatile uint8_t dtr_enable = 0;

#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

void usbd_configure_done_callback(void)
{
    usbd_ep_start_read(CDC_OUT_EP, read_buffer, sizeof(read_buffer));
}

void usbd_cdc_acm_bulk_out(uint8_t ep, uint32_t nbytes)
{
    cdc_acm_rx_buf_t *rx_buf = &client_rx_buf;
    if (nbytes && rx_buf->buf)
    {
        const uint8_t *buf = read_buffer;
        uint32_t pointer = *rx_buf->write_pointer;
        while (nbytes)
        {
            const uint32_t rem = rx_buf->size - pointer;
            if (rem == 0u)
            {
                pointer = 0u;
                continue;
            }

            const uint32_t size = rem < nbytes ? rem : nbytes;
            memcpy(rx_buf->buf + pointer, buf, size);
            buf += size;
            nbytes -= size;
            pointer += size;
        }

        /* Keep the ISR-visible producer index canonical: [0, size). */
        if (pointer == rx_buf->size)
            pointer = 0u;
        *rx_buf->write_pointer = pointer;
    }

    usbd_ep_start_read(CDC_OUT_EP, read_buffer, sizeof(read_buffer));
}

void usbd_cdc_acm_bulk_in(uint8_t ep, uint32_t nbytes)
{
    if ((nbytes % CDC_MAX_MPS) == 0 && nbytes) {
        usbd_ep_start_write(CDC_IN_EP, NULL, 0);
    } else {
        ep_tx_busy_flag = false;
    }
}

struct usbd_endpoint cdc_out_ep = {
    .ep_addr = CDC_OUT_EP,
    .ep_cb = usbd_cdc_acm_bulk_out
};

struct usbd_endpoint cdc_in_ep = {
    .ep_addr = CDC_IN_EP,
    .ep_cb = usbd_cdc_acm_bulk_in
};

struct usbd_interface intf0;
struct usbd_interface intf1;

int cdc_acm_init(cdc_acm_rx_buf_t rx_buf)
{
    memcpy(&client_rx_buf, &rx_buf, sizeof(client_rx_buf));
    *client_rx_buf.write_pointer = 0u;
    ep_tx_busy_flag = false;
    dtr_enable = 0u;

    usbd_desc_register(cdc_descriptor);
    usbd_add_interface(usbd_cdc_acm_init_intf(&intf0));
    usbd_add_interface(usbd_cdc_acm_init_intf(&intf1));
    usbd_add_endpoint(&cdc_out_ep);
    usbd_add_endpoint(&cdc_in_ep);
    return usbd_initialize();
}

void usbd_cdc_acm_set_dtr(uint8_t intf, bool dtr)
{
    dtr_enable = dtr ? 1u : 0u;
}

void cdc_acm_data_send_with_dtr(const uint8_t *buf, uint32_t size)
{
    if (dtr_enable && size != 0u)
    {
        ep_tx_busy_flag = true;
        usbd_ep_start_write(CDC_IN_EP, buf, size);
        uint32_t timeout = 100000u;
        while (ep_tx_busy_flag && --timeout)
            ;
        if (!timeout) {
            ep_tx_busy_flag = false;
            dtr_enable = 0u;
        }
    }
}

void cdc_acm_data_send_with_dtr_async(const uint8_t *buf, uint32_t size)
{
    if (size != 0u)
        usbd_ep_start_write(CDC_IN_EP, buf, size);
}
