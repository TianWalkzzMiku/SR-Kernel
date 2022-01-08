/*
 * Copyright (c) 2014-2018, 2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/moduleparam.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <trace/events/power.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/kthread.h>

/* To handle cpufreq min/max request */
struct cpu_status {
	unsigned int min;
	unsigned int max;
};
static DEFINE_PER_CPU(struct cpu_status, cpu_stats);

struct events {
	spinlock_t cpu_hotplug_lock;
	bool cpu_hotplug;
	bool init_success;
};
static struct events events_group;
static struct task_struct *events_notify_thread;

/*******************************sysfs start************************************/
static int set_cpu_min_freq(const char *buf, const struct kernel_param *kp)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpu_status *i_cpu_stats;
	struct cpufreq_policy policy;
	cpumask_var_t limit_mask;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_mask);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > (num_present_cpus() - 1))
			return -EINVAL;

		i_cpu_stats = &per_cpu(cpu_stats, cpu);

		i_cpu_stats->min = val;
		cpumask_set_cpu(cpu, limit_mask);

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	/*
	 * Since on synchronous systems policy is shared amongst multiple
	 * CPUs only one CPU needs to be updated for the limit to be
	 * reflected for the entire cluster. We can avoid updating the policy
	 * of other CPUs in the cluster once it is done for at least one CPU
	 * in the cluster
	 */
	get_online_cpus();
	for_each_cpu(i, limit_mask) {
		i_cpu_stats = &per_cpu(cpu_stats, i);

		if (cpufreq_get_policy(&policy, i))
			continue;

		if (cpu_online(i) && (policy.min != i_cpu_stats->min))
			cpufreq_update_policy(i);

		for_each_cpu(j, policy.related_cpus)
			cpumask_clear_cpu(j, limit_mask);
	}
	put_online_cpus();

	return 0;
}

static int get_cpu_min_freq(char *buf, const struct kernel_param *kp)
{
	int cnt = 0, cpu;

	for_each_present_cpu(cpu) {
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt,
				"%d:%u ", cpu, per_cpu(cpu_stats, cpu).min);
	}
	cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	return cnt;
}

static const struct kernel_param_ops param_ops_cpu_min_freq = {
	.set = set_cpu_min_freq,
	.get = get_cpu_min_freq,
};
module_param_cb(cpu_min_freq, &param_ops_cpu_min_freq, NULL, 0644);

static int set_cpu_max_freq(const char *buf, const struct kernel_param *kp)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	struct cpu_status *i_cpu_stats;
	struct cpufreq_policy policy;
	cpumask_var_t limit_mask;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	cpumask_clear(limit_mask);
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > (num_present_cpus() - 1))
			return -EINVAL;

		i_cpu_stats = &per_cpu(cpu_stats, cpu);

		i_cpu_stats->max = val;
		cpumask_set_cpu(cpu, limit_mask);

		cp = strnchr(cp, strlen(cp), ' ');
		cp++;
	}

	get_online_cpus();
	for_each_cpu(i, limit_mask) {
		i_cpu_stats = &per_cpu(cpu_stats, i);
		if (cpufreq_get_policy(&policy, i))
			continue;

		if (cpu_online(i) && (policy.max != i_cpu_stats->max))
			cpufreq_update_policy(i);

		for_each_cpu(j, policy.related_cpus)
			cpumask_clear_cpu(j, limit_mask);
	}
	put_online_cpus();

	return 0;
}

static int get_cpu_max_freq(char *buf, const struct kernel_param *kp)
{
	int cnt = 0, cpu;

	for_each_present_cpu(cpu) {
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt,
				"%d:%u ", cpu, per_cpu(cpu_stats, cpu).max);
	}
	cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	return cnt;
}

static const struct kernel_param_ops param_ops_cpu_max_freq = {
	.set = set_cpu_max_freq,
	.get = get_cpu_max_freq,
};
module_param_cb(cpu_max_freq, &param_ops_cpu_max_freq, NULL, 0644);

static struct kobject *events_kobj;

static ssize_t show_cpu_hotplug(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "\n");
}
static struct kobj_attribute cpu_hotplug_attr =
__ATTR(cpu_hotplug, 0444, show_cpu_hotplug, NULL);

