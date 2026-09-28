#include <ctype.h>
#include <device.h>
#include <drivers/can.h>
#include <drivers/gpio.h>
#include <drivers/uart.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/atomic.h>
#include <sys/printk.h>
#include <sys/util.h>
#include <usb/usb_device.h>
#include <zephyr.h>

#define LED0_NODE DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(LED0_NODE, okay)
#define LED_PORT DT_GPIO_LABEL(LED0_NODE, gpios)
#define LED_PIN DT_GPIO_PIN(LED0_NODE, gpios)
#define LED_FLAGS DT_GPIO_FLAGS(LED0_NODE, gpios)
#else
#error "LED0 node not found in device tree"
#endif

/* ========================================================================= */
/* Identificadores Base CAN                                                  */
/* ========================================================================= */
#define CAN_COMMAND_BASE_ID 0x200U
#define CAN_STATUS_BASE_ID  0x280U
#define CAN_POS_BASE_ID     0x290U
#define CAN_EVENT_BASE_ID   0x300U
#define CAN_ID_GROUP_MASK   0x7F0U
#define CAN_NODE_ID_MIN     1U
#define CAN_NODE_ID_MAX     10U

/* Os filtros 0x7F0 reservam blocos de 16 IDs. Impedir que uma alteração
 * futura no limite faça status, posição e eventos ocuparem a mesma faixa. */
BUILD_ASSERT(CAN_NODE_ID_MAX <= 0x0FU,
             "CAN node IDs must fit inside one 0x7F0 filter group");
BUILD_ASSERT(CAN_STATUS_BASE_ID + CAN_NODE_ID_MAX < CAN_POS_BASE_ID,
             "CAN status and position ID ranges overlap");
BUILD_ASSERT(CAN_POS_BASE_ID + CAN_NODE_ID_MAX < CAN_EVENT_BASE_ID,
             "CAN position and event ID ranges overlap");

#define USB_CDC_NODE DT_NODELABEL(cdc_acm_uart0)
#define CAN_PRIMARY_NODE DT_CHOSEN(zephyr_can_primary)

/* OpCodes CAN (Master -> Slaves) */
#define CAN_OP_PING           0x01U
#define CAN_OP_STATUS_REQUEST 0x02U
#define CAN_OP_ENABLE         0x10U
#define CAN_OP_SPEED          0x11U
#define CAN_OP_AXIS_SPEED     0x12U
#define CAN_OP_AXIS_ACCEL     0x13U
#define CAN_OP_MOVE_PROFILE   0x14U
#define CAN_OP_MOVE           0x20U
#define CAN_OP_HOME           0x21U
#define CAN_OP_MOVE_FORCE     0x22U
#define CAN_OP_MOVE_SYNC      0x23U
#define CAN_OP_STOP           0x24U /* [0x24, flags]; bit0 = apagar lasers (E-STOP) */
#define CAN_OP_LASER          0x30U
#define CAN_OP_FAN            0x31U
#define CAN_OP_OTA_START      0x40U
#define CAN_OP_OTA_DATA       0x41U
#define CAN_OP_OTA_END        0x42U
#define CAN_OP_OTA_ABORT      0x43U

/* Eventos CAN (Slaves -> Master) */
#define CAN_EVT_HEARTBEAT     0x80U
#define CAN_EVT_PONG          0x81U
#define CAN_EVT_STATUS        0x82U
#define CAN_EVT_ACK           0x83U
#define CAN_EVT_DONE          0x84U
#define CAN_EVT_OTA_READY     0x90U
#define CAN_EVT_OTA_PROGRESS  0x91U
#define CAN_EVT_OTA_DONE      0x92U
#define CAN_EVT_OTA_ERROR     0x93U
#define CAN_EVT_ERROR         0xE0U

/* Byte 2 do CAN_EVT_STATUS: o frame de posição seguinte do mesmo nó usa o formato v2
 * (C/A int16 em décimos de grau, com sinal). Sem o bit: v1 (uint16 centésimos 0..360). */
#define CAN_STATUS_FLAG_POS_V2 0x40U
#define STOP_FLAG_LASERS_OFF   0x01U

/* Tentativas de 100 us para obter o slot de TX: comandos comuns esperam até 1 ms;
 * o STOP espera até 20 ms para não ser descartado atrás de um frame em andamento. */
#define CAN_TX_BUSY_RETRIES_NORMAL 10
#define CAN_TX_BUSY_RETRIES_STOP   200

/* ========================================================================= */
/* Configuração da Fita de LED WS2812 (12 LEDs no Pino 14 do Teensy 4.1)     */
/* ========================================================================= */
#define NUM_LEDS 12
#define WS2812_PORT_NAME "GPIO_1"
#define WS2812_PIN_NUM   18   /* Pino 14 do Teensy 4.1 = GPIO1_IO18 */

#define WS2812_GPIO_BASE 0x401B8000U
#define WS2812_DR_SET    (*(volatile uint32_t *)(WS2812_GPIO_BASE + 0x84U))
#define WS2812_DR_CLEAR  (*(volatile uint32_t *)(WS2812_GPIO_BASE + 0x88U))
#define WS2812_PIN_MASK  (1U << WS2812_PIN_NUM)

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

static rgb_color_t leds[NUM_LEDS];

/* Estado de atividade dos nós e barramentos */
typedef struct {
    uint32_t last_seen_ms;
    uint32_t cmd_flash_until_ms;
    uint32_t error_flash_until_ms;
    uint32_t ota_flash_until_ms;
} node_led_state_t;

typedef struct {
    int32_t status;
    uint8_t node_id;
} can_tx_result_t;

static node_led_state_t node_states[CAN_NODE_ID_MAX + 1U];
/* Formato de posição anunciado pelo último STATUS de cada nó (ver CAN_STATUS_FLAG_POS_V2) */
static atomic_t node_pos_v2[CAN_NODE_ID_MAX + 1U];
static uint32_t can_last_activity_ms = 0;
static uint32_t can_flash_until_ms = 0;
static uint32_t usb_last_activity_ms = 0;
static uint32_t usb_flash_until_ms = 0;
static bool can_activity_seen = false;
static bool usb_activity_seen = false;

/* Fila de mensagens para recepção de pacotes CAN */
CAN_DEFINE_MSGQ(can_rx_msgq, 32);
K_MSGQ_DEFINE(can_tx_result_msgq, sizeof(can_tx_result_t), 8, 4);
K_MUTEX_DEFINE(usb_tx_mutex);
static atomic_t can_tx_pending;
static atomic_t can_raw_trace_enabled;

/* Pilhas de execução das threads secundárias */
K_THREAD_STACK_DEFINE(can_rx_stack_area, 2048);
K_THREAD_STACK_DEFINE(blink_stack_area, 512);
K_THREAD_STACK_DEFINE(led_strip_stack_area, 1024);

struct k_thread can_rx_thread_data;
struct k_thread blink_thread_data;
struct k_thread led_strip_thread_data;
static const struct device *const usb_cdc_dev = DEVICE_DT_GET(USB_CDC_NODE);
static const struct device *const can_primary_dev = DEVICE_DT_GET(CAN_PRIMARY_NODE);

