/*
 * iSCSI Administration library
 *
 * Copyright (C) 2008-2009 Red Hat, Inc. All rights reserved.
 * Copyright (C) 2008-2009 Hans de Goede <hdegoede@redhat.com>
 * Copyright (C) 2015      Peter Hatina <phatina@redhat.com>
 * maintained by open-iscsi@googlegroups.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syslog.h>
#include "libiscsi.h"
#include "idbm.h"
#include "discovery.h"
#include "log.h"
#include "sysfs.h"
#include "iscsi_sysfs.h"
#include "session_info.h"
#include "iscsi_util.h"
#include "sysdeps.h"
#include "iface.h"
#include "iscsi_proto.h"
#include "fw_context.h"
#include "iscsid_req.h"
#include "iscsi_err.h"

#define CHECK(a) { context->error_str[0] = 0; rc = a; if (rc) goto leave; }

/* UGLY, not thread safe :( */
static int sysfs_initialized = 0;

struct libiscsi_context {
	char error_str[256];
	/* For get_parameter_helper() */
	const char *parameter;
	char *value;
};

static void libiscsi_log(int prio, void *priv, const char *fmt, va_list ap)
{
	struct libiscsi_context *context = priv;

	if (prio > LOG_ERR) /* We are only interested in errors (or worse) */
		return;

	vsnprintf(context->error_str, sizeof(context->error_str), fmt, ap);
}

struct libiscsi_context *libiscsi_init(void)
{
	struct libiscsi_context *context;

	context = calloc(1, sizeof *context);
	if (!context)
		return NULL;

	log_init("libiscsi", 1024, libiscsi_log, context);
	if (!sysfs_initialized) {
		sysfs_init();
		sysfs_initialized = 1;
	}
	increase_max_files();
	if (idbm_init(NULL)) {
		sysfs_cleanup();
		free(context);
		return NULL;
	}

	iface_setup_host_bindings();

	return context;
}

void libiscsi_cleanup(struct libiscsi_context *context)
{
	idbm_terminate();
	free_transports();
	sysfs_cleanup();
	free(context);
}

static void free_iface_list(struct list_head *ifaces)
{
	struct iface_rec *iface, *tmp_iface;

	list_for_each_entry_safe(iface, tmp_iface, ifaces, list) {
		list_del(&iface->list);
		free(iface);
	}
}

static void free_rec_list(struct list_head *rec_list)
{
	struct node_rec *rec, *tmp;
	
	list_for_each_entry_safe(rec, tmp, rec_list, list) {
		list_del(&rec->list);
		free(rec);
	}
}

