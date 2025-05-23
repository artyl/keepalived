/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        BFD child process handling
 *
 * Author:      Ilya Voronin, <ivoronin@gmail.com>
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
 * Copyright (C) 2015-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <sys/time.h>
#ifdef _WITH_PROFILING_
#include <sys/gmon.h>
#endif

#include "bfd.h"
#include "bfd_daemon.h"
#include "bfd_data.h"
#include "bfd_parser.h"
#include "bfd_scheduler.h"
#include "bfd_event.h"
#include "pidfile.h"
#include "logger.h"
#include "signals.h"
#include "main.h"
#include "parser.h"
#include "time.h"
#include "global_data.h"
#include "bitops.h"
#include "utils.h"
#include "scheduler.h"
#include "process.h"
#include "utils.h"
#ifdef _WITH_TRACK_PROCESS_
#include "track_process.h"
#endif
#ifdef _USE_SYSTEMD_NOTIFY_
#include "systemd.h"
#endif
#ifndef _ONE_PROCESS_DEBUG_
#include "config_notify.h"
#endif


/* Global variables */
int bfd_vrrp_event_pipe[2] = { -1, -1};
int bfd_checker_event_pipe[2] = { -1, -1};

/* Local variables */
static const char *bfd_syslog_ident;

#ifndef _ONE_PROCESS_DEBUG_
static void reload_bfd_thread(thread_ref_t);
static timeval_t bfd_start_time;
static unsigned bfd_next_restart_delay;
#endif

/* Daemon stop sequence */
static void
stop_bfd(int status)
{
	if (__test_bit(CONFIG_TEST_BIT, &debug))
		return;

	/* Stop daemon */
	pidfile_rm(&bfd_pidfile);

	/* Clean data */
	free_global_data(&global_data);
	bfd_dispatcher_release(bfd_data);
	free_bfd_data(&bfd_data);
	free_bfd_buffer();
	thread_destroy_master(master);
	free_parent_mallocs_exit();

	/*
	 * Reached when terminate signal catched.
	 * finally return to parent process.
	 */
	log_stopping();

#ifdef ENABLE_LOG_TO_FILE
	if (log_file_name)
		close_log_file();
#endif
	closelog();

#ifndef _MEM_CHECK_LOG_
	FREE_CONST_PTR(bfd_syslog_ident);
#else
	if (bfd_syslog_ident)
		free(no_const_char_p(bfd_syslog_ident));	/* malloc'd by make_syslog_ident() */
#endif
	close_std_fd();

	exit(status);
}

/* Daemon init sequence */
bool
open_bfd_pipes(void)
{
#ifdef _WITH_VRRP_
	/* Open BFD VRRP control pipe */
	if (open_pipe(bfd_vrrp_event_pipe) == -1) {
		log_message(LOG_ERR, "Unable to create BFD vrrp event pipe: %m");
		return false;
	}
#endif

#ifdef _WITH_LVS_
	/* Open BFD checker control pipe */
	if (open_pipe(bfd_checker_event_pipe) == -1) {
		log_message(LOG_ERR, "Unable to create BFD checker event pipe: %m");
		return false;
	}
#endif

	return true;
}

