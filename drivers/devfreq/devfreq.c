/*
 * devfreq: Generic Dynamic Voltage and Frequency Scaling (DVFS) Framework
 *	    for Non-CPU Devices.
 *
 * Copyright (C) 2011 Samsung Electronics
 *	MyungJoo Ham <myungjoo.ham@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/pm_opp.h>
#include <linux/devfreq.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/hrtimer.h>
#include <linux/of.h>
#include "governor.h"

static struct class *devfreq_class;

/*
 * devfreq core provides delayed work based load monitoring helper
 * functions. Governors can use these or can implement their own
 * monitoring mechanism.
 */
static struct workqueue_struct *devfreq_wq;

/* The list of all device-devfreq governors */
static LIST_HEAD(devfreq_governor_list);
/* The list of all device-devfreq */
static LIST_HEAD(devfreq_list);
static DEFINE_MUTEX(devfreq_list_lock);

/**
 * find_device_devfreq() - find devfreq struct using device pointer
 * @dev:	device pointer used to lookup device devfreq.
 *
 * Search the list of device devfreqs and return the matched device's
 * devfreq info. devfreq_list_lock should be held by the caller.
 */
static struct devfreq *find_device_devfreq(struct device *dev)
{
	struct devfreq *tmp_devfreq;

	if (IS_ERR_OR_NULL(dev)) {
		pr_err("DEVFREQ: %s: Invalid parameters\n", __func__);
		return ERR_PTR(-EINVAL);
	}
	WARN(!mutex_is_locked(&devfreq_list_lock),
	     "devfreq_list_lock must be locked.");

	list_for_each_entry(tmp_devfreq, &devfreq_list, node) {
		if (tmp_devfreq->dev.parent == dev)
			return tmp_devfreq;
	}

	return ERR_PTR(-ENODEV);
}

/**
 * devfreq_set_freq_limits() - Set min and max frequency from freq_table
 * @devfreq:	the devfreq instance
 */
static void devfreq_set_freq_limits(struct devfreq *devfreq)
{
	int idx;
	unsigned long min = ~0, max = 0;

	if (!devfreq->profile->freq_table)
		return;

	for (idx = 0; idx < devfreq->profile->max_state; idx++) {
		if (min > devfreq->profile->freq_table[idx])
			min = devfreq->profile->freq_table[idx];
		if (max < devfreq->profile->freq_table[idx])
			max = devfreq->profile->freq_table[idx];
	}

	devfreq->min_freq = min;
	devfreq->max_freq = max;
}

/**
 * devfreq_get_freq_level() - Lookup freq_table for the frequency
 * @devfreq:	the devfreq instance
 * @freq:	the target frequency
 */
static int devfreq_get_freq_level(struct devfreq *devfreq, unsigned long freq)
{
	int lev;

	for (lev = 0; lev < devfreq->profile->max_state; lev++)
		if (freq == devfreq->profile->freq_table[lev])
			return lev;

	return -EINVAL;
}

/**
 * devfreq_set_freq_table() - Initialize freq_table for the frequency
 * @devfreq:	the devfreq instance
 */
static void devfreq_set_freq_table(struct devfreq *devfreq)
{
	struct devfreq_dev_profile *profile = devfreq->profile;
	struct dev_pm_opp *opp;
	unsigned long freq;
	int i, count;

	/* Initialize the freq_table from OPP table */
	count = dev_pm_opp_get_opp_count(devfreq->dev.parent);
	if (count <= 0)
		return;

	profile->max_state = count;
	profile->freq_table = devm_kcalloc(devfreq->dev.parent,
					profile->max_state,
					sizeof(*profile->freq_table),
					GFP_KERNEL);
	if (!profile->freq_table) {
		profile->max_state = 0;
		return;
	}

	rcu_read_lock();
	for (i = 0, freq = 0; i < profile->max_state; i++, freq++) {
		opp = dev_pm_opp_find_freq_ceil(devfreq->dev.parent, &freq);
		if (IS_ERR(opp)) {
			devm_kfree(devfreq->dev.parent, profile->freq_table);
			profile->max_state = 0;
			rcu_read_unlock();
			return;
		}
		profile->freq_table[i] = freq;
	}
	rcu_read_unlock();
}

/**
 * devfreq_update_status() - Update statistics of devfreq behavior
 * @devfreq:	the devfreq instance
 * @freq:	the update target frequency
 */
int devfreq_update_status(struct devfreq *devfreq, unsigned long freq)
{
	int lev, prev_lev, ret = 0;
	unsigned long cur_time;

	lockdep_assert_held(&devfreq->lock);
	cur_time = jiffies;

	/* Immediately exit if previous_freq is not initialized yet. */
	if (!devfreq->previous_freq)
		goto out;

	prev_lev = devfreq_get_freq_level(devfreq, devfreq->previous_freq);
	if (prev_lev < 0) {
		ret = prev_lev;
		goto out;
	}

	devfreq->time_in_state[prev_lev] +=
			 cur_time - devfreq->last_stat_updated;

	lev = devfreq_get_freq_level(devfreq, freq);
	if (lev < 0) {
		ret = lev;
		goto out;
	}

	if (lev != prev_lev) {
		devfreq->trans_table[(prev_lev *
				devfreq->profile->max_state) + lev]++;
		devfreq->total_trans++;
	}

out:
	devfreq->last_stat_updated = cur_time;
	return ret;
}
EXPORT_SYMBOL(devfreq_update_status);

/**
 * find_devfreq_governor() - find devfreq governor from name
 * @name:	name of the governor
 *
 * Search the list of devfreq governors and return the matched
 * governor's pointer. devfreq_list_lock should be held by the caller.
 */
static struct devfreq_governor *find_devfreq_governor(const char *name)
{
	struct devfreq_governor *tmp_governor;

	if (IS_ERR_OR_NULL(name)) {
		pr_err("DEVFREQ: %s: Invalid parameters\n", __func__);
		return ERR_PTR(-EINVAL);
	}
	WARN(!mutex_is_locked(&devfreq_list_lock),
	     "devfreq_list_lock must be locked.");

