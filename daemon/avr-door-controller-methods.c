/*
 * Copyright (C) 2017 Alban Bedel <albeu@free.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "avr-door-controller-daemon.h"
#include "../firmware/ctrl-cmd-types.h"
#include <endian.h>

static const struct blobmsg_policy get_device_descriptor_args[] = {
};

static int read_get_device_descriptor_response(
	const void *response, struct blob_buf *bbuf)
{
	const struct device_descriptor *desc = response;

	blobmsg_add_u32(bbuf, "major_version", desc->major_version);
	blobmsg_add_u32(bbuf, "minor_version", desc->minor_version);
	blobmsg_add_u32(bbuf, "num_doors", desc->num_doors);
	blobmsg_add_u32(bbuf, "num_access_records",
			le16toh(desc->num_access_records));
	return 0;
}

static const struct blobmsg_policy get_door_config_args[] = {
	{
		.name = "index",
		.type = BLOBMSG_TYPE_INT32,
	},
};

static int write_get_door_config_query(
	struct blob_attr *const *const args,
	void *query, struct blob_buf *bbuf)
{
	struct ctrl_cmd_get_door_config *cmd = query;

	blobmsg_add_u32(bbuf, "index", blobmsg_get_u32(args[0]));
	cmd->index = blobmsg_get_u32(args[0]);
	return 0;
}

static int read_get_door_config_response(
	const void *response, struct blob_buf *bbuf)
{
	const struct door_config *cfg = (struct door_config *)response;

	blobmsg_add_u32(bbuf, "open_time", le16toh(cfg->open_time));
	return 0;
}

static char *access_record_types[] = { "none", "pin", "card", "pin+card" };

static const struct blobmsg_policy get_access_record_args[] = {
	{
		.name = "index",
		.type = BLOBMSG_TYPE_INT32,
	},
};

static int write_get_access_record_query(
	struct blob_attr *const *const args,
	void *query, struct blob_buf *bbuf)
{
	struct ctrl_cmd_get_access_record *cmd = query;

	blobmsg_add_u32(bbuf, "index", blobmsg_get_u32(args[0]));
	cmd->index = htole16(blobmsg_get_u32(args[0]));
	return 0;
}

static int read_get_access_record_response(
	const void *response, struct blob_buf *bbuf)
{
	const struct access_record *rec = (struct access_record *)response;
	uint32_t key;
	char skey[9];
	uint8_t perms;
	int i, j;

	/* The bit fields are broken with Chaos Calmer MIPS compiler */
	perms = ((uint8_t*)rec)[4];
	key = le32toh(rec->key);

	/* If the record is invalid ignore it */
	if (perms & BIT(2))
		perms = 0;

	blobmsg_add_string(bbuf, "type", access_record_types[perms & 0x3]);

	switch (perms & 0x3) {
	case ACCESS_TYPE_NONE:
		/* Empty record, nothing to add */
		return 0;
	case ACCESS_TYPE_PIN:
		for (i = 7, j = 0; i >= 0; i--) {
			uint8_t digit = (key >> (i * 4)) & 0xF;
			if (digit == 0xF)
				continue;
			skey[j++] = digit + '0';
		}
		skey[j] = 0;
		break;
	case ACCESS_TYPE_CARD:
	case ACCESS_TYPE_CARD_AND_PIN:
		snprintf(skey, sizeof(skey), "%u", key);
		break;
	}

	blobmsg_add_string(bbuf, "key", skey);
	blobmsg_add_u32(bbuf, "doors", perms >> 4);
	return 0;
}

#define SET_ACCESS_RECORD_INDEX		0
#define SET_ACCESS_RECORD_PIN		1
#define SET_ACCESS_RECORD_CARD		2
#define SET_ACCESS_RECORD_DOORS		3
#define SET_ACCESS_RECORD_ARGS_COUNT	ARRAY_SIZE(set_access_record_args)

static const struct blobmsg_policy set_access_record_args[] = {
	[SET_ACCESS_RECORD_INDEX] = {
		.name = "index",
		.type = BLOBMSG_TYPE_INT32,
	},
	[SET_ACCESS_RECORD_PIN] = {
		.name = "pin",
		.type = BLOBMSG_TYPE_STRING,
	},
	[SET_ACCESS_RECORD_CARD] = {
		.name = "card",
		.type = BLOBMSG_TYPE_STRING,
	},
	[SET_ACCESS_RECORD_DOORS] = {
		.name = "doors",
		.type = BLOBMSG_TYPE_INT32,
	},
};