static struct attribute *events_attrs[] = {
	&cpu_hotplug_attr.attr,
	NULL,
};

static struct attribute_group events_attr_group = {
	.attrs = events_attrs,
};

/*******************************sysfs ends************************************/
static int perf_adjust_notify(struct notifier_block *nb, unsigned long val,
							void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int cpu = policy->cpu;
	struct cpu_status *cpu_st = &per_cpu(cpu_stats, cpu);
	unsigned int min = cpu_st->min, max = cpu_st->max;


	if (val != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	pr_debug("msm_perf: CPU%u policy before: %u:%u kHz\n", cpu,
						policy->min, policy->max);
	pr_debug("msm_perf: CPU%u seting min:max %u:%u kHz\n", cpu, min, max);

	cpufreq_verify_within_limits(policy, min, max);

	pr_debug("msm_perf: CPU%u policy after: %u:%u kHz\n", cpu,
						policy->min, policy->max);

	return NOTIFY_OK;
}

static struct notifier_block perf_cpufreq_nb = {
	.notifier_call = perf_adjust_notify,
};

static inline void hotplug_notify(int action)
{
	unsigned long flags;

	if (!events_group.init_success)
		return;

	if ((action == CPU_ONLINE) || (action == CPU_DEAD)) {
		spin_lock_irqsave(&(events_group.cpu_hotplug_lock), flags);
		events_group.cpu_hotplug = true;
		spin_unlock_irqrestore(&(events_group.cpu_hotplug_lock), flags);
		wake_up_process(events_notify_thread);
	}
}

static int __ref msm_performance_cpu_callback(struct notifier_block *nfb,
		unsigned long action, void *hcpu)
{

	hotplug_notify(action);

	return NOTIFY_OK;
}

static struct notifier_block __refdata msm_performance_cpu_notifier = {
	.notifier_call = msm_performance_cpu_callback,
};

static int events_notify_userspace(void *data)
{
	unsigned long flags;
	bool notify_change;

	while (1) {

		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&(events_group.cpu_hotplug_lock), flags);

		if (!events_group.cpu_hotplug) {
			spin_unlock_irqrestore(&(events_group.cpu_hotplug_lock),
									flags);

			schedule();
			if (kthread_should_stop())
				break;
			spin_lock_irqsave(&(events_group.cpu_hotplug_lock),
									flags);
		}

		set_current_state(TASK_RUNNING);
		notify_change = events_group.cpu_hotplug;
		events_group.cpu_hotplug = false;
		spin_unlock_irqrestore(&(events_group.cpu_hotplug_lock), flags);

		if (notify_change)
			sysfs_notify(events_kobj, NULL, "cpu_hotplug");
	}

	return 0;
}

static int init_events_group(void)
{
	int ret;
	struct kobject *module_kobj;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("msm_perf: Couldn't find module kobject\n");
		return -ENOENT;
	}

	events_kobj = kobject_create_and_add("events", module_kobj);
	if (!events_kobj) {
		pr_err("msm_perf: Failed to add events_kobj\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(events_kobj, &events_attr_group);
	if (ret) {
		pr_err("msm_perf: Failed to create sysfs\n");
		return ret;
	}

	spin_lock_init(&(events_group.cpu_hotplug_lock));
	events_notify_thread = kthread_run(events_notify_userspace,
					NULL, "msm_perf:events_notify");
	if (IS_ERR(events_notify_thread))
		return PTR_ERR(events_notify_thread);

	events_group.init_success = true;

	return 0;
}

static int __init msm_performance_init(void)
{
	unsigned int cpu;

	cpufreq_register_notifier(&perf_cpufreq_nb, CPUFREQ_POLICY_NOTIFIER);

	for_each_present_cpu(cpu)
		per_cpu(cpu_stats, cpu).max = UINT_MAX;

	register_cpu_notifier(&msm_performance_cpu_notifier);

	init_events_group();

	return 0;
}
late_initcall(msm_performance_init);


<<<<<<< HEAD
=======
	cl->last_perf_cl_check_ts = now;
	if (ret_mode != cl->perf_cl_peak) {
		pr_debug("msm_perf: Mode changed to %u\n", ret_mode);
		cl->perf_cl_peak = ret_mode;
		cl->perf_cl_detect_state_change = true;
	}

	trace_cpu_mode_detect(cpumask_first(cl->cpus), max_load,
		cl->single_enter_cycle_cnt, cl->single_exit_cycle_cnt,
		total_load, cl->multi_enter_cycle_cnt,
		cl->multi_exit_cycle_cnt, cl->perf_cl_peak_enter_cycle_cnt,
		cl->perf_cl_peak_exit_cycle_cnt, cl->mode, cpu_cnt);

	spin_unlock_irqrestore(&cl->perf_cl_peak_lock, flags);

	if (cl->perf_cl_detect_state_change)
		wake_up_process(notify_thread);

}

