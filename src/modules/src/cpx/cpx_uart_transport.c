/**
 * ,---------,       ____  _ __
 * |  ,-^-,  |      / __ )(_) /_______________ _____  ___
 * | (  O  ) |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * | / ,--´  |    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *    +------`   /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie control firmware
 *
 * Copyright (C) 2022 Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define DEBUG_MODULE "CPX-UART-TRANSP"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "config.h"
#include "console.h"
#include "uart2.h"
#include "debug.h"
#include "deck.h"
#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"
#include "queue.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "log.h"
#include "param.h"
#include "queue.h"
#include "stm32fxxx.h"
#include "system.h"
#include "autoconf.h"

#include "cpx.h"
#include "cpx_uart_transport.h"

static uint8_t byte;

#define UART_TX_QUEUE_LENGTH 4
#define UART_RX_QUEUE_LENGTH 4

static xQueueHandle uartTxQueue;
static xQueueHandle uartRxQueue;

// Length of start + payloadLength
#define UART_HEADER_LENGTH 2
#define UART_CRC_LENGTH 1
#define UART_META_LENGTH (UART_HEADER_LENGTH + UART_CRC_LENGTH)

/*typedef struct {
  CPXTarget_t destination : 3;
  CPXTarget_t source : 3;
  bool lastPacket : 1;
  bool reserved : 1;
  CPXFunction_t function : 8;
} __attribute__((packed)) CPXRoutingPacked_t;*/

typedef struct {
  uint8_t cmd;
  uint32_t startAddress;
  uint32_t writeSize;
} __attribute__((packed)) GAP8BlCmdPacket_t;

typedef struct {
  uint8_t cmd;
} __attribute__((packed)) ESP32SysPacket_t;

#define GAP8_BL_CMD_START_WRITE (0x02)
#define GAP8_BL_CMD_MD5         (0x04)

#define ESP32_SYS_CMD_RESET_GAP8 (0x10)

#define CPX_ROUTING_PACKED_SIZE (sizeof(CPXRoutingPacked_t))

typedef struct {
    CPXRoutingPacked_t route;
    uint8_t data[CPX_UART_TRANSPORT_MTU - CPX_ROUTING_PACKED_SIZE];
} __attribute__((packed)) uartTransportPayload_t;

typedef struct {
    uint8_t start;
    uint8_t payloadLength; // Excluding start and crc
    union {
        uartTransportPayload_t routablePayload;
        uint8_t payload[CPX_UART_TRANSPORT_MTU];
    };

    uint8_t crcPlaceHolder; // Not actual position. CRC is added after the last byte of payload
} __attribute__((packed)) uart_transport_packet_t;

// Used when sending/receiving data on the UART
static uart_transport_packet_t espTxp;
static CPXPacket_t cpxTxp;
static uart_transport_packet_t espRxp;

static EventGroupHandle_t evGroup;
#define ESP_CTS_EVENT (1 << 0)
#define ESP_CTR_EVENT (1 << 1)
#define ESP_TXQ_EVENT (1 << 2)

static EventGroupHandle_t bootloaderSync;
#define CPX_WAIT_FOR_BOOTLOADER_REPLY (1<<0)

static uint8_t calcCrc(const uart_transport_packet_t* packet) {
  const uint8_t* start = (const uint8_t*) packet;
  const uint8_t* end = &packet->payload[packet->payloadLength];

  uint8_t crc = 0;
  for (const uint8_t* p = start; p < end; p++) {
    crc ^= *p;
  }

  return crc;
}

static void assemblePacket(const CPXPacket_t *packet, uart_transport_packet_t * txp) {
  ASSERT((packet->route.destination >> 4) == 0);
  ASSERT((packet->route.source >> 4) == 0);
  ASSERT((packet->route.function >> 8) == 0);
  ASSERT(packet->dataLength <= CPX_UART_TRANSPORT_MTU - CPX_ROUTING_PACKED_SIZE);

  txp->payloadLength = packet->dataLength + CPX_ROUTING_PACKED_SIZE;
  txp->routablePayload.route.destination = packet->route.destination;
  txp->routablePayload.route.source = packet->route.source;
  txp->routablePayload.route.lastPacket = packet->route.lastPacket;
  txp->routablePayload.route.function = packet->route.function;
  memcpy(txp->routablePayload.data, &packet->data, packet->dataLength);
  txp->payload[txp->payloadLength] = calcCrc(txp);
}

static void sendDataToEspUart(uint32_t size, uint8_t* data) {
  uart2SendData(size, data);
}

static void getDataFromEspUart(uint8_t *c) {
  bool readSuccess = false;
  while(!readSuccess) {
      readSuccess = uart2GetDataWithTimeout(c, M2T(100));
  }
}