	list_for_each_entry(tmp_governor, &devfreq_governor_list, node) {
		if (!strncmp(tmp_governor->name, name, DEVFREQ_NAME_LEN))
			return tmp_governor;
	}

	return ERR_PTR(-ENODEV);
}

static int devfreq_notify_transition(struct devfreq *devfreq,
		struct devfreq_freqs *freqs, unsigned int state)
{
	if (!devfreq)
		return -EINVAL;

	switch (state) {
	case DEVFREQ_PRECHANGE:
		srcu_notifier_call_chain(&devfreq->transition_notifier_list,
				DEVFREQ_PRECHANGE, freqs);
		break;

	case DEVFREQ_POSTCHANGE:
		srcu_notifier_call_chain(&devfreq->transition_notifier_list,
				DEVFREQ_POSTCHANGE, freqs);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* Load monitoring helper functions for governors use */

/**
 * update_devfreq() - Reevaluate the device and configure frequency.
 * @devfreq:	the devfreq instance.
 *
 * Note: Lock devfreq->lock before calling update_devfreq
 *	 This function is exported for governors.
 */
int update_devfreq(struct devfreq *devfreq)
{
	struct devfreq_freqs freqs;
	unsigned long freq, cur_freq;
	int err = 0;
	u32 flags = 0;

	if (!mutex_is_locked(&devfreq->lock)) {
		WARN(true, "devfreq->lock must be locked by the caller.\n");
		return -EINVAL;
	}

	if (!devfreq->governor)
		return -EINVAL;

	if (devfreq->max_boost) {
		/* Use the max freq for max boosts */
		freq = ULONG_MAX;
	} else {
		/* Reevaluate the proper frequency */
		err = devfreq->governor->get_target_freq(devfreq, &freq);
		if (err)
			return err;
	}

	/*
	 * Adjust the frequency with user freq and QoS.
	 *
	 * List from the highest priority
	 * max_freq
	 * min_freq
	 */

	if (devfreq->min_freq && freq < devfreq->min_freq) {
		freq = devfreq->min_freq;
		flags &= ~DEVFREQ_FLAG_LEAST_UPPER_BOUND; /* Use GLB */
	}
	if (devfreq->max_freq && freq > devfreq->max_freq) {
		freq = devfreq->max_freq;
		flags |= DEVFREQ_FLAG_LEAST_UPPER_BOUND; /* Use LUB */
	}

	if (devfreq->profile->get_cur_freq)
		devfreq->profile->get_cur_freq(devfreq->dev.parent, &cur_freq);
	else
		cur_freq = devfreq->previous_freq;

	freqs.old = cur_freq;
	freqs.new = freq;
	devfreq_notify_transition(devfreq, &freqs, DEVFREQ_PRECHANGE);

	err = devfreq->profile->target(devfreq->dev.parent, &freq, flags);
	if (err) {
		freqs.new = cur_freq;
		devfreq_notify_transition(devfreq, &freqs, DEVFREQ_POSTCHANGE);
		return err;
	}

	freqs.new = freq;
	devfreq_notify_transition(devfreq, &freqs, DEVFREQ_POSTCHANGE);

	if (devfreq->profile->freq_table)
		if (devfreq_update_status(devfreq, freq))
			dev_err(&devfreq->dev,
				"Couldn't update frequency transition information.\n");

	devfreq->previous_freq = freq;
	return err;
}
EXPORT_SYMBOL(update_devfreq);

/**
 * devfreq_monitor() - Periodically poll devfreq objects.
 * @work:	the work struct used to run devfreq_monitor periodically.
 *
 */
static void devfreq_monitor(struct work_struct *work)
{
	int err;
	struct devfreq *devfreq = container_of(work,
					struct devfreq, work.work);

	mutex_lock(&devfreq->lock);
	err = update_devfreq(devfreq);
	if (err)
		dev_err(&devfreq->dev, "dvfs failed with (%d) error\n", err);

	queue_delayed_work(devfreq_wq, &devfreq->work,
				msecs_to_jiffies(devfreq->profile->polling_ms));
	mutex_unlock(&devfreq->lock);
}

/**
 * devfreq_monitor_start() - Start load monitoring of devfreq instance
 * @devfreq:	the devfreq instance.
 *
 * Helper function for starting devfreq device load monitoing. By
 * default delayed work based monitoring is supported. Function
 * to be called from governor in response to DEVFREQ_GOV_START
 * event when device is added to devfreq framework.
 */
void devfreq_monitor_start(struct devfreq *devfreq)
{
	INIT_DEFERRABLE_WORK(&devfreq->work, devfreq_monitor);
	if (devfreq->profile->polling_ms)
		queue_delayed_work(devfreq_wq, &devfreq->work,
			msecs_to_jiffies(devfreq->profile->polling_ms));
}
EXPORT_SYMBOL(devfreq_monitor_start);

/**
 * devfreq_monitor_stop() - Stop load monitoring of a devfreq instance
 * @devfreq:	the devfreq instance.
 *
 * Helper function to stop devfreq device load monitoing. Function
 * to be called from governor in response to DEVFREQ_GOV_STOP
 * event when device is removed from devfreq framework.
 */
void devfreq_monitor_stop(struct devfreq *devfreq)
{
	cancel_delayed_work_sync(&devfreq->work);
}
EXPORT_SYMBOL(devfreq_monitor_stop);

/**
 * devfreq_monitor_suspend() - Suspend load monitoring of a devfreq instance
 * @devfreq:	the devfreq instance.
 *
 * Helper function to suspend devfreq device load monitoing. Function
 * to be called from governor in response to DEVFREQ_GOV_SUSPEND
 * event or when polling interval is set to zero.
 *
 * Note: Though this function is same as devfreq_monitor_stop(),
 * intentionally kept separate to provide hooks for collecting
 * transition statistics.
 */
void devfreq_monitor_suspend(struct devfreq *devfreq)
{
	mutex_lock(&devfreq->lock);
	if (devfreq->stop_polling) {
		mutex_unlock(&devfreq->lock);
		return;
	}

	devfreq_update_status(devfreq, devfreq->previous_freq);
	devfreq->stop_polling = true;
	mutex_unlock(&devfreq->lock);
	cancel_delayed_work_sync(&devfreq->work);
}
EXPORT_SYMBOL(devfreq_monitor_suspend);

/**
 * devfreq_monitor_resume() - Resume load monitoring of a devfreq instance
 * @devfreq:    the devfreq instance.
 *
 * Helper function to resume devfreq device load monitoing. Function
 * to be called from governor in response to DEVFREQ_GOV_RESUME
 * event or when polling interval is set to non-zero.
 */
void devfreq_monitor_resume(struct devfreq *devfreq)
{
	unsigned long freq;

	mutex_lock(&devfreq->lock);
	if (!devfreq->stop_polling)
		goto out;

	if (!delayed_work_pending(&devfreq->work) &&
			devfreq->profile->polling_ms)
		queue_delayed_work(devfreq_wq, &devfreq->work,
			msecs_to_jiffies(devfreq->profile->polling_ms));

	devfreq->last_stat_updated = jiffies;
	devfreq->stop_polling = false;

	if (devfreq->profile->get_cur_freq &&
		!devfreq->profile->get_cur_freq(devfreq->dev.parent, &freq))
		devfreq->previous_freq = freq;

out:
	mutex_unlock(&devfreq->lock);
}
EXPORT_SYMBOL(devfreq_monitor_resume);

/**
 * devfreq_interval_update() - Update device devfreq monitoring interval
 * @devfreq:    the devfreq instance.
 * @delay:      new polling interval to be set.
 *
 * Helper function to set new load monitoring polling interval. Function
 * to be called from governor in response to DEVFREQ_GOV_INTERVAL event.
 */
void devfreq_interval_update(struct devfreq *devfreq, unsigned int *delay)
{
	unsigned int cur_delay = devfreq->profile->polling_ms;
	unsigned int new_delay = *delay;

	mutex_lock(&devfreq->lock);
	devfreq->profile->polling_ms = new_delay;

	if (devfreq->stop_polling)
		goto out;

	/* if new delay is zero, stop polling */
	if (!new_delay) {
		mutex_unlock(&devfreq->lock);
		cancel_delayed_work_sync(&devfreq->work);
		return;
	}

	/* if current delay is zero, start polling with new delay */
	if (!cur_delay) {
		queue_delayed_work(devfreq_wq, &devfreq->work,
			msecs_to_jiffies(devfreq->profile->polling_ms));
		goto out;
	}

	/* if current delay is greater than new delay, restart polling */
	if (cur_delay > new_delay) {
		mutex_unlock(&devfreq->lock);
		cancel_delayed_work_sync(&devfreq->work);
		mutex_lock(&devfreq->lock);
		if (!devfreq->stop_polling)
			queue_delayed_work(devfreq_wq, &devfreq->work,
			      msecs_to_jiffies(devfreq->profile->polling_ms));
	}
out:
	mutex_unlock(&devfreq->lock);
}
EXPORT_SYMBOL(devfreq_interval_update);

/**
 * devfreq_notifier_call() - Notify that the device frequency requirements
 *			   has been changed out of devfreq framework.
 * @nb:		the notifier_block (supposed to be devfreq->nb)
 * @type:	not used
 * @devp:	not used
 *
 * Called by a notifier that uses devfreq->nb.
 */
static int devfreq_notifier_call(struct notifier_block *nb, unsigned long type,
				 void *devp)
{
	struct devfreq *devfreq = container_of(nb, struct devfreq, nb);
	int ret;

	mutex_lock(&devfreq->lock);
	ret = update_devfreq(devfreq);
	mutex_unlock(&devfreq->lock);

	return ret;
}

/**
 * _remove_devfreq() - Remove devfreq from the list and release its resources.
 * @devfreq:	the devfreq struct
 */
static void _remove_devfreq(struct devfreq *devfreq)
{
	mutex_lock(&devfreq_list_lock);
	list_del(&devfreq->node);
	mutex_unlock(&devfreq_list_lock);

	if (devfreq->governor)
		devfreq->governor->event_handler(devfreq,
						 DEVFREQ_GOV_STOP, NULL);

	if (devfreq->profile->exit)
		devfreq->profile->exit(devfreq->dev.parent);

	mutex_destroy(&devfreq->lock);
	mutex_destroy(&devfreq->event_lock);
	kfree(devfreq);
}

/**
 * devfreq_dev_release() - Callback for struct device to release the device.
 * @dev:	the devfreq device
 *
 * This calls _remove_devfreq() if _remove_devfreq() is not called.
 */
static void devfreq_dev_release(struct device *dev)
{
	struct devfreq *devfreq = to_devfreq(dev);

	_remove_devfreq(devfreq);
}

/**
 * devfreq_add_device() - Add devfreq feature to the device
 * @dev:	the device to add devfreq feature.
 * @profile:	device-specific profile to run devfreq.
 * @governor_name:	name of the policy to choose frequency.
 * @data:	private data for the governor. The devfreq framework does not
 *		touch this value.
 */
struct devfreq *devfreq_add_device(struct device *dev,
				   struct devfreq_dev_profile *profile,
				   const char *governor_name,
				   void *data)
{
	struct devfreq *devfreq;
	struct devfreq_governor *governor;
	int err = 0;

	if (!dev || !profile || !governor_name) {
		dev_err(dev, "%s: Invalid parameters.\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	mutex_lock(&devfreq_list_lock);
	devfreq = find_device_devfreq(dev);
	mutex_unlock(&devfreq_list_lock);
	if (!IS_ERR(devfreq)) {
		dev_err(dev, "%s: Unable to create devfreq for the device. It already has one.\n", __func__);
		err = -EINVAL;
		goto err_out;
	}

	devfreq = kzalloc(sizeof(struct devfreq), GFP_KERNEL);
	if (!devfreq) {
		dev_err(dev, "%s: Unable to create devfreq for the device\n",
			__func__);
		err = -ENOMEM;
		goto err_out;
	}

	mutex_init(&devfreq->lock);
	mutex_init(&devfreq->event_lock);
	mutex_lock(&devfreq->lock);
	devfreq->dev.parent = dev;
	devfreq->dev.class = devfreq_class;
	devfreq->dev.release = devfreq_dev_release;
	INIT_LIST_HEAD(&devfreq->node);
	devfreq->profile = profile;
	strncpy(devfreq->governor_name, governor_name, DEVFREQ_NAME_LEN);
	devfreq->previous_freq = profile->initial_freq;
	devfreq->last_status.current_frequency = profile->initial_freq;
	devfreq->data = data;
	devfreq->nb.notifier_call = devfreq_notifier_call;

	if (!devfreq->profile->max_state && !devfreq->profile->freq_table) {
		mutex_unlock(&devfreq->lock);
		devfreq_set_freq_table(devfreq);
		mutex_lock(&devfreq->lock);
	}
	devfreq_set_freq_limits(devfreq);

	dev_set_name(&devfreq->dev, "%s", dev_name(dev));
	err = device_register(&devfreq->dev);
	if (err) {
		mutex_unlock(&devfreq->lock);
		goto err_dev;
	}

	devfreq->trans_table =	devm_kzalloc(&devfreq->dev, sizeof(unsigned int) *
						devfreq->profile->max_state *
						devfreq->profile->max_state,
						GFP_KERNEL);
	devfreq->time_in_state = devm_kzalloc(&devfreq->dev, sizeof(unsigned long) *
						devfreq->profile->max_state,
						GFP_KERNEL);
	devfreq->last_stat_updated = jiffies;

	srcu_init_notifier_head(&devfreq->transition_notifier_list);

	mutex_unlock(&devfreq->lock);

	mutex_lock(&devfreq_list_lock);
	list_add(&devfreq->node, &devfreq_list);

	governor = find_devfreq_governor(devfreq->governor_name);
	if (IS_ERR(governor)) {
		dev_err(dev, "%s: Unable to find governor for the device\n",
			__func__);
		err = PTR_ERR(governor);
		goto err_init;
	}

	devfreq->governor = governor;
	err = devfreq->governor->event_handler(devfreq, DEVFREQ_GOV_START,
						NULL);
	if (err) {
		dev_err(dev, "%s: Unable to start governor for the device\n",
			__func__);
		goto err_init;
	}
	mutex_unlock(&devfreq_list_lock);

	return devfreq;

err_init:
	list_del(&devfreq->node);
	mutex_unlock(&devfreq_list_lock);

	device_unregister(&devfreq->dev);
err_dev:
	if (devfreq)
		kfree(devfreq);
err_out:
	return ERR_PTR(err);
}
EXPORT_SYMBOL(devfreq_add_device);

/**
 * devfreq_remove_device() - Remove devfreq feature from a device.
 * @devfreq:	the devfreq instance to be removed
 *
 * The opposite of devfreq_add_device().
 */
int devfreq_remove_device(struct devfreq *devfreq)
{
	if (!devfreq)
		return -EINVAL;

	device_unregister(&devfreq->dev);

	return 0;
}
EXPORT_SYMBOL(devfreq_remove_device);

static int devm_devfreq_dev_match(struct device *dev, void *res, void *data)
{
	struct devfreq **r = res;

	if (WARN_ON(!r || !*r))
		return 0;

	return *r == data;
}

static void devm_devfreq_dev_release(struct device *dev, void *res)
{
	devfreq_remove_device(*(struct devfreq **)res);
}

/**
 * devm_devfreq_add_device() - Resource-managed devfreq_add_device()
 * @dev:	the device to add devfreq feature.
 * @profile:	device-specific profile to run devfreq.
 * @governor_name:	name of the policy to choose frequency.
 * @data:	private data for the governor. The devfreq framework does not
 *		touch this value.
 *
 * This function manages automatically the memory of devfreq device using device
 * resource management and simplify the free operation for memory of devfreq
 * device.
 */
struct devfreq *devm_devfreq_add_device(struct device *dev,
					struct devfreq_dev_profile *profile,
					const char *governor_name,
					void *data)
{
	struct devfreq **ptr, *devfreq;

	ptr = devres_alloc(devm_devfreq_dev_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	devfreq = devfreq_add_device(dev, profile, governor_name, data);
	if (IS_ERR(devfreq)) {
		devres_free(ptr);
		return devfreq;
	}

	*ptr = devfreq;
	devres_add(dev, ptr);

	return devfreq;
}
EXPORT_SYMBOL(devm_devfreq_add_device);

#ifdef CONFIG_OF
/*
 * devfreq_get_devfreq_by_phandle - Get the devfreq device from devicetree
 * @dev - instance to the given device
 * @index - index into list of devfreq
 *
 * return the instance of devfreq device
 */
struct devfreq *devfreq_get_devfreq_by_phandle(struct device *dev, int index)
{
	struct device_node *node;
	struct devfreq *devfreq;

	if (!dev)
		return ERR_PTR(-EINVAL);

	if (!dev->of_node)
		return ERR_PTR(-EINVAL);

	node = of_parse_phandle(dev->of_node, "devfreq", index);
	if (!node)
		return ERR_PTR(-ENODEV);

	mutex_lock(&devfreq_list_lock);
	list_for_each_entry(devfreq, &devfreq_list, node) {
		if (devfreq->dev.parent
			&& devfreq->dev.parent->of_node == node) {
			mutex_unlock(&devfreq_list_lock);
			of_node_put(node);
			return devfreq;
		}
	}
	mutex_unlock(&devfreq_list_lock);
	of_node_put(node);

	return ERR_PTR(-EPROBE_DEFER);
}
#else
struct devfreq *devfreq_get_devfreq_by_phandle(struct device *dev, int index)
{
	return ERR_PTR(-ENODEV);
}
#endif /* CONFIG_OF */
EXPORT_SYMBOL_GPL(devfreq_get_devfreq_by_phandle);

/**
 * devm_devfreq_remove_device() - Resource-managed devfreq_remove_device()
 * @dev:	the device to add devfreq feature.
 * @devfreq:	the devfreq instance to be removed
 */
void devm_devfreq_remove_device(struct device *dev, struct devfreq *devfreq)
{
	WARN_ON(devres_release(dev, devm_devfreq_dev_release,
			       devm_devfreq_dev_match, devfreq));
}
EXPORT_SYMBOL(devm_devfreq_remove_device);

/**
 * devfreq_suspend_device() - Suspend devfreq of a device.
 * @devfreq: the devfreq instance to be suspended
 *
 * This function is intended to be called by the pm callbacks
 * (e.g., runtime_suspend, suspend) of the device driver that
 * holds the devfreq.
 */
int devfreq_suspend_device(struct devfreq *devfreq)
{
	int ret;

	if (!devfreq)
		return -EINVAL;

	if (!devfreq->governor)
		return 0;

	mutex_lock(&devfreq->event_lock);
	ret = devfreq->governor->event_handler(devfreq,
				DEVFREQ_GOV_SUSPEND, NULL);
	mutex_unlock(&devfreq->event_lock);
	return ret;
}
EXPORT_SYMBOL(devfreq_suspend_device);

/**
 * devfreq_resume_device() - Resume devfreq of a device.
 * @devfreq: the devfreq instance to be resumed
 *
 * This function is intended to be called by the pm callbacks
 * (e.g., runtime_resume, resume) of the device driver that
 * holds the devfreq.
 */
int devfreq_resume_device(struct devfreq *devfreq)
{
	int ret;
	if (!devfreq)
		return -EINVAL;

	if (!devfreq->governor)
		return 0;

	mutex_lock(&devfreq->event_lock);
	ret = devfreq->governor->event_handler(devfreq,
				DEVFREQ_GOV_RESUME, NULL);
	mutex_unlock(&devfreq->event_lock);
	return ret;
}
EXPORT_SYMBOL(devfreq_resume_device);

/**
 * devfreq_add_governor() - Add devfreq governor
 * @governor:	the devfreq governor to be added
 */
int devfreq_add_governor(struct devfreq_governor *governor)
{
	struct devfreq_governor *g;
	struct devfreq *devfreq;
	int err = 0;

	if (!governor) {
		pr_err("%s: Invalid parameters.\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&devfreq_list_lock);
	g = find_devfreq_governor(governor->name);
	if (!IS_ERR(g)) {
		pr_err("%s: governor %s already registered\n", __func__,
		       g->name);
		err = -EINVAL;
		goto err_out;
	}

	list_add(&governor->node, &devfreq_governor_list);

	list_for_each_entry(devfreq, &devfreq_list, node) {
		int ret = 0;
		struct device *dev = devfreq->dev.parent;

		if (!strncmp(devfreq->governor_name, governor->name,
			     DEVFREQ_NAME_LEN)) {
			/* The following should never occur */
			if (devfreq->governor) {
				dev_warn(dev,
					 "%s: Governor %s already present\n",
					 __func__, devfreq->governor->name);
				ret = devfreq->governor->event_handler(devfreq,
							DEVFREQ_GOV_STOP, NULL);
				if (ret) {
					dev_warn(dev,
						 "%s: Governor %s stop = %d\n",
						 __func__,
						 devfreq->governor->name, ret);
				}
				/* Fall through */
			}
			devfreq->governor = governor;
			ret = devfreq->governor->event_handler(devfreq,
						DEVFREQ_GOV_START, NULL);
			if (ret) {
				dev_warn(dev, "%s: Governor %s start=%d\n",
					 __func__, devfreq->governor->name,
					 ret);
			}
		}
	}

err_out:
	mutex_unlock(&devfreq_list_lock);

	return err;
}
EXPORT_SYMBOL(devfreq_add_governor);

/**
 * devfreq_remove_device() - Remove devfreq feature from a device.
 * @governor:	the devfreq governor to be removed
 */
int devfreq_remove_governor(struct devfreq_governor *governor)
{
	struct devfreq_governor *g;
	struct devfreq *devfreq;
	int err = 0;

	if (!governor) {
		pr_err("%s: Invalid parameters.\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&devfreq_list_lock);
	g = find_devfreq_governor(governor->name);
	if (IS_ERR(g)) {
		pr_err("%s: governor %s not registered\n", __func__,
		       governor->name);
		err = PTR_ERR(g);
		goto err_out;
	}
	list_for_each_entry(devfreq, &devfreq_list, node) {
		int ret;
		struct device *dev = devfreq->dev.parent;

		if (!strncmp(devfreq->governor_name, governor->name,
			     DEVFREQ_NAME_LEN)) {
			/* we should have a devfreq governor! */
			if (!devfreq->governor) {
				dev_warn(dev, "%s: Governor %s NOT present\n",
					 __func__, governor->name);
				continue;
				/* Fall through */
			}
			ret = devfreq->governor->event_handler(devfreq,
						DEVFREQ_GOV_STOP, NULL);
			if (ret) {
				dev_warn(dev, "%s: Governor %s stop=%d\n",
					 __func__, devfreq->governor->name,
					 ret);
			}
			devfreq->governor = NULL;
		}
	}

	list_del(&governor->node);
err_out:
	mutex_unlock(&devfreq_list_lock);

	return err;
}
EXPORT_SYMBOL(devfreq_remove_governor);

static ssize_t governor_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	if (!to_devfreq(dev)->governor)
		return -EINVAL;

	return sprintf(buf, "%s\n", to_devfreq(dev)->governor->name);
}

static ssize_t governor_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct devfreq *df = to_devfreq(dev);
	int ret;
	char str_governor[DEVFREQ_NAME_LEN + 1];
	const struct devfreq_governor *governor, *prev_gov;

	ret = sscanf(buf, "%" __stringify(DEVFREQ_NAME_LEN) "s", str_governor);
	if (ret != 1)
		return -EINVAL;

     /* Governor white list */
	if (strncmp(str_governor, "simple_ondemand", DEVFREQ_NAME_LEN) &&
		strncmp(str_governor, "cpufreq", DEVFREQ_NAME_LEN) &&
		strncmp(str_governor, "performance", DEVFREQ_NAME_LEN) &&
		strncmp(str_governor, "powersave", DEVFREQ_NAME_LEN) &&
		strncmp(str_governor, "msm-adreno-tz", DEVFREQ_NAME_LEN))
		return -EINVAL;

	mutex_lock(&devfreq_list_lock);
	governor = find_devfreq_governor(str_governor);
	if (IS_ERR(governor)) {
		ret = PTR_ERR(governor);
		goto out;
	}
	if (df->governor == governor) {
		ret = 0;
		goto out;
	} else if ((df->governor && df->governor->immutable) ||
					governor->immutable) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&df->event_lock);
	if (df->governor) {
		ret = df->governor->event_handler(df, DEVFREQ_GOV_STOP, NULL);
		if (ret) {
			dev_warn(dev, "%s: Governor %s not stopped(%d)\n",
				 __func__, df->governor->name, ret);
			goto gov_stop_out;
		}
	}
	prev_gov = df->governor;
	df->governor = governor;
	strncpy(df->governor_name, governor->name, DEVFREQ_NAME_LEN);
	ret = df->governor->event_handler(df, DEVFREQ_GOV_START, NULL);
	if (ret) {
		dev_warn(dev, "%s: Governor %s not started(%d)\n",
			 __func__, df->governor->name, ret);
		if (prev_gov) {
			df->governor = prev_gov;
			strlcpy(df->governor_name, prev_gov->name,
				DEVFREQ_NAME_LEN);
			df->governor->event_handler(df, DEVFREQ_GOV_START,
						    NULL);
		}
	}

gov_stop_out:
	mutex_unlock(&df->event_lock);
out:
	mutex_unlock(&devfreq_list_lock);

	if (!ret)
		ret = count;
	return ret;
}
static DEVICE_ATTR_RW(governor);

static ssize_t available_governors_show(struct device *d,
					struct device_attribute *attr,
					char *buf)
{
	struct devfreq *df = to_devfreq(d);
	ssize_t count = 0;

	mutex_lock(&devfreq_list_lock);

	/*
	 * The devfreq with immutable governor (e.g., passive) shows
	 * only own governor.
	 */
	if (df->governor && df->governor->immutable) {
		count = scnprintf(&buf[count], DEVFREQ_NAME_LEN,
				   "%s ", df->governor_name);
	/*
	 * The devfreq device shows the registered governor except for
	 * immutable governors such as passive governor .
	 */
	} else {
		struct devfreq_governor *governor;

		list_for_each_entry(governor, &devfreq_governor_list, node) {
			if (governor->immutable)
				continue;
			count += scnprintf(&buf[count], (PAGE_SIZE - count - 2),
					   "%s ", governor->name);
		}
	}

	mutex_unlock(&devfreq_list_lock);

	/* Truncate the trailing space */
	if (count)
		count--;

	count += sprintf(&buf[count], "\n");

	return count;
}
static DEVICE_ATTR_RO(available_governors);

static ssize_t cur_freq_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	unsigned long freq;
	struct devfreq *devfreq = to_devfreq(dev);

	if (devfreq->profile->get_cur_freq &&
		!devfreq->profile->get_cur_freq(devfreq->dev.parent, &freq))
			return sprintf(buf, "%lu\n", freq);

	return sprintf(buf, "%lu\n", devfreq->previous_freq);
}
static DEVICE_ATTR_RO(cur_freq);

static ssize_t target_freq_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", to_devfreq(dev)->previous_freq);
}
static DEVICE_ATTR_RO(target_freq);

static ssize_t polling_interval_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", to_devfreq(dev)->profile->polling_ms);
}

static ssize_t polling_interval_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct devfreq *df = to_devfreq(dev);
	unsigned int value;
	int ret;

	if (!df->governor)
		return -EINVAL;

	ret = sscanf(buf, "%u", &value);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&df->event_lock);
	df->governor->event_handler(df, DEVFREQ_GOV_INTERVAL, &value);
	ret = count;
	mutex_unlock(&df->event_lock);

	return ret;
}
static DEVICE_ATTR_RW(polling_interval);

static ssize_t min_freq_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct devfreq *df = to_devfreq(dev);
	unsigned long value;
	int ret;
	unsigned long max;