/* Daemon init sequence */
static void
start_bfd(__attribute__((unused)) data_t *prev_global_data)
{
	srandom(time(NULL));

	if (reload)
		global_data = alloc_global_data();
	if (!(bfd_data = alloc_bfd_data())) {
		stop_bfd(KEEPALIVED_EXIT_FATAL);
		return;
	}

	alloc_bfd_buffer();

	init_data(conf_file, bfd_init_keywords, false);

	if (reload)
		init_global_data(global_data, prev_global_data, true);

	/* Update process name if necessary */
	if ((!prev_global_data &&		// startup
	    global_data->bfd_process_name) ||
	    (prev_global_data &&		// reload
	     (!global_data->bfd_process_name != !prev_global_data->bfd_process_name ||
	      (global_data->bfd_process_name && strcmp(global_data->bfd_process_name, prev_global_data->bfd_process_name)))))
		set_process_name(global_data->bfd_process_name);

	/* If we are just testing the configuration, then we terminate now */
	if (__test_bit(CONFIG_TEST_BIT, &debug))
		return;

	bfd_complete_init();

#ifndef _ONE_PROCESS_DEBUG_
	if (global_data->reload_check_config && get_config_status() != CONFIG_OK) {
		stop_bfd(KEEPALIVED_EXIT_CONFIG);
		return;
	}

	/* Notify parent config has been read if appropriate */
	if (!__test_bit(CONFIG_TEST_BIT, &debug))
		notify_config_read();
#endif

	if (__test_bit(DUMP_CONF_BIT, &debug))
		dump_bfd_data(NULL, bfd_data);

	thread_add_event(master, bfd_dispatcher_init, bfd_data, 0);

	/* Set the process priority and non swappable if configured */
	if (reload)
		restore_priority(
				global_data->bfd_realtime_priority, global_data->max_auto_priority, global_data->min_auto_priority_delay,
				global_data->bfd_rlimit_rt, global_data->bfd_process_priority, global_data->bfd_no_swap ? BFD_STACK_SIZE : 0);
	else
		set_process_priorities(
				global_data->bfd_realtime_priority, global_data->max_auto_priority, global_data->min_auto_priority_delay,
				global_data->bfd_rlimit_rt, global_data->bfd_process_priority, global_data->bfd_no_swap ? BFD_STACK_SIZE : 0);

	/* Set the process cpu affinity if configured */
	set_process_cpu_affinity(&global_data->bfd_cpu_mask, "bfd");
}

void
bfd_validate_config(void)
{
	start_bfd(NULL);
}

#ifndef _ONE_PROCESS_DEBUG_
static void
print_bfd_thread(__attribute__((unused)) thread_ref_t thread)
{
	bfd_print_data();
}

/* Reload handler */
static void
sigreload_bfd(__attribute__ ((unused)) void *v,
	   __attribute__ ((unused)) int sig)
{
	thread_add_event(master, reload_bfd_thread, NULL, 0);
}

static void
sigdump_bfd(__attribute__((unused)) void *v, __attribute__((unused)) int sig)
{
	log_message(LOG_INFO, "Printing BFD data for process(%d) on signal",
		    our_pid);
	thread_add_event(master, print_bfd_thread, NULL, 0);
}

/* Terminate handler */
static void
sigend_bfd(__attribute__ ((unused)) void *v,
	   __attribute__ ((unused)) int sig)
{
	if (master)
		thread_add_terminate_event(master);
}

/* BFD Child signal handling */
static void
bfd_signal_init(void)
{
	signal_set(SIGHUP, sigreload_bfd, NULL);
	if (ignore_sigint)
		signal_ignore(SIGINT);
	else
		signal_set(SIGINT, sigend_bfd, NULL);
	signal_set(SIGTERM, sigend_bfd, NULL);
	signal_set(SIGUSR1, sigdump_bfd, NULL);
#ifdef THREAD_DUMP
	signal_set(SIGTDUMP, thread_dump_signal, NULL);
#endif
	signal_ignore(SIGPIPE);
}