/* ========================================================================= */
/* Notificações de Atividade para Animação dos LEDs                          */
/* ========================================================================= */
static void notify_node_activity(unsigned node_id) {
    if (node_id >= CAN_NODE_ID_MIN && node_id <= CAN_NODE_ID_MAX) {
        node_states[node_id].last_seen_ms = k_uptime_get_32();
    }
}

static void notify_node_command(unsigned node_id) {
    uint32_t now = k_uptime_get_32();
    if (node_id >= CAN_NODE_ID_MIN && node_id <= CAN_NODE_ID_MAX) {
        node_states[node_id].cmd_flash_until_ms = now + 180;
    }
}

static void notify_node_error(unsigned node_id) {
    uint32_t now = k_uptime_get_32();
    if (node_id >= CAN_NODE_ID_MIN && node_id <= CAN_NODE_ID_MAX) {
        node_states[node_id].error_flash_until_ms = now + 1500;
    }
}

static void notify_node_ota(unsigned node_id) {
    uint32_t now = k_uptime_get_32();
    if (node_id >= CAN_NODE_ID_MIN && node_id <= CAN_NODE_ID_MAX) {
        node_states[node_id].last_seen_ms = now;
        node_states[node_id].ota_flash_until_ms = now + 4000;
    } else if (node_id == 0U) {
        for (unsigned i = CAN_NODE_ID_MIN; i <= CAN_NODE_ID_MAX; i++) {
            node_states[i].last_seen_ms = now;
            node_states[i].ota_flash_until_ms = now + 4000;
        }
    }
}

static void notify_usb_activity(void) {
    uint32_t now = k_uptime_get_32();
    usb_last_activity_ms = now;
    usb_flash_until_ms = now + 120;
    usb_activity_seen = true;
}

static void notify_can_activity(void) {
    uint32_t now = k_uptime_get_32();
    can_last_activity_ms = now;
    can_flash_until_ms = now + 100;
    can_activity_seen = true;
}

static bool deadline_is_pending(uint32_t now, uint32_t deadline) {
    return (int32_t)(deadline - now) > 0;
}

/* ========================================================================= */
/* Transmissor WS2812 de Alta Precisão (Cortex-M7 @ 600 MHz)                */
/* ========================================================================= */
static bool ws2812_init_hardware(void) {
    const struct device *gpio_dev = device_get_binding(WS2812_PORT_NAME);
    uint32_t cycle_start;

    if (gpio_dev != NULL) {
        gpio_pin_configure(gpio_dev, WS2812_PIN_NUM, GPIO_OUTPUT_INACTIVE);
    }
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();

    /* Alguns estados de debug/boot deixam o DWT bloqueado. Sem esta prova,
     * os waits de timing abaixo nunca terminam com as IRQs desabilitadas e
     * congelam também USB e CAN. */
    cycle_start = DWT->CYCCNT;
    for (volatile unsigned i = 0; i < 32U; i++) {
        __NOP();
    }
    if (DWT->CYCCNT == cycle_start) {
        return false;
    }

    WS2812_DR_CLEAR = WS2812_PIN_MASK;
    return true;
}

static inline void ws2812_send_pixel(uint8_t r, uint8_t g, uint8_t b) {
    /* Formato padrão WS2812: Ordem GRB (24 bits) */
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
    for (int bit = 23; bit >= 0; bit--) {
        if ((grb >> bit) & 1U) {
            /* Bit 1: 700ns HIGH (~420 ciclos @ 600MHz), 550ns LOW (~330 ciclos) */
            WS2812_DR_SET = WS2812_PIN_MASK;
            uint32_t start = DWT->CYCCNT;
            while ((DWT->CYCCNT - start) < 420U);
            WS2812_DR_CLEAR = WS2812_PIN_MASK;
            while ((DWT->CYCCNT - start) < 750U);
        } else {
            /* Bit 0: 350ns HIGH (~210 ciclos @ 600MHz), 900ns LOW (~540 ciclos) */
            WS2812_DR_SET = WS2812_PIN_MASK;
            uint32_t start = DWT->CYCCNT;
            while ((DWT->CYCCNT - start) < 210U);
            WS2812_DR_CLEAR = WS2812_PIN_MASK;
            while ((DWT->CYCCNT - start) < 750U);
        }
    }
}

static void ws2812_show(void) {
    unsigned int key = irq_lock();
    for (int i = 0; i < NUM_LEDS; i++) {
        ws2812_send_pixel(leds[i].r, leds[i].g, leds[i].b);
    }
    WS2812_DR_CLEAR = WS2812_PIN_MASK;
    irq_unlock(key);
}

/* ========================================================================= */
/* Thread de Animação e Atualização dos 12 LEDs                              */
/* ========================================================================= */
void led_strip_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    if (!ws2812_init_hardware()) {
        printk("Aviso: DWT indisponivel; LEDs WS2812 desativados.\n");
        return;
    }
    uint32_t tick = 0;

    while (1) {
        uint32_t now = k_uptime_get_32();
        tick++;

        /* ----------------------------------------------------------------- */
        /* LED 0: Status do Barramento CAN                                   */
        /* ----------------------------------------------------------------- */
        if (deadline_is_pending(now, can_flash_until_ms)) {
            leds[0] = (rgb_color_t){0, 100, 255}; /* Piscada Azul (Atividade) */
        } else if (can_activity_seen && now - can_last_activity_ms < 2500) {
            leds[0] = (rgb_color_t){0, 180, 0};   /* Verde Fixo (CAN OK) */
        } else {
            leds[0] = (rgb_color_t){180, 0, 0};   /* Vermelho (Sem tráfego) */
        }

        /* ----------------------------------------------------------------- */
        /* LED 1: Status da Conexão Serial USB CDC-ACM                      */
        /* ----------------------------------------------------------------- */
        if (deadline_is_pending(now, usb_flash_until_ms)) {
            leds[1] = (rgb_color_t){0, 100, 255}; /* Piscada Azul (Comando RX/TX) */
        } else if (usb_activity_seen && now - usb_last_activity_ms < 4000) {
            leds[1] = (rgb_color_t){0, 180, 0};   /* Verde (USB Conectada) */
        } else {
            leds[1] = (rgb_color_t){60, 20, 0};   /* Laranja/Âmbar (Aguardando) */
        }

        /* ----------------------------------------------------------------- */
        /* LEDs 2 a 11: Status dos 10 Nós (Node 1 a Node 10)                 */
        /* ----------------------------------------------------------------- */
        for (unsigned node = CAN_NODE_ID_MIN; node <= CAN_NODE_ID_MAX; node++) {
            unsigned led_idx = node + 1U; /* LED 2 = Node 1, ..., LED 11 = Node 10 */

            if (deadline_is_pending(now, node_states[node].error_flash_until_ms)) {
                /* ERRO: Vermelho piscante rápido */
                leds[led_idx] = (tick % 4 < 2) ? (rgb_color_t){255, 0, 0} : (rgb_color_t){40, 0, 0};
            } else if (deadline_is_pending(now, node_states[node].ota_flash_until_ms)) {
                /* ATUALIZAÇÃO OTA: Ciano pulsante rápido (~4 Hz) */
                leds[led_idx] = (tick % 6 < 3) ? (rgb_color_t){0, 180, 240} : (rgb_color_t){0, 25, 45};
            } else if (deadline_is_pending(now, node_states[node].cmd_flash_until_ms)) {
                /* COMANDO OK: Piscada rápida Azul */
                leds[led_idx] = (rgb_color_t){0, 60, 255};
            } else if (node_states[node].last_seen_ms != 0U &&
                       now - node_states[node].last_seen_ms < 2500) {
                /* ONLINE: Verde piscante rápido (4 Hz) */
                leds[led_idx] = (tick % 6 < 3) ? (rgb_color_t){0, 200, 10} : (rgb_color_t){0, 20, 0};
            } else {
                /* OFFLINE: Vermelho fraco estático */
                leds[led_idx] = (rgb_color_t){30, 0, 0};
            }
        }

        ws2812_show();
        k_msleep(30); /* ~33 FPS para transições suaves */
    }
}

