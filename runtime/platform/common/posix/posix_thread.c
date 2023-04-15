/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "platform_api.h"
#include "platform_api_extension.h"

typedef struct {
    thread_start_routine_t start;
    void *arg;
#ifdef OS_ENABLE_HW_BOUND_CHECK
    os_signal_handler signal_handler;
#endif
} thread_wrapper_arg;

#ifdef OS_ENABLE_HW_BOUND_CHECK
/* The signal handler passed to os_thread_signal_init() */
static os_thread_local_attribute os_signal_handler signal_handler;
#endif

korp_tid
os_self_thread()
{
    return (korp_tid)pthread_self();
}

int
os_mutex_init(korp_mutex *mutex)
{
    return pthread_mutex_init(mutex, NULL) == 0 ? BHT_OK : BHT_ERROR;
}

int
os_mutex_destroy(korp_mutex *mutex)
{
    int ret;

    assert(mutex);
    ret = pthread_mutex_destroy(mutex);

    return ret == 0 ? BHT_OK : BHT_ERROR;
}

int
os_mutex_lock(korp_mutex *mutex)
{
    int ret;

    assert(mutex);
    ret = pthread_mutex_lock(mutex);

    return ret == 0 ? BHT_OK : BHT_ERROR;
}

int
os_mutex_unlock(korp_mutex *mutex)
{
    int ret;

    assert(mutex);
    ret = pthread_mutex_unlock(mutex);

    return ret == 0 ? BHT_OK : BHT_ERROR;
}

int
os_cond_init(korp_cond *cond)
{
    assert(cond);

    if (pthread_cond_init(cond, NULL) != BHT_OK)
        return BHT_ERROR;

    return BHT_OK;
}

int
os_cond_destroy(korp_cond *cond)
{
    assert(cond);

    if (pthread_cond_destroy(cond) != BHT_OK)
        return BHT_ERROR;

    return BHT_OK;
}

int
os_cond_wait(korp_cond *cond, korp_mutex *mutex)
{
    assert(cond);
    assert(mutex);

    if (pthread_cond_wait(cond, mutex) != BHT_OK)
        return BHT_ERROR;

    return BHT_OK;
}