static void check_cpu_load(struct cluster *cl, u64 now)
{
	struct load_stats *pcpu_st;
	unsigned int i, max_load = 0, total_load = 0, ret_mode, cpu_cnt = 0;
	unsigned int total_load_ceil, total_load_floor;
	unsigned long flags;

	spin_lock_irqsave(&cl->mode_lock, flags);

	if (((now - cl->last_mode_check_ts)
		< (cl->timer_rate - LAST_LD_CHECK_TOL)) ||
		!(workload_detect & MODE_DETECT)) {
		spin_unlock_irqrestore(&cl->mode_lock, flags);
		return;
	}

	for_each_cpu(i, cl->cpus) {
		pcpu_st = &per_cpu(cpu_load_stats, i);
		if ((now - pcpu_st->last_wallclock)
			> (cl->timer_rate + LAST_UPDATE_TOL))
			continue;
		if (pcpu_st->cpu_load > max_load)
			max_load = pcpu_st->cpu_load;
		total_load += pcpu_st->cpu_load;
		cpu_cnt++;
	}

	if (cpu_cnt > 1) {
		total_load_ceil = cl->pcpu_multi_enter_load * cpu_cnt;
		total_load_floor = cl->pcpu_multi_exit_load * cpu_cnt;
	} else {
		total_load_ceil = UINT_MAX;
		total_load_floor = UINT_MAX;
	}

	ret_mode = cl->mode;
	if (!(cl->mode & SINGLE)) {
		if (max_load >= cl->single_enter_load) {
			cl->single_enter_cycle_cnt++;
			if (cl->single_enter_cycle_cnt
				>= cl->single_enter_cycles) {
				ret_mode |= SINGLE;
				cl->single_enter_cycle_cnt = 0;
			}
		} else {
			cl->single_enter_cycle_cnt = 0;
		}
	} else {
		if (max_load < cl->single_exit_load) {
			start_timer(cl);
			cl->single_exit_cycle_cnt++;
			if (cl->single_exit_cycle_cnt
				>= cl->single_exit_cycles) {
				ret_mode &= ~SINGLE;
				cl->single_exit_cycle_cnt = 0;
				disable_timer(cl);
			}
		} else {
			cl->single_exit_cycle_cnt = 0;
			disable_timer(cl);
		}
	}

	if (!(cl->mode & MULTI)) {
		if (total_load >= total_load_ceil) {
			cl->multi_enter_cycle_cnt++;
			if (cl->multi_enter_cycle_cnt
				>= cl->multi_enter_cycles) {
				ret_mode |= MULTI;
				cl->multi_enter_cycle_cnt = 0;
			}
		} else {
			cl->multi_enter_cycle_cnt = 0;
		}
	} else {
		if (total_load < total_load_floor) {
			cl->multi_exit_cycle_cnt++;
			if (cl->multi_exit_cycle_cnt
				>= cl->multi_exit_cycles) {
				ret_mode &= ~MULTI;
				cl->multi_exit_cycle_cnt = 0;
			}
		} else {
			cl->multi_exit_cycle_cnt = 0;
		}
	}

	cl->last_mode_check_ts = now;

	if (ret_mode != cl->mode) {
		cl->mode = ret_mode;
		cl->mode_change = true;
		pr_debug("msm_perf: Mode changed to %u\n", ret_mode);
	}

	trace_cpu_mode_detect(cpumask_first(cl->cpus), max_load,
		cl->single_enter_cycle_cnt, cl->single_exit_cycle_cnt,
		total_load, cl->multi_enter_cycle_cnt,
		cl->multi_exit_cycle_cnt, cl->perf_cl_peak_enter_cycle_cnt,
		cl->perf_cl_peak_exit_cycle_cnt, cl->mode, cpu_cnt);

	spin_unlock_irqrestore(&cl->mode_lock, flags);

	if (cl->mode_change)
		wake_up_process(notify_thread);
}