	/* Minfreq is managed by devfreq_boost */
	if (df->is_boost_device)
		return count;

	ret = sscanf(buf, "%lu", &value);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&df->event_lock);
	mutex_lock(&df->lock);
	max = df->max_freq;
	if (value && max && value > max) {
		ret = -EINVAL;
		goto unlock;
	}

	df->min_freq = value;
	update_devfreq(df);
	ret = count;
unlock:
	mutex_unlock(&df->lock);
	mutex_unlock(&df->event_lock);
	return ret;
}

static ssize_t max_freq_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct devfreq *df = to_devfreq(dev);
	unsigned long value;
	int ret;
	unsigned long min;

	ret = sscanf(buf, "%lu", &value);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&df->event_lock);
	mutex_lock(&df->lock);
	min = df->min_freq;
	if (value && min && value < min) {
		ret = -EINVAL;
		goto unlock;
	}

	df->max_freq = value;
	update_devfreq(df);
	ret = count;
unlock:
	mutex_unlock(&df->lock);
	mutex_unlock(&df->event_lock);
	return ret;
}

#define show_one(name)						\
static ssize_t name##_show					\
(struct device *dev, struct device_attribute *attr, char *buf)	\
{								\
	return sprintf(buf, "%lu\n", to_devfreq(dev)->name);	\
}
show_one(min_freq);
show_one(max_freq);