/* Reload thread */
static void
reload_bfd_thread(__attribute__((unused)) thread_ref_t thread)
{
	timeval_t timer;
	timer = timer_now();

	log_message(LOG_INFO, "Reloading");

	/* Use standard scheduling while reloading */
	reset_priority();

#ifndef _ONE_PROCESS_DEBUG_
	save_config(false, "bfd", dump_bfd_data_global);
#endif

	/* set the reloading flag */
	SET_RELOAD;

	/* Destroy master thread */
	bfd_dispatcher_release(bfd_data);
	thread_cleanup_master(master, true);
	thread_add_base_threads(master, false);

	old_bfd_data = bfd_data;
	bfd_data = NULL;
	old_global_data = global_data;
	global_data = NULL;

	reinitialise_global_vars();

	/* Reload the conf */
	signal_set(SIGCHLD, thread_child_handler, master);
	start_bfd(old_global_data);

	free_bfd_data(&old_bfd_data);
	free_global_data(&old_global_data);

#ifndef _ONE_PROCESS_DEBUG_
	save_config(true, "bfd", dump_bfd_data_global);
#endif

	UNSET_RELOAD;

	set_time_now();
	log_message(LOG_INFO, "Reload finished in %lu usec", -timer_long(timer_sub_now(timer)));

	/* Post initializations */
#ifdef _MEM_CHECK_
	log_message(LOG_INFO, "Configuration is using : %zu Bytes", get_keepalived_cur_mem_allocated());
#endif
}

/* This function runs in the parent process. */
static void
delayed_restart_bfd_child_thread(__attribute__((unused)) thread_ref_t thread)
{
	start_bfd_child();
}

/* BFD Child respawning thread. This function runs in the parent process. */
static void
bfd_respawn_thread(thread_ref_t thread)
{
	unsigned restart_delay;
	int ret;

	/* We catch a SIGCHLD, handle it */
	bfd_child = 0;

	if ((ret = report_child_status(thread->u.c.status, thread->u.c.pid, NULL)))
		thread_add_parent_terminate_event(thread->master, ret);
	else if (!__test_bit(DONT_RESPAWN_BIT, &debug)) {
		log_child_died("BFD", thread->u.c.pid);

		restart_delay = calc_restart_delay(&bfd_start_time, &bfd_next_restart_delay, "BFD");
		if (!restart_delay)
			start_bfd_child();
		else
			thread_add_timer(thread->master, delayed_restart_bfd_child_thread, NULL, restart_delay * TIMER_HZ);
	} else {
		log_message(LOG_ALERT, "BFD child process(%d) died: Exiting", thread->u.c.pid);
		raise(SIGTERM);
	}
}

#ifdef THREAD_DUMP
static void
register_bfd_thread_addresses(void)
{
	register_scheduler_addresses();
	register_signal_thread_addresses();

	register_bfd_scheduler_addresses();

	register_thread_address("bfd_dispatcher_init", bfd_dispatcher_init);
	register_thread_address("reload_bfd_thread", reload_bfd_thread);
	register_thread_address("print_bfd_thread", print_bfd_thread);

	register_signal_handler_address("sigreload_bfd", sigreload_bfd);
	register_signal_handler_address("sigdump_bfd", sigdump_bfd);
	register_signal_handler_address("sigend_bfd", sigend_bfd);
	register_signal_handler_address("thread_child_handler", thread_child_handler);
#ifdef THREAD_DUMP
	register_signal_handler_address("thread_dump_signal", thread_dump_signal);
#endif
}
#endif
#endif