static void
msec_nsec_to_abstime(struct timespec *ts, uint64 usec)
{
    struct timeval tv;
    time_t tv_sec_new;
    long int tv_nsec_new;

    gettimeofday(&tv, NULL);

    tv_sec_new = (time_t)(tv.tv_sec + usec / 1000000);
    if (tv_sec_new >= tv.tv_sec) {
        ts->tv_sec = tv_sec_new;
    }
    else {
        /* integer overflow */
        ts->tv_sec = BH_TIME_T_MAX;
        os_printf("Warning: os_cond_reltimedwait exceeds limit, "
                  "set to max timeout instead\n");
    }

    tv_nsec_new = (long int)(tv.tv_usec * 1000 + (usec % 1000000) * 1000);
    if (tv.tv_usec * 1000 >= tv.tv_usec && tv_nsec_new >= tv.tv_usec * 1000) {
        ts->tv_nsec = tv_nsec_new;
    }
    else {
        /* integer overflow */
        ts->tv_nsec = LONG_MAX;
        os_printf("Warning: os_cond_reltimedwait exceeds limit, "
                  "set to max timeout instead\n");
    }

    if (ts->tv_nsec >= 1000000000L && ts->tv_sec < BH_TIME_T_MAX) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

int
os_cond_reltimedwait(korp_cond *cond, korp_mutex *mutex, uint64 useconds)
{
    int ret;
    struct timespec abstime;

    if (useconds == BHT_WAIT_FOREVER)
        ret = pthread_cond_wait(cond, mutex);
    else {
        msec_nsec_to_abstime(&abstime, useconds);
        ret = pthread_cond_timedwait(cond, mutex, &abstime);
    }

    if (ret != BHT_OK && ret != ETIMEDOUT)
        return BHT_ERROR;

    return ret;
}

int
os_cond_signal(korp_cond *cond)
{
    assert(cond);

    if (pthread_cond_signal(cond) != BHT_OK)
        return BHT_ERROR;

    return BHT_OK;
}

int
os_cond_broadcast(korp_cond *cond)
{
    assert(cond);

    if (pthread_cond_broadcast(cond) != BHT_OK)
        return BHT_ERROR;

    return BHT_OK;
}

void
os_thread_exit(void *retval)
{
#ifdef OS_ENABLE_HW_BOUND_CHECK
    os_thread_signal_destroy();
#endif
    return pthread_exit(retval);
}

#if defined(os_thread_local_attribute)
static os_thread_local_attribute uint8 *thread_stack_boundary = NULL;
#endif

#ifdef OS_ENABLE_HW_BOUND_CHECK

#define SIG_ALT_STACK_SIZE (32 * 1024)

/**
 * Whether thread signal enviornment is initialized:
 *   the signal handler is registered, the stack pages are touched,
 *   the stack guard pages are set and signal alternate stack are set.
 */
static os_thread_local_attribute bool thread_signal_inited = false;

#if WASM_DISABLE_STACK_HW_BOUND_CHECK == 0
/* The signal alternate stack base addr */
static os_thread_local_attribute uint8 *sigalt_stack_base_addr;

#if defined(__clang__)
#pragma clang optimize off
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("O0")
__attribute__((no_sanitize_address))
#endif
static uint32
touch_pages(uint8 *stack_min_addr, uint32 page_size)
{
    uint8 sum = 0;
    while (1) {
        volatile uint8 *touch_addr = (volatile uint8 *)os_alloca(page_size / 2);
        if (touch_addr < stack_min_addr + page_size) {
            sum += *(stack_min_addr + page_size - 1);
            break;
        }
        *touch_addr = 0;
        sum += *touch_addr;
    }
    return sum;
}
#if defined(__clang__)
#pragma clang optimize on
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif

static bool
init_stack_guard_pages()
{
    uint32 page_size = os_getpagesize();
    uint32 guard_page_count = STACK_OVERFLOW_CHECK_GUARD_PAGE_COUNT;
    uint8 *stack_min_addr = os_thread_get_stack_boundary();

    if (stack_min_addr == NULL)
        return false;

    /* Touch each stack page to ensure that it has been mapped: the OS
       may lazily grow the stack mapping as a guard page is hit. */
    (void)touch_pages(stack_min_addr, page_size);
    /* First time to call aot function, protect guard pages */
    if (os_mprotect(stack_min_addr, page_size * guard_page_count,
                    MMAP_PROT_NONE)
        != 0) {
        return false;
    }
    return true;
}

static void
destroy_stack_guard_pages()
{
    uint32 page_size = os_getpagesize();
    uint32 guard_page_count = STACK_OVERFLOW_CHECK_GUARD_PAGE_COUNT;
    uint8 *stack_min_addr = os_thread_get_stack_boundary();

    os_mprotect(stack_min_addr, page_size * guard_page_count,
                MMAP_PROT_READ | MMAP_PROT_WRITE);
}
#endif /* end of WASM_DISABLE_STACK_HW_BOUND_CHECK == 0 */

static void
mask_signals(int how)
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGSEGV);
    sigaddset(&set, SIGBUS);
    pthread_sigmask(how, &set, NULL);
}

static os_thread_local_attribute struct sigaction prev_sig_act_SIGSEGV;
static os_thread_local_attribute struct sigaction prev_sig_act_SIGBUS;

static void
signal_callback(int sig_num, siginfo_t *sig_info, void *sig_ucontext)
{
    void *sig_addr = sig_info->si_addr;
    struct sigaction *prev_sig_act = NULL;

    mask_signals(SIG_BLOCK);

    /* Try to handle signal with the registered signal handler */
    if (signal_handler && (sig_num == SIGSEGV || sig_num == SIGBUS)) {
        signal_handler(sig_addr);
    }

    if (sig_num == SIGSEGV)
        prev_sig_act = &prev_sig_act_SIGSEGV;
    else if (sig_num == SIGBUS)
        prev_sig_act = &prev_sig_act_SIGBUS;

    /* Forward the signal to next handler if found */
    if (prev_sig_act && (prev_sig_act->sa_flags & SA_SIGINFO)) {
        prev_sig_act->sa_sigaction(sig_num, sig_info, sig_ucontext);
    }
    else if (prev_sig_act
             && ((void *)prev_sig_act->sa_sigaction == SIG_DFL
                 || (void *)prev_sig_act->sa_sigaction == SIG_IGN)) {
        sigaction(sig_num, prev_sig_act, NULL);
    }
    /* Output signal info and then crash if signal is unhandled */
    else {
        switch (sig_num) {
            case SIGSEGV:
                os_printf("unhandled SIGSEGV, si_addr: %p\n", sig_addr);
                break;
            case SIGBUS:
                os_printf("unhandled SIGBUS, si_addr: %p\n", sig_addr);
                break;
            default:
                os_printf("unhandle signal %d, si_addr: %p\n", sig_num,
                          sig_addr);
                break;
        }

        abort();
    }
}