static DEVICE_ATTR_RW(min_freq);
static DEVICE_ATTR_RW(max_freq);

static ssize_t available_frequencies_show(struct device *d,
					  struct device_attribute *attr,
					  char *buf)
{
	struct devfreq *df = to_devfreq(d);
	struct device *dev = df->dev.parent;
	struct dev_pm_opp *opp;
	unsigned int i = 0, max_state = df->profile->max_state;
	bool use_opp;
	ssize_t count = 0;
	unsigned long freq = 0;

	rcu_read_lock();
	use_opp = dev_pm_opp_get_opp_count(dev) > 0;
	while (use_opp || (!use_opp && i < max_state)) {
		if (use_opp) {
			opp = dev_pm_opp_find_freq_ceil(dev, &freq);
			if (IS_ERR(opp))
				break;
		} else {
			freq = df->profile->freq_table[i++];
		}

		count += scnprintf(&buf[count], (PAGE_SIZE - count - 2),
				   "%lu ", freq);
		freq++;
	}
	rcu_read_unlock();

	/* Truncate the trailing space */
	if (count)
		count--;

	count += sprintf(&buf[count], "\n");

	return count;
}
static DEVICE_ATTR_RO(available_frequencies);

static ssize_t trans_stat_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct devfreq *devfreq = to_devfreq(dev);
	ssize_t len;
	int i, j;
	unsigned int max_state = devfreq->profile->max_state;

	if (max_state == 0)
		return sprintf(buf, "Not Supported.\n");

	mutex_lock(&devfreq->lock);
	if (!devfreq->stop_polling &&
			devfreq_update_status(devfreq, devfreq->previous_freq)) {
		mutex_unlock(&devfreq->lock);
		return 0;
	}
	mutex_unlock(&devfreq->lock);

	len = sprintf(buf, "     From  :   To\n");
	len += sprintf(buf + len, "           :");
	for (i = 0; i < max_state; i++)
		len += sprintf(buf + len, "%10lu",
				devfreq->profile->freq_table[i]);

	len += sprintf(buf + len, "   time(ms)\n");

	for (i = 0; i < max_state; i++) {
		if (devfreq->profile->freq_table[i]
					== devfreq->previous_freq) {
			len += sprintf(buf + len, "*");
		} else {
			len += sprintf(buf + len, " ");
		}
		len += sprintf(buf + len, "%10lu:",
				devfreq->profile->freq_table[i]);
		for (j = 0; j < max_state; j++)
			len += sprintf(buf + len, "%10u",
				devfreq->trans_table[(i * max_state) + j]);
		len += sprintf(buf + len, "%10u\n",
			jiffies_to_msecs(devfreq->time_in_state[i]));
	}

	len += sprintf(buf + len, "Total transition : %u\n",
					devfreq->total_trans);
	return len;
}
static DEVICE_ATTR_RO(trans_stat);

