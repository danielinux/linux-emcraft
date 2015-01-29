/*********************************************************************
PicoTCP. Copyright (c) 2013-2015 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.
Do not redistribute without a written permission by the Copyright
holders.

Author: Daniele Lacamera, Maxime Vincent
*********************************************************************/

#include "pico_device.h"
#include "pico_config.h"    /* for zalloc and free */
#include "pico_stack.h"
#include "pico_dev_loop.h"
#include <picotcp.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/jiffies.h>

volatile int pico_stack_is_ready;
static struct workqueue_struct *picotcp_workqueue;
static struct delayed_work picotcp_delayed_work;
static struct work_struct picotcp_work;
wait_queue_head_t picotcp_stack_init_wait;
/* pico stack lock */
void * picoLock = NULL;

extern int sysctl_picotcp_tick_count;
static void picotcp_schedule_work(void);

extern int ip_route_proc_init(void);

extern int sysctl_picotcp_dutycycle;

static void picotcp_tick(struct work_struct *unused)
{
    (void)unused;
    if (pico_stack_is_ready) {
        pico_bsd_stack_tick();
        sysctl_picotcp_tick_count++;
    }
    picotcp_schedule_work();
}

#ifdef CONFIG_PICOTCP_DEVLOOP
static int picotcp_loopback_init(void)
{
    struct pico_device *lo;
    struct pico_ip4 ipaddr, netmask;

    lo = pico_loop_create();
    if (!lo)
      return -ENOMEM;
    pico_string_to_ipv4("127.0.0.1", &ipaddr.addr);
    pico_string_to_ipv4("255.0.0.0", &netmask.addr);
    pico_ipv4_link_add(lo, ipaddr, netmask);

    return 0;
}
#else
static int picotcp_loopback_init(void)
{
    return 0;
}
#endif


/* Public functions */
void pico_bsd_init(void)
{
    picoLock = pico_mutex_init();
}

void pico_bsd_deinit(void)
{
    pico_mutex_deinit(picoLock);
}

void pico_bsd_stack_tick(void)
{
    pico_mutex_lock(picoLock);
    pico_stack_tick();
    pico_mutex_unlock(picoLock);
}

#ifdef CONFIG_HIGH_RES_TIMERS
#include "linux/hrtimer.h"
static struct hrtimer pico_tick_hrtimer;

static enum hrtimer_restart picotcp_tick_timeout(struct hrtimer *t)
{
    queue_work(picotcp_workqueue, &picotcp_work);
    return HRTIMER_NORESTART;
}

static void picotcp_init_workqueues(void)
{
    INIT_WORK(&picotcp_work, picotcp_tick);
    hrtimer_init(&pico_tick_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    pico_tick_hrtimer.function = picotcp_tick_timeout;
    printk("PicoTCP created [using high-speed timer].\n");
}

static void picotcp_schedule_work(void)
{
    ktime_t interval;
    interval = ktime_set(0, sysctl_picotcp_dutycycle * 1000000); /* ns to ms */
    hrtimer_start(&pico_tick_hrtimer, interval, HRTIMER_MODE_REL);
}

#else /* NO CONFIG_HIGH_RES_TIMERS */
static void picotcp_init_workqueues(void)
{
    INIT_DELAYED_WORK(&picotcp_delayed_work, picotcp_tick);
    printk("PicoTCP created [using delayed workqueue].\n");
}

static void picotcp_schedule_work(void)
{
    queue_delayed_work(picotcp_workqueue, &picotcp_delayed_work, sysctl_picotcp_dutycycle);
}
#endif

/* Stack Init Function */
int __init picotcp_init(void)
{

    init_waitqueue_head(&picotcp_stack_init_wait);
    if (pico_stack_init() < 0)
        panic("Unable to start picoTCP\n");
    pico_bsd_init();
    picotcp_workqueue = create_singlethread_workqueue("picotcp_tick");
    picotcp_init_workqueues();
    pico_stack_is_ready++;
    wake_up_interruptible_all(&picotcp_stack_init_wait);

    af_inet_picotcp_init();
    if (ip_route_proc_init() < 0)
      printk("Failed initializing /proc/net/route\n");

    if (picotcp_loopback_init() < 0)
      printk("picoTCP: failed to initialize loopback device.\n");
    
    picotcp_schedule_work();
    return 0;
}
fs_initcall(picotcp_init);