int
os_thread_signal_init(os_signal_handler handler)
{
    struct sigaction sig_act;
#if WASM_DISABLE_STACK_HW_BOUND_CHECK == 0
    stack_t sigalt_stack_info;
    uint32 map_size = SIG_ALT_STACK_SIZE;
    uint8 *map_addr;
#endif

    if (thread_signal_inited)
        return 0;

#if WASM_DISABLE_STACK_HW_BOUND_CHECK == 0
    if (!init_stack_guard_pages()) {
        os_printf("Failed to init stack guard pages\n");
        return -1;
    }

    /* Initialize memory for signal alternate stack of current thread */
    if (!(map_addr = os_mmap(NULL, map_size, MMAP_PROT_READ | MMAP_PROT_WRITE,
                             MMAP_MAP_NONE))) {
        os_printf("Failed to mmap memory for alternate stack\n");
        goto fail1;
    }

    /* Initialize signal alternate stack */
    memset(map_addr, 0, map_size);
    sigalt_stack_info.ss_sp = map_addr;
    sigalt_stack_info.ss_size = map_size;
    sigalt_stack_info.ss_flags = 0;
    if (sigaltstack(&sigalt_stack_info, NULL) != 0) {
        os_printf("Failed to init signal alternate stack\n");
        goto fail2;
    }
#endif

    memset(&prev_sig_act_SIGSEGV, 0, sizeof(struct sigaction));
    memset(&prev_sig_act_SIGBUS, 0, sizeof(struct sigaction));

    /* Install signal hanlder */
    sig_act.sa_sigaction = signal_callback;
    sig_act.sa_flags = SA_SIGINFO | SA_NODEFER;
#if WASM_DISABLE_STACK_HW_BOUND_CHECK == 0
    sig_act.sa_flags |= SA_ONSTACK;
#endif
    sigemptyset(&sig_act.sa_mask);
    if (sigaction(SIGSEGV, &sig_act, &prev_sig_act_SIGSEGV) != 0
        || sigaction(SIGBUS, &sig_act, &prev_sig_act_SIGBUS) != 0) {
        os_printf("Failed to register signal handler\n");
        goto fail3;
    }

#if WASM_DISABLE_STACK_HW_BOUND_CHECK == 0
    sigalt_stack_base_addr = map_addr;
#endif
    signal_handler = handler;
    thread_signal_inited = true;
    return 0;

fail3:
#if WASM_DISABLE_STACK_HW_BOUND_CHECK == 0
    memset(&sigalt_stack_info, 0, sizeof(stack_t));
    sigalt_stack_info.ss_flags = SS_DISABLE;
    sigalt_stack_info.ss_size = map_size;
    sigaltstack(&sigalt_stack_info, NULL);
fail2:
    os_munmap(map_addr, map_size);
fail1:
    destroy_stack_guard_pages();
#endif
    return -1;
}

void
os_thread_signal_destroy()
{
#if WASM_DISABLE_STACK_HW_BOUND_CHECK == 0
    stack_t sigalt_stack_info;
#endif

    if (!thread_signal_inited)
        return;

#if WASM_DISABLE_STACK_HW_BOUND_CHECK == 0
    /* Disable signal alternate stack */
    memset(&sigalt_stack_info, 0, sizeof(stack_t));
    sigalt_stack_info.ss_flags = SS_DISABLE;
    sigalt_stack_info.ss_size = SIG_ALT_STACK_SIZE;
    sigaltstack(&sigalt_stack_info, NULL);

    os_munmap(sigalt_stack_base_addr, SIG_ALT_STACK_SIZE);

    destroy_stack_guard_pages();
#endif

    thread_signal_inited = false;
}

bool
os_thread_signal_inited()
{
    return thread_signal_inited;
}

void
os_signal_unmask()
{
    mask_signals(SIG_UNBLOCK);
}

void
os_sigreturn()
{
#if WASM_DISABLE_STACK_HW_BOUND_CHECK == 0
#if defined(__APPLE__)
#define UC_RESET_ALT_STACK 0x80000000
    extern int __sigreturn(void *, int);

    /* It's necessary to call __sigreturn to restore the sigaltstack state
       after exiting the signal handler. */
    __sigreturn(NULL, UC_RESET_ALT_STACK);
#endif
#endif
}
#endif /* end of OS_ENABLE_HW_BOUND_CHECK */
