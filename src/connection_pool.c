/*
 * Copyright (c) 2020 YuQing <384681@qq.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the Lesser GNU General Public License, version 3
 * or later ("LGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the Lesser GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include "logger.h"
#include "sockopt.h"
#include "shared_func.h"
#include "sched_thread.h"
#include "server_id_func.h"
#include "connection_pool.h"

ConnectionCallbacks g_connection_callbacks = {
    false, {{conn_pool_connect_server_ex1,
        conn_pool_disconnect_server,
        conn_pool_is_connected},
    {NULL, NULL, NULL}}, {NULL}
};

static int node_init_for_socket(ConnectionNode *node,
        ConnectionPool *cp)
{
    node->conn = (ConnectionInfo *)(node + 1);
    return 0;
}

static int node_init_for_rdma(ConnectionNode *node,
        ConnectionPool *cp)
{
    node->conn = (ConnectionInfo *)(node + 1);
    node->conn->arg1 = node->conn->args + cp->extra_data_size;
    return G_RDMA_CONNECTION_CALLBACKS.init_connection(node->conn,
            cp->extra_params.buffer_size, cp->extra_params.pd);
}

int conn_pool_init_ex1(ConnectionPool *cp, int connect_timeout,
	const int max_count_per_entry, const int max_idle_time,
    const int socket_domain, const int htable_init_capacity,
    fc_connection_callback_func connect_done_func, void *connect_done_args,
    fc_connection_callback_func validate_func, void *validate_args,
    const int extra_data_size, const ConnectionExtraParams *extra_params)
{
    const int64_t alloc_elements_limit = 0;
	int result;
    int init_capacity;
    int extra_connection_size;
    fast_mblock_object_init_func obj_init_func;

	if ((result=init_pthread_lock(&cp->lock)) != 0)
	{
		return result;
	}
	cp->connect_timeout_ms = connect_timeout * 1000;
	cp->max_count_per_entry = max_count_per_entry;
	cp->max_idle_time = max_idle_time;
	cp->extra_data_size = extra_data_size;
	cp->socket_domain = socket_domain;
    cp->connect_done_callback.func = connect_done_func;
    cp->connect_done_callback.args = connect_done_args;
    cp->validate_callback.func = validate_func;
    cp->validate_callback.args = validate_args;

    init_capacity = htable_init_capacity > 0 ? htable_init_capacity : 256;
    if ((result=fast_mblock_init_ex1(&cp->manager_allocator, "cpool-manager",
                    sizeof(ConnectionManager), init_capacity,
                    alloc_elements_limit, NULL, NULL, false)) != 0)
    {
        return result;
    }

    if (extra_params != NULL) {
        extra_connection_size = G_RDMA_CONNECTION_CALLBACKS.
            get_connection_size();
        obj_init_func = (fast_mblock_object_init_func)node_init_for_rdma;
        cp->extra_params = *extra_params;
    } else {
        extra_connection_size = 0;
        cp->extra_params.buffer_size = 0;
        cp->extra_params.pd = NULL;
        obj_init_func = (fast_mblock_object_init_func)node_init_for_socket;
    }
    if ((result=fast_mblock_init_ex1(&cp->node_allocator, "cpool-node",
                    sizeof(ConnectionNode) + sizeof(ConnectionInfo) +
                    extra_data_size + extra_connection_size, init_capacity,
                    alloc_elements_limit, obj_init_func, cp, true)) != 0)
    {
        return result;
    }

	return fc_hash_init(&(cp->hash_array), fc_simple_hash, init_capacity, 0.75);
}

static int coon_pool_close_connections(const int index,
        const HashData *data, void *args)
{
    ConnectionPool *cp;
    ConnectionManager *cm;

    cp = (ConnectionPool *)args;
    cm = (ConnectionManager *)data->value;
    if (cm != NULL)
    {
        ConnectionNode *node;
        ConnectionNode *deleted;

        node = cm->head;
        while (node != NULL)
        {
            deleted = node;
            node = node->next;

            G_COMMON_CONNECTION_CALLBACKS[deleted->conn->comm_type].
                close_connection(deleted->conn);
            fast_mblock_free_object(&cp->node_allocator, deleted);
        }

        fast_mblock_free_object(&cp->manager_allocator, cm);
    }

    return 0;
}

void conn_pool_destroy(ConnectionPool *cp)
{
	pthread_mutex_lock(&cp->lock);
    fc_hash_walk(&(cp->hash_array), coon_pool_close_connections, cp);
	fc_hash_destroy(&(cp->hash_array));
	pthread_mutex_unlock(&cp->lock);

	pthread_mutex_destroy(&cp->lock);
}

void conn_pool_disconnect_server(ConnectionInfo *conn)
{
    if (conn->sock >= 0)
    {
        close(conn->sock);
        conn->sock = -1;
    }
}

bool conn_pool_is_connected(ConnectionInfo *conn)
{
    return (conn->sock >= 0);
}

int conn_pool_connect_server_ex1(ConnectionInfo *conn,
        const char *service_name, const int connect_timeout_ms,
        const char *bind_ipaddr, const bool log_connect_error)
{
	int result;

	if (conn->sock >= 0)
	{
		close(conn->sock);
	}

    if ((conn->sock=socketCreateEx2(conn->socket_domain, conn->ip_addr,
                    O_NONBLOCK, bind_ipaddr, &result)) < 0)
    {
        return result;
    }

	if ((result=connectserverbyip_nb(conn->sock, conn->ip_addr,
                    conn->port, connect_timeout_ms / 1000)) != 0)
	{
        if (log_connect_error)
        {
            logError("file: "__FILE__", line: %d, "
                    "connect to %s%sserver %s:%u fail, errno: %d, "
                    "error info: %s", __LINE__, service_name != NULL ?
                    service_name : "", service_name != NULL ?  " " : "",
                    conn->ip_addr, conn->port, result, STRERROR(result));
        }

		close(conn->sock);
		conn->sock = -1;
		return result;
	}

	return 0;
}

int conn_pool_async_connect_server_ex(ConnectionInfo *conn,
        const char *bind_ipaddr)
{
    int result;

    if (conn->sock >= 0)
    {
        close(conn->sock);
    }

    if ((conn->sock=socketCreateEx2(conn->socket_domain,
                    conn->ip_addr, O_NONBLOCK, bind_ipaddr,
                    &result)) < 0)
    {
        return result;
    }

    result = asyncconnectserverbyip(conn->sock, conn->ip_addr, conn->port);
    if (!(result == 0 || result == EINPROGRESS))
    {
        logError("file: "__FILE__", line: %d, "
                "connect to server %s:%u fail, errno: %d, "
                "error info: %s", __LINE__, conn->ip_addr,
                conn->port, result, STRERROR(result));
        close(conn->sock);
        conn->sock = -1;
    }

    return result;
}

static inline void  conn_pool_get_key(const ConnectionInfo *conn,
        char *key, int *key_len)
{
    *key_len = sprintf(key, "%s_%u", conn->ip_addr, conn->port);
}

ConnectionInfo *conn_pool_get_connection_ex(ConnectionPool *cp,
	const ConnectionInfo *conn, const char *service_name, int *err_no)
{
	char key[INET6_ADDRSTRLEN + 8];
	int key_len;
	ConnectionManager *cm;
	ConnectionNode *node;
	ConnectionInfo *ci;
	time_t current_time;

	conn_pool_get_key(conn, key, &key_len);

	pthread_mutex_lock(&cp->lock);
	cm = (ConnectionManager *)fc_hash_find(&cp->hash_array, key, key_len);
	if (cm == NULL)
	{
		cm = (ConnectionManager *)fast_mblock_alloc_object(
                &cp->manager_allocator);
		if (cm == NULL)
		{
			*err_no = ENOMEM;
			logError("file: "__FILE__", line: %d, "
				"malloc %d bytes fail", __LINE__,
				(int)sizeof(ConnectionManager));
			pthread_mutex_unlock(&cp->lock);
			return NULL;
		}

		cm->head = NULL;
		cm->total_count = 0;
		cm->free_count = 0;
		if ((*err_no=init_pthread_lock(&cm->lock)) != 0)
		{
			pthread_mutex_unlock(&cp->lock);
			return NULL;
		}
		fc_hash_insert(&cp->hash_array, key, key_len, cm);
	}
	pthread_mutex_unlock(&cp->lock);

	current_time = get_current_time();
	pthread_mutex_lock(&cm->lock);
	while (1)
	{
		if (cm->head == NULL)
		{
			if ((cp->max_count_per_entry > 0) && 
				(cm->total_count >= cp->max_count_per_entry))
			{
				*err_no = ENOSPC;
				logError("file: "__FILE__", line: %d, "
					"connections: %d of %s%sserver %s:%u exceed limit: %d",
                    __LINE__, cm->total_count, service_name != NULL ?
                    service_name : "", service_name != NULL ? " " : "",
                    conn->ip_addr, conn->port, cp->max_count_per_entry);
				pthread_mutex_unlock(&cm->lock);
				return NULL;
			}

            node = (ConnectionNode *)fast_mblock_alloc_object(
                    &cp->node_allocator);
			if (node == NULL)
            {
                *err_no = ENOMEM;
                logError("file: "__FILE__", line: %d, "
                        "malloc %d bytes fail", __LINE__, (int)
                        (sizeof(ConnectionNode) + sizeof(ConnectionInfo)));
                pthread_mutex_unlock(&cm->lock);
                return NULL;
            }

			node->manager = cm;
			node->next = NULL;
			node->atime = 0;

			cm->total_count++;
			pthread_mutex_unlock(&cm->lock);

			memcpy(node->conn->ip_addr, conn->ip_addr, sizeof(conn->ip_addr));
            node->conn->port = conn->port;
            node->conn->comm_type = conn->comm_type;
            node->conn->socket_domain = cp->socket_domain;
			node->conn->sock = -1;
            node->conn->validate_flag = false;
			*err_no = G_COMMON_CONNECTION_CALLBACKS[conn->comm_type].
                make_connection(node->conn, service_name,
                        cp->connect_timeout_ms, NULL, true);
            if (*err_no == 0 && cp->connect_done_callback.func != NULL)
            {
                *err_no = cp->connect_done_callback.func(node->conn,
                        cp->connect_done_callback.args);
            }
			if (*err_no != 0)
			{
                G_COMMON_CONNECTION_CALLBACKS[conn->comm_type].
                    close_connection(node->conn);
                pthread_mutex_lock(&cm->lock);
                cm->total_count--;  //rollback
                fast_mblock_free_object(&cp->node_allocator, node);
                pthread_mutex_unlock(&cm->lock);

				return NULL;
			}

			logDebug("file: "__FILE__", line: %d, " \
				"server %s:%u, new connection: %d, " \
				"total_count: %d, free_count: %d",   \
				__LINE__, conn->ip_addr, conn->port, \
				node->conn->sock, cm->total_count, \
				cm->free_count);
			return node->conn;
		}
		else
		{
            bool invalid;

			node = cm->head;
			ci = node->conn;
			cm->head = node->next;
			cm->free_count--;

			if (current_time - node->atime > cp->max_idle_time)
            {
                if (cp->validate_callback.func != NULL)
                {
                    ci->validate_flag = true;
                }
                invalid = true;
            }
            else
            {
                invalid = false;
            }

            if (ci->validate_flag)
            {
                ci->validate_flag = false;
                if (cp->validate_callback.func != NULL)
                {
                    invalid = cp->validate_callback.func(ci,
                            cp->validate_callback.args) != 0;
                }
                else
                {
                    invalid = false;
                }
            }

            if (invalid)
            {
				cm->total_count--;

				logDebug("file: "__FILE__", line: %d, "
					"server %s:%u, connection: %d idle "
					"time: %d exceeds max idle time: %d, "
					"total_count: %d, free_count: %d", __LINE__,
                    conn->ip_addr, conn->port, ci->sock, (int)
                    (current_time - node->atime), cp->max_idle_time,
                    cm->total_count, cm->free_count);

                G_COMMON_CONNECTION_CALLBACKS[ci->comm_type].
                    close_connection(ci);
                fast_mblock_free_object(&cp->node_allocator, node);
				continue;
			}

			pthread_mutex_unlock(&cm->lock);
			logDebug("file: "__FILE__", line: %d, " \
				"server %s:%u, reuse connection: %d, " \
				"total_count: %d, free_count: %d", 
				__LINE__, conn->ip_addr, conn->port, 
				ci->sock, cm->total_count, cm->free_count);
            *err_no = 0;
			return ci;
		}
	}
}

int conn_pool_close_connection_ex(ConnectionPool *cp, ConnectionInfo *conn, 
	const bool bForce)
{
	char key[INET6_ADDRSTRLEN + 8];
	int key_len;
	ConnectionManager *cm;
	ConnectionNode *node;

	conn_pool_get_key(conn, key, &key_len);

	pthread_mutex_lock(&cp->lock);
	cm = (ConnectionManager *)fc_hash_find(&cp->hash_array, key, key_len);
	pthread_mutex_unlock(&cp->lock);
	if (cm == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"hash entry of server %s:%u not exist", __LINE__, \
			conn->ip_addr, conn->port);
		return ENOENT;
	}

	node = (ConnectionNode *)(((char *)conn) - sizeof(ConnectionNode));
	if (node->manager != cm)
	{
		logError("file: "__FILE__", line: %d, " \
			"manager of server entry %s:%u is invalid!", \
			__LINE__, conn->ip_addr, conn->port);
		return EINVAL;
	}

	pthread_mutex_lock(&cm->lock);
	if (bForce)
    {
        cm->total_count--;

        logDebug("file: "__FILE__", line: %d, "
                "server %s:%u, release connection: %d, "
                "total_count: %d, free_count: %d",
                __LINE__, conn->ip_addr, conn->port,
                conn->sock, cm->total_count, cm->free_count);

        G_COMMON_CONNECTION_CALLBACKS[conn->comm_type].
            close_connection(conn);
        fast_mblock_free_object(&cp->node_allocator, node);

        node = cm->head;
        while (node != NULL)
        {
            node->conn->validate_flag = true;
            node = node->next;
        }
    }
	else
	{
		node->atime = get_current_time();
		node->next = cm->head;
		cm->head = node;
		cm->free_count++;

		logDebug("file: "__FILE__", line: %d, " \
			"server %s:%u, free connection: %d, " \
			"total_count: %d, free_count: %d", 
			__LINE__, conn->ip_addr, conn->port, 
			conn->sock, cm->total_count, cm->free_count);
	}
	pthread_mutex_unlock(&cm->lock);

	return 0;
}

static int _conn_count_walk(const int index, const HashData *data, void *args)
{
	int *count;
	ConnectionManager *cm;
	ConnectionNode *node;

	count = (int *)args;
	cm = (ConnectionManager *)data->value;
	node = cm->head;
	while (node != NULL)
	{
		(*count)++;
		node = node->next;
	}

	return 0;
}

int conn_pool_get_connection_count(ConnectionPool *cp)
{
	int count;
	count = 0;
	fc_hash_walk(&cp->hash_array, _conn_count_walk, &count);
	return count;
}

int conn_pool_parse_server_info(const char *pServerStr,
        ConnectionInfo *pServerInfo, const int default_port)
{
    char *parts[2];
    char server_info[256];
    int len;
    int count;

    len = strlen(pServerStr);
    if (len == 0) {
        logError("file: "__FILE__", line: %d, "
            "host \"%s\" is empty!",
            __LINE__, pServerStr);
        return EINVAL;
    }
    if (len >= sizeof(server_info)) {
        logError("file: "__FILE__", line: %d, "
            "host \"%s\" is too long!",
            __LINE__, pServerStr);
        return ENAMETOOLONG;
    }

    memcpy(server_info, pServerStr, len);
    *(server_info + len) = '\0';

    count = splitEx(server_info, ':', parts, 2);
    if (count == 1) {
        pServerInfo->port = default_port;
    }
    else {
        char *endptr = NULL;
        pServerInfo->port = (int)strtol(parts[1], &endptr, 10);
        if ((endptr != NULL && *endptr != '\0') || pServerInfo->port <= 0) {
            logError("file: "__FILE__", line: %d, "
                "host: %s, invalid port: %s!",
                __LINE__, pServerStr, parts[1]);
            return EINVAL;
        }
    }

    if (getIpaddrByName(parts[0], pServerInfo->ip_addr,
        sizeof(pServerInfo->ip_addr)) == INADDR_NONE)
    {
        logError("file: "__FILE__", line: %d, "
            "host: %s, invalid hostname: %s!",
            __LINE__, pServerStr, parts[0]);
        return EINVAL;
    }

    pServerInfo->socket_domain = AF_INET;
    pServerInfo->sock = -1;
    pServerInfo->comm_type = fc_comm_type_sock;
    return 0;
}

int conn_pool_load_server_info(IniContext *pIniContext, const char *filename,
        const char *item_name, ConnectionInfo *pServerInfo,
        const int default_port)
{
    char *pServerStr;

	pServerStr = iniGetStrValue(NULL, item_name, pIniContext);
    if (pServerStr == NULL) {
        logError("file: "__FILE__", line: %d, "
                "config file: %s, item \"%s\" not exist!",
                __LINE__, filename, item_name);
        return ENOENT;
    }

    return conn_pool_parse_server_info(pServerStr, pServerInfo, default_port);
}

#define API_PREFIX_NAME  "fast_rdma_client_"

#define LOAD_API(callbacks, fname) \
    do { \
        callbacks.fname = dlsym(dlhandle, API_PREFIX_NAME#fname); \
        if (callbacks.fname == NULL) {  \
            logError("file: "__FILE__", line: %d, "  \
                    "dlsym api %s fail, error info: %s", \
                    __LINE__, API_PREFIX_NAME#fname, dlerror()); \
            return ENOENT; \
        } \
    } while (0)

int conn_pool_global_init_for_rdma()
{
    const char *library = "libfastrdma.so";
    void *dlhandle;

    if (g_connection_callbacks.inited) {
        return 0;
    }

    dlhandle = dlopen(library, RTLD_LAZY);
    if (dlhandle == NULL) {
        logError("file: "__FILE__", line: %d, "
                "dlopen %s fail, error info: %s",
                __LINE__, library, dlerror());
        return EFAULT;
    }

    LOAD_API(G_COMMON_CONNECTION_CALLBACKS[fc_comm_type_rdma],
            make_connection);
    LOAD_API(G_COMMON_CONNECTION_CALLBACKS[fc_comm_type_rdma],
            close_connection);
    LOAD_API(G_COMMON_CONNECTION_CALLBACKS[fc_comm_type_rdma],
            is_connected);

    LOAD_API(G_RDMA_CONNECTION_CALLBACKS, alloc_pd);
    LOAD_API(G_RDMA_CONNECTION_CALLBACKS, get_connection_size);
    LOAD_API(G_RDMA_CONNECTION_CALLBACKS, init_connection);
    LOAD_API(G_RDMA_CONNECTION_CALLBACKS, make_connection);
    LOAD_API(G_RDMA_CONNECTION_CALLBACKS, close_connection);
    LOAD_API(G_RDMA_CONNECTION_CALLBACKS, destroy_connection);
    LOAD_API(G_RDMA_CONNECTION_CALLBACKS, is_connected);
    LOAD_API(G_RDMA_CONNECTION_CALLBACKS, get_buffer);
    LOAD_API(G_RDMA_CONNECTION_CALLBACKS, request_by_buf1);
    LOAD_API(G_RDMA_CONNECTION_CALLBACKS, request_by_buf2);
    LOAD_API(G_RDMA_CONNECTION_CALLBACKS, request_by_iov);
    LOAD_API(G_RDMA_CONNECTION_CALLBACKS, request_by_mix);

    g_connection_callbacks.inited = true;
    return 0;
}

ConnectionInfo *conn_pool_alloc_connection_ex(
        const FCCommunicationType comm_type,
        const int extra_data_size,
        const ConnectionExtraParams *extra_params,
        int *err_no)
{
    ConnectionInfo *conn;
    int bytes;

    if (comm_type == fc_comm_type_rdma) {
        bytes = sizeof(ConnectionInfo) + extra_data_size +
            G_RDMA_CONNECTION_CALLBACKS.get_connection_size();
    } else {
        bytes = sizeof(ConnectionInfo) + extra_data_size;
    }
    if ((conn=fc_malloc(bytes)) == NULL) {
        *err_no = ENOMEM;
        return NULL;
    }
    memset(conn, 0, bytes);

    if (comm_type == fc_comm_type_rdma) {
        conn->arg1 = conn->args + extra_data_size;
        if ((*err_no=G_RDMA_CONNECTION_CALLBACKS.init_connection(
                        conn, extra_params->buffer_size,
                        extra_params->pd)) != 0)
        {
            free(conn);
            return NULL;
        }
    } else {
        *err_no = 0;
    }

    conn->comm_type = comm_type;
    return conn;
}

int conn_pool_set_rdma_extra_params(ConnectionExtraParams *extra_params,
        struct fc_server_config *server_cfg, const int server_group_index)
{
    const int padding_size = 1024;
    FCServerGroupInfo *server_group;
    FCServerInfo *first_server;
    int result;

    if ((server_group=fc_server_get_group_by_index(server_cfg,
                    server_group_index)) == NULL)
    {
        return ENOENT;
    }

    if (server_group->comm_type == fc_comm_type_sock) {
        extra_params->buffer_size = 0;
        extra_params->pd = NULL;
        return 0;
    } else {
        first_server = FC_SID_SERVERS(*server_cfg);
        extra_params->buffer_size = server_cfg->buffer_size + padding_size;
        extra_params->pd = fc_alloc_rdma_pd(G_RDMA_CONNECTION_CALLBACKS.
                alloc_pd, &first_server->group_addrs[server_group_index].
                address_array, &result);
        return result;
    }
}