static void check_workload_stats(unsigned int cpu, unsigned int rate, u64 now)
{
	struct cluster *cl = NULL;
	unsigned int i;

	for (i = 0; i < num_clusters; i++) {
		if (cpumask_test_cpu(cpu, managed_clusters[i]->cpus)) {
			cl = managed_clusters[i];
			break;
		}
	}
	if (cl == NULL)
		return;

	cl->timer_rate = rate;
	check_cluster_iowait(cl, now);
	check_cpu_load(cl, now);
	check_perf_cl_peak_load(cl, now);
}

static int perf_govinfo_notify(struct notifier_block *nb, unsigned long val,
								void *data)
{
	struct cpufreq_govinfo *gov_info = data;
	unsigned int cpu = gov_info->cpu;
	struct load_stats *cpu_st = &per_cpu(cpu_load_stats, cpu);
	u64 now, cur_iowait, time_diff, iowait_diff;

	if (!clusters_inited || !workload_detect)
		return NOTIFY_OK;

	cur_iowait = get_cpu_iowait_time_us(cpu, &now);
	if (cur_iowait >= cpu_st->last_iowait)
		iowait_diff = cur_iowait - cpu_st->last_iowait;
	else
		iowait_diff = 0;

	if (now > cpu_st->last_wallclock)
		time_diff = now - cpu_st->last_wallclock;
	else
		return NOTIFY_OK;

	if (iowait_diff <= time_diff) {
		iowait_diff *= 100;
		cpu_st->last_iopercent = div64_u64(iowait_diff, time_diff);
	} else {
		cpu_st->last_iopercent = 100;
	}

	cpu_st->last_wallclock = now;
	cpu_st->last_iowait = cur_iowait;
	cpu_st->cpu_load = gov_info->load;

	/*
	 * Avoid deadlock in case governor notifier ran in the context
	 * of notify_work thread
	 */
	if (current == notify_thread)
		return NOTIFY_OK;

	check_workload_stats(cpu, gov_info->sampling_rate_us, now);

	return NOTIFY_OK;
}
static int perf_cputrans_notify(struct notifier_block *nb, unsigned long val,
								void *data)
{
	struct cpufreq_freqs *freq = data;
	unsigned int cpu = freq->cpu;
	unsigned long flags;
	unsigned int i;
	struct cluster *cl = NULL;
	struct load_stats *cpu_st = &per_cpu(cpu_load_stats, cpu);

	if (!clusters_inited || !workload_detect)
		return NOTIFY_OK;

	for (i = 0; i < num_clusters; i++) {
		if (cpumask_test_cpu(cpu, managed_clusters[i]->cpus)) {
			cl = managed_clusters[i];
			break;
		}
	}
	if (cl == NULL)
		return NOTIFY_OK;

	if (val == CPUFREQ_POSTCHANGE) {
		spin_lock_irqsave(&cl->perf_cl_peak_lock, flags);
		cpu_st->freq = freq->new;
		spin_unlock_irqrestore(&cl->perf_cl_peak_lock, flags);
	}
	/*
	* Avoid deadlock in case governor notifier ran in the context
	* of notify_work thread
	*/
	if (current == notify_thread)
		return NOTIFY_OK;

	return NOTIFY_OK;
}

static struct notifier_block perf_govinfo_nb = {
	.notifier_call = perf_govinfo_notify,
};
static struct notifier_block perf_cputransitions_nb = {
	.notifier_call = perf_cputrans_notify,
};

/*
 * Attempt to offline CPUs based on their power cost.
 * CPUs with higher power costs are offlined first.
 */
