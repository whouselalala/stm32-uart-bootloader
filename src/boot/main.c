#include "boot.h"
#include "stm32f4xx_hal.h"
#include <string.h>

static UART_HandleTypeDef uart;

void HAL_UART_MspInit(UART_HandleTypeDef *handle) {
    if (handle->Instance != USART2) return;
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &gpio);
}

static int erase_app(void *ctx) {
    (void)ctx;
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t failed_sector = 0;
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase.Sector = FLASH_SECTOR_2;
    erase.NbSectors = 6;
    if (HAL_FLASH_Unlock() != HAL_OK) return 0;
    HAL_StatusTypeDef result = HAL_FLASHEx_Erase(&erase, &failed_sector);
    HAL_FLASH_Lock();
    return result == HAL_OK;
}
static int write_word(void *ctx, uint32_t addr, uint32_t word) {
    (void)ctx;
    if ((addr & 3u) || addr < BL_HEADER_ADDR || addr > BL_FLASH_END - 4u ||
        (addr >= BL_HEADER_ADDR + 12u && addr < BL_APP_ADDR)) return 0;
    if (HAL_FLASH_Unlock() != HAL_OK) return 0;
    HAL_StatusTypeDef result = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word);
    HAL_FLASH_Lock();
    return result == HAL_OK && *(volatile uint32_t *)addr == word;
}
static uint32_t read_word(void *ctx, uint32_t addr) {
    (void)ctx;
    return *(volatile uint32_t *)addr;
}
static void send_status(uint8_t command, uint16_t seq, uint8_t status) {
    uint8_t frame[10];
    size_t n = bl_encode(command, seq, &status, 1u, frame, sizeof frame);
    HAL_UART_Transmit(&uart, frame, (uint16_t)n, 1000u);
}
static int read_byte(uint8_t *byte, uint32_t timeout) {
    return HAL_UART_Receive(&uart, byte, 1u, timeout) == HAL_OK;
}
static int receive_frame(uint8_t *frame, size_t *length, uint32_t first_timeout) {
    uint8_t byte = 0;
    if (!read_byte(&byte, first_timeout) || byte != 'B') return 0;
    frame[0] = byte;
    if (!read_byte(frame + 1, 100u) || frame[1] != 'L') return 0;
    for (unsigned i = 2; i < 7; ++i)
        if (!read_byte(frame + i, 100u)) return 0;
    uint16_t payload = (uint16_t)(frame[5] | ((uint16_t)frame[6] << 8u));
    if (payload > BL_DATA_MAX) return 0;
    for (uint16_t i = 0; i < payload + 2u; ++i)
        if (!read_byte(frame + 7u + i, 100u)) return 0;
    *length = 9u + payload;
    return 1;
}

static void jump_to_app(void) {
    uint32_t sp = *(volatile uint32_t *)BL_APP_ADDR;
    uint32_t pc = *(volatile uint32_t *)(BL_APP_ADDR + 4u);
    HAL_UART_DeInit(&uart);
    HAL_RCC_DeInit();
    HAL_DeInit();
    __disable_irq();
    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL = 0u;
    for (unsigned i = 0; i < 8u; ++i) { NVIC->ICER[i] = 0xffffffffu; NVIC->ICPR[i] = 0xffffffffu; }
    SCB->VTOR = BL_APP_ADDR;
    __DSB(); __ISB();
    __set_MSP(sp);
    ((void (*)(void))pc)();
    for (;;) { }
}

int main(void) {
    HAL_Init();
    uart.Instance = USART2;
    uart.Init.BaudRate = 115200;
    uart.Init.WordLength = UART_WORDLENGTH_8B;
    uart.Init.StopBits = UART_STOPBITS_1;
    uart.Init.Parity = UART_PARITY_NONE;
    uart.Init.Mode = UART_MODE_TX_RX;
    uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    uart.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&uart) != HAL_OK) for (;;) { }
    bl_flash_t flash = {erase_app, write_word, read_word, NULL};
    bl_state_t state = {0};
    uint8_t frame[9u + BL_DATA_MAX], greeting[] = "BOOT READY\r\n";
    HAL_UART_Transmit(&uart, greeting, sizeof greeting - 1u, 1000u);
    for (;;) {
        int valid = bl_app_valid(&flash);
        size_t length = 0;
        if (!receive_frame(frame, &length, valid && !state.active ? 8000u : 1000u)) {
            if (valid && !state.active) jump_to_app();
            continue;
        }
        bl_packet_t packet = {0};
        if (!bl_decode(frame, length, &packet)) continue;
        uint8_t status = bl_process(&state, &flash, &packet);
        send_status(packet.command, packet.sequence, status);
        if (packet.command == BL_END && status == BL_OK && bl_app_valid(&flash)) {
            HAL_Delay(50u);
            jump_to_app();
        }
    }
}
