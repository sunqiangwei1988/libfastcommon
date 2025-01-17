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
#include "logger.h"
#include "sockopt.h"
#include "shared_func.h"
#include "local_ip_func.h"

int g_local_host_ip_count = 0;
char g_local_host_ip_addrs[FAST_MAX_LOCAL_IP_ADDRS * \
				IP_ADDRESS_SIZE];
char g_if_alias_prefix[FAST_IF_ALIAS_PREFIX_MAX_SIZE] = {0};

bool is_local_host_ip(const char *client_ip)
{
	char *p;
	char *pEnd;

	pEnd = g_local_host_ip_addrs + \
		IP_ADDRESS_SIZE * g_local_host_ip_count;
	for (p=g_local_host_ip_addrs; p<pEnd; p+=IP_ADDRESS_SIZE)
	{
		if (strcmp(client_ip, p) == 0)
		{
			return true;
		}
	}

	return false;
}

int insert_into_local_host_ip(const char *client_ip)
{
	if (is_local_host_ip(client_ip))
	{
		return 0;
	}

	if (g_local_host_ip_count >= FAST_MAX_LOCAL_IP_ADDRS)
	{
		return -1;
	}

	strcpy(g_local_host_ip_addrs + \
		IP_ADDRESS_SIZE * g_local_host_ip_count, \
		client_ip);
	g_local_host_ip_count++;
	return 1;
}

const char *local_host_ip_addrs_to_string(char *buff, const int size)
{
	char *p;
	char *pEnd;
	int len;

	len = snprintf(buff, size, "local_host_ip_count: %d,",
            g_local_host_ip_count);
	pEnd = g_local_host_ip_addrs +
		IP_ADDRESS_SIZE * g_local_host_ip_count;
	for (p=g_local_host_ip_addrs; p<pEnd; p+=IP_ADDRESS_SIZE)
	{
		len += snprintf(buff + len, size - len, "  %s", p);
	}

    return buff;
}

void log_local_host_ip_addrs()
{
	char buff[512];
	logInfo("%s", local_host_ip_addrs_to_string(buff, sizeof(buff)));
}

void load_local_host_ip_addrs()
{
#define STORAGE_MAX_ALIAS_PREFIX_COUNT   4
	char ip_addresses[FAST_MAX_LOCAL_IP_ADDRS][IP_ADDRESS_SIZE];
	int count;
	int k;
	char *if_alias_prefixes[STORAGE_MAX_ALIAS_PREFIX_COUNT];
	int alias_count;

	insert_into_local_host_ip(LOCAL_LOOPBACK_IPv4);

	memset(if_alias_prefixes, 0, sizeof(if_alias_prefixes));
	if (*g_if_alias_prefix == '\0')
	{
		alias_count = 0;
	}
	else
	{
		alias_count = splitEx(g_if_alias_prefix, ',', \
			if_alias_prefixes, STORAGE_MAX_ALIAS_PREFIX_COUNT);
		for (k=0; k<alias_count; k++)
		{
			fc_trim(if_alias_prefixes[k]);
		}
	}

	if (gethostaddrs(if_alias_prefixes, alias_count, ip_addresses, \
			FAST_MAX_LOCAL_IP_ADDRS, &count) != 0)
	{
		return;
	}

	for (k=0; k<count; k++)
	{
		insert_into_local_host_ip(ip_addresses[k]);
	}

	// log_local_host_ip_addrs();
	// print_local_host_ip_addrs();
}

void print_local_host_ip_addrs()
{
	char *p;
	char *pEnd;

	printf("local_host_ip_count=%d\n", g_local_host_ip_count);
	pEnd = g_local_host_ip_addrs + \
		IP_ADDRESS_SIZE * g_local_host_ip_count;
	for (p=g_local_host_ip_addrs; p<pEnd; p+=IP_ADDRESS_SIZE)
	{
		printf("%d. %s\n", (int)((p-g_local_host_ip_addrs)/ \
				IP_ADDRESS_SIZE)+1, p);
	}

	printf("\n");
}

const char *get_next_local_ip(const char *previous_ip)
{
    char *p;
	char *pEnd;
    bool found;

    if (g_local_host_ip_count == 0)
    {
        load_local_host_ip_addrs();
    }

    found = (previous_ip == NULL);
	pEnd = g_local_host_ip_addrs + \
		IP_ADDRESS_SIZE * g_local_host_ip_count;
	for (p=g_local_host_ip_addrs; p<pEnd; p+=IP_ADDRESS_SIZE)
	{
	    if (strcmp(p, LOCAL_LOOPBACK_IPv4) != 0 &&
		     strcmp(p, LOCAL_LOOPBACK_IPv6) !=0 )
        {
            if (found)
            {
                return p;
            }
            else if (strcmp(p, previous_ip) == 0)
            {
                found = true;
            }
        }
	}

    return NULL;
}

const char *get_first_local_ip()
{
    const char *first_ip;
    first_ip = get_next_local_ip(NULL);
    if (first_ip != NULL)
    {
        return first_ip;
    }
    else
    {
		// 注意，当系统存在IPv6回环地址时，为了简化系统的改动，会将IPv6回环地址修改成IPv4回环地址返回
        return LOCAL_LOOPBACK_IPv4;
    }
}

const char *get_first_local_private_ip()
{
    const char *ip;

    ip = NULL;
    do
    {
        ip = get_next_local_ip(ip);
        if (ip == NULL)
        {
            return NULL;
        }
        if (is_private_ip(ip))
        {
            return ip;
        }
    } while (1);

    return NULL;
}