static int __ref rm_high_pwr_cost_cpus(struct cluster *cl)
{
	unsigned int cpu, i;
	struct cpu_pwr_stats *per_cpu_info = get_cpu_pwr_stats();
	struct cpu_pstate_pwr *costs;
	unsigned int *pcpu_pwr;
	unsigned int max_cost_cpu, max_cost;
	int any_cpu = -1;

	if (!per_cpu_info)
		return -ENOSYS;

	for_each_cpu(cpu, cl->cpus) {
		costs = per_cpu_info[cpu].ptable;
		if (!costs || !costs[0].freq)
			continue;

		i = 1;
		while (costs[i].freq)
			i++;

		pcpu_pwr = &per_cpu(cpu_power_cost, cpu);
		*pcpu_pwr = costs[i - 1].power;
		any_cpu = (int)cpu;
		pr_debug("msm_perf: CPU:%d Power:%u\n", cpu, *pcpu_pwr);
	}

	if (any_cpu < 0)
		return -EAGAIN;

	for (i = 0; i < cpumask_weight(cl->cpus); i++) {
		max_cost = 0;
		max_cost_cpu = cpumask_first(cl->cpus);

		for_each_cpu(cpu, cl->cpus) {
			pcpu_pwr = &per_cpu(cpu_power_cost, cpu);
			if (max_cost < *pcpu_pwr) {
				max_cost = *pcpu_pwr;
				max_cost_cpu = cpu;
			}
		}

		if (!cpu_online(max_cost_cpu))
			goto end;

		pr_debug("msm_perf: Offlining CPU%d Power:%d\n", max_cost_cpu,
								max_cost);
		cpumask_set_cpu(max_cost_cpu, cl->offlined_cpus);
		lock_device_hotplug();
		if (device_offline(get_cpu_device(max_cost_cpu))) {
			cpumask_clear_cpu(max_cost_cpu, cl->offlined_cpus);
			pr_debug("msm_perf: Offlining CPU%d failed\n",
								max_cost_cpu);
		}
		unlock_device_hotplug();

end:
		pcpu_pwr = &per_cpu(cpu_power_cost, max_cost_cpu);
		*pcpu_pwr = 0;
		if (num_online_managed(cl->cpus) <= cl->max_cpu_request)
			break;
	}

	if (num_online_managed(cl->cpus) > cl->max_cpu_request)
		return -EAGAIN;
	else
		return 0;
}

/*
 * try_hotplug tries to online/offline cores based on the current requirement.
 * It loops through the currently managed CPUs and tries to online/offline
 * them until the max_cpu_request criteria is met.
 */
static void __ref try_hotplug(struct cluster *data)
{
	unsigned int i;
	struct device *dev;

	if (!clusters_inited)
		return;

	pr_debug("msm_perf: Trying hotplug...%d:%d\n",
			num_online_managed(data->cpus),	num_online_cpus());

	mutex_lock(&managed_cpus_lock);
	if (num_online_managed(data->cpus) > data->max_cpu_request) {
		if (!rm_high_pwr_cost_cpus(data)) {
			mutex_unlock(&managed_cpus_lock);
			return;
		}

		/*
		 * If power aware offlining fails due to power cost info
		 * being unavaiable fall back to original implementation
		 */
		for (i = num_present_cpus() - 1; i >= 0 &&
						i < num_present_cpus(); i--) {
			if (!cpumask_test_cpu(i, data->cpus) ||	!cpu_online(i))
				continue;

			pr_debug("msm_perf: Offlining CPU%d\n", i);
			cpumask_set_cpu(i, data->offlined_cpus);
			lock_device_hotplug();
			dev = get_cpu_device(i);
			if (!dev || device_offline(dev)) {
				cpumask_clear_cpu(i, data->offlined_cpus);
				pr_debug("msm_perf: Offlining CPU%d failed\n",
									i);
				unlock_device_hotplug();
				continue;
			}
			unlock_device_hotplug();
			if (num_online_managed(data->cpus) <=
							data->max_cpu_request)
				break;
		}
	} else {
		for_each_cpu(i, data->cpus) {
			if (cpu_online(i))
				continue;
			pr_debug("msm_perf: Onlining CPU%d\n", i);
			lock_device_hotplug();
			dev = get_cpu_device(i);
			if (!dev || device_online(dev)) {
				pr_debug("msm_perf: Onlining CPU%d failed\n",
									i);
				unlock_device_hotplug();
				continue;
			}
			unlock_device_hotplug();
			cpumask_clear_cpu(i, data->offlined_cpus);
			if (num_online_managed(data->cpus) >=
							data->max_cpu_request)
				break;
		}
	}
	mutex_unlock(&managed_cpus_lock);
}

