/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        General program utils.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

/* System includes */
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#if defined _WITH_LVS_ || defined _HAVE_LIBIPSET_
#include <sys/wait.h>
#endif
#ifdef _WITH_PERF_
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#endif
#if defined USE_CLOSE_RANGE_SYSCALL
#include <sys/syscall.h>
#endif
#if !defined HAVE_CLOSE_RANGE || !HAVE_DECL_CLOSE_RANGE_CLOEXEC
#include <dirent.h>
#include <stdlib.h>
#include <ctype.h>
#endif

#ifdef _WITH_STACKTRACE_
#include <sys/stat.h>
#include <execinfo.h>
#include <memory.h>
#endif
#ifdef DO_STACKTRACE
#include <inttypes.h>
#include <unistd.h>
#endif
#ifdef _HAVE_LIBKMOD_
#include <libkmod.h>
#endif

/* Local includes */
#include "utils.h"
#include "memory.h"
#include "utils.h"
#include "signals.h"
#include "bitops.h"
#include "parser.h"
#include "logger.h"
#include "process.h"
#include "timer.h"

#if defined USE_CLOSE_RANGE_SYSCALL && !defined SYS_close_range
#define SYS_close_range __NR_close_range
#endif

/* global vars */
unsigned long debug = 0;
mode_t umask_val = S_IXUSR | S_IRWXG | S_IRWXO;
const char *tmp_dir;

#ifdef _EINTR_DEBUG_
bool do_eintr_debug;
#endif

#ifdef DO_STACKSIZE
#define STACK_UNUSED 0xdeadbeeffeedcafe
#define STACKSIZE_DEBUG 0

static void *orig_stack_base;
#endif

/* Display a buffer into a HEXA formated output */
void
dump_buffer(const char *buff, size_t count, FILE* fp, int indent)
{
	size_t i, j, c;
	bool printnext = true;

	if (count % 16)
		c = count + (16 - count % 16);
	else
		c = count;

	for (i = 0; i < c; i++) {
		if (printnext) {
			printnext = false;
			fprintf(fp, "%*s%.4zu ", indent, "", i & 0xffff);
		}
		if (i < count)
			fprintf(fp, "%3.2x", (unsigned char)buff[i] & 0xff);
		else
			fprintf(fp, "   ");
		if (!((i + 1) % 8)) {
			if ((i + 1) % 16)
				fprintf(fp, " -");
			else {
				fprintf(fp, "   ");
				for (j = i - 15; j <= i; j++)
					if (j < count) {
						if ((buff[j] & 0xff) >= 0x20
						    && (buff[j] & 0xff) <= 0x7e)
							fprintf(fp, "%c",
							       buff[j] & 0xff);
						else
							fprintf(fp, ".");
					} else
						fprintf(fp, " ");
				fprintf(fp, "\n");
				printnext = true;
			}
		}
	}
}

#if defined _CHECKSUM_DEBUG_ || defined _RECVMSG_DEBUG_
void
log_buffer(const char *msg, const void *buff, size_t count)
{
	char op_buf[60];	// Probably 56 really
	const unsigned char *bufp = buff;
	char *ptr;
	size_t offs = 0;
	unsigned i;

	log_message(LOG_INFO, "%s - len %zu", msg, count);

	while (offs < count) {
		ptr = op_buf;
		ptr += snprintf(ptr, op_buf + sizeof(op_buf) - ptr, "%4.4zx ", offs);

		for (i = 0; i < 16 && offs < count; i++) {
			if (i == 8)
				*ptr++ = ' ';
			ptr += snprintf(ptr, op_buf + sizeof(op_buf) - ptr, " %2.2x", bufp[offs++]);
		}

		log_message(LOG_INFO, "%s", op_buf);
	}
}
#endif

