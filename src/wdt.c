/*
 * Thread lockup detection
 *
 * Copyright 2000-2019 Willy Tarreau <willy@haproxy.org>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <signal.h>
#include <time.h>

#include <haproxy/activity.h>
#include <haproxy/api.h>
#include <haproxy/cfgparse.h>
#include <haproxy/clock.h>
#include <haproxy/debug.h>
#include <haproxy/errors.h>
#include <haproxy/global.h>
#include <haproxy/signal-t.h>
#include <haproxy/task.h>
#include <haproxy/thread.h>
#include <haproxy/tools.h>


/*
 * It relies on timer_create() and timer_settime() which are only available in
 * this case.
 */
#if defined(USE_RT) && defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0) && defined(_POSIX_THREAD_CPUTIME)

/* define a dummy value to designate "no timer". Use only 32 bits. */
#ifndef TIMER_INVALID
#define TIMER_INVALID ((timer_t)(unsigned long)(0xfffffffful))
#endif

/* per-thread context for the watchdog, permits to store timers, counters,
 * task pointers, etc (anything that helps providing accurate reports).
 */
static struct {
	timer_t timer;
} per_thread_wd_ctx[MAX_THREADS];

/* warn about stuck tasks after this delay (ns) */
static unsigned int wdt_warn_blocked_traffic_ns = 100000000U;

/* Setup (or ping) the watchdog timer for thread <thr>. Returns non-zero on
 * success, zero on failure. It interrupts once per second of CPU time. It
 * happens that timers based on the CPU time are not automatically re-armed
 * so we only use the value and leave the interval unset.
 */
int wdt_ping(int thr)
{
	struct itimerspec its;

	its.it_value.tv_sec    = wdt_warn_blocked_traffic_ns / 1000000000U;
	its.it_value.tv_nsec   = wdt_warn_blocked_traffic_ns % 1000000000U;
	its.it_interval.tv_sec = 0; its.it_interval.tv_nsec = 0;
	return timer_settime(per_thread_wd_ctx[thr].timer, 0, &its, NULL) == 0;
}

/* This is the WDTSIG signal handler */
void wdt_handler(int sig, siginfo_t *si, void *arg)
{
	unsigned long long n, p;
	ulong thr_bit;
	int thr, tgrp;

	/* inform callees to be careful, we're in a signal handler! */
	_HA_ATOMIC_OR(&th_ctx->flags, TH_FL_IN_WDT_HANDLER);

	switch (si->si_code) {
	case SI_TIMER:
		/* A thread's timer fired, the thread ID is in si_int. We have
		 * no guarantee that the thread handling this signal is in any
		 * way related to the one triggering it, so we need to retrieve
		 * the thread number from there. Note: this thread might
		 * continue to execute in parallel.
		 */
		thr = si->si_value.sival_int;

		/* cannot happen unless an unknown timer tries to play with our
		 * nerves. Let's die for now if this happens.
		 */
		if (thr < 0 || thr >= global.nbthread)
			break;

		tgrp = ha_thread_info[thr].tgid;
		thr_bit = ha_thread_info[thr].ltid_bit;
		p = ha_thread_ctx[thr].prev_cpu_time;
		n = now_cpu_time_thread(thr);

		/* not yet reached the deadline of 1 sec,
		 * or p wasn't initialized yet
		 */
		if (!p)
			goto update_and_leave;

		if ((_HA_ATOMIC_LOAD(&ha_thread_ctx[thr].flags) & TH_FL_SLEEPING) ||
		    (_HA_ATOMIC_LOAD(&ha_tgroup_ctx[tgrp-1].threads_harmless) & thr_bit)) {
			/* This thread is currently doing exactly nothing
			 * waiting in the poll loop (unlikely but possible),
			 * waiting for all other threads to join the rendez-vous
			 * point (common), or waiting for another thread to
			 * finish an isolated operation (unlikely but possible).
			 */
			goto update_and_leave;
		}

		/* So the thread indeed appears locked up. In order to be
		 * certain that we're not witnessing an exceptional spike of
		 * CPU usage due to a configuration issue (like running tens
		 * of thousands of tasks in a single loop), we'll check if the
		 * scheduler is still alive by setting the TH_FL_STUCK flag
		 * that the scheduler clears when switching to the next task.
		 * If it's already set, then it's our second call with no
		 * progress and the thread is dead. However, if we figure
		 * that the scheduler made no progress since last time, we'll
		 * at least emit a warning.
		 */
		if (!(_HA_ATOMIC_LOAD(&ha_thread_ctx[thr].flags) & TH_FL_STUCK)) {
			/* after one second it's clear that we're stuck */
			if (n - p >= 1000000000ULL)
				_HA_ATOMIC_OR(&ha_thread_ctx[thr].flags, TH_FL_STUCK);
			else if (n - p < (ullong)wdt_warn_blocked_traffic_ns) {
				/* if we haven't crossed the warning boundary,
				 * let's just refresh the reporting thread's timer.
				 */
				goto update_and_leave;
			}

			/* OK so we've crossed the warning boundary and possibly the
			 * panic one as well. This may only be reported by the original
			 * thread. Let's fall back to the common code below which will
			 * possibly bounce to the reporting thread, which will then
			 * check the ctxsw count and decide whether to do nothing, to
			 * warn, or either panic.
			 */
		}

		/* No doubt now, there's no hop to recover, die loudly! */
		break;

#if defined(USE_THREAD) && defined(SI_TKILL) /* Linux uses this */

	case SI_TKILL:
		/* we got a pthread_kill, stop on it */
		thr = tid;
		break;

#elif defined(USE_THREAD) && defined(SI_LWP) /* FreeBSD uses this */

	case SI_LWP:
		/* we got a pthread_kill, stop on it */
		thr = tid;
		break;

#endif
	default:
		/* unhandled other conditions */
		_HA_ATOMIC_AND(&th_ctx->flags, ~TH_FL_IN_WDT_HANDLER);
		return;
	}

	/* Right here, we either got a bounce from another thread's WDT to
	 * report a crossed period, or we noticed it for the current thread.
	 * For other threads, we're bouncing.
	 */
#ifdef USE_THREAD
	if (thr != tid) {
		ha_tkill(thr, sig);
		goto leave;
	}
#endif

	/* Now the interesting things begin. The timer was at least as large
	 * as the warning threshold. If the stuck bit was set, we must now
	 * panic. Otherwise we're checking if we're still context-switching
	 * or not and we'll either warn if not, or just update the ctxsw
	 * counter to check next time.
	 */
	if (_HA_ATOMIC_LOAD(&th_ctx->flags) & TH_FL_STUCK)
		ha_panic();

	if (!is_sched_alive())
		ha_stuck_warning();

	/* let's go on */

 update_and_leave:
	wdt_ping(thr);
 leave:
	_HA_ATOMIC_AND(&th_ctx->flags, ~TH_FL_IN_WDT_HANDLER);
}