/* ========================================================================= */
/* Comunicação Serial USB CDC-ACM e CAN                                     */
/* ========================================================================= */
static bool usb_serial_is_open(const struct device *dev) {
    uint32_t dtr = 0U;

    return dev != NULL &&
           uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr != 0U;
}

static void write_usb_serial(const struct device *dev, const char *str) {
    if (dev == NULL) {
        return;
    }

    /* O CDC ACM do Zephyr 2.7 implementa poll_out sem bloqueio: se o USB não
     * estiver pronto ele descarta o byte, e se o ring buffer lotar substitui o
     * byte mais antigo. Não condicionar a DTR mantém compatibilidade com hosts
     * Windows que não propagam essa linha de controle. */
    k_mutex_lock(&usb_tx_mutex, K_FOREVER);
    while (*str) {
        uart_poll_out(dev, *str++);
    }
    k_mutex_unlock(&usb_tx_mutex);
    notify_usb_activity();
}

static bool can_node_id_is_valid(unsigned node_id) {
    /* node 0 é o ID de broadcast da aplicação. */
    return node_id <= CAN_NODE_ID_MAX;
}

static bool can_unicast_node_id_is_valid(unsigned node_id) {
    return node_id >= CAN_NODE_ID_MIN && node_id <= CAN_NODE_ID_MAX;
}

static bool can_id_to_node(uint32_t frame_id, uint32_t base_id,
                           unsigned *node_id) {
    if (frame_id <= base_id || frame_id > (base_id + CAN_NODE_ID_MAX)) {
        return false;
    }

    *node_id = (unsigned)(frame_id - base_id);
    return true;
}

static bool can_axis_is_valid(char axis) {
    return axis == 'C' || axis == 'A' || axis == 'Z';
}

static char can_axis_to_canonical(char axis) {
    axis = (char)toupper((unsigned char)axis);
    if (axis == 'X') {
        return 'C';
    }
    if (axis == 'Y') {
        return 'A';
    }
    return axis;
}

static void write_invalid_argument(const struct device *usb_dev,
                                   const char *argument) {
    char reply_buf[64];

    snprintf(reply_buf, sizeof(reply_buf),
             "TEENSY_ERROR INVALID_ARGUMENT %s\r\n", argument);
    write_usb_serial(usb_dev, reply_buf);
}

/* Posição inválida sai como "nan": com ângulos com sinal (v2), "-1.00" seria um valor legítimo. */
static void format_centi(char *buf, size_t buf_size, uint16_t value) {
    if (value == UINT16_MAX) {
        snprintf(buf, buf_size, "nan");
    } else {
        snprintf(buf, buf_size, "%u.%02u", value / 100U, value % 100U);
    }
}

static void format_signed_deci(char *buf, size_t buf_size, int16_t value) {
    int32_t magnitude = value;
    const char *sign = "";

    if (value == INT16_MIN) {
        snprintf(buf, buf_size, "nan");
        return;
    }
    if (magnitude < 0) {
        sign = "-";
        magnitude = -magnitude;
    }
    snprintf(buf, buf_size, "%s%ld.%ld", sign,
             (long)(magnitude / 10), (long)(magnitude % 10));
}

static void format_deci(char *buf, size_t buf_size, int16_t value) {
    int32_t magnitude = value;
    const char *sign = "";

    if (value == INT16_MIN) {
        snprintf(buf, buf_size, "-99.9");
        return;
    }
    if (magnitude < 0) {
        sign = "-";
        magnitude = -magnitude;
    }
    snprintf(buf, buf_size, "%s%ld.%ld", sign,
             (long)(magnitude / 10), (long)(magnitude % 10));
}

static bool encode_sync_fixed(float value, float scale, int16_t *encoded) {
    float scaled;
    long rounded;

    if (!isfinite(value) || encoded == NULL) {
        return false;
    }
    scaled = value * scale;
    if (scaled < (float)INT16_MIN || scaled > (float)INT16_MAX) {
        return false;
    }
    rounded = lroundf(scaled);
    if (rounded < INT16_MIN || rounded > INT16_MAX) {
        return false;
    }
    *encoded = (int16_t)rounded;
    return true;
}

static void encode_i16_le(uint8_t *dest, int16_t value) {
    uint16_t raw = (uint16_t)value;
    dest[0] = (uint8_t)(raw & 0xFFU);
    dest[1] = (uint8_t)((raw >> 8) & 0xFFU);
}

static void write_can_raw_frame(const struct device *usb_dev,
                                const struct zcan_frame *frame) {
    char reply_buf[80];
    size_t used;

    if (usb_dev == NULL || frame == NULL) {
        return;
    }

    used = (size_t)snprintf(reply_buf, sizeof(reply_buf),
                            "CAN_RAW %03lX %u", (unsigned long)frame->id,
                            frame->dlc);
    for (uint8_t index = 0U;
         index < frame->dlc && index < CAN_MAX_DLC && used < sizeof(reply_buf);
         index++) {
        int written = snprintf(&reply_buf[used], sizeof(reply_buf) - used,
                               " %02X", frame->data[index]);
        if (written < 0 || (size_t)written >= sizeof(reply_buf) - used) {
            used = sizeof(reply_buf) - 1U;
            break;
        }
        used += (size_t)written;
    }
    if (used < sizeof(reply_buf) - 2U) {
        reply_buf[used++] = '\r';
        reply_buf[used++] = '\n';
        reply_buf[used] = '\0';
    } else {
        reply_buf[sizeof(reply_buf) - 3U] = '\r';
        reply_buf[sizeof(reply_buf) - 2U] = '\n';
        reply_buf[sizeof(reply_buf) - 1U] = '\0';
    }
    write_usb_serial(usb_dev, reply_buf);
}

/* Executado pelo driver FlexCAN em contexto de interrupção. Manter apenas
 * operações ISR-safe; a formatação e o envio USB ficam no loop principal. */