static struct attribute *devfreq_attrs[] = {
	&dev_attr_governor.attr,
	&dev_attr_available_governors.attr,
	&dev_attr_cur_freq.attr,
	&dev_attr_available_frequencies.attr,
	&dev_attr_target_freq.attr,
	&dev_attr_polling_interval.attr,
	&dev_attr_min_freq.attr,
	&dev_attr_max_freq.attr,
	&dev_attr_trans_stat.attr,
	NULL,
};
ATTRIBUTE_GROUPS(devfreq);

static int __init devfreq_init(void)
{
	devfreq_class = class_create(THIS_MODULE, "devfreq");
	if (IS_ERR(devfreq_class)) {
		pr_err("%s: couldn't create class\n", __FILE__);
		return PTR_ERR(devfreq_class);
	}

	devfreq_wq = create_freezable_workqueue("devfreq_wq");
	if (!devfreq_wq) {
		class_destroy(devfreq_class);
		pr_err("%s: couldn't create workqueue\n", __FILE__);
		return -ENOMEM;
	}
	devfreq_class->dev_groups = devfreq_groups;

	return 0;
}
subsys_initcall(devfreq_init);

/*
 * The followings are helper functions for devfreq user device drivers with
 * OPP framework.
 */

/**
 * devfreq_recommended_opp() - Helper function to get proper OPP for the
 *			     freq value given to target callback.
 * @dev:	The devfreq user device. (parent of devfreq)
 * @freq:	The frequency given to target function
 * @flags:	Flags handed from devfreq framework.
 *
 * Locking: This function must be called under rcu_read_lock(). opp is a rcu
 * protected pointer. The reason for the same is that the opp pointer which is
 * returned will remain valid for use with opp_get_{voltage, freq} only while
 * under the locked area. The pointer returned must be used prior to unlocking
 * with rcu_read_unlock() to maintain the integrity of the pointer.
 */