int libiscsi_discover_sendtargets(struct libiscsi_context *context,
	const char *address, int port,
	const struct libiscsi_auth_info *auth_info,
	int *nr_found, struct libiscsi_node **found_nodes)
{
	struct discovery_rec drec;
	LIST_HEAD(bound_rec_list);
	struct node_rec *rec;
	int rc = 0, found = 0;

	INIT_LIST_HEAD(&bound_rec_list);

	if (nr_found)
		*nr_found = 0;
	if (found_nodes)
		*found_nodes = NULL;

	CHECK(libiscsi_verify_auth_info(context, auth_info))

	/* Fill the drec struct with all needed info */
	memset(&drec, 0, sizeof drec);
	idbm_sendtargets_defaults(&drec.u.sendtargets);
	drec.type = DISCOVERY_TYPE_SENDTARGETS;
	strlcpy(drec.address, address, sizeof(drec.address));
	drec.port = port ? port : ISCSI_LISTEN_PORT;

        if (auth_info && auth_info->method == libiscsi_auth_chap) {
		drec.u.sendtargets.auth.authmethod = AUTH_METHOD_CHAP;
		strlcpy(drec.u.sendtargets.auth.username,
			auth_info->chap.username, AUTH_STR_MAX_LEN);
		strlcpy((char *)drec.u.sendtargets.auth.password,
			auth_info->chap.password, AUTH_STR_MAX_LEN);
		drec.u.sendtargets.auth.password_length =
			strlen((char *)drec.u.sendtargets.auth.password);
		strlcpy(drec.u.sendtargets.auth.username_in,
			auth_info->chap.reverse_username, AUTH_STR_MAX_LEN);
		strlcpy((char *)drec.u.sendtargets.auth.password_in,
			auth_info->chap.reverse_password, AUTH_STR_MAX_LEN);
		drec.u.sendtargets.auth.password_in_length =
			strlen((char *)drec.u.sendtargets.auth.password_in);
	}

	CHECK(idbm_add_discovery(&drec))

	CHECK(idbm_bind_ifaces_to_nodes(discovery_sendtargets,
					&drec, NULL, &bound_rec_list))

	/* now add/update records */
	list_for_each_entry(rec, &bound_rec_list, list) {
		CHECK(idbm_add_node(rec, &drec, 1 /* overwrite */))
		found++;
	}

	if (nr_found)
		*nr_found = found;

	if (found_nodes && found) {
		*found_nodes = calloc(found, sizeof **found_nodes);
		if (*found_nodes == NULL) {
			snprintf(context->error_str,
				 sizeof(context->error_str), strerror(ENOMEM));
			rc = ISCSI_ERR_NOMEM;
			goto leave;
		}
		found = 0;
		list_for_each_entry(rec, &bound_rec_list, list) {
			strlcpy((*found_nodes)[found].name, rec->name,
				 LIBISCSI_VALUE_MAXLEN);
			(*found_nodes)[found].tpgt = rec->tpgt;
			strlcpy((*found_nodes)[found].address,
				 rec->conn[0].address, NI_MAXHOST);
			(*found_nodes)[found].port = rec->conn[0].port;
			strlcpy((*found_nodes)[found].iface,
				 rec->iface.name, LIBISCSI_VALUE_MAXLEN);
			found++;
		}
	}

leave:
	free_rec_list(&bound_rec_list);
	return rc;
}

int libiscsi_discover_firmware(struct libiscsi_context *context,
	int *nr_found, struct libiscsi_node **found_nodes)
{
	struct list_head targets, ifaces, rec_list;
	discovery_rec_t drec;
	int rc = 0;

	INIT_LIST_HEAD(&targets);
	INIT_LIST_HEAD(&ifaces);
	INIT_LIST_HEAD(&rec_list);

	if (nr_found) {
		*nr_found = 0;
	}

	if (found_nodes) {
		*found_nodes = NULL;
	}

	rc = fw_get_targets(&targets);
	if (rc) {
		log_error("%s: Could not get list of targets from firmware "
			  "(err %d).\n", __func__, rc);
		return rc;
	}

	CHECK(iface_create_ifaces_from_boot_contexts(&ifaces, &targets));

	memset(&drec, 0, sizeof(drec));
	drec.type = DISCOVERY_TYPE_FW;
	rc = idbm_bind_ifaces_to_nodes(discovery_fw, &drec, &ifaces, &rec_list);
	if (rc) {
		log_error("%s: Could not determine target nodes from firmware "
			  "(err %d).\n", __func__, rc);
		goto leave;
	}

	int node_count = 0;
	struct list_head *pos;
	list_for_each(pos, &rec_list) {
		++node_count;
	}

	struct libiscsi_node* new_nodes;
	/* allocate enough space for all the nodes */
	new_nodes = calloc(node_count, sizeof *new_nodes);
	if (new_nodes == NULL) {
		rc = ISCSI_ERR_NOMEM;
		log_error("%s: %s.\n", __func__, strerror(ENOMEM));
		goto leave;
	}

	struct node_rec *rec;
	struct libiscsi_node *new_node = new_nodes;
	/* in one loop, add nodes to idbm and create libiscsi_node entries */
	list_for_each_entry(rec, &rec_list, list) {
		CHECK(idbm_add_node(rec, NULL, 1 /* overwrite */));

		strlcpy(new_node->name, rec->name, LIBISCSI_VALUE_MAXLEN);
		new_node->tpgt = rec->tpgt;
		strlcpy(new_node->address, rec->conn[0].address, NI_MAXHOST);
		new_node->port = rec->conn[0].port;
		strlcpy(new_node->iface, rec->iface.name, LIBISCSI_VALUE_MAXLEN);

		++new_node;
	}

	/* update output parameters */
	if (nr_found) {
		*nr_found = node_count;
	}
	if (found_nodes) {
		*found_nodes = new_nodes;
	}

leave:
	fw_free_targets(&targets);

	free_iface_list(&ifaces);
	free_rec_list(&rec_list);

	return rc;
}