/* parse the "warn-blocked-traffic-after" parameter */
static int wdt_parse_warn_blocked(char **args, int section_type, struct proxy *curpx,
                                  const struct proxy *defpx, const char *file, int line,
                                  char **err)
{
	const char *res;
	uint value;

	if (!*args[1]) {
		memprintf(err, "'%s' expects <time> as argument between 1 and 1000 ms.\n", args[0]);
		return -1;
	}

	res = parse_time_err(args[1], &value, TIME_UNIT_MS);
	if (res == PARSE_TIME_OVER) {
		memprintf(err, "timer overflow in argument '%s' to '%s' (maximum value is 1000 ms)",
			  args[1], args[0]);
		return -1;
	}
	else if (res == PARSE_TIME_UNDER) {
		memprintf(err, "timer underflow in argument '%s' to '%s' (minimum value is 1 ms)",
			  args[1], args[0]);
		return -1;
	}
	else if (res) {
		memprintf(err, "unexpected character '%c' in argument to <%s>.\n", *res, args[0]);
		return -1;
	}
	else if (value > 1000 || value < 1) {
		memprintf(err, "timer out of range in argument '%s' to '%s' (value must be between 1 and 1000 ms)",
			  args[1], args[0]);
		return -1;
	}

	wdt_warn_blocked_traffic_ns = value * 1000000U;
	return 0;
}

int init_wdt_per_thread()
{
	if (!clock_setup_signal_timer(&per_thread_wd_ctx[tid].timer, WDTSIG, tid))
		goto fail1;

	if (!wdt_ping(tid))
		goto fail2;

	return 1;

 fail2:
	timer_delete(per_thread_wd_ctx[tid].timer);
 fail1:
	per_thread_wd_ctx[tid].timer = TIMER_INVALID;
	ha_warning("Failed to setup watchdog timer for thread %u, disabling lockup detection.\n", tid);
	return 1;
}

void deinit_wdt_per_thread()
{
	if (per_thread_wd_ctx[tid].timer != TIMER_INVALID)
		timer_delete(per_thread_wd_ctx[tid].timer);
}

/* registers the watchdog signal handler and returns 0. This sets up the signal
 * handler for WDTSIG, so it must be called once per process.
 */
int init_wdt()
{
	struct sigaction sa;

	sa.sa_handler = NULL;
	sa.sa_sigaction = wdt_handler;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, WDTSIG);
#ifdef DEBUGSIG
	sigaddset(&sa.sa_mask, DEBUGSIG);
#endif
#if defined(DEBUG_DEV)
	sigaddset(&sa.sa_mask, SIGRTMAX);
#endif
	sa.sa_flags = SA_SIGINFO;
	sigaction(WDTSIG, &sa, NULL);
	return ERR_NONE;
}

static struct cfg_kw_list cfg_kws = {ILH, {
	{ CFG_GLOBAL, "warn-blocked-traffic-after", wdt_parse_warn_blocked },
	{ 0, NULL, NULL },
}};

INITCALL1(STG_REGISTER, cfg_register_keywords, &cfg_kws);
REGISTER_POST_CHECK(init_wdt);
REGISTER_PER_THREAD_INIT(init_wdt_per_thread);
REGISTER_PER_THREAD_DEINIT(deinit_wdt_per_thread);
#endif