struct dev_pm_opp *devfreq_recommended_opp(struct device *dev,
					   unsigned long *freq,
					   u32 flags)
{
	struct dev_pm_opp *opp;

	if (flags & DEVFREQ_FLAG_LEAST_UPPER_BOUND) {
		/* The freq is an upper bound. opp should be lower */
		opp = dev_pm_opp_find_freq_floor(dev, freq);

		/* If not available, use the closest opp */
		if (opp == ERR_PTR(-ERANGE))
			opp = dev_pm_opp_find_freq_ceil(dev, freq);
	} else {
		/* The freq is an lower bound. opp should be higher */
		opp = dev_pm_opp_find_freq_ceil(dev, freq);

		/* If not available, use the closest opp */
		if (opp == ERR_PTR(-ERANGE))
			opp = dev_pm_opp_find_freq_floor(dev, freq);
	}

	return opp;
}
EXPORT_SYMBOL(devfreq_recommended_opp);

/**
 * devfreq_register_opp_notifier() - Helper function to get devfreq notified
 *				   for any changes in the OPP availability
 *				   changes
 * @dev:	The devfreq user device. (parent of devfreq)
 * @devfreq:	The devfreq object.
 */
int devfreq_register_opp_notifier(struct device *dev, struct devfreq *devfreq)
{
	struct srcu_notifier_head *nh;
	int ret = 0;

	rcu_read_lock();
	nh = dev_pm_opp_get_notifier(dev);
	if (IS_ERR(nh))
		ret = PTR_ERR(nh);
	rcu_read_unlock();
	if (!ret)
		ret = srcu_notifier_chain_register(nh, &devfreq->nb);

	return ret;
}
EXPORT_SYMBOL(devfreq_register_opp_notifier);