int libiscsi_verify_auth_info(struct libiscsi_context *context,
	const struct libiscsi_auth_info *auth_info)
{
	switch(auth_info ? auth_info->method : libiscsi_auth_none) {
	case libiscsi_auth_none:
		break;
	case libiscsi_auth_chap:
		if (!auth_info->chap.username[0]) {
			strcpy(context->error_str, "Empty username");
			return ISCSI_ERR_INVAL;
		}
		if (!auth_info->chap.password[0]) {
			strcpy(context->error_str, "Empty password");
			return ISCSI_ERR_INVAL;
		}
		if (auth_info->chap.reverse_username[0] &&
		    !auth_info->chap.reverse_password[0]) {
			strcpy(context->error_str, "Empty reverse password");
			return ISCSI_ERR_INVAL;
		}
		break;
	default:
		sprintf(context->error_str,
			"Invalid authentication method: %d",
			(int)auth_info->method);
		return ISCSI_ERR_INVAL;
	}
	return ISCSI_SUCCESS;
}

int libiscsi_node_set_auth(struct libiscsi_context *context,
    const struct libiscsi_node *node,
    const struct libiscsi_auth_info *auth_info)
{
	int rc = 0;

	CHECK(libiscsi_verify_auth_info(context, auth_info))

	switch(auth_info ? auth_info->method : libiscsi_auth_none) {
	case libiscsi_auth_none:
		CHECK(libiscsi_node_set_parameter(context, node,
			"node.session.auth.authmethod", "None"))
		CHECK(libiscsi_node_set_parameter(context, node,
			"node.session.auth.username", ""))
		CHECK(libiscsi_node_set_parameter(context, node,
			"node.session.auth.password", ""))
		CHECK(libiscsi_node_set_parameter(context, node,
			"node.session.auth.username_in", ""))
		CHECK(libiscsi_node_set_parameter(context, node,
			"node.session.auth.password_in", ""))
		break;

	case libiscsi_auth_chap:
		CHECK(libiscsi_node_set_parameter(context, node,
			"node.session.auth.authmethod", "CHAP"))
		CHECK(libiscsi_node_set_parameter(context, node,
			"node.session.auth.username",
			auth_info->chap.username))
		CHECK(libiscsi_node_set_parameter(context, node,
			"node.session.auth.password",
			auth_info->chap.password))
		CHECK(libiscsi_node_set_parameter(context, node,
			"node.session.auth.username_in",
			auth_info->chap.reverse_username))
		CHECK(libiscsi_node_set_parameter(context, node,
			"node.session.auth.password_in",
			auth_info->chap.reverse_password))
		break;
	}
leave:
	return rc;
}

int libiscsi_node_get_auth(struct libiscsi_context *context,
    const struct libiscsi_node *node,
    struct libiscsi_auth_info *auth_info)
{
	int rc = 0;
	char value[LIBISCSI_VALUE_MAXLEN];

	memset(auth_info, 0, sizeof *auth_info);

	CHECK(libiscsi_node_get_parameter(context, node,
			"node.session.auth.authmethod", value))