int
start_bfd_child(void)
{
#ifndef _ONE_PROCESS_DEBUG_
	pid_t pid;
	int ret;
	const char *syslog_ident;

	/* Initialize child process */
#ifdef ENABLE_LOG_TO_FILE
	if (log_file_name)
		flush_log_file();
#endif

	pid = fork();

	if (pid < 0) {
		log_message(LOG_INFO, "BFD child process: fork error(%m)");
		return -1;
	} else if (pid) {
		bfd_child = pid;
		bfd_start_time = time_now;

		log_message(LOG_INFO, "Starting BFD child process, pid=%d",
			    pid);

		/* Start respawning thread */
		thread_add_child(master, bfd_respawn_thread, NULL,
				 pid, TIMER_NEVER);
		return 0;
	}

	our_pid = getpid();

#ifdef _WITH_PROFILING_
	/* See https://lists.gnu.org/archive/html/bug-gnu-utils/2001-09/msg00047.html for details */
	monstartup ((u_long) &_start, (u_long) &etext);
#endif

	prctl(PR_SET_PDEATHSIG, SIGTERM);

	/* Check our parent hasn't already changed since the fork */
	if (main_pid != getppid())
		kill(our_pid, SIGTERM);

	prog_type = PROG_TYPE_BFD;

	close_other_pidfiles();

	/* Close the read end of the event notification pipes, and the track_process fd */
#ifdef _WITH_VRRP_
	close(bfd_vrrp_event_pipe[0]);
#ifdef _WITH_TRACK_PROCESS_
	close_track_processes();
#endif
#endif
#ifdef _WITH_LVS_
	close(bfd_checker_event_pipe[0]);
#endif

#ifdef THREAD_DUMP
	/* Remove anything we might have inherited from parent */
	deregister_thread_addresses();
#endif

	initialise_debug_options();

	if ((global_data->instance_name || global_data->network_namespace) &&
	     (bfd_syslog_ident = make_syslog_ident(PROG_BFD)))
		syslog_ident = bfd_syslog_ident;
	else
		syslog_ident = PROG_BFD;

	/* Opening local BFD syslog channel */
	if (!__test_bit(NO_SYSLOG_BIT, &debug))
		open_syslog(syslog_ident);

#ifdef ENABLE_LOG_TO_FILE
	if (log_file_name)
		open_log_file(log_file_name,
				"bfd",
				global_data->network_namespace,
				global_data->instance_name);
#endif

#ifdef DO_STACKSIZE
	get_stacksize(false);
#endif

#ifdef _MEM_CHECK_
	mem_log_init(PROG_BFD, "BFD child process");
#endif

	free_parent_mallocs_startup(true);

	/* Clear any child finder functions set in parent */
	set_child_finder_name(NULL);

	/* Create an independent file descriptor for the shared config file */
	separate_config_file();

	/* Child process part, write pidfile */
	if (!pidfile_write(&bfd_pidfile)) {
		/* Fatal error */
		log_message(LOG_INFO,
			    "BFD child process: cannot write pidfile");
		exit(0);
	}

#ifdef _USE_SYSTEMD_NOTIFY_
	systemd_unset_notify();
#endif

	/* Create the new master thread */
	thread_destroy_master(master);
	master = thread_make_master();

	/* change to / dir */
	ret = chdir("/");
	if (ret < 0) {
		log_message(LOG_INFO, "BFD child process: error chdir");
	}
#endif

	/* If last process died during a reload, we can get there and we
	 * don't want to loop again, because we're not reloading anymore.
	 */
	UNSET_RELOAD;

#ifndef _ONE_PROCESS_DEBUG_
	/* Signal handling initialization */
	bfd_signal_init();

	/* Register emergency shutdown function */
	register_shutdown_function(stop_bfd);
#endif

	/* Start BFD daemon */
	start_bfd(NULL);

#ifdef _ONE_PROCESS_DEBUG_
	return 0;
#else

#ifdef THREAD_DUMP
	register_bfd_thread_addresses();
#endif

	/* Post initializations */
#ifdef _MEM_CHECK_
	log_message(LOG_INFO, "Configuration is using : %zu Bytes", get_keepalived_cur_mem_allocated());
#endif

	/* Launch the scheduling I/O multiplexer */
	launch_thread_scheduler(master);

#ifdef THREAD_DUMP
	deregister_thread_addresses();
#endif

#ifdef DO_STACKSIZE
	get_stacksize(true);
#endif

	/* Finish BFD daemon process */
	stop_bfd(EXIT_SUCCESS);

	/* unreachable */
	exit(EXIT_SUCCESS);
#endif
}

#ifdef THREAD_DUMP
void
register_bfd_parent_addresses(void)
{
#ifndef _ONE_PROCESS_DEBUG_
	register_thread_address("bfd_respawn_thread", bfd_respawn_thread);
	register_thread_address("delayed_restart_bfd_child_thread", delayed_restart_bfd_child_thread);
#endif
}
#endif