static void CPX_UART_RX(void *param)
{
  systemWaitStart();

  while (1)
  {
    // Wait for start!
    do
    {
      getDataFromEspUart(&espRxp.start);
    } while (espRxp.start != 0xFF);

    getDataFromEspUart(&espRxp.payloadLength);

    if (espRxp.payloadLength == 0)
    {
      xEventGroupSetBits(evGroup, ESP_CTS_EVENT);
    }
    else
    {
      for (int i = 0; i < espRxp.payloadLength; i++)
      {
        getDataFromEspUart(&espRxp.payload[i]);
      }

      uint8_t crc;
      getDataFromEspUart(&crc);
      ASSERT(crc == calcCrc(&espRxp));

      xQueueSend(uartRxQueue, &espRxp, portMAX_DELAY);
      xEventGroupSetBits(evGroup, ESP_CTR_EVENT);
    }
  }
}

static void CPX_UART_TX(void *param)
{
  systemWaitStart();

  uint8_t ctr[] = {0xFF, 0x00};
  EventBits_t evBits = 0;

  // We need to hold off here to make sure that the RX task
  // has started up and is waiting for chars, otherwise we might send
  // CTR and miss CTS (which means that the ESP32 will stop sending CTS
  // too early and we cannot sync)
  vTaskDelay(100);

  // Sync with ESP32 so both are in CTS
  do
  {
    sendDataToEspUart(sizeof(ctr), (uint8_t *)&ctr);
    vTaskDelay(100);
    evBits = xEventGroupGetBits(evGroup);
  } while ((evBits & ESP_CTS_EVENT) != ESP_CTS_EVENT);

  while (1)
  {
    // If we have nothing to send then wait, either for something to be
    // queued or for a request to send CTR
    if (uxQueueMessagesWaiting(uartTxQueue) == 0)
    {
      evBits = xEventGroupWaitBits(evGroup,
                                   ESP_CTR_EVENT | ESP_TXQ_EVENT,
                                   pdTRUE,  // Clear bits before returning
                                   pdFALSE, // Wait for any bit
                                   portMAX_DELAY);
      if ((evBits & ESP_CTR_EVENT) == ESP_CTR_EVENT)
      {
        sendDataToEspUart(sizeof(ctr), (uint8_t *)&ctr);
      }
    }

    if (uxQueueMessagesWaiting(uartTxQueue) > 0)
    {
      //DEBUG_PRINT("Sending CPX message\n");
      // Dequeue and wait for either CTS or CTR
      xQueueReceive(uartTxQueue, &cpxTxp, 0);
      espTxp.start = 0xFF;
      assemblePacket(&cpxTxp, &espTxp);
      do
      {
        evBits = xEventGroupWaitBits(evGroup,
                                     ESP_CTR_EVENT | ESP_CTS_EVENT,
                                     pdTRUE,  // Clear bits before returning
                                     pdFALSE, // Wait for any bit
                                     portMAX_DELAY);
        if ((evBits & ESP_CTR_EVENT) == ESP_CTR_EVENT)
        {
          sendDataToEspUart(sizeof(ctr), (uint8_t *)&ctr);
        }
      } while ((evBits & ESP_CTS_EVENT) != ESP_CTS_EVENT);
      sendDataToEspUart((uint32_t) espTxp.payloadLength + UART_META_LENGTH, (uint8_t *)&espTxp);
    }
  }
}

void cpxUARTTransportSend(const CPXRoutablePacket_t* packet) {
  xQueueSend(uartTxQueue, packet, portMAX_DELAY);
  xEventGroupSetBits(evGroup, ESP_TXQ_EVENT);
}

void cpxUARTTransportReceive(CPXRoutablePacket_t* packet) {

  static uart_transport_packet_t cpxRxp;

  xQueueReceive(uartRxQueue, &cpxRxp, portMAX_DELAY);

  packet->dataLength = (uint32_t) cpxRxp.payloadLength - CPX_ROUTING_PACKED_SIZE;
  packet->route.destination = cpxRxp.routablePayload.route.destination;
  packet->route.source = cpxRxp.routablePayload.route.source;
  packet->route.function = cpxRxp.routablePayload.route.function;
  packet->route.lastPacket = cpxRxp.routablePayload.route.lastPacket;
  memcpy(&packet->data, cpxRxp.routablePayload.data, packet->dataLength);

}

void cpxUARTTransportInit() {

  uartTxQueue = xQueueCreate(UART_TX_QUEUE_LENGTH, sizeof(CPXPacket_t));
  uartRxQueue = xQueueCreate(UART_RX_QUEUE_LENGTH, sizeof(uart_transport_packet_t));

  evGroup = xEventGroupCreate();
  bootloaderSync = xEventGroupCreate();

  // TODO: This should be in Kbuild
  uart2Init(CONFIG_DECK_CRTP_OVER_UART2_BAUDRATE);

  // Initialize task for the ESP while it's held in reset
  xTaskCreate(CPX_UART_RX, AIDECK_ESP_RX_TASK_NAME, AI_DECK_TASK_STACKSIZE, NULL,
              AI_DECK_TASK_PRI, NULL);
  xTaskCreate(CPX_UART_TX, AIDECK_ESP_TX_TASK_NAME, AI_DECK_TASK_STACKSIZE, NULL,
              AI_DECK_TASK_PRI, NULL);
}