static void can_tx_callback(uint32_t error_flags, void *arg) {
    can_tx_result_t result = {
        .status = (int32_t)error_flags,
        .node_id = (uint8_t)POINTER_TO_UINT(arg),
    };

    (void)k_msgq_put(&can_tx_result_msgq, &result, K_NO_WAIT);
    atomic_clear(&can_tx_pending);
}

static void report_can_tx_results(const struct device *usb_dev) {
    can_tx_result_t result;

    while (k_msgq_get(&can_tx_result_msgq, &result, K_NO_WAIT) == 0) {
        if (result.status == CAN_TX_OK) {
            notify_can_activity();
        } else {
            char reply_buf[80];

            snprintf(reply_buf, sizeof(reply_buf),
                     "TEENSY_ERROR CAN_TX_FAILED %u %ld\r\n",
                     result.node_id, (long)result.status);
            write_usb_serial(usb_dev, reply_buf);
            notify_node_error(result.node_id);
        }
    }
}

static int hex_char_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_string_to_bytes(const char *hex_str, uint8_t *dest, size_t max_bytes) {
    size_t len = strlen(hex_str);
    if (len == 0 || (len % 2) != 0 || (len / 2) > max_bytes) {
        return -1;
    }
    size_t byte_count = len / 2;
    for (size_t i = 0; i < byte_count; i++) {
        int high = hex_char_to_val(hex_str[i * 2]);
        int low = hex_char_to_val(hex_str[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return -1;
        }
        dest[i] = (uint8_t)((high << 4) | low);
    }
    return (int)byte_count;
}

static int send_can_command_ex(const struct device *usb_dev,
                               const struct device *can_dev, unsigned node_id,
                               const uint8_t *payload, size_t payload_len,
                               int busy_retries) {
    struct zcan_frame frame = {0};
    char reply_buf[96];
    int err;

    if (!can_node_id_is_valid(node_id)) {
        snprintf(reply_buf, sizeof(reply_buf),
                 "TEENSY_ERROR INVALID_NODE_ID %u\r\n", node_id);
        write_usb_serial(usb_dev, reply_buf);
        return -EINVAL;
    }

    if (payload_len == 0U || payload_len > ARRAY_SIZE(frame.data)) {
        write_usb_serial(usb_dev, "TEENSY_ERROR INVALID_PAYLOAD_LENGTH\r\n");
        return -EMSGSIZE;
    }

    frame.id_type = CAN_STANDARD_IDENTIFIER;
    frame.rtr = CAN_DATAFRAME;
    frame.id = CAN_COMMAND_BASE_ID + node_id;
    frame.dlc = (uint8_t)payload_len;

    memcpy(frame.data, payload, payload_len);

    /* can_send() síncrono no driver FlexCAN do Zephyr 2.7 espera K_FOREVER
     * pela conclusão. Sem transceiver ou ACK isso congelava também a USB.
     * Manter um único envio assíncrono pendente evita bloquear o protocolo e
     * impede esgotar mailboxes se o hardware CAN estiver desconectado.
     * Durante streaming de blocos OTA, permite até 1ms de micro-retry para
     * que a conclusão do frame anterior libere o TX sem gerar erro falso de BUSY. */
    bool acquired = false;
    for (int retry = 0; retry < busy_retries; retry++) {
        if (atomic_cas(&can_tx_pending, 0, 1)) {
            acquired = true;
            break;
        }
        k_busy_wait(100);
    }
    if (!acquired) {
        snprintf(reply_buf, sizeof(reply_buf),
                 "TEENSY_ERROR CAN_TX_BUSY %u\r\n", node_id);
        write_usb_serial(usb_dev, reply_buf);
        return -EBUSY;
    }

    err = can_send(can_dev, &frame, K_MSEC(50), can_tx_callback,
                   UINT_TO_POINTER(node_id));
    if (err != 0) {
        atomic_clear(&can_tx_pending);
        snprintf(reply_buf, sizeof(reply_buf),
                 "TEENSY_ERROR CAN_SEND_FAILED %u %d\r\n", node_id, err);
        write_usb_serial(usb_dev, reply_buf);
        notify_node_error(node_id);
    } else {
        notify_node_command(node_id);
    }

    return err;
}

static int send_can_command(const struct device *usb_dev,
                            const struct device *can_dev, unsigned node_id,
                            const uint8_t *payload, size_t payload_len) {
    return send_can_command_ex(usb_dev, can_dev, node_id, payload, payload_len,
                               CAN_TX_BUSY_RETRIES_NORMAL);
}

/* Processa as mensagens vindas do TouchDesigner / App via Porta COM */
static void process_usb_command(const struct device *usb_dev,
                                const struct device *can_dev, char *cmd) {
    unsigned node_id = 0;
    char axis = '\0';
    int steps = 0;
    unsigned arg1 = 0;
    unsigned arg2 = 0;

    notify_usb_activity();

    while (isspace((unsigned char)*cmd)) {
        cmd++;
    }
    if (strlen(cmd) == 0) {
        return;
    }

    /* Parada imediata: X <node> [flags] | STOP <node> | ESTOP <node>.
     * Tratado antes de tudo; node 0 = broadcast (menor ID do barramento = maior prioridade). */
    if (((cmd[0] == 'X' || cmd[0] == 'x') && (cmd[1] == ' ' || cmd[1] == '\0')) ||
        strncmp(cmd, "STOP", 4) == 0 || strncmp(cmd, "stop", 4) == 0 ||
        strncmp(cmd, "ESTOP", 5) == 0 || strncmp(cmd, "estop", 5) == 0) {
        bool is_estop = (cmd[0] == 'E' || cmd[0] == 'e');
        const char *args = cmd;
        unsigned flags = is_estop ? STOP_FLAG_LASERS_OFF : 0U;
        int parsed;

        while (*args != '\0' && !isspace((unsigned char)*args)) {
            args++;
        }
        node_id = 0U;
        parsed = sscanf(args, "%u %u", &node_id, &arg1);
        if (parsed >= 2) {
            if (arg1 > 0xFFU) {
                write_invalid_argument(usb_dev, "STOP");
                return;
            }
            flags = arg1;
        }
        uint8_t payload[2] = {CAN_OP_STOP, (uint8_t)flags};
        if (send_can_command_ex(usb_dev, can_dev, node_id, payload, ARRAY_SIZE(payload),
                                CAN_TX_BUSY_RETRIES_STOP) == 0) {
            char reply_buf[48];
            snprintf(reply_buf, sizeof(reply_buf), "TEENSY_OK STOP %u %u\r\n", node_id, flags);
            write_usb_serial(usb_dev, reply_buf);
        }
    }
    /* Movimento sincronizado físico (MS/MSF <node> <deg_c> <deg_a> <mm_z>) */
    if ((strncmp(cmd, "MS ", 3) == 0 || strncmp(cmd, "ms ", 3) == 0 ||
         strncmp(cmd, "MSF ", 4) == 0 || strncmp(cmd, "msf ", 4) == 0)) {
        float angle_c_deg = 0.0f;
        float angle_a_deg = 0.0f;
        float distance_z_mm = 0.0f;
        int16_t angle_c_deci = 0;
        int16_t angle_a_deci = 0;
        int16_t distance_z_centi = 0;
        uint8_t payload[8];
        bool force_sync = (strncmp(cmd, "MSF ", 4) == 0 ||
                           strncmp(cmd, "msf ", 4) == 0);

        if (sscanf(cmd, "%*s %u %f %f %f", &node_id, &angle_c_deg,
                   &angle_a_deg, &distance_z_mm) != 4 ||
            !encode_sync_fixed(angle_c_deg, 10.0f, &angle_c_deci) ||
            !encode_sync_fixed(angle_a_deg, 10.0f, &angle_a_deci) ||
            !encode_sync_fixed(distance_z_mm, 100.0f, &distance_z_centi)) {
            write_invalid_argument(usb_dev, "MOVE_SYNC");
            return;
        }

        payload[0] = CAN_OP_MOVE_SYNC;
        encode_i16_le(&payload[1], angle_c_deci);
        encode_i16_le(&payload[3], angle_a_deci);
        encode_i16_le(&payload[5], distance_z_centi);
        payload[7] = force_sync ? 0x01U : 0x00U;
        (void)send_can_command(usb_dev, can_dev, node_id, payload, ARRAY_SIZE(payload));
    }
    /* Movimento Normal com correção (M <node> <axis> <steps>) */
    else if ((cmd[0] == 'M' || cmd[0] == 'm') && cmd[1] == ' ' &&
         sscanf(cmd, "%*c %u %c %d", &node_id, &axis, &steps) == 3) {
        uint8_t payload[6];
        uint32_t encoded_steps = (uint32_t)steps;
        axis = can_axis_to_canonical(axis);
        if (!can_axis_is_valid(axis)) {
            write_invalid_argument(usb_dev, "AXIS");
            return;
        }
        payload[0] = CAN_OP_MOVE;
        payload[1] = (uint8_t)axis;
        payload[2] = (uint8_t)(encoded_steps & 0xFFU);
        payload[3] = (uint8_t)((encoded_steps >> 8) & 0xFFU);
        payload[4] = (uint8_t)((encoded_steps >> 16) & 0xFFU);
        payload[5] = (uint8_t)((encoded_steps >> 24) & 0xFFU);
        (void)send_can_command(usb_dev, can_dev, node_id, payload, ARRAY_SIZE(payload));
    }
    /* Movimento Forçado sem limites (MF <node> <axis> <steps>) */
    else if ((strncmp(cmd, "MF", 2) == 0 || strncmp(cmd, "mf", 2) == 0) &&
             sscanf(cmd, "%*s %u %c %d", &node_id, &axis, &steps) == 3) {
        uint8_t payload[6];
        uint32_t encoded_steps = (uint32_t)steps;
        axis = can_axis_to_canonical(axis);
        if (!can_axis_is_valid(axis)) {
            write_invalid_argument(usb_dev, "AXIS");
            return;
        }
        payload[0] = CAN_OP_MOVE_FORCE;
        payload[1] = (uint8_t)axis;
        payload[2] = (uint8_t)(encoded_steps & 0xFFU);
        payload[3] = (uint8_t)((encoded_steps >> 8) & 0xFFU);
        payload[4] = (uint8_t)((encoded_steps >> 16) & 0xFFU);
        payload[5] = (uint8_t)((encoded_steps >> 24) & 0xFFU);
        (void)send_can_command(usb_dev, can_dev, node_id, payload, ARRAY_SIZE(payload));
    }
    /* Homing (H <node> <axis>) */
    else if ((cmd[0] == 'H' || cmd[0] == 'h') && sscanf(cmd, "%*c %u %c", &node_id, &axis) == 2) {
        uint8_t payload[2];
        axis = can_axis_to_canonical(axis);
        if (!can_axis_is_valid(axis)) {
            write_invalid_argument(usb_dev, "AXIS");
            return;
        }
        payload[0] = CAN_OP_HOME;
        payload[1] = (uint8_t)axis;
        (void)send_can_command(usb_dev, can_dev, node_id, payload, ARRAY_SIZE(payload));
    }
    /* Habilita Drivers (E <node> <0|1>) */
    else if ((cmd[0] == 'E' || cmd[0] == 'e') && sscanf(cmd, "%*c %u %u", &node_id, &arg1) == 2) {
        if (arg1 > 1U) {
            write_invalid_argument(usb_dev, "ENABLE");
            return;
        }
        uint8_t payload[2] = {CAN_OP_ENABLE, (uint8_t)arg1};
        (void)send_can_command(usb_dev, can_dev, node_id, payload, ARRAY_SIZE(payload));
    }
    /* Nível de Velocidade Geral 1..5 (S <node> <level>) */
    else if ((cmd[0] == 'S' || cmd[0] == 's') && cmd[1] == ' ' && sscanf(cmd, "%*c %u %u", &node_id, &arg1) == 2) {
        if (arg1 < 1U || arg1 > 5U) {
            write_invalid_argument(usb_dev, "SPEED");
            return;
        }
        uint8_t payload[2] = {CAN_OP_SPEED, (uint8_t)arg1};
        (void)send_can_command(usb_dev, can_dev, node_id, payload, ARRAY_SIZE(payload));
    }
    /* Laser PWM 12-bit (L <node> <laser_num 1|2> <level 0..4095>) */
    else if ((cmd[0] == 'L' || cmd[0] == 'l') && sscanf(cmd, "%*c %u %u %u", &node_id, &arg1, &arg2) == 3) {
        uint8_t payload[4];
        if (arg1 < 1U || arg1 > 2U || arg2 > 4095U) {
            write_invalid_argument(usb_dev, "LASER");
            return;
        }
        payload[0] = CAN_OP_LASER;
        payload[1] = (uint8_t)arg1;
        payload[2] = (uint8_t)(arg2 & 0xFFU);
        payload[3] = (uint8_t)((arg2 >> 8) & 0xFFU);
        (void)send_can_command(usb_dev, can_dev, node_id, payload, ARRAY_SIZE(payload));
    }
    /* Fan (F <node> <mode 0=Off, 1=On, 2=Auto>) */
    else if ((cmd[0] == 'F' || cmd[0] == 'f') && sscanf(cmd, "%*c %u %u", &node_id, &arg1) == 2) {
        if (arg1 > 2U) {
            write_invalid_argument(usb_dev, "FAN");
            return;
        }
        uint8_t payload[2] = {CAN_OP_FAN, (uint8_t)arg1};
        (void)send_can_command(usb_dev, can_dev, node_id, payload, ARRAY_SIZE(payload));
    }
    /* Ping (P <node> [arg1] [arg2]) */
    else if (cmd[0] == 'P' || cmd[0] == 'p') {
        uint8_t payload[3] = {CAN_OP_PING, 0, 0};
        int parsed = sscanf(cmd, "%*c %u %u %u", &node_id, &arg1, &arg2);
        if (parsed >= 1) {
            if (!can_unicast_node_id_is_valid(node_id)) {
                char reply_buf[64];
                snprintf(reply_buf, sizeof(reply_buf),
                         "TEENSY_ERROR INVALID_NODE_ID %u\r\n", node_id);
                write_usb_serial(usb_dev, reply_buf);
                return;
            }
            if ((parsed >= 2 && arg1 > UINT8_MAX) ||
                (parsed >= 3 && arg2 > UINT8_MAX)) {
                write_invalid_argument(usb_dev, "PING");
                return;
            }
            payload[1] = (uint8_t)arg1;
            payload[2] = (uint8_t)arg2;
            (void)send_can_command(usb_dev, can_dev, node_id, payload, ARRAY_SIZE(payload));
        } else {
            write_usb_serial(usb_dev, "TEENSY_ERROR UNKNOWN_COMMAND\r\n");
        }
    }
    /* Diagnóstico dos frames CAN crus (RAW <0|1>) */
    else if ((strncmp(cmd, "RAW ", 4) == 0 ||
              strncmp(cmd, "raw ", 4) == 0) &&
             sscanf(cmd, "%*s %u", &arg1) == 1) {
        if (arg1 > 1U) {
            write_invalid_argument(usb_dev, "CAN_RAW");
            return;
        }
        atomic_set(&can_raw_trace_enabled, (atomic_val_t)arg1);
        write_usb_serial(usb_dev, arg1 != 0U ?
                         "TEENSY_OK CAN_RAW 1\r\n" :
                         "TEENSY_OK CAN_RAW 0\r\n");
    }
    /* Solicitação de Status (R <node>) */
    else if ((cmd[0] == 'R' || cmd[0] == 'r') && sscanf(cmd, "%*c %u", &node_id) == 1) {
        const uint8_t payload[1] = {CAN_OP_STATUS_REQUEST};
        if (!can_unicast_node_id_is_valid(node_id)) {
            char reply_buf[64];
            snprintf(reply_buf, sizeof(reply_buf),
                     "TEENSY_ERROR INVALID_NODE_ID %u\r\n", node_id);
            write_usb_serial(usb_dev, reply_buf);
            return;
        }
        (void)send_can_command(usb_dev, can_dev, node_id, payload, ARRAY_SIZE(payload));
    }
    /* Comandos de Atualização OTA via CAN (OTA_START, OTA_DATA, OTA_END, OTA_ABORT) */
    else if ((strncmp(cmd, "OTA_START ", 10) == 0 || strncmp(cmd, "ota_start ", 10) == 0 ||
              strncmp(cmd, "OTA START ", 10) == 0 || strncmp(cmd, "ota start ", 10) == 0)) {
        const char *p = cmd + 10;
        uint32_t img_size = 0;
        if (sscanf(p, "%u %u", &node_id, &img_size) == 2) {
            if (!can_node_id_is_valid(node_id)) {
                write_invalid_argument(usb_dev, "NODE_ID");
                return;
            }
            uint8_t payload[7] = {
                CAN_OP_OTA_START,
                (uint8_t)node_id,
                (uint8_t)(img_size & 0xFFU),
                (uint8_t)((img_size >> 8) & 0xFFU),
                (uint8_t)((img_size >> 16) & 0xFFU),
                (uint8_t)((img_size >> 24) & 0xFFU),
                0x00U /* flags */
            };
            int err = send_can_command(usb_dev, can_dev, node_id, payload, sizeof(payload));
            if (err == 0) {
                notify_node_ota(node_id);
                char reply_buf[64];
                snprintf(reply_buf, sizeof(reply_buf), "TEENSY_OK OTA_START %u\r\n", node_id);
                write_usb_serial(usb_dev, reply_buf);
            }
        } else {
            write_invalid_argument(usb_dev, "OTA_START");
        }
    }
    else if ((strncmp(cmd, "OTA_DATA ", 9) == 0 || strncmp(cmd, "ota_data ", 9) == 0 ||
              strncmp(cmd, "OTA DATA ", 9) == 0 || strncmp(cmd, "ota data ", 9) == 0)) {
        const char *p = cmd + 9;
        unsigned seq_num = 0;
        char hex_buf[32] = {0};
        if (sscanf(p, "%u %u %31s", &node_id, &seq_num, hex_buf) == 3) {
            if (!can_node_id_is_valid(node_id) || seq_num > 255U) {
                write_invalid_argument(usb_dev, "OTA_DATA");
                return;
            }
            uint8_t chunk_data[6];
            int chunk_len = hex_string_to_bytes(hex_buf, chunk_data, sizeof(chunk_data));
            if (chunk_len <= 0) {
                write_invalid_argument(usb_dev, "OTA_HEX");
                return;
            }
            uint8_t payload[8];
            payload[0] = CAN_OP_OTA_DATA;
            payload[1] = (uint8_t)seq_num;
            memcpy(&payload[2], chunk_data, (size_t)chunk_len);
            int err = send_can_command(usb_dev, can_dev, node_id, payload, (size_t)(chunk_len + 2));
            if (err == 0) {
                notify_node_ota(node_id);
            }
        } else {
            write_invalid_argument(usb_dev, "OTA_DATA");
        }
    }
    else if ((strncmp(cmd, "OTA_END ", 8) == 0 || strncmp(cmd, "ota_end ", 8) == 0 ||
              strncmp(cmd, "OTA END ", 8) == 0 || strncmp(cmd, "ota end ", 8) == 0)) {
        const char *p = cmd + 8;
        unsigned checksum = 0;
        int parsed = sscanf(p, "%u %u", &node_id, &checksum);
        if (parsed >= 1) {
            if (!can_node_id_is_valid(node_id)) {
                write_invalid_argument(usb_dev, "NODE_ID");
                return;
            }
            uint8_t payload[4] = {
                CAN_OP_OTA_END,
                (uint8_t)node_id,
                (uint8_t)(checksum & 0xFFU),
                (uint8_t)((checksum >> 8) & 0xFFU)
            };
            int err = send_can_command(usb_dev, can_dev, node_id, payload, sizeof(payload));
            if (err == 0) {
                notify_node_ota(node_id);
                char reply_buf[64];
                snprintf(reply_buf, sizeof(reply_buf), "TEENSY_OK OTA_END %u\r\n", node_id);
                write_usb_serial(usb_dev, reply_buf);
            }
        } else {
            write_invalid_argument(usb_dev, "OTA_END");
        }
    }
    else if ((strncmp(cmd, "OTA_ABORT", 9) == 0 || strncmp(cmd, "ota_abort", 9) == 0 ||
              strncmp(cmd, "OTA ABORT", 9) == 0 || strncmp(cmd, "ota abort", 9) == 0)) {
        const char *p = (cmd[3] == '_') ? (cmd + 9) : (cmd + 9);
        node_id = 0;
        (void)sscanf(p, "%u", &node_id);
        if (!can_node_id_is_valid(node_id)) {
            write_invalid_argument(usb_dev, "NODE_ID");
            return;
        }
        uint8_t payload[2] = {
            CAN_OP_OTA_ABORT,
            (uint8_t)node_id
        };
        int err = send_can_command(usb_dev, can_dev, node_id, payload, sizeof(payload));
        if (err == 0) {
            notify_node_activity(node_id);
            char reply_buf[64];
            snprintf(reply_buf, sizeof(reply_buf), "TEENSY_OK OTA_ABORT %u\r\n", node_id);
            write_usb_serial(usb_dev, reply_buf);
        }
    }
    else {
        write_usb_serial(usb_dev, "TEENSY_ERROR UNKNOWN_COMMAND\r\n");
    }
}

/* Thread para receber pacotes CAN dos ESP32 e repassar à USB */
void can_rx_thread(void *p1, void *p2, void *p3) {
    const struct device *usb_dev = usb_cdc_dev;
    struct zcan_frame frame;
    char reply_buf[128];

    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        k_msgq_get(&can_rx_msgq, &frame, K_FOREVER);

        if (atomic_get(&can_raw_trace_enabled) != 0) {
            write_can_raw_frame(usb_dev, &frame);
        }

        if (frame.dlc > 0) {
            unsigned node_id;
            uint8_t op = frame.data[0];

            /* 1. Status Geral (IDs 0x281..0x28A) */
            if (can_id_to_node(frame.id, CAN_STATUS_BASE_ID, &node_id) &&
                op == CAN_EVT_STATUS && frame.dlc >= 8) {
                uint8_t flags = frame.data[2];
                uint16_t l1 = (uint16_t)frame.data[3] |
                              ((uint16_t)frame.data[4] << 8);
                uint16_t l2 = (uint16_t)frame.data[5] |
                              ((uint16_t)frame.data[6] << 8);
                uint8_t fan_byte = frame.data[7];
                uint8_t fan_on = fan_byte & 0x01;

                atomic_set(&node_pos_v2[node_id],
                           (flags & CAN_STATUS_FLAG_POS_V2) != 0U ? 1 : 0);
                uint8_t fan_mode = (fan_byte >> 1) & 0x07;
                uint8_t speed = (fan_byte >> 4) & 0x0F;

                snprintf(reply_buf, sizeof(reply_buf),
                         "STATUS %u %u %u %u %u %u %u\r\n",
                         node_id, flags, l1, l2, fan_on, fan_mode,
                         speed);
                notify_can_activity();
                notify_node_activity(node_id);
                write_usb_serial(usb_dev, reply_buf);
            }
            /* 2. Posição e temperatura (IDs 0x291..0x29A) */
            else if (can_id_to_node(frame.id, CAN_POS_BASE_ID, &node_id) &&
                     frame.dlc >= 8) {
                uint16_t raw_c = (uint16_t)frame.data[0] | ((uint16_t)frame.data[1] << 8);
                uint16_t raw_a = (uint16_t)frame.data[2] | ((uint16_t)frame.data[3] << 8);
                uint16_t z_steps = (uint16_t)frame.data[4] | ((uint16_t)frame.data[5] << 8);
                int16_t temp_deci = (int16_t)((uint16_t)frame.data[6] | ((uint16_t)frame.data[7] << 8));
                char pos_c[16];
                char pos_a[16];
                char temp_c[16];

                /* O ESP32 envia STATUS e posição em sequência: o bit do STATUS diz o formato. */
                if (atomic_get(&node_pos_v2[node_id]) != 0) {
                    format_signed_deci(pos_c, sizeof(pos_c), (int16_t)raw_c);
                    format_signed_deci(pos_a, sizeof(pos_a), (int16_t)raw_a);
                } else {
                    format_centi(pos_c, sizeof(pos_c), raw_c);
                    format_centi(pos_a, sizeof(pos_a), raw_a);
                }
                format_deci(temp_c, sizeof(temp_c), temp_deci);

                snprintf(reply_buf, sizeof(reply_buf),
                         "POS %u %s %s %u %s\r\n",
                         node_id, pos_c, pos_a, z_steps, temp_c);
                notify_can_activity();
                notify_node_activity(node_id);
                write_usb_serial(usb_dev, reply_buf);
            }
            /* 3. Eventos (IDs 0x301..0x30A) */
            else if (can_id_to_node(frame.id, CAN_EVENT_BASE_ID, &node_id) &&
                     op == CAN_EVT_HEARTBEAT && frame.dlc >= 2) {
                snprintf(reply_buf, sizeof(reply_buf), "HEARTBEAT %u\r\n", node_id);
                notify_can_activity();
                notify_node_activity(node_id);
                write_usb_serial(usb_dev, reply_buf);
            } else if (can_id_to_node(frame.id, CAN_EVENT_BASE_ID, &node_id) &&
                       op == CAN_EVT_PONG && frame.dlc >= 4) {
                snprintf(reply_buf, sizeof(reply_buf), "PONG %u %u %u\r\n", node_id,
                         frame.data[2], frame.data[3]);
                notify_can_activity();
                notify_node_activity(node_id);
                write_usb_serial(usb_dev, reply_buf);
            } else if (can_id_to_node(frame.id, CAN_EVENT_BASE_ID, &node_id) &&
                       op == CAN_EVT_ACK && frame.dlc >= 3) {
                snprintf(reply_buf, sizeof(reply_buf), "ACK %u %02X\r\n", node_id,
                         frame.data[2]);
                notify_can_activity();
                notify_node_activity(node_id);
                write_usb_serial(usb_dev, reply_buf);
                notify_node_command(node_id);
            } else if (can_id_to_node(frame.id, CAN_EVENT_BASE_ID, &node_id) &&
                       op == CAN_EVT_DONE && frame.dlc >= 3) {
                snprintf(reply_buf, sizeof(reply_buf), "DONE %u %02X\r\n", node_id,
                         frame.data[2]);
                notify_can_activity();
                notify_node_activity(node_id);
                write_usb_serial(usb_dev, reply_buf);
                notify_node_command(node_id);
            } else if (can_id_to_node(frame.id, CAN_EVENT_BASE_ID, &node_id) &&
                       op == CAN_EVT_OTA_READY && frame.dlc >= 3) {
                uint8_t status = (frame.dlc >= 4) ? frame.data[3] : 0;
                snprintf(reply_buf, sizeof(reply_buf), "OTA_READY %u %u\r\n", node_id, status);
                notify_can_activity();
                notify_node_activity(node_id);
                notify_node_command(node_id);
                notify_node_ota(node_id);
                write_usb_serial(usb_dev, reply_buf);
            } else if (can_id_to_node(frame.id, CAN_EVENT_BASE_ID, &node_id) &&
                       op == CAN_EVT_OTA_PROGRESS && frame.dlc >= 3) {
                uint8_t pct = (frame.dlc >= 4) ? frame.data[3] : 0;
                snprintf(reply_buf, sizeof(reply_buf), "OTA_PROGRESS %u %u\r\n", node_id, pct);
                notify_can_activity();
                notify_node_activity(node_id);
                notify_node_ota(node_id);
                write_usb_serial(usb_dev, reply_buf);
            } else if (can_id_to_node(frame.id, CAN_EVENT_BASE_ID, &node_id) &&
                       op == CAN_EVT_OTA_DONE && frame.dlc >= 2) {
                snprintf(reply_buf, sizeof(reply_buf), "OTA_DONE %u\r\n", node_id);
                notify_can_activity();
                notify_node_activity(node_id);
                notify_node_command(node_id);
                write_usb_serial(usb_dev, reply_buf);
            } else if (can_id_to_node(frame.id, CAN_EVENT_BASE_ID, &node_id) &&
                       op == CAN_EVT_OTA_ERROR && frame.dlc >= 3) {
                uint8_t err_code = (frame.dlc >= 4) ? frame.data[3] : 0xFF;
                snprintf(reply_buf, sizeof(reply_buf), "OTA_ERROR %u %u\r\n", node_id, err_code);
                notify_can_activity();
                notify_node_activity(node_id);
                notify_node_error(node_id);
                write_usb_serial(usb_dev, reply_buf);
            } else if (can_id_to_node(frame.id, CAN_EVENT_BASE_ID, &node_id) &&
                       op == CAN_EVT_ERROR && frame.dlc >= 4) {
                snprintf(reply_buf, sizeof(reply_buf), "ERROR %u %02X %02X\r\n",
                         node_id, frame.data[2], frame.data[3]);
                notify_can_activity();
                notify_node_activity(node_id);
                write_usb_serial(usb_dev, reply_buf);
                notify_node_error(node_id);
            }
        }
    }
}