#ifdef _WITH_STACKTRACE_
void
write_stacktrace(const char *file_name, const char *str)
{
	int fd;
	void *buffer[100];
	unsigned int nptrs;
	unsigned int i;
	char **strs;
	char *cmd;
	const char *tmp_filename = NULL;

	nptrs = backtrace(buffer, 100);
	if (file_name) {
		fd = open(file_name, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
		if (str)
			dprintf(fd, "%s\n", str);
		backtrace_symbols_fd(buffer, nptrs, fd);
		if (write(fd, "\n", 1) != 1) {
			/* We don't care, but this stops a warning on Ubuntu */
		}
		close(fd);
	} else {
		if (str)
			log_message(LOG_INFO, "%s", str);
		strs = backtrace_symbols(buffer, nptrs);
		if (strs == NULL) {
			log_message(LOG_INFO, "Unable to get stack backtrace");
			return;
		}

		/* We don't need the call to this function, or the first two entries on the stack */
		nptrs -= 2;
		for (i = 1; i < nptrs; i++)
			log_message(LOG_INFO, "  %s", strs[i]);
		free(strs);	/* malloc'd by backtrace_symbols */
	}

	/* gstack() gives a more detailed stacktrace, using gdb and the bt command */
	if (!file_name)
		tmp_filename = make_tmp_filename("keepalived.stack");
	else if (file_name[0] != '/')
		tmp_filename = make_tmp_filename(file_name);
	cmd = MALLOC(6 + 1 + PID_MAX_DIGITS + 1 + 2 + ( tmp_filename ? strlen(tmp_filename) : strlen(file_name)) + 1);
	sprintf(cmd, "gstack %d >>%s", our_pid, tmp_filename ? tmp_filename : file_name);

	i = system(cmd);	/* We don't care about return value but gcc thinks we should */

	FREE(cmd);
	FREE_CONST_PTR(tmp_filename);
}
#endif

#ifdef DO_STACKSIZE
RELAX_STACK_PROTECTOR_START
int
get_stacksize(bool end)
{
	/* We use a struct for all local variables so that we
	 * know the address of the lowest variable on the stack */
	struct {
		void *stack_base, *stack_top;
		uintptr_t write_end;
		uint64_t *p;
		FILE *fp;
		uintptr_t aligned_base;
		unsigned num_ent;
		unsigned i;
		uint64_t *base;
		int page_size;
		char buf[257];
	} s;

	if (!(s.fp = fopen("/proc/self/maps", "re")))
		return -1;

	while (fgets(s.buf, sizeof(s.buf), s.fp)) {
		if (!strstr(s.buf, "[stack]\n"))
			continue;

		sscanf(s.buf, "%p-%p ", &s.stack_base, &s.stack_top);
		break;
	}
	fclose(s.fp);

#if STACKSIZE_DEBUG
	log_message(LOG_INFO, "stack from %p to %p, stack now ~= %p", s.stack_base, s.stack_top, &s);
#endif

	s.write_end = (uintptr_t)&s & ~(sizeof(uint64_t) - 1);
	s.page_size = sysconf(_SC_PAGESIZE);

	if (!end) {
		s.aligned_base = (uintptr_t)s.stack_base;
		s.aligned_base &= ~(s.page_size - 1);
		s.num_ent = ((char *)s.write_end - (char *)s.aligned_base) / sizeof(uint64_t) - 1;
		s.num_ent -= s.page_size / sizeof(uint64_t);
RELAX_ALLOCA_START
		s.base = alloca(s.num_ent * sizeof(uint64_t));
RELAX_ALLOCA_END
#if STACKSIZE_DEBUG
		log_message(LOG_INFO, "alloca() gave us %p, &s = %p", s.base, &s);
#endif
		for (s.p = s.base, s.i = 0; s.i < s.num_ent; s.i++, s.p++)
			*s.p = STACK_UNUSED;

		orig_stack_base = s.stack_base;
	} else if (s.stack_base != orig_stack_base)
#if STACKSIZE_DEBUG
		log_message(LOG_INFO, "Stack base changed from %1$p to %2$p, used > 0x%3$lx (%3$lu) bytes", orig_stack_base, s.stack_base,
			    (unsigned long)((char *)s.stack_top - (char *)orig_stack_base));
#else
		log_message(LOG_INFO, "Stack used > 0x%1$lx (%1$lu) bytes",
			    (unsigned long)((char *)s.stack_top - (char *)orig_stack_base));
#endif
	else {
		for (s.p = (uint64_t *)((char *)s.stack_base + s.page_size); s.p != s.stack_top; s.p++) {
			if (*s.p != STACK_UNUSED)
				break;
		}

#if STACKSIZE_DEBUG
		log_message(LOG_INFO, "Lowest stack use at %1$p, value %2$" PRIx64 ", used 0x%3$lx (%3$lu) bytes", s.p, *s.p,
			    (unsigned long)((char *)s.stack_top - (char *)s.p));
#else
		log_message(LOG_INFO, "Stack used 0x%1$lx (%1$lu) bytes",
			    (unsigned long)((char *)s.stack_top - (char *)s.p));
#endif
	}

	return 0;
}
RELAX_STACK_PROTECTOR_END
#endif

const char *
make_file_name(const char *name, const char *prog, const char *namespace, const char *instance)
{
	const char *extn_start;
	const char *dir_end;
	size_t len;
	char *file_name;

	if (!name)
		return NULL;

	if (name[0] != '/')
		len = strlen(tmp_dir) + 1;
	else
		len = 0;
	len += strlen(name);
	if (prog)
		len += strlen(prog) + 1;
	if (namespace)
		len += strlen(namespace) + 1;
	if (instance)
		len += strlen(instance) + 1;

	file_name = MALLOC(len + 1);
	dir_end = strrchr(name, '/');
	extn_start = strrchr(dir_end ? dir_end : name, '.');

	if (name[0] != '/') {
		strcpy(file_name, tmp_dir);
		strcat(file_name, "/");
	} else
		file_name[0] = '\0';

	strncat(file_name, name, extn_start ? (size_t)(extn_start - name) : len);

	if (prog) {
		strcat(file_name, "_");
		strcat(file_name, prog);
	}
	if (namespace) {
		strcat(file_name, "_");
		strcat(file_name, namespace);
	}
	if (instance) {
		strcat(file_name, "_");
		strcat(file_name, instance);
	}
	if (extn_start)
		strcat(file_name, extn_start);

	return file_name;
}

void
set_process_name(const char *name)
{
	if (!name)
		name = "keepalived";

	if (prctl(PR_SET_NAME, name))
		log_message(LOG_INFO, "Failed to set process name '%s'", name);
}

#ifdef _WITH_PERF_
void
run_perf(const char *process, const char *network_namespace, const char *instance_name)
{
	int ret;
	pid_t pid;
	char *orig_name = NULL;
	const char *new_name;
	const char *perf_name = "perf.data";
	int in = -1;
	int ep = -1;

	do {
		orig_name = MALLOC(PATH_MAX);
		if (!getcwd(orig_name, PATH_MAX)) {
			log_message(LOG_INFO, "Unable to get cwd");
			break;
		}

		in = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
		if (in == -1) {
			log_message(LOG_INFO, "inotify_init failed %d - %m", errno);
			break;
		}

		if (inotify_add_watch(in, orig_name, IN_CREATE) == -1) {
			log_message(LOG_INFO, "inotify_add_watch of %s failed %d - %m", orig_name, errno);
			break;
		}

		pid = fork();

		if (pid == -1) {
			log_message(LOG_INFO, "fork() for perf failed");
			break;
		}

		/* Child */
		if (!pid) {
			char buf[PID_MAX_DIGITS + 1];

			snprintf(buf, sizeof buf, "%d", getppid());
			execlp("perf", "perf", "record", "-p", buf, "-q", "-g", "--call-graph", "fp", NULL);
			exit(0);
		}

		/* Parent */
		char buf[sizeof(struct inotify_event) + NAME_MAX + 1] __attribute__((aligned(__alignof__(struct inotify_event))));
		struct inotify_event *ie = PTR_CAST(struct inotify_event, buf);
		struct epoll_event ee = { .events = EPOLLIN, .data.fd = in };

		if ((ep = epoll_create1(EPOLL_CLOEXEC)) == -1) {
			log_message(LOG_INFO, "perf epoll_create1 failed errno %d - %m", errno);
			break;
		}

		if (epoll_ctl(ep, EPOLL_CTL_ADD, in, &ee) == -1) {
			log_message(LOG_INFO, "perf epoll_ctl failed errno %d - %m", errno);
			break;
		}

		do {
			ret = epoll_wait(ep, &ee, 1, 1000);
			if (ret == 0) {
				log_message(LOG_INFO, "Timed out waiting for creation of %s", perf_name);
				break;
			}
			else if (ret == -1) {
				if (check_EINTR(errno))
					continue;

				log_message(LOG_INFO, "perf epoll returned errno %d - %m", errno);
				break;
			}

			ret = read(in, buf, sizeof(buf));
			if (ret == -1) {
				if (check_EINTR(errno))
					continue;

				log_message(LOG_INFO, "perf inotify read returned errno %d %m", errno);
				break;
			}
			if (ret < (int)sizeof(*ie)) {
				log_message(LOG_INFO, "read returned %d", ret);
				break;
			}
			if (!(ie->mask & IN_CREATE)) {
				log_message(LOG_INFO, "mask is 0x%x", ie->mask);
				continue;
			}
			if (!ie->len) {
				log_message(LOG_INFO, "perf inotify read returned no len");
				continue;
			}

			if (strcmp(ie->name, perf_name))
				continue;

			/* Rename the /perf.data file */
			strcat(orig_name, perf_name);
			new_name = make_file_name(orig_name, process,
							network_namespace,
							instance_name);

			if (rename(orig_name, new_name))
				log_message(LOG_INFO, "Rename %s to %s failed - %m (%d)", orig_name, new_name, errno);

			FREE_CONST(new_name);
		} while (false);
	} while (false);

	if (ep != -1)
		close(ep);
	if (in != -1)
		close(in);
	if (orig_name)
		FREE(orig_name);
}
#endif

/* Compute a checksum */
#ifdef USE_MEMCPY_FOR_ALIASING
uint16_t
in_csum(const void *addr, size_t len, uint32_t csum, uint32_t *acc)
{
	size_t nleft = len;
	uint16_t w16;
	const unsigned char *b_addr = addr;

	/*
	 *  Our algorithm is simple, using a 32 bit accumulator (sum),
	 *  we add sequential 16 bit words to it, and at the end, fold
	 *  back all the carry bits from the top 16 bits into the lower
	 *  16 bits.
	 */

	/* What is not so simple is dealing with strict aliasing. We can only
	 * access the data via a char/unsigned char pointer, since that is the
	 * only type of pointer that can be used for aliasing.
	 * The trick here is we use memcpy to copy the uint16_t via an unsigned
	 * char *. The memcpy doesn't actually happen since the compiler sees
	 * what is happening, optimizes it out and accesses the original memory,
	 * but since the code is written to only access the original memory via
	 * the unsigned char *, the compiler knows this might be aliasing.
	 */
	while (nleft > 1) {
		memcpy(&w16, b_addr, sizeof(w16));
		csum += w16;
		b_addr += sizeof(w16);
		nleft -= sizeof(w16);
	}

	/* mop up an odd byte, if necessary */
	if (nleft == 1)
		csum += htons(*b_addr << 8);

	if (acc)
		*acc = csum;

	/*
	 * add back carry outs from top 16 bits to low 16 bits
	 */
	csum = (csum >> 16) + (csum & 0xffff);	/* add hi 16 to low 16 */
	csum += (csum >> 16);			/* add carry */
	return ~csum & 0xffff;			/* truncate to 16 bits */
}
#else
typedef uint16_t __attribute__((may_alias)) uint16_t_a;
uint16_t
in_csum(const void *addr, size_t len, uint32_t csum, uint32_t *acc)
{
	size_t nleft = len;
	const uint16_t_a *w = addr;
	uint16_t answer;
	uint32_t sum = csum;

	/*
	 *  Our algorithm is simple, using a 32 bit accumulator (sum),
	 *  we add sequential 16 bit words to it, and at the end, fold
	 *  back all the carry bits from the top 16 bits into the lower
	 *  16 bits.
	 */
	while (nleft > 1) {
		sum += *w++;
		nleft -= 2;
	}

	/* mop up an odd byte, if necessary */
	if (nleft == 1)
		sum += htons(*PTR_CAST_CONST(u_char, w) << 8);

	if (acc)
		*acc = sum;

	/*
	 * add back carry outs from top 16 bits to low 16 bits
	 */
	sum = (sum >> 16) + (sum & 0xffff);	/* add hi 16 to low 16 */
	sum += (sum >> 16);			/* add carry */
	answer = (~sum & 0xffff);		/* truncate to 16 bits */
	return (answer);
}
#endif

/* IP network to ascii representation - address is in network byte order */
const char *
inet_ntop2(uint32_t ip)
{
	static char buf[16];
	const unsigned char (*bytep)[4] = (const unsigned char (*)[4])&ip;

	sprintf(buf, "%d.%d.%d.%d", (*bytep)[0], (*bytep)[1], (*bytep)[2], (*bytep)[3]);
	return buf;
}

#ifdef _INCLUDE_UNUSED_CODE_
/*
 * IP network to ascii representation. To use
 * for multiple IP address convertion into the same call.
 */
char *
inet_ntoa2(uint32_t ip, char *buf)
{
	const unsigned char *bytep;

	bytep = PTR_CAST_CONST(unsigned char, &ip);
	sprintf(buf, "%d.%d.%d.%d", bytep[0], bytep[1], bytep[2], bytep[3]);
	return buf;
}
#endif

/* IP string to network range representation. */
bool
inet_stor(const char *addr, uint32_t *range_end)
{
	const char *cp;
	char *endptr;
	unsigned long range;
	int family = strchr(addr, ':') ? AF_INET6 : AF_INET;
	const char *warn = "";

#ifndef _STRICT_CONFIG_
	if (!__test_bit(CONFIG_TEST_BIT, &debug))
		warn = "WARNING - ";
#endif

	/* Return UINT32_MAX to indicate no range */
	if (!(cp = strchr(addr, '-'))) {
		*range_end = UINT32_MAX;
		return true;
	}

	errno = 0;
	range = strtoul(cp + 1, &endptr, family == AF_INET6 ? 16 : 10);
	*range_end = range;

	if (*endptr)
		report_config_error(CONFIG_INVALID_NUMBER, "%sVirtual server group range '%s' has extra characters at end '%s'", warn, addr, endptr);
	else if (errno == ERANGE ||
		 (family == AF_INET6 && range > 0xffff) ||
		 (family == AF_INET && range > 255)) {
		report_config_error(CONFIG_INVALID_NUMBER, "Virtual server group range '%s' end '%s' too large", addr, cp + 1);

		/* Indicate error */
		return false;
	}
	else
		return true;

#ifdef _STRICT_CONFIG_
	return false;
#else
	return !__test_bit(CONFIG_TEST_BIT, &debug);
#endif
}

/* Domain to sockaddr_ka */
int
domain_stosockaddr(const char *domain, const char *port, sockaddr_t *addr)
{
	struct addrinfo *res = NULL;
	unsigned port_num;

	if (port) {
		if (!read_unsigned(port, &port_num, 1, 65535, true)) {
			addr->ss_family = AF_UNSPEC;
			return -1;
		}
	}

	if (getaddrinfo(domain, NULL, NULL, &res) != 0 || !res) {
		addr->ss_family = AF_UNSPEC;
		return -1;
	}

	addr->ss_family = (sa_family_t)res->ai_family;

	/* Tempting as it is to do something like:
	 *	*(struct sockaddr_in6 *)addr = *(struct sockaddr_in6 *)res->ai_addr;
	 *  the alignment of struct sockaddr (short int) is less than the alignment of
	 *  sockaddr_t (long).
	 */
	memcpy(addr, res->ai_addr, res->ai_addrlen);

	if (port) {
		if (addr->ss_family == AF_INET6)
			PTR_CAST(struct sockaddr_in6, addr)->sin6_port = htons(port_num);
		else
			PTR_CAST(struct sockaddr_in, addr)->sin_port = htons(port_num);
	}

	freeaddrinfo(res);

	return 0;
}

/* IP string to sockaddr_ka
 *   return value is "error". */
bool
inet_stosockaddr(const char *ip, const char *port, sockaddr_t *addr)
{
	void *addr_ip;
	const char *cp;
	char *ip_str = NULL;
	unsigned port_num;
	int res;

	addr->ss_family = (strchr(ip, ':')) ? AF_INET6 : AF_INET;

	if (port) {
		if (!read_unsigned(port, &port_num, 1, 65535, true)) {
			addr->ss_family = AF_UNSPEC;
			return true;
		}
	}

	if (addr->ss_family == AF_INET6) {
		struct sockaddr_in6 *addr6 = PTR_CAST(struct sockaddr_in6, addr);
		if (port)
			addr6->sin6_port = htons(port_num);
		addr_ip = &addr6->sin6_addr;
	} else {
		struct sockaddr_in *addr4 = PTR_CAST(struct sockaddr_in, addr);
		if (port)
			addr4->sin_port = htons(port_num);
		addr_ip = &addr4->sin_addr;
	}

	/* remove range and mask stuff */
	if ((cp = strchr(ip, '-')) ||
	    (cp = strchr(ip, '/')))
		ip_str = STRNDUP(ip, cp - ip);

	res = inet_pton(addr->ss_family, ip_str ? ip_str : ip, addr_ip);

	if (ip_str)
		FREE(ip_str);

	if (!res) {
		addr->ss_family = AF_UNSPEC;
		return true;
	}

	return false;
}

/* IPv4 to sockaddr_ka */
void
inet_ip4tosockaddr(const struct in_addr *sin_addr, sockaddr_t *addr)
{
	struct sockaddr_in *addr4 = PTR_CAST(struct sockaddr_in, addr);
	addr4->sin_family = AF_INET;
	addr4->sin_addr = *sin_addr;
}

/* IPv6 to sockaddr_ka */
void
inet_ip6tosockaddr(const struct in6_addr *sin_addr, sockaddr_t *addr)
{
	struct sockaddr_in6 *addr6 = PTR_CAST(struct sockaddr_in6, addr);
	addr6->sin6_family = AF_INET6;
	addr6->sin6_addr = *sin_addr;
}

/* Check address, possibly with mask, is valid */
bool
check_valid_ipaddress(const char *str, bool allow_subnet_mask)
{
	int family;
	unsigned long prefixlen;
	const char *p;
	char *endptr;
	union {
		struct in_addr in;
		struct in6_addr in6;
	} addr;
	int res;
	const char *str_dup = NULL;

	if (!strchr(str, ':') && !strchr(str, '.'))
		return false;

	family = (strchr(str, ':')) ? AF_INET6 : AF_INET;

	if (allow_subnet_mask)
		p = strchr(str, '/');
	else
		p = NULL;

	if (p) {
		if (!p[1])
			return false;
		prefixlen = strtoul(p + 1, &endptr, 10);
		if (*endptr || prefixlen > (family == AF_INET6 ? 128 : 32))
			return false;
		str_dup = STRNDUP(str, p - str);
	}

	res = inet_pton(family, str_dup ? str_dup : str, &addr);

	if (str_dup)
		FREE_CONST(str_dup);

	return res;
}

/* IP network to string representation */
static char *
inet_sockaddrtos2(const sockaddr_t *addr, char *addr_str)
{
	const void *addr_ip;

	if (addr->ss_family == AF_UNSPEC) {
		strcpy(addr_str, "(none)");
		return addr_str;
	}

	if (addr->ss_family == AF_INET6) {
		const struct sockaddr_in6 *addr6 = PTR_CAST_CONST(struct sockaddr_in6, addr);
		addr_ip = &addr6->sin6_addr;
	} else {
		const struct sockaddr_in *addr4 = PTR_CAST_CONST(struct sockaddr_in, addr);
		addr_ip = &addr4->sin_addr;
	}

	if (!inet_ntop(addr->ss_family, addr_ip, addr_str, INET6_ADDRSTRLEN))
		return NULL;

	return addr_str;
}

const char *
inet_sockaddrtos(const sockaddr_t *addr)
{
	static char addr_str[INET6_ADDRSTRLEN];
	inet_sockaddrtos2(addr, addr_str);
	return addr_str;
}

uint16_t __attribute__ ((pure))
inet_sockaddrport(const sockaddr_t *addr)
{
	if (addr->ss_family == AF_INET6) {
		const struct sockaddr_in6 *addr6 = PTR_CAST_CONST(struct sockaddr_in6, addr);
		return addr6->sin6_port;
	}

	/* Note: this might be AF_UNSPEC if it is the sequence number of
	 * a virtual server in a virtual server group */
	const struct sockaddr_in *addr4 = PTR_CAST_CONST(struct sockaddr_in, addr);
	return addr4->sin_port;
}

void
inet_set_sockaddrport(sockaddr_t *addr, uint16_t port)
{
	if (addr->ss_family == AF_INET6) {
		struct sockaddr_in6 *addr6 = PTR_CAST(struct sockaddr_in6, addr);
		addr6->sin6_port = port;
	} else {
		struct sockaddr_in *addr4 = PTR_CAST(struct sockaddr_in, addr);
		addr4->sin_port = port;
	}
}

const char *
inet_sockaddrtopair(const sockaddr_t *addr)
{
	char addr_str[INET6_ADDRSTRLEN];
	static char ret[sizeof(addr_str) + 8];	/* '[' + addr_str + ']' + ':' + 'nnnnn' */

	inet_sockaddrtos2(addr, addr_str);
	snprintf(ret, sizeof(ret), "[%s]:%d"
		, addr_str
		, ntohs(inet_sockaddrport(addr)));
	return ret;
}

char *
inet_sockaddrtotrio_r(const sockaddr_t *addr, uint16_t proto, char *buf)
{
	char addr_str[INET6_ADDRSTRLEN];
	const char *proto_str =
			proto == IPPROTO_TCP ? "tcp" :
			proto == IPPROTO_UDP ? "udp" :
			proto == IPPROTO_SCTP ? "sctp" :
			proto == 0 ? "none" : "?";

	inet_sockaddrtos2(addr, addr_str);
	snprintf(buf, SOCKADDRTRIO_STR_LEN, "[%s]:%s:%d", addr_str, proto_str,
		 ntohs(inet_sockaddrport(addr)));
	return buf;
}

const char *
inet_sockaddrtotrio(const sockaddr_t *addr, uint16_t proto)
{
	static char ret[SOCKADDRTRIO_STR_LEN];

	inet_sockaddrtotrio_r(addr, proto, ret);

	return ret;
}

uint32_t __attribute__ ((pure))
inet_sockaddrip4(const sockaddr_t *addr)
{
	if (addr->ss_family != AF_INET)
		return 0xffffffff;

	return PTR_CAST_CONST(struct sockaddr_in, addr)->sin_addr.s_addr;
}

int
inet_sockaddrip6(const sockaddr_t *addr, struct in6_addr *ip6)
{
	if (addr->ss_family != AF_INET6)
		return -1;

	*ip6 = PTR_CAST_CONST(struct sockaddr_in6, addr)->sin6_addr;
	return 0;
}

/* IPv6 address compare */
int __attribute__ ((pure))
inet_inaddrcmp(const int family, const void *a, const void *b)
{
	int64_t addr_diff;

	if (family == AF_INET)
		addr_diff = (int64_t)ntohl(*PTR_CAST_CONST(uint32_t, a)) - (int64_t)ntohl(*PTR_CAST_CONST(uint32_t, b));
	else if (family == AF_INET6) {
		int i;

		for (i = 0; i < 4; i++ ) {
			if ((addr_diff = (int64_t)ntohl(PTR_CAST_CONST(uint32_t, (a))[i]) - (int64_t)ntohl(PTR_CAST_CONST(uint32_t, (b))[i])))
				break;
		}
	} else
		return -2;

	if (addr_diff > 0)
		return 1;
	if (addr_diff < 0)
		return -1;

	return 0;
}

/* inet_sockaddcmp is similar to sockstorage_equal except the latter also compares the port */
int  __attribute__ ((pure))
inet_sockaddrcmp(const sockaddr_t *a, const sockaddr_t *b)
{
	if (a->ss_family != b->ss_family)
		return -2;

	if (a->ss_family == AF_INET)
		return inet_inaddrcmp(a->ss_family,
				      &PTR_CAST_CONST(struct sockaddr_in, a)->sin_addr,
				      &PTR_CAST_CONST(struct sockaddr_in, b)->sin_addr);
	if (a->ss_family == AF_INET6)
		return inet_inaddrcmp(a->ss_family,
				      &PTR_CAST_CONST(struct sockaddr_in6, a)->sin6_addr,
				      &PTR_CAST_CONST(struct sockaddr_in6, b)->sin6_addr);
	return 0;
}


#ifdef _INCLUDE_UNUSED_CODE_
/*
 * IP string to network representation
 * Highly inspired from Paul Vixie code.
 */
int
inet_ston(const char *addr, uint32_t *dst)
{
	static char digits[] = "0123456789";
	int saw_digit, octets, ch;
	u_char tmp[INADDRSZ], *tp;

	saw_digit = 0;
	octets = 0;
	*(tp = tmp) = 0;

	while ((ch = *addr++) != '\0' && ch != '/' && ch != '-') {
		const char *pch;
		if ((pch = strchr(digits, ch)) != NULL) {
			u_int new = *tp * 10 + (pch - digits);
			if (new > 255)
				return 0;
			*tp = new;
			if (!saw_digit) {
				if (++octets > 4)
					return 0;
				saw_digit = 1;
			}
		} else if (ch == '.' && saw_digit) {
			if (octets == 4)
				return 0;
			*++tp = 0;
			saw_digit = 0;
		} else
			return 0;
	}

	if (octets < 4)
		return 0;

	memcpy(dst, tmp, INADDRSZ);
	return 1;
}

/*
 * Return broadcast address from network and netmask.
 */
uint32_t
inet_broadcast(uint32_t network, uint32_t netmask)
{
	return 0xffffffff - netmask + network;
}

/*
 * Convert CIDR netmask notation to long notation.
 */
uint32_t
inet_cidrtomask(uint8_t cidr)
{
	uint32_t mask = 0;
	int b;

	for (b = 0; b < cidr; b++)
		mask |= (1 << (31 - b));
	return ntohl(mask);
}
#endif

void
format_mac_buf(char *op, size_t op_len, const unsigned char *addr, size_t addr_len)
{
	size_t i;
	char *buf_end = op + op_len;

	/* If there is no address, clear the op buffer */
	if (!addr_len && op_len) {
		op[0] = '\0';
		return;
	}

	for (i = 0; i < addr_len; i++) {
		op += snprintf(op, buf_end - op, "%.2x%s",
		      addr[i], i < addr_len -1 ? ":" : "");
		if (op >= buf_end - 1)
			break;
	}
}

const char *
format_decimal(unsigned long val, int dp)
{
	static char buf[22];	/* Sufficient for 2^64 as decimal plus decimal point */
	unsigned dp_factor = 1;
	int i;

	for (i = 0; i < dp; i++)
		dp_factor *= 10;

	snprintf(buf, sizeof(buf), "%lu.%*.*lu", val / dp_factor, dp, dp, val % dp_factor);

	return buf;
}

/* Getting localhost official canonical name */
const char * __attribute__((malloc))
get_local_name(void)
{
	struct utsname name;
	struct addrinfo hints, *res = NULL;
	char *canonname = NULL;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_flags = AI_CANONNAME;

	if (uname(&name) < 0)
		return NULL;

	if (getaddrinfo(name.nodename, NULL, &hints, &res) != 0)
		return NULL;

	if (res && res->ai_canonname)
		canonname = STRDUP(res->ai_canonname);

	freeaddrinfo(res);

	return canonname;
}

/* String compare with NULL string handling */
bool __attribute__ ((pure))
string_equal(const char *str1, const char *str2)
{
	if (!str1 && !str2)
		return true;
	if (!str1 != !str2)
		return false;

	return !strcmp(str1, str2);
}

/* Convert an integer into a string */
int
integer_to_string(const int value, char *str, size_t size)
{
	int i, len = 0, t = value, s = size;

	for (i = value; i; i/=10) {
		if (++len > s)
			return -1;
	}

	for (i = 0; i < len; i++,t/=10)
		str[len - (i + 1)] = t % 10 + '0';

	return len;
}

/* Like ctime_r() but to microseconds and no terminating '\n'.
   Buf must be at least 32 bytes */
char *
ctime_us_r(const timeval_t *timep, char *buf)
{
	struct tm tm;

	localtime_r(&timep->tv_sec, &tm);
	asctime_r(&tm, buf);
	snprintf(buf + 19, 8, ".%6.6" PRI_tv_usec, timep->tv_usec);
	strftime(buf + 26, 6, " %Y", &tm);

	return buf;
}

/* We need to use O_NOFOLLOW if opening a file for write, so that a non privileged user can't
 * create a symbolic link from the path to a system file and cause a system file to be overwritten. */
FILE * __attribute__((malloc))
fopen_safe(const char *path, const char *mode)
{
	int fd;
	FILE *file;
#ifdef ENABLE_LOG_FILE_APPEND
	int flags = O_NOFOLLOW | O_CREAT | O_CLOEXEC;
#endif
	int sav_errno;
	char file_tmp_name[PATH_MAX];

	if (mode[0] == 'r')
		return fopen(path, mode);

	if ((mode[0] != 'a' && mode[0] != 'w') ||
	    (mode[1] &&
	     ((mode[1] != 'e' && mode[1] != '+') || mode[2]))) {
		errno = EINVAL;
		return NULL;
	}

	if (mode[0] == 'w') {
		/* If we truncate an existing file, any non-privileged user who already has the file
		 * open would be able to read what we write, even though the file access mode is changed.
		 *
		 * If we unlink an existing file and the desired file is subsequently created via open,
		 * it leaves a window for someone else to create the same file between the unlink and the open.
		 *
		 * The solution is to create a temporary file that we will rename to the desired file name.
		 * Since the temporary file is created owned by root with the only file access permissions being
		 * owner read and write, no non root user will have access to the file. Further, the rename to
		 * the requested filename is atomic, and so there is no window when someone else could create
		 * another file of the same name.
		 */
		strcpy_safe(file_tmp_name, path);
		if (strlen(path) + 6 < sizeof(file_tmp_name))
			strcat(file_tmp_name, "XXXXXX");
		else
			strcpy(file_tmp_name + sizeof(file_tmp_name) - 6 - 1, "XXXXXX");
		fd = mkostemp(file_tmp_name, O_CLOEXEC);
	} else {
		/* Only allow append mode if debugging features requiring append are enabled. Since we
		 * can't unlink the file, there may be a non privileged user who already has the file open
		 * for read (e.g. tail -f). If these debug option aren't enabled, there is no potential
		 * security risk in that respect. */
#ifndef ENABLE_LOG_FILE_APPEND
		log_message(LOG_INFO, "BUG - shouldn't be opening file for append with current build options");
		errno = EINVAL;
		return NULL;
#else
		flags |= O_APPEND;

		if (mode[1])
			flags |= O_RDWR;
		else
			flags |= O_WRONLY;

		fd = open(path, flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
#endif
	}

	if (fd == -1) {
		sav_errno = errno;
		log_message(LOG_INFO, "Unable to open '%s' - errno %d (%m)", path, errno);
		errno = sav_errno;
		return NULL;
	}

#ifndef ENABLE_LOG_FILE_APPEND
	/* Change file ownership to root */
	if (mode[0] == 'a' && fchown(fd, 0, 0)) {
		sav_errno = errno;
		log_message(LOG_INFO, "Unable to change file ownership of %s- errno %d (%m)", path, errno);
		close(fd);
		errno = sav_errno;
		return NULL;
	}
#endif

	/* Set file mode, default rw------- */
	if (fchmod(fd, (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) & ~umask_val)) {
		sav_errno = errno;
		log_message(LOG_INFO, "Unable to change file permission of %s - errno %d (%m)", path, errno);
		close(fd);
		errno = sav_errno;
		return NULL;
	}

	if (mode[0] == 'w') {
		/* Rename the temporary file to the one we want */
		if (rename(file_tmp_name, path)) {
			sav_errno = errno;
			log_message(LOG_INFO, "Failed to rename %s to %s - errno %d (%m)", file_tmp_name, path, errno);
			close(fd);
			errno = sav_errno;
			return NULL;
		}
	}

	file = fdopen(fd, mode);
	if (!file) {
		sav_errno = errno;
		log_message(LOG_INFO, "fdopen(\"%s\") failed - errno %d (%m)", path, errno);
		close(fd);
		errno = sav_errno;
		return NULL;
	}

	return file;
}

void
set_std_fd(bool force)
{
	int fd;

	if (force || __test_bit(DONT_FORK_BIT, &debug)) {
		fd = open("/dev/null", O_RDWR);
		if (fd != -1) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}
	}

	/* coverity[leaked_handle] */
}

void
close_std_fd(void)
{
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
}

#if defined _WITH_VRRP_ || defined _WITH_BFD_
int
open_pipe(int pipe_arr[2])
{
	/* Open pipe */
	if (pipe2(pipe_arr, O_CLOEXEC | O_NONBLOCK) == -1)
		return -1;

	return 0;
}
#endif

/*
 * memcmp time constant variant.
 * Need to ensure compiler doesnt get too smart by optimizing generated asm code.
 * So long as LTO is not in use, the loop cannot be short-circuited since the
 * compiler doesn't know how ret is used.
 * If LTO is in use, there is a risk that the compiler/linker will work out
 * that the return value is only checked for non-zero, and that since the loop
 * can only set additional bits in ret, once ret becomes non-zero it can return
 * a non-zero value. We there need to ensure that a local copy of the function,
 * which can then be optimised, cannot be generated. Stopping inlining and cloning
 * should force this.
 */
__attribute__((pure, noinline, ATTRIBUTE_NOCLONE)) int
memcmp_constant_time(const void *s1, const void *s2, size_t n)
{
	const unsigned char *a, *b;
	unsigned char ret = 0;
	size_t i;

	for (i = 0, a = s1, b = s2; i < n; i++)
		ret |= (*a++ ^ *b++);

	return ret;
}

/*
 * Utility functions coming from Wensong code
 */

#if defined _WITH_LVS_ || defined _HAVE_LIBIPSET_

#ifdef _HAVE_LIBKMOD_
bool
keepalived_modprobe(const char *mod_name)
{
	struct kmod_ctx *ctx;
	struct kmod_list *list = NULL, *l;
	int err;
	int flags;
	const char *null_config = NULL;

	if (!(ctx = kmod_new(NULL, &null_config))) {
		log_message(LOG_INFO, "kmod_new failed, err %d - %m", errno);

		return false;
	}

	kmod_load_resources(ctx);

	err = kmod_module_new_from_lookup(ctx, mod_name, &list);
	if (list == NULL || err < 0) {
		log_message(LOG_INFO, "kmod_module_new_from_lookup failed - err %d", -err);
		kmod_unref(ctx);
		return false;
	}

	flags = KMOD_PROBE_APPLY_BLACKLIST_ALIAS_ONLY; // | KMOD_PROBE_FAIL_ON_LOADED;
	kmod_list_foreach(l, list) {
		struct kmod_module *mod = kmod_module_get_module(l);
		err = kmod_module_probe_insert_module(mod, flags, NULL, NULL, NULL, NULL);
		if (err < 0) {
			errno = -err;
			log_message(LOG_INFO, "kmod_module_probe_insert_module %s failed - err %d - %m", kmod_module_get_name(mod), -err);
			kmod_module_unref(mod);
			kmod_module_unref_list(list);
			kmod_unref(ctx);
			return false;
		}

		kmod_module_unref(mod);
	}

	kmod_module_unref_list(list);

	kmod_unref(ctx);

	return true;
}

#else

static char*
get_modprobe(void)
{
	int procfile;
	char *ret;
	ssize_t count;
	struct stat buf;

	ret = MALLOC(PATH_MAX);
	if (!ret)
		return NULL;

	procfile = open("/proc/sys/kernel/modprobe", O_RDONLY | O_CLOEXEC);
	if (procfile < 0) {
		FREE(ret);
		return NULL;
	}

	count = read(procfile, ret, PATH_MAX - 1);
	ret[PATH_MAX - 1] = '\0';
	close(procfile);

	if (count > 0 && count < PATH_MAX - 1)
	{
		if (ret[count - 1] == '\n')
			ret[count - 1] = '\0';
		else
			ret[count] = '\0';

		/* Check it is a regular file, with a execute bit set */
		if (!stat(ret, &buf) &&
		    S_ISREG(buf.st_mode) &&
		    (buf.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
			return ret;
	}

	FREE(ret);

	return NULL;
}

bool
keepalived_modprobe(const char *mod_name)
{
	const char *argv[] = { "/sbin/modprobe", "-s", "--", mod_name, NULL };
	int child;
	int status;
	int rc;
	char *modprobe = get_modprobe();
	struct sigaction act, old_act;
	union non_const_args args;

	if (modprobe)
		argv[0] = modprobe;

	act.sa_handler = SIG_DFL;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;

	sigaction(SIGCHLD, &act, &old_act);

#ifdef ENABLE_LOG_TO_FILE
	if (log_file_name)
		flush_log_file();
#endif

	do {
		if (!(child = fork())) {
			args.args = argv;
			/* coverity[tainted_string] */
			execv(argv[0], args.execve_args);
			exit(1);
		}

		rc = waitpid(child, &status, 0);
		if (rc < 0)
			log_message(LOG_INFO, "modprobe: waitpid error (%s)"
					    , strerror(errno));

		/* It has been reported (see issue #2040) that some modprobes
		 * do not support the -s option, so try without if we get a
		 * failure. */
		if (!WIFEXITED(status) || !WEXITSTATUS(status))
			break;

		if (!argv[2])
			break;

		argv[1] = mod_name;
		argv[2] = NULL;
	 } while (true);

	sigaction(SIGCHLD, &old_act, NULL);

	if (modprobe)
		FREE(modprobe);

	return WIFEXITED(status) && !WEXITSTATUS(status);
}
#endif
#endif

void
set_tmp_dir(void)
{
	if (!(tmp_dir = getenv("TMPDIR")) || tmp_dir[0] != '/')
		tmp_dir = KA_TMP_DIR;
}

const char *
make_tmp_filename(const char *file_name)
{
	size_t tmp_dir_len = strlen(tmp_dir);
	char *path = MALLOC(tmp_dir_len + 1 + strlen(file_name) + 1);

	strcpy(path, tmp_dir);
	path[tmp_dir_len] = '/';
	strcpy(path + tmp_dir_len + 1, file_name);

	return path;
}

#if defined USE_CLOSE_RANGE_SYSCALL
int
close_range(unsigned first, unsigned last, int flags)
{
	int ret;

	ret = syscall(SYS_close_range, first, last, flags);

	return ret;
}
#endif

#if !defined HAVE_CLOSE_RANGE || !HAVE_DECL_CLOSE_RANGE_CLOEXEC
unsigned
get_open_fds(uint64_t *fds, unsigned num_ent)
{
	DIR *dir = opendir("/proc/self/fd");
	struct dirent *ent;
	unsigned long fd_num;
	unsigned i;
	unsigned max_fd = 0;
	struct stat stbuf;

	for (i = 0; i < num_ent; i++)
		fds[i] = 0;
	
	while ((ent = readdir(dir))) {
		// Allow for . and ..
		if (!isdigit(ent->d_name[0]))
			continue;

		fd_num = strtoul(ent->d_name, NULL, 10);

		/* Make sure it isn't a directory - i.e. the fd returned by opendir() */
		if (fstat(fd_num, &stbuf) || S_ISDIR(stbuf.st_mode))
			continue;

		fds[fd_num / 64] |= 1UL << (fd_num % 64) ;

		if (fd_num > max_fd)
			max_fd = fd_num;
	}

	closedir(dir);

	return max_fd;
}
#endif

void
log_stopping(void)
{
	if (__test_bit(LOG_DETAIL_BIT, &debug)) {
		struct rusage usage, child_usage;

		getrusage(RUSAGE_SELF, &usage);
		getrusage(RUSAGE_CHILDREN, &child_usage);

		if (child_usage.ru_utime.tv_sec || child_usage.ru_utime.tv_usec)
			log_message(LOG_INFO, "Stopped - used (self/children) %" PRI_tv_sec ".%6.6" PRI_tv_usec "/%" PRI_tv_sec ".%6.6" PRI_tv_usec " user time,"
				        " %" PRI_tv_sec ".%6.6" PRI_tv_usec "/%" PRI_tv_sec ".%6.6" PRI_tv_usec " system time",
					usage.ru_utime.tv_sec, usage.ru_utime.tv_usec, child_usage.ru_utime.tv_sec, child_usage.ru_utime.tv_usec,
					usage.ru_stime.tv_sec, usage.ru_stime.tv_usec, child_usage.ru_stime.tv_sec, child_usage.ru_stime.tv_usec);
		else
			log_message(LOG_INFO, "Stopped - used %" PRI_tv_sec ".%6.6" PRI_tv_usec " user time, %" PRI_tv_sec ".%6.6" PRI_tv_usec " system time",
					usage.ru_utime.tv_sec, usage.ru_utime.tv_usec, usage.ru_stime.tv_sec, usage.ru_stime.tv_usec);
	} else
		log_message(LOG_INFO, "Stopped");
}