/**
 * devfreq_unregister_opp_notifier() - Helper function to stop getting devfreq
 *				     notified for any changes in the OPP
 *				     availability changes anymore.
 * @dev:	The devfreq user device. (parent of devfreq)
 * @devfreq:	The devfreq object.
 *
 * At exit() callback of devfreq_dev_profile, this must be included if
 * devfreq_recommended_opp is used.
 */
int devfreq_unregister_opp_notifier(struct device *dev, struct devfreq *devfreq)
{
	struct srcu_notifier_head *nh;
	int ret = 0;

	rcu_read_lock();
	nh = dev_pm_opp_get_notifier(dev);
	if (IS_ERR(nh))
		ret = PTR_ERR(nh);
	rcu_read_unlock();
	if (!ret)
		ret = srcu_notifier_chain_unregister(nh, &devfreq->nb);

	return ret;
}
EXPORT_SYMBOL(devfreq_unregister_opp_notifier);

static void devm_devfreq_opp_release(struct device *dev, void *res)
{
	devfreq_unregister_opp_notifier(dev, *(struct devfreq **)res);
}

/**
 * devm_ devfreq_register_opp_notifier()
 *		- Resource-managed devfreq_register_opp_notifier()
 * @dev:	The devfreq user device. (parent of devfreq)
 * @devfreq:	The devfreq object.
 */