static void __ref release_cluster_control(struct cpumask *off_cpus)
{
	int cpu;
	struct device *dev;

	for_each_cpu(cpu, off_cpus) {
		pr_debug("msm_perf: Release CPU %d\n", cpu);
		lock_device_hotplug();
		dev = get_cpu_device(cpu);
		if (!dev) {
			pr_debug("msm_perf: Failed to get CPU%d\n",
								cpu);
			unlock_device_hotplug();
			continue;
		}
		if (!device_online(dev))
			cpumask_clear_cpu(cpu, off_cpus);
		unlock_device_hotplug();
	}
}

/* Work to evaluate current online CPU status and hotplug CPUs as per need*/
static void check_cluster_status(struct work_struct *work)
{
	int i;
	struct cluster *i_cl;

	for (i = 0; i < num_clusters; i++) {
		i_cl = managed_clusters[i];

		if (cpumask_empty(i_cl->cpus))
			continue;

		if (i_cl->max_cpu_request < 0) {
			if (!cpumask_empty(i_cl->offlined_cpus))
				release_cluster_control(i_cl->offlined_cpus);
			continue;
		}

		if (num_online_managed(i_cl->cpus) !=
					i_cl->max_cpu_request)
			try_hotplug(i_cl);
	}
}

static int __ref msm_performance_cpu_callback(struct notifier_block *nfb,
		unsigned long action, void *hcpu)
{
	uint32_t cpu = (uintptr_t)hcpu;
	unsigned int i;
	struct cluster *i_cl = NULL;

	hotplug_notify(action);

	if (!clusters_inited)
		return NOTIFY_OK;

	for (i = 0; i < num_clusters; i++) {
		if (managed_clusters[i]->cpus == NULL)
			return NOTIFY_OK;
		if (cpumask_test_cpu(cpu, managed_clusters[i]->cpus)) {
			i_cl = managed_clusters[i];
			break;
		}
	}

	if (i_cl == NULL)
		return NOTIFY_OK;

	if (action == CPU_UP_PREPARE || action == CPU_UP_PREPARE_FROZEN) {
		/*
		 * Prevent onlining of a managed CPU if max_cpu criteria is
		 * already satisfied
		 */
		if (i_cl->offlined_cpus == NULL)
			return NOTIFY_OK;
		if (i_cl->max_cpu_request <=
					num_online_managed(i_cl->cpus)) {
			pr_debug("msm_perf: Prevent CPU%d onlining\n", cpu);
			cpumask_set_cpu(cpu, i_cl->offlined_cpus);
			return NOTIFY_BAD;
		}
		cpumask_clear_cpu(cpu, i_cl->offlined_cpus);

	} else if (action == CPU_DEAD) {
		if (i_cl->offlined_cpus == NULL)
			return NOTIFY_OK;
		if (cpumask_test_cpu(cpu, i_cl->offlined_cpus))
			return NOTIFY_OK;
		/*
		 * Schedule a re-evaluation to check if any more CPUs can be
		 * brought online to meet the max_cpu_request requirement. This
		 * work is delayed to account for CPU hotplug latencies
		 */
		if (schedule_delayed_work(&evaluate_hotplug_work, 0)) {
			trace_reevaluate_hotplug(cpumask_bits(i_cl->cpus)[0],
							i_cl->max_cpu_request);
			pr_debug("msm_perf: Re-evaluation scheduled %d\n", cpu);
		} else {
			pr_debug("msm_perf: Work scheduling failed %d\n", cpu);
		}
	}

	return NOTIFY_OK;
}

static struct notifier_block __refdata msm_performance_cpu_notifier = {
	.notifier_call = msm_performance_cpu_callback,
};

