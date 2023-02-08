/* bttester.c - Bluetooth Tester */

/*
 * Copyright (c) 2015-2016 Intel Corporation
 * Copyright (c) 2022 Codecoup
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/toolchain.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/uart_pipe.h>

#include <zephyr/logging/log.h>
#define LOG_MODULE_NAME bttester
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include "btp/btp.h"

#define STACKSIZE 2048
static K_THREAD_STACK_DEFINE(stack, STACKSIZE);
static struct k_thread cmd_thread;

#define CMD_QUEUED 2
struct btp_buf {
	intptr_t _reserved;
	union {
		uint8_t data[BTP_MTU];
		struct btp_hdr hdr;
	};
};

static struct btp_buf cmd_buf[CMD_QUEUED];

static K_FIFO_DEFINE(cmds_queue);
static K_FIFO_DEFINE(avail_queue);

static void cmd_handler(void *p1, void *p2, void *p3)
{
	while (1) {
		struct btp_buf *cmd;
		uint16_t len;

		cmd = k_fifo_get(&cmds_queue, K_FOREVER);

		len = sys_le16_to_cpu(cmd->hdr.len);

		/* TODO
		 * verify if service is registered before calling handler
		 */

		switch (cmd->hdr.service) {
		case BTP_SERVICE_ID_CORE:
			tester_handle_core(cmd->hdr.opcode, cmd->hdr.index,
					   cmd->hdr.data, len);
			break;
		case BTP_SERVICE_ID_GAP:
			tester_handle_gap(cmd->hdr.opcode, cmd->hdr.index,
					  cmd->hdr.data, len);
			break;
		case BTP_SERVICE_ID_GATT:
			tester_handle_gatt(cmd->hdr.opcode, cmd->hdr.index,
					   cmd->hdr.data, len);
			break;
#if defined(CONFIG_BT_L2CAP_DYNAMIC_CHANNEL)
		case BTP_SERVICE_ID_L2CAP:
			tester_handle_l2cap(cmd->hdr.opcode, cmd->hdr.index,
					    cmd->hdr.data, len);
#endif /* CONFIG_BT_L2CAP_DYNAMIC_CHANNEL */
			break;
#if defined(CONFIG_BT_MESH)
		case BTP_SERVICE_ID_MESH:
			tester_handle_mesh(cmd->hdr.opcode, cmd->hdr.index,
					   cmd->hdr.data, len);
			break;
#endif /* CONFIG_BT_MESH */
#if defined(CONFIG_BT_VCP_VOL_REND)
		case BTP_SERVICE_ID_VCS:
			tester_handle_vcs(cmd->hdr.opcode, cmd->hdr.index,
					  cmd->hdr.data, len);
			break;
#endif /* CONFIG_BT_VCP_VOL_REND */
#if defined(CONFIG_BT_AICS)
		case BTP_SERVICE_ID_AICS:
			tester_handle_aics(cmd->hdr.opcode, cmd->hdr.index,
					   cmd->hdr.data, len);
			break;
#endif /* CONFIG_BT_AICS */
#if defined(CONFIG_BT_VOCS)
		case BTP_SERVICE_ID_VOCS:
			tester_handle_vocs(cmd->hdr.opcode, cmd->hdr.index,
					   cmd->hdr.data, len);
			break;
#endif /* CONFIG_BT_VOCS */
#if defined(CONFIG_BT_PACS)
		case BTP_SERVICE_ID_PACS:
			tester_handle_pacs(cmd->hdr.opcode, cmd->hdr.index,
					   cmd->hdr.data, len);
			break;
#endif /* CONFIG_BT_PACS */
		default:
			LOG_WRN("unknown service: 0x%x", cmd->hdr.service);
			tester_rsp(cmd->hdr.service, cmd->hdr.opcode,
				   cmd->hdr.index, BTP_STATUS_FAILED);
			break;
		}

		k_fifo_put(&avail_queue, cmd);
	}
}

static uint8_t *recv_cb(uint8_t *buf, size_t *off)
{
	struct btp_hdr *cmd = (void *) buf;
	struct btp_buf *new_buf;
	uint16_t len;

	if (*off < sizeof(*cmd)) {
		return buf;
	}

	len = sys_le16_to_cpu(cmd->len);
	if (len > BTP_MTU - sizeof(*cmd)) {
		LOG_ERR("BT tester: invalid packet length");
		*off = 0;
		return buf;
	}

	if (*off < sizeof(*cmd) + len) {
		return buf;
	}

	new_buf =  k_fifo_get(&avail_queue, K_NO_WAIT);
	if (!new_buf) {
		LOG_ERR("BT tester: RX overflow");
		*off = 0;
		return buf;
	}

	k_fifo_put(&cmds_queue, CONTAINER_OF(buf, struct btp_buf, data));

	*off = 0;
	return new_buf->data;
}

#if defined(CONFIG_UART_PIPE)
/* Uart Pipe */
static void uart_init(uint8_t *data)
{
	uart_pipe_register(data, BTP_MTU, recv_cb);
}

static void uart_send(uint8_t *data, size_t len)
{
	uart_pipe_send(data, len);
}
#else /* !CONFIG_UART_PIPE */
static uint8_t *recv_buf;
static size_t recv_off;
static const struct device *const dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static void timer_expiry_cb(struct k_timer *timer)
{
	uint8_t c;

	while (uart_poll_in(dev, &c) == 0) {
		recv_buf[recv_off++] = c;
		recv_buf = recv_cb(recv_buf, &recv_off);
	}
}

K_TIMER_DEFINE(timer, timer_expiry_cb, NULL);

/* Uart Poll */
static void uart_init(uint8_t *data)
{
	__ASSERT_NO_MSG(device_is_ready(dev));

	recv_buf = data;

	k_timer_start(&timer, K_MSEC(10), K_MSEC(10));
}

static void uart_send(uint8_t *data, size_t len)
{
	int i;

	for (i = 0; i < len; i++) {
		uart_poll_out(dev, data[i]);
	}
}
#endif /* CONFIG_UART_PIPE */

void tester_init(void)
{
	int i;
	struct btp_buf *buf;

	LOG_DBG("Initializing tester");

	for (i = 0; i < CMD_QUEUED; i++) {
		k_fifo_put(&avail_queue, &cmd_buf[i]);
	}

	k_thread_create(&cmd_thread, stack, STACKSIZE, cmd_handler,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

	buf = k_fifo_get(&avail_queue, K_NO_WAIT);

	uart_init(buf->data);

	tester_send(BTP_SERVICE_ID_CORE, BTP_CORE_EV_IUT_READY, BTP_INDEX_NONE,
		    NULL, 0);
}

void tester_send(uint8_t service, uint8_t opcode, uint8_t index, uint8_t *data,
		 size_t len)
{
	struct btp_hdr msg;

	msg.service = service;
	msg.opcode = opcode;
	msg.index = index;
	msg.len = len;

	uart_send((uint8_t *)&msg, sizeof(msg));
	if (data && len) {
		uart_send(data, len);
	}
}

void tester_rsp(uint8_t service, uint8_t opcode, uint8_t index, uint8_t status)
{
	struct btp_status s;

	if (status == BTP_STATUS_SUCCESS) {
		tester_send(service, opcode, index, NULL, 0);
		return;
	}

	s.code = status;
	tester_send(service, BTP_STATUS, index, (uint8_t *) &s, sizeof(s));
}