int devm_devfreq_register_opp_notifier(struct device *dev,
				       struct devfreq *devfreq)
{
	struct devfreq **ptr;
	int ret;

	ptr = devres_alloc(devm_devfreq_opp_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	ret = devfreq_register_opp_notifier(dev, devfreq);
	if (ret) {
		devres_free(ptr);
		return ret;
	}

	*ptr = devfreq;
	devres_add(dev, ptr);

	return 0;
}
EXPORT_SYMBOL(devm_devfreq_register_opp_notifier);

/**
 * devm_devfreq_unregister_opp_notifier()
 *		- Resource-managed devfreq_unregister_opp_notifier()
 * @dev:	The devfreq user device. (parent of devfreq)
 * @devfreq:	The devfreq object.
 */
void devm_devfreq_unregister_opp_notifier(struct device *dev,
					 struct devfreq *devfreq)
{
	WARN_ON(devres_release(dev, devm_devfreq_opp_release,
			       devm_devfreq_dev_match, devfreq));
}
EXPORT_SYMBOL(devm_devfreq_unregister_opp_notifier);

/**
 * devfreq_register_notifier() - Register a driver with devfreq
 * @devfreq:	The devfreq object.
 * @nb:		The notifier block to register.
 * @list:	DEVFREQ_TRANSITION_NOTIFIER.
 */
int devfreq_register_notifier(struct devfreq *devfreq,
				struct notifier_block *nb,
				unsigned int list)
{
	int ret = 0;

	if (!devfreq)
		return -EINVAL;

	switch (list) {
	case DEVFREQ_TRANSITION_NOTIFIER:
		ret = srcu_notifier_chain_register(
				&devfreq->transition_notifier_list, nb);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL(devfreq_register_notifier);

/*
 * devfreq_unregister_notifier() - Unregister a driver with devfreq
 * @devfreq:	The devfreq object.
 * @nb:		The notifier block to be unregistered.
 * @list:	DEVFREQ_TRANSITION_NOTIFIER.
 */
int devfreq_unregister_notifier(struct devfreq *devfreq,
				struct notifier_block *nb,
				unsigned int list)
{
	int ret = 0;

	if (!devfreq)
		return -EINVAL;

	switch (list) {
	case DEVFREQ_TRANSITION_NOTIFIER:
		ret = srcu_notifier_chain_unregister(
				&devfreq->transition_notifier_list, nb);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL(devfreq_unregister_notifier);

struct devfreq_notifier_devres {
	struct devfreq *devfreq;
	struct notifier_block *nb;
	unsigned int list;
};

static void devm_devfreq_notifier_release(struct device *dev, void *res)
{
	struct devfreq_notifier_devres *this = res;

	devfreq_unregister_notifier(this->devfreq, this->nb, this->list);
}

/**
 * devm_devfreq_register_notifier()
	- Resource-managed devfreq_register_notifier()
 * @dev:	The devfreq user device. (parent of devfreq)
 * @devfreq:	The devfreq object.
 * @nb:		The notifier block to be unregistered.
 * @list:	DEVFREQ_TRANSITION_NOTIFIER.
 */
int devm_devfreq_register_notifier(struct device *dev,
				struct devfreq *devfreq,
				struct notifier_block *nb,
				unsigned int list)
{
	struct devfreq_notifier_devres *ptr;
	int ret;

	ptr = devres_alloc(devm_devfreq_notifier_release, sizeof(*ptr),
				GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	ret = devfreq_register_notifier(devfreq, nb, list);
	if (ret) {
		devres_free(ptr);
		return ret;
	}

	ptr->devfreq = devfreq;
	ptr->nb = nb;
	ptr->list = list;
	devres_add(dev, ptr);

	return 0;
}
EXPORT_SYMBOL(devm_devfreq_register_notifier);

/**
 * devm_devfreq_unregister_notifier()
	- Resource-managed devfreq_unregister_notifier()
 * @dev:	The devfreq user device. (parent of devfreq)
 * @devfreq:	The devfreq object.
 * @nb:		The notifier block to be unregistered.
 * @list:	DEVFREQ_TRANSITION_NOTIFIER.
 */
void devm_devfreq_unregister_notifier(struct device *dev,
				struct devfreq *devfreq,
				struct notifier_block *nb,
				unsigned int list)
{
	WARN_ON(devres_release(dev, devm_devfreq_notifier_release,
			       devm_devfreq_dev_match, devfreq));
}
EXPORT_SYMBOL(devm_devfreq_unregister_notifier);