static void single_mod_exit_timer(unsigned long data)
{
	int i;
	struct cluster *i_cl = NULL;
	unsigned long flags;

	if (!clusters_inited)
		return;

	for (i = 0; i < num_clusters; i++) {
		if (cpumask_test_cpu(data,
			managed_clusters[i]->cpus)) {
			i_cl = managed_clusters[i];
			break;
		}
	}

	if (i_cl == NULL)
		return;

	spin_lock_irqsave(&i_cl->mode_lock, flags);
	if (i_cl->mode & SINGLE) {
		/* Disable SINGLE mode and exit since the timer expired */
		i_cl->mode = i_cl->mode & ~SINGLE;
		i_cl->single_enter_cycle_cnt = 0;
		i_cl->single_exit_cycle_cnt = 0;
		trace_single_mode_timeout(cpumask_first(i_cl->cpus),
			i_cl->single_enter_cycles, i_cl->single_enter_cycle_cnt,
			i_cl->single_exit_cycles, i_cl->single_exit_cycle_cnt,
			i_cl->multi_enter_cycles, i_cl->multi_enter_cycle_cnt,
			i_cl->multi_exit_cycles, i_cl->multi_exit_cycle_cnt,
			i_cl->timer_rate, i_cl->mode);
	}
	spin_unlock_irqrestore(&i_cl->mode_lock, flags);
	wake_up_process(notify_thread);
}

static void perf_cl_peak_mod_exit_timer(unsigned long data)
{
	int i;
	struct cluster *i_cl = NULL;
	unsigned long flags;

	if (!clusters_inited)
		return;

	for (i = 0; i < num_clusters; i++) {
		if (cpumask_test_cpu(data,
			managed_clusters[i]->cpus)) {
			i_cl = managed_clusters[i];
			break;
		}
	}

	if (i_cl == NULL)
		return;

	spin_lock_irqsave(&i_cl->perf_cl_peak_lock, flags);
	if (i_cl->perf_cl_peak & PERF_CL_PEAK) {
		/* Disable PERF_CL_PEAK mode and exit since the timer expired */
		i_cl->perf_cl_peak = i_cl->perf_cl_peak & ~PERF_CL_PEAK;
		i_cl->perf_cl_peak_enter_cycle_cnt = 0;
		i_cl->perf_cl_peak_exit_cycle_cnt = 0;
	}
	spin_unlock_irqrestore(&i_cl->perf_cl_peak_lock, flags);
	wake_up_process(notify_thread);
}

static int init_cluster_control(void)
{
	unsigned int i;
	int ret = 0;
	struct kobject *module_kobj;

	managed_clusters = kcalloc(num_clusters, sizeof(struct cluster *),
								GFP_KERNEL);
	if (!managed_clusters)
		return -ENOMEM;
	for (i = 0; i < num_clusters; i++) {
		managed_clusters[i] = kcalloc(1, sizeof(struct cluster),
								GFP_KERNEL);
		if (!managed_clusters[i]) {
			pr_err("msm_perf:Cluster %u mem alloc failed\n", i);
			ret = -ENOMEM;
			goto error;
		}
		if (!alloc_cpumask_var(&managed_clusters[i]->cpus,
		     GFP_KERNEL)) {
			pr_err("msm_perf:Cluster %u cpu alloc failed\n",
			       i);
			ret = -ENOMEM;
			goto error;
		}
		if (!alloc_cpumask_var(&managed_clusters[i]->offlined_cpus,
		     GFP_KERNEL)) {
			pr_err("msm_perf:Cluster %u off_cpus alloc failed\n",
			       i);
			ret = -ENOMEM;
			goto error;
		}

		managed_clusters[i]->max_cpu_request = -1;
		managed_clusters[i]->single_enter_load = DEF_SINGLE_ENT;
		managed_clusters[i]->single_exit_load = DEF_SINGLE_EX;
		managed_clusters[i]->single_enter_cycles
						= DEF_SINGLE_ENTER_CYCLE;
		managed_clusters[i]->single_exit_cycles
						= DEF_SINGLE_EXIT_CYCLE;
		managed_clusters[i]->pcpu_multi_enter_load
						= DEF_PCPU_MULTI_ENT;
		managed_clusters[i]->pcpu_multi_exit_load = DEF_PCPU_MULTI_EX;
		managed_clusters[i]->multi_enter_cycles = DEF_MULTI_ENTER_CYCLE;
		managed_clusters[i]->multi_exit_cycles = DEF_MULTI_EXIT_CYCLE;
		managed_clusters[i]->perf_cl_peak_enter_load =
						DEF_PERF_CL_PEAK_ENT;
		managed_clusters[i]->perf_cl_peak_exit_load =
						DEF_PERF_CL_PEAK_EX;
		managed_clusters[i]->perf_cl_peak_enter_cycles =
						DEF_PERF_CL_PEAK_ENTER_CYCLE;
		managed_clusters[i]->perf_cl_peak_exit_cycles =
						DEF_PERF_CL_PEAK_EXIT_CYCLE;

		/* Initialize trigger threshold */
		thr.perf_cl_trigger_threshold = CLUSTER_1_THRESHOLD_FREQ;
		thr.pwr_cl_trigger_threshold = CLUSTER_0_THRESHOLD_FREQ;
		thr.ip_evt_threshold = INPUT_EVENT_CNT_THRESHOLD;
		spin_lock_init(&(managed_clusters[i]->iowait_lock));
		spin_lock_init(&(managed_clusters[i]->mode_lock));
		spin_lock_init(&(managed_clusters[i]->timer_lock));
		spin_lock_init(&(managed_clusters[i]->perf_cl_peak_lock));
		init_timer(&managed_clusters[i]->mode_exit_timer);
		managed_clusters[i]->mode_exit_timer.function =
			single_mod_exit_timer;
		init_timer(&managed_clusters[i]->perf_cl_peak_mode_exit_timer);
		managed_clusters[i]->perf_cl_peak_mode_exit_timer.function =
			perf_cl_peak_mod_exit_timer;

	}
	ip_evts = kcalloc(1, sizeof(struct input_events), GFP_KERNEL);
	if (!ip_evts) {
		ret = -ENOMEM;
		goto error;
	}

	INIT_DELAYED_WORK(&evaluate_hotplug_work, check_cluster_status);
	mutex_init(&managed_cpus_lock);

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("msm_perf: Couldn't find module kobject\n");
		ret = -ENOENT;
		goto error;
	}
	mode_kobj = kobject_create_and_add("workload_modes", module_kobj);
	if (!mode_kobj) {
		pr_err("msm_perf: Failed to add mode_kobj\n");
		ret = -ENOMEM;
		kobject_put(module_kobj);
		goto error;
	}
	ret = sysfs_create_group(mode_kobj, &attr_group);
	if (ret) {
		pr_err("msm_perf: Failed to create sysfs\n");
		kobject_put(module_kobj);
		kobject_put(mode_kobj);
		goto error;
	}
	notify_thread = kthread_run(notify_userspace, NULL, "wrkld_notify");
	clusters_inited = true;

	return 0;