	if (!strcmp(value, "None")) {
		auth_info->method = libiscsi_auth_none;
	} else if (!strcmp(value, "CHAP")) {
		auth_info->method = libiscsi_auth_chap;
		CHECK(libiscsi_node_get_parameter(context, node,
			"node.session.auth.username",
			auth_info->chap.username))
		CHECK(libiscsi_node_get_parameter(context, node,
			"node.session.auth.password",
			auth_info->chap.password))
		CHECK(libiscsi_node_get_parameter(context, node,
			"node.session.auth.username_in",
			auth_info->chap.reverse_username))
		CHECK(libiscsi_node_get_parameter(context, node,
			"node.session.auth.password_in",
			auth_info->chap.reverse_password))
	} else {
		snprintf(context->error_str, sizeof(context->error_str),
			 "unknown authentication method: %s", value);
		rc = ISCSI_ERR_INVAL;
	}
leave:
	return rc;
}

static void node_to_rec(const struct libiscsi_node *node,
	struct node_rec *rec)
{
	memset(rec, 0, sizeof *rec);
	idbm_node_setup_defaults(rec);
	strlcpy(rec->name, node->name, TARGET_NAME_MAXLEN);
	rec->tpgt = node->tpgt;
	strlcpy(rec->conn[0].address, node->address, NI_MAXHOST);
	rec->conn[0].port = node->port;
}

static int login_helper(void *data, node_rec_t *rec)
{
	char *iface = (char*)data;
	if (strcmp(iface, rec->iface.name)) {
		/* different iface, skip it */
		return ISCSI_ERR;
        }

	int rc = iscsid_req_by_rec(MGMT_IPC_SESSION_LOGIN, rec);
	if (rc)
		iscsi_err_print_msg(rc);

	return rc;
}

int libiscsi_node_login(struct libiscsi_context *context,
	const struct libiscsi_node *node)
{
	int nr_found = 0, rc;

	CHECK(idbm_for_each_iface(&nr_found, (void*)node->iface, login_helper,
		(char *)node->name, node->tpgt,
		(char *)node->address, node->port))
	if (nr_found == 0) {
		strcpy(context->error_str, "No such node");
		rc = ISCSI_ERR_NO_OBJS_FOUND;
	}
leave:
	return rc;
}

static int logout_helper(void *data, struct session_info *info)
{
	int rc;
	struct node_rec *rec = data;

	if (!iscsi_match_session(rec, info)) {
		/* Tell iscsi_sysfs_for_each_session this session was not a
		   match so that it will not increase nr_found. */
		return ISCSI_ERR;
        }

	rc = iscsid_req_by_sid(MGMT_IPC_SESSION_LOGOUT, info->sid);
	if (rc)
		iscsi_err_print_msg(rc);

	return rc;
}

int libiscsi_node_logout(struct libiscsi_context *context,
	const struct libiscsi_node *node)
{
	int nr_found = 0, rc;
	struct node_rec rec;

	node_to_rec(node, &rec);
	CHECK(iscsi_sysfs_for_each_session(&rec, &nr_found, logout_helper,0))
	if (nr_found == 0) {
		strcpy(context->error_str, "No matching session");
		rc = ISCSI_ERR_NO_OBJS_FOUND;
	}
leave:
	return rc;
}

struct libiscsi_session_array {
	int cnt;
	int size;
	struct libiscsi_session_info *data;
};

static void libiscsi_session_array_init(struct libiscsi_session_array *arr)
{
	arr->cnt = 0;
	arr->size = 0;
	arr->data = NULL;
}

static int libiscsi_session_array_grow(struct libiscsi_session_array *arr)
{
	if (arr->size == 0)
		arr->size = 4;
	else
		arr->size *= 2;

	arr->data = (struct libiscsi_session_info *) realloc(
		arr->data,
		arr->size * sizeof(struct libiscsi_session_info));

	return arr->data ? 0 : 1;
}

static int libiscsi_session_array_grow_ondemand(struct libiscsi_session_array *arr)
{
	if (arr->size == arr->cnt)
		return libiscsi_session_array_grow(arr);
	return ISCSI_SUCCESS;
}