static int write_set_access_record_query(
	struct blob_attr *const *const args,
	void *query, struct blob_buf *bbuf)
{
	struct ctrl_cmd_set_access_record *cmd = query;
	uint32_t card = 0, pin = 0;
	char *str_card, *str_pin;
	uint8_t doors = 0;
	uint8_t type;
	int i;

	cmd->index = htole16(blobmsg_get_u32(args[SET_ACCESS_RECORD_INDEX]));

	str_pin = blobmsg_get_string(args[SET_ACCESS_RECORD_PIN]);
	str_card = blobmsg_get_string(args[SET_ACCESS_RECORD_CARD]);
	if (args[SET_ACCESS_RECORD_DOORS])
		doors = blobmsg_get_u32(args[SET_ACCESS_RECORD_DOORS]) & 0xF;

	if (str_card && str_pin)
		type = ACCESS_TYPE_CARD_AND_PIN;
	else if (str_card)
		type = ACCESS_TYPE_CARD;
	else if (str_pin)
		type = ACCESS_TYPE_PIN;
	else
		type = ACCESS_TYPE_NONE;

	switch (type) {
	case ACCESS_TYPE_NONE:
		break;

	case ACCESS_TYPE_CARD:
	case ACCESS_TYPE_CARD_AND_PIN:
		if (sscanf(str_card, "%u", &card) != 1)
			return UBUS_STATUS_INVALID_ARGUMENT;
		if (type == ACCESS_TYPE_CARD)
			break;
		/* Fallthrough for card and pin */

	case ACCESS_TYPE_PIN:
		pin = 0xFFFFFFFF;
		for (i = 0; i < strlen(str_pin); i++) {
			uint8_t digit = str_pin[i] - '0';
			if (digit > 9)
				return UBUS_STATUS_INVALID_ARGUMENT;
			pin = (pin << 4) | digit;
		}
		break;

	default:
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	cmd->record.key = htole32(card ^ pin);
	((uint8_t*)&cmd->record)[4] = (doors << 4) | type;

	return 0;
}

#define SET_ACCESS_PIN		0
#define SET_ACCESS_CARD		1
#define SET_ACCESS_DOORS	2
#define SET_ACCESS_ARGS_COUNT	ARRAY_SIZE(set_access_args)

static const struct blobmsg_policy set_access_args[] = {
	[SET_ACCESS_PIN] = {
		.name = "pin",
		.type = BLOBMSG_TYPE_STRING,
	},
	[SET_ACCESS_CARD] = {
		.name = "card",
		.type = BLOBMSG_TYPE_STRING,
	},
	[SET_ACCESS_DOORS] = {
		.name = "doors",
		.type = BLOBMSG_TYPE_INT32,
	},
};

static int write_set_access_query(
	struct blob_attr *const *const args,
	void *query, struct blob_buf *bbuf)
{
	struct access_record *rec = query;
	uint32_t card = 0, pin = 0;
	char *str_card, *str_pin;
	uint8_t doors = 0;
	uint8_t type;
	int i;

	str_pin = blobmsg_get_string(args[SET_ACCESS_PIN]);
	str_card = blobmsg_get_string(args[SET_ACCESS_CARD]);
	if (args[SET_ACCESS_DOORS])
		doors = blobmsg_get_u32(args[SET_ACCESS_DOORS]) & 0xF;

	if (str_card && str_pin)
		type = ACCESS_TYPE_CARD_AND_PIN;
	else if (str_card)
		type = ACCESS_TYPE_CARD;
	else if (str_pin)
		type = ACCESS_TYPE_PIN;
	else
		return UBUS_STATUS_INVALID_ARGUMENT;

	switch (type) {
	case ACCESS_TYPE_CARD:
	case ACCESS_TYPE_CARD_AND_PIN:
		if (sscanf(str_card, "%u", &card) != 1)
			return UBUS_STATUS_INVALID_ARGUMENT;
		if (type == ACCESS_TYPE_CARD)
			break;
		/* Fallthrough for card and pin */

	case ACCESS_TYPE_PIN:
		pin = 0xFFFFFFFF;
		for (i = 0; i < strlen(str_pin); i++) {
			uint8_t digit = str_pin[i] - '0';
			if (digit > 9)
				return UBUS_STATUS_INVALID_ARGUMENT;
			pin = (pin << 4) | digit;
		}
		break;
	}

	rec->key = htole32(card ^ pin);
	((uint8_t*)rec)[4] = (doors << 4) | type;

	return 0;
}

static const struct blobmsg_policy remove_all_access_args[] = {
};

#define AVR_DOOR_CTRL_METHOD(method, opt_args, cmd_id,			\
			     wr_query, qr_size, rd_resp, resp_size)	\
	{								\
		.name = #method,					\
		.args = method ## _args,				\
		.num_args = ARRAY_SIZE(method ## _args),		\
		.optional_args = opt_args,				\
		.cmd = cmd_id,				       		\
		.write_query = wr_query,				\
		.query_size = qr_size,					\
		.read_response = rd_resp,				\
		.response_size = resp_size,				\
	}

const struct avr_door_ctrl_method avr_door_ctrl_methods[] = {
	AVR_DOOR_CTRL_METHOD(
		get_device_descriptor, 0,
		CTRL_CMD_GET_DEVICE_DESCRIPTOR,
		NULL, 0,
		read_get_device_descriptor_response,
		sizeof(struct device_descriptor)),

	AVR_DOOR_CTRL_METHOD(
		get_door_config, 0,
		CTRL_CMD_GET_DOOR_CONFIG,
		write_get_door_config_query,
		sizeof(struct ctrl_cmd_get_door_config),
		read_get_door_config_response,
		sizeof(struct door_config)),

	AVR_DOOR_CTRL_METHOD(
		get_access_record, 0,
		CTRL_CMD_GET_ACCESS_RECORD,
		write_get_access_record_query,
		sizeof(struct ctrl_cmd_get_access_record),
		read_get_access_record_response,
		sizeof(struct access_record)),

	AVR_DOOR_CTRL_METHOD(
		set_access_record, 0,
		CTRL_CMD_SET_ACCESS_RECORD,
		write_set_access_record_query,
		sizeof(struct ctrl_cmd_set_access_record),
		NULL, 0),

	AVR_DOOR_CTRL_METHOD(
		set_access,
		BIT(SET_ACCESS_PIN) |
		BIT(SET_ACCESS_CARD) |
		BIT(SET_ACCESS_DOORS),
		CTRL_CMD_SET_ACCESS,
		write_set_access_query,
		sizeof(struct access_record),
		NULL, 0),

	AVR_DOOR_CTRL_METHOD(
		remove_all_access, 0,
		CTRL_CMD_REMOVE_ALL_ACCESS,
		NULL, 0, NULL, 0),
};

const struct avr_door_ctrl_method *avr_door_ctrl_get_method(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(avr_door_ctrl_methods); i++)
		if (!strcmp(avr_door_ctrl_methods[i].name, name))
			return &avr_door_ctrl_methods[i];

	return NULL;
}

static struct ubus_method
avr_door_ctrl_umethods[ARRAY_SIZE(avr_door_ctrl_methods)] = {};

static struct ubus_object_type avr_door_ctrl_utype =
	UBUS_OBJECT_TYPE("door_ctrl", avr_door_ctrl_umethods);


void avr_door_ctrld_init_door_uobject(
	const char *name, struct ubus_object *uobj)
{
	static bool umethods_inited = false;
	int i;

	if (!umethods_inited) {
		for (i = 0; i < ARRAY_SIZE(avr_door_ctrl_methods); i++) {
			const struct avr_door_ctrl_method *m =
				&avr_door_ctrl_methods[i];
			struct ubus_method *u =
				&avr_door_ctrl_umethods[i];
			u->name = m->name;
			u->handler = avr_door_ctrl_method_handler;
			u->policy = m->args;
			u->n_policy = m->num_args;
		}
		umethods_inited = true;
	}

	uobj->name = name;
	uobj->type = &avr_door_ctrl_utype;
	uobj->methods = avr_door_ctrl_umethods;
	uobj->n_methods = ARRAY_SIZE(avr_door_ctrl_umethods);
}