error:
	for (i = 0; i < num_clusters; i++) {
		if (!managed_clusters[i])
			break;
		if (managed_clusters[i]->offlined_cpus != NULL)
			free_cpumask_var(managed_clusters[i]->offlined_cpus);
		if (managed_clusters[i]->cpus != NULL)
			free_cpumask_var(managed_clusters[i]->cpus);
		kfree(managed_clusters[i]);
	}
	kfree(managed_clusters);
	return ret;
}

static int init_events_group(void)
{
	int ret;
	struct kobject *module_kobj;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("msm_perf: Couldn't find module kobject\n");
		return -ENOENT;
	}

	events_kobj = kobject_create_and_add("events", module_kobj);
	if (!events_kobj) {
		pr_err("msm_perf: Failed to add events_kobj\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(events_kobj, &events_attr_group);
	if (ret) {
		pr_err("msm_perf: Failed to create sysfs\n");
		return ret;
	}

	spin_lock_init(&(events_group.cpu_hotplug_lock));
	events_notify_thread = kthread_run(events_notify_userspace,
					NULL, "msm_perf:events_notify");
	if (IS_ERR(events_notify_thread))
		return PTR_ERR(events_notify_thread);

	events_group.init_success = true;

	return 0;
}

static int __init msm_performance_init(void)
{
	unsigned int cpu;

	cpufreq_register_notifier(&perf_cpufreq_nb, CPUFREQ_POLICY_NOTIFIER);
	cpufreq_register_notifier(&perf_govinfo_nb, CPUFREQ_GOVINFO_NOTIFIER);
	cpufreq_register_notifier(&perf_cputransitions_nb,
					CPUFREQ_TRANSITION_NOTIFIER);

	for_each_present_cpu(cpu)
		per_cpu(cpu_stats, cpu).max = UINT_MAX;

	register_cpu_notifier(&msm_performance_cpu_notifier);

	init_events_group();

	return 0;
}
late_initcall(msm_performance_init);
>>>>>>> b6d67ffd0210b18294cee1228f58565ed70e0a08