static int libiscsi_session_array_resize_precize(struct libiscsi_session_array *arr)
{
	arr->data = (struct libiscsi_session_info *) realloc(
		arr->data,
		arr->cnt * sizeof(struct libiscsi_session_info));
	arr->size = arr->cnt;

	return arr->data ? ISCSI_SUCCESS : ISCSI_ERR;
}

static void copy_session_info_to_libiscsi_session_info(
	struct libiscsi_session_info *info,
	struct session_info *s_info)
{
	/* Copy session info to public struct. */
	info->sid = s_info->sid;
	/* Timeouts */
	memcpy(&info->tmo, &s_info->tmo, sizeof(struct libiscsi_session_timeout));
	/* CHAP authentication information */
	memcpy(&info->chap, &s_info->chap, sizeof(struct libiscsi_chap_auth_info));
	/* Target information */
	strncpy(info->targetname, s_info->targetname, LIBISCSI_VALUE_MAXLEN);
	strncpy(info->address, s_info->address, NI_MAXHOST);
	strncpy(info->persistent_address, s_info->persistent_address, NI_MAXHOST);
	info->tpgt = s_info->tpgt;
	info->persistent_port = s_info->persistent_port;
}

static int get_sessions_helper(void *data, struct session_info *s_info)
{
	struct libiscsi_session_array *arr = (struct libiscsi_session_array *) data;

	if (libiscsi_session_array_grow_ondemand(arr) != 0)
		return ISCSI_ERR_NOMEM;

	copy_session_info_to_libiscsi_session_info(&arr->data[arr->cnt++], s_info);

	return ISCSI_SUCCESS;
}

int libiscsi_get_session_infos(struct libiscsi_context *context,
	struct libiscsi_session_info **infos,
	int *nr_sessions)
{
	int rc = 0;
	int nr_found = 0;
	struct libiscsi_session_array arr;

	if (!context || !infos || !nr_sessions)
		return ISCSI_ERR_INVAL;

	libiscsi_session_array_init(&arr);

	rc = iscsi_sysfs_for_each_session((void *) &arr, &nr_found,
		get_sessions_helper, 0);
	if (rc != 0 || nr_found == 0) {
		strcpy(context->error_str, "No matching session");
		return ISCSI_ERR_NO_OBJS_FOUND;
	}

	if (libiscsi_session_array_resize_precize(&arr) != 0) {
		strcpy(context->error_str, "Can't allocate memory for session infos");
		return ISCSI_ERR_NOMEM;
	}

	*infos = arr.data;
	*nr_sessions = nr_found;

	return ISCSI_SUCCESS;
}

int libiscsi_get_session_info_by_id(struct libiscsi_context *context,
	struct libiscsi_session_info *info,
	const char *session)
{
	struct session_info s_info;

	if (!context || !info || !session)
		return ISCSI_ERR_INVAL;

	if (iscsi_sysfs_get_sessioninfo_by_id(&s_info, (char*) session) != 0) {
		strcpy(context->error_str, "No matching session");
		return ISCSI_ERR_NO_OBJS_FOUND;
	}

	copy_session_info_to_libiscsi_session_info(info, &s_info);

	return ISCSI_SUCCESS;
}

int libiscsi_node_set_parameter(struct libiscsi_context *context,
	const struct libiscsi_node *node,
	const char *parameter, const char *value)
{
	int nr_found = 0, rc;
	struct user_param *param;
	struct list_head params;

	INIT_LIST_HEAD(&params);
	param = idbm_alloc_user_param(parameter, value);
	if (!param) {
		rc = ISCSI_ERR_NOMEM;
		goto leave;
	}
	list_add_tail(&params, &param->list);

	CHECK(idbm_for_each_iface(&nr_found, &params, idbm_node_set_param,
		(char *)node->name, node->tpgt,
		(char *)node->address, node->port))
	if (nr_found == 0) {
		strcpy(context->error_str, "No such node");
		rc = ISCSI_ERR_NO_OBJS_FOUND;
	}
	free(param->name);
	free(param);
leave:
	return rc;
}