/* Thread para piscar o LED de atividade integrado (Pino 13) */
void blink_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    const struct device *gpio_dev = device_get_binding(LED_PORT);
    if (gpio_dev == NULL) {
        return;
    }
    gpio_pin_configure(gpio_dev, LED_PIN, GPIO_OUTPUT_ACTIVE | LED_FLAGS);

    while (1) {
        gpio_pin_toggle(gpio_dev, LED_PIN);
        k_msleep(500);
    }
}

void main(void) {
    const struct device *usb_dev = usb_cdc_dev;
    const struct device *can_dev = can_primary_dev;
    char usb_rx_buf[128];
    size_t usb_rx_idx = 0U;
    bool usb_rx_overflow = false;
    bool usb_was_open = false;
    bool can_filters_ready;
    int status_filter_id;
    int pos_filter_id;
    int event_filter_id;

    printk("Inicializando Teensy 4.1 USB/CAN Bridge + WS2812 Status LED Matrix...\n");

    if (!device_is_ready(usb_dev)) {
        printk("Erro: Nao encontrou porta USB CDC ACM.\n");
        return;
    }

    if (!device_is_ready(can_dev)) {
        printk("Erro: Controlador CAN primario nao esta pronto.\n");
        return;
    }

    if (usb_enable(NULL)) {
        printk("Erro: Falha ao inicializar dispositivo USB.\n");
        return;
    }

    /* Cada máscara 0x7F0 cobre exatamente um grupo de 16 IDs. */
    struct zcan_filter status_filter = {
        .id_type = CAN_STANDARD_IDENTIFIER,
        .rtr = CAN_DATAFRAME,
        .rtr_mask = 1,
        .id = CAN_STATUS_BASE_ID,
        .id_mask = CAN_ID_GROUP_MASK
    };

    struct zcan_filter pos_filter = {
        .id_type = CAN_STANDARD_IDENTIFIER,
        .rtr = CAN_DATAFRAME,
        .rtr_mask = 1,
        .id = CAN_POS_BASE_ID,
        .id_mask = CAN_ID_GROUP_MASK
    };

    struct zcan_filter event_filter = {
        .id_type = CAN_STANDARD_IDENTIFIER,
        .rtr = CAN_DATAFRAME,
        .rtr_mask = 1,
        .id = CAN_EVENT_BASE_ID,
        .id_mask = CAN_ID_GROUP_MASK
    };

    status_filter_id = can_attach_msgq(can_dev, &can_rx_msgq, &status_filter);
    pos_filter_id = can_attach_msgq(can_dev, &can_rx_msgq, &pos_filter);
    event_filter_id = can_attach_msgq(can_dev, &can_rx_msgq, &event_filter);
    can_filters_ready = status_filter_id >= 0 && pos_filter_id >= 0 &&
                        event_filter_id >= 0;
    if (!can_filters_ready) {
        /* A COM continua operacional para que a falha também seja observável
         * sem depender do console UART físico. */
        printk("Erro: Filtros CAN: status=%d pos=%d eventos=%d.\n",
               status_filter_id, pos_filter_id, event_filter_id);
    }

    /* Cria Threads secundárias */
    k_thread_create(&can_rx_thread_data, can_rx_stack_area,
                    K_THREAD_STACK_SIZEOF(can_rx_stack_area), can_rx_thread, NULL,
                    NULL, NULL, 7, 0, K_NO_WAIT);

    k_thread_create(&blink_thread_data, blink_stack_area,
                    K_THREAD_STACK_SIZEOF(blink_stack_area), blink_thread, NULL,
                    NULL, NULL, 10, 0, K_NO_WAIT);

    k_thread_create(&led_strip_thread_data, led_strip_stack_area,
                    K_THREAD_STACK_SIZEOF(led_strip_stack_area), led_strip_thread, NULL,
                    NULL, NULL, 8, 0, K_NO_WAIT);

    printk("USB, CAN e WS2812 configurados com sucesso! Aguardando comandos...\n");

    /* Loop principal de leitura da USB CDC ACM */
    while (1) {
        bool usb_is_open = usb_serial_is_open(usb_dev);

        report_can_tx_results(usb_dev);

        if (usb_is_open && !usb_was_open) {
            write_usb_serial(usb_dev, "TEENSY_READY 1\r\n");
            if (!can_filters_ready) {
                char filter_error[96];

                snprintf(filter_error, sizeof(filter_error),
                         "TEENSY_ERROR CAN_FILTER_ATTACH_FAILED %d %d %d\r\n",
                         status_filter_id, pos_filter_id, event_filter_id);
                write_usb_serial(usb_dev, filter_error);
            }
        }
        usb_was_open = usb_is_open;

        uint8_t c;
        int rx_count = 0;
        while (rx_count < 64 && uart_poll_in(usb_dev, &c) == 0) {
            rx_count++;
            if (c == '\n' || c == '\r') {
                if (usb_rx_overflow) {
                    write_usb_serial(usb_dev,
                                     "TEENSY_ERROR LINE_TOO_LONG\r\n");
                    usb_rx_overflow = false;
                    usb_rx_idx = 0;
                } else if (usb_rx_idx > 0) {
                    usb_rx_buf[usb_rx_idx] = '\0';
                    process_usb_command(usb_dev, can_dev, usb_rx_buf);
                    usb_rx_idx = 0;
                }
            } else if (!usb_rx_overflow) {
                if (usb_rx_idx < (sizeof(usb_rx_buf) - 1)) {
                    usb_rx_buf[usb_rx_idx++] = (char)c;
                } else {
                    usb_rx_overflow = true;
                    usb_rx_idx = 0;
                }
            }
        }
        if (rx_count == 0) {
            k_msleep(1);
        }
    }
}