static int get_parameter_helper(void *data, node_rec_t *rec)
{
	struct libiscsi_context *context = data;
	recinfo_t *info;
	int i;

	info = idbm_recinfo_alloc(MAX_KEYS);
	if (!info) {
		snprintf(context->error_str, sizeof(context->error_str),
			 strerror(ENOMEM));
		return ISCSI_ERR_NOMEM;
	}

	idbm_recinfo_node(rec, info);

	for (i = 0; i < MAX_KEYS; i++) {
		if (!info[i].visible)
			continue;

		if (strcmp(context->parameter, info[i].name))
			continue;

		strlcpy(context->value, info[i].value, LIBISCSI_VALUE_MAXLEN);
		break;
	}

	free(info);

	if (i == MAX_KEYS) {
		strcpy(context->error_str, "No such parameter");
		return ISCSI_ERR_INVAL;
	}

	return ISCSI_SUCCESS;
}

int libiscsi_node_get_parameter(struct libiscsi_context *context,
	const struct libiscsi_node *node, const char *parameter, char *value)
{
	int nr_found = 0, rc = 0;

	context->parameter = parameter;
	context->value = value;

	/* Note we assume there is only one interface, if not we will get
	   the value from the last interface iterated over!
	   This (multiple interfaces) can only happen if someone explicitly
	   created ones using iscsiadm. Even then this should not be a problem
	   as most settings should be the same independent of the iface. */
	CHECK(idbm_for_each_iface(&nr_found, context, get_parameter_helper,
		(char *)node->name, node->tpgt,
		(char *)node->address, node->port))
	if (nr_found == 0) {
		strcpy(context->error_str, "No such node");
		rc = ISCSI_ERR_NO_OBJS_FOUND;
	}
leave:
	return rc;
}

const char *libiscsi_get_error_string(struct libiscsi_context *context)
{
	/* Sometimes the core open-iscsi code does not give us an error
	   message */
	if (!context->error_str[0])
		return "Unknown error";

	return context->error_str;
}


/************************** Utility functions *******************************/

int libiscsi_get_firmware_network_config(
    struct libiscsi_network_config *config)
{
	struct boot_context fw_entry;

	if (!sysfs_initialized) {
		sysfs_init();
		sysfs_initialized = 1;
	}

	memset(config, 0, sizeof *config);
	memset(&fw_entry, 0, sizeof fw_entry);
	if (fw_get_entry(&fw_entry))
		return ISCSI_ERR_NO_OBJS_FOUND;

	config->dhcp = strlen(fw_entry.dhcp) ? 1 : 0;
	strlcpy(config->iface_name, fw_entry.iface, LIBISCSI_VALUE_MAXLEN);
	strlcpy(config->mac_address, fw_entry.mac, LIBISCSI_VALUE_MAXLEN);
	strlcpy(config->ip_address, fw_entry.ipaddr, LIBISCSI_VALUE_MAXLEN);
	strlcpy(config->netmask, fw_entry.mask, LIBISCSI_VALUE_MAXLEN);
	strlcpy(config->gateway, fw_entry.gateway, LIBISCSI_VALUE_MAXLEN);
	strlcpy(config->primary_dns, fw_entry.primary_dns, LIBISCSI_VALUE_MAXLEN);
	strlcpy(config->secondary_dns, fw_entry.secondary_dns, LIBISCSI_VALUE_MAXLEN);

	return ISCSI_SUCCESS;
}

int libiscsi_get_firmware_initiator_name(char *initiatorname)
{
	struct boot_context fw_entry;

	if (!sysfs_initialized) {
		sysfs_init();
		sysfs_initialized = 1;
	}

	memset(initiatorname, 0, LIBISCSI_VALUE_MAXLEN);
	memset(&fw_entry, 0, sizeof fw_entry);
	if (fw_get_entry(&fw_entry))
		return ISCSI_ERR_NO_OBJS_FOUND;

	strlcpy(initiatorname, fw_entry.initiatorname, LIBISCSI_VALUE_MAXLEN);

	return ISCSI_SUCCESS;
}
