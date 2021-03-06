#include <linux/cgroup.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/reciprocal_div.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

#include <trace/events/sched.h>

#include "sched.h"

#if MET_SCHED_DEBUG
#include <mt-plat/met_drv.h>
#endif

unsigned int sysctl_sched_cfs_boost __read_mostly;

/*
 * System energy normalization constants
 */
static struct target_nrg {
	unsigned long min_power;
	unsigned long max_power;
	struct reciprocal_value rdiv;
} schedtune_target_nrg;

/* Performance Boost region (B) threshold params */
static int perf_boost_idx;

/* Performance Constraint region (C) threshold params */
static int perf_constrain_idx;

/**
 * Performance-Energy (P-E) Space thresholds constants
 */
struct threshold_params {
	int nrg_gain;
	int cap_gain;
};

/*
 * System specific P-E space thresholds constants
 */
static struct threshold_params
threshold_gains[] = {
	{ 0, 5 }, /*   < 10% */
	{ 1, 5 }, /*   < 20% */
	{ 2, 5 }, /*   < 30% */
	{ 3, 5 }, /*   < 40% */
	{ 4, 5 }, /*   < 50% */
	{ 5, 4 }, /*   < 60% */
	{ 5, 3 }, /*   < 70% */
	{ 5, 2 }, /*   < 80% */
	{ 5, 1 }, /*   < 90% */
	{ 5, 0 }  /* <= 100% */
};

static int
__schedtune_accept_deltas(int nrg_delta, int cap_delta,
			  int perf_boost_idx, int perf_constrain_idx)
{
	int payoff = -INT_MAX;
	int gain_idx = -1;
	int region = 0;

	/* Performance Boost (B) region */
	if (nrg_delta >= 0 && cap_delta > 0) {
		gain_idx = perf_boost_idx;
		region = 8;
	}
	/* Performance Constraint (C) region */
	else if (nrg_delta < 0 && cap_delta <= 0) {
		gain_idx = perf_constrain_idx;
		region = 6;
	}

	/* Default: reject schedule candidate */
	if (gain_idx == -1)
		return payoff;

	/*
	 * Evaluate "Performance Boost" vs "Energy Increase"
	 *
	 * - Performance Boost (B) region
	 *
	 *   Condition: nrg_delta > 0 && cap_delta > 0
	 *   Payoff criteria:
	 *     cap_gain / nrg_gain  < cap_delta / nrg_delta =
	 *     cap_gain * nrg_delta < cap_delta * nrg_gain
	 *   Note that since both nrg_gain and nrg_delta are positive, the
	 *   inequality does not change. Thus:
	 *
	 *     payoff = (cap_delta * nrg_gain) - (cap_gain * nrg_delta)
	 *
	 * - Performance Constraint (C) region
	 *
	 *   Condition: nrg_delta < 0 && cap_delta < 0
	 *   payoff criteria:
	 *     cap_gain / nrg_gain  > cap_delta / nrg_delta =
	 *     cap_gain * nrg_delta < cap_delta * nrg_gain
	 *   Note that since nrg_gain > 0 while nrg_delta < 0, the
	 *   inequality change. Thus:
	 *
	 *     payoff = (cap_delta * nrg_gain) - (cap_gain * nrg_delta)
	 *
	 * This means that, in case of same positive defined {cap,nrg}_gain
	 * for both the B and C regions, we can use the same payoff formula
	 * where a positive value represents the accept condition.
	 */
	payoff  = cap_delta * threshold_gains[gain_idx].nrg_gain;
	payoff -= nrg_delta * threshold_gains[gain_idx].cap_gain;

	trace_sched_tune_filter(
				nrg_delta, cap_delta,
				threshold_gains[gain_idx].nrg_gain,
				threshold_gains[gain_idx].cap_gain,
				payoff, region);

	return payoff;
}

#if MET_SCHED_DEBUG
static
void met_stune_boost(int idx, int boost)
{
	char boost_str[64] = {0};

	snprintf(boost_str, sizeof(boost_str), "sched_boost_idx%d", idx);
	met_tag_oneshot(0, boost_str, boost);
}
#endif

#ifdef CONFIG_CGROUP_SCHEDTUNE

/*
 * EAS scheduler tunables for task groups.
 */

/* SchdTune tunables for a group of tasks */
struct schedtune {
	/* SchedTune CGroup subsystem */
	struct cgroup_subsys_state css;

	/* Boost group allocated ID */
	int idx;

	/* Boost value for tasks on that SchedTune CGroup */
	int boost;

	/* Performance Boost (B) region threshold params */
	int perf_boost_idx;

	/* Performance Constraint (C) region threshold params */
	int perf_constrain_idx;
};

static inline struct schedtune *css_st(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct schedtune, css) : NULL;
}

static inline struct schedtune *task_schedtune(struct task_struct *tsk)
{
	return css_st(task_css(tsk, schedtune_cgrp_id));
}

static inline struct schedtune *parent_st(struct schedtune *st)
{
	return css_st(st->css.parent);
}

/*
 * SchedTune root control group
 * The root control group is used to defined a system-wide boosting tuning,
 * which is applied to all tasks in the system.
 * Task specific boost tuning could be specified by creating and
 * configuring a child control group under the root one.
 * By default, system-wide boosting is disabled, i.e. no boosting is applied
 * to tasks which are not into a child control group.
 */
static struct schedtune
root_schedtune = {
	.boost	= 0,
	.perf_boost_idx = 0,
	.perf_constrain_idx = 0,
};

int
schedtune_accept_deltas(int nrg_delta, int cap_delta,
			struct task_struct *task)
{
	struct schedtune *ct;
	int perf_boost_idx;
	int perf_constrain_idx;

	/* Optimal (O) region */
	if (nrg_delta < 0 && cap_delta > 0) {
		trace_sched_tune_filter(nrg_delta, cap_delta, 0, 0, 1, 0);
		return INT_MAX;
	}

	/* Suboptimal (S) region */
	if (nrg_delta > 0 && cap_delta < 0) {
		trace_sched_tune_filter(nrg_delta, cap_delta, 0, 0, -1, 5);
		return -INT_MAX;
	}

	/* Get task specific perf Boost/Constraints indexes */
	rcu_read_lock();
	ct = task_schedtune(task);
	perf_boost_idx = ct->perf_boost_idx;
	perf_constrain_idx = ct->perf_constrain_idx;
	rcu_read_unlock();

	return __schedtune_accept_deltas(nrg_delta, cap_delta,
			perf_boost_idx, perf_constrain_idx);
}

/*
 * Maximum number of boost groups to support
 * When per-task boosting is used we still allow only limited number of
 * boost groups for two main reasons:
 * 1. on a real system we usually have only few classes of workloads which
 *    make sense to boost with different values (e.g. background vs foreground
 *    tasks, interactive vs low-priority tasks)
 * 2. a limited number allows for a simpler and more memory/time efficient
 *    implementation especially for the computation of the per-CPU boost
 *    value
 */
#define BOOSTGROUPS_COUNT 4

/* Array of configured boostgroups */
static struct schedtune *allocated_group[BOOSTGROUPS_COUNT] = {
	&root_schedtune,
	NULL,
};

/* SchedTune boost groups
 * Keep track of all the boost groups which impact on CPU, for example when a
 * CPU has two RUNNABLE tasks belonging to two different boost groups and thus
 * likely with different boost values.
 * Since on each system we expect only a limited number of boost groups, here
 * we use a simple array to keep track of the metrics required to compute the
 * maximum per-CPU boosting value.
 */
struct boost_groups {
	/* Maximum boost value for all RUNNABLE tasks on a CPU */
	unsigned boost_max;
	struct {
		/* The boost for tasks on that boost group */
		unsigned boost;
		/* Count of RUNNABLE tasks on that boost group */
		unsigned tasks;
	} group[BOOSTGROUPS_COUNT];
};

/* Boost groups affecting each CPU in the system */
DEFINE_PER_CPU(struct boost_groups, cpu_boost_groups);

static void
schedtune_cpu_update(int cpu)
{
	struct boost_groups *bg;
	unsigned boost_max;
	int idx;

	bg = &per_cpu(cpu_boost_groups, cpu);

	/* The root boost group is always active */
	boost_max = bg->group[0].boost;
	for (idx = 1; idx < BOOSTGROUPS_COUNT; ++idx) {
		/*
		 * A boost group affects a CPU only if it has
		 * RUNNABLE tasks on that CPU
		 */
		if (bg->group[idx].tasks == 0)
			continue;
		boost_max = max(boost_max, bg->group[idx].boost);
	}

	bg->boost_max = boost_max;
}

static int
schedtune_boostgroup_update(int idx, int boost)
{
	struct boost_groups *bg;
	int cur_boost_max;
	int old_boost;
	int cpu;

	/* Update per CPU boost groups */
	for_each_possible_cpu(cpu) {
		bg = &per_cpu(cpu_boost_groups, cpu);

		/*
		 * Keep track of current boost values to compute the per CPU
		 * maximum only when it has been affected by the new value of
		 * the updated boost group
		 */
		cur_boost_max = bg->boost_max;
		old_boost = bg->group[idx].boost;

		/* Update the boost value of this boost group */
		bg->group[idx].boost = boost;

		/* Check if this update increase current max */
		if (boost > cur_boost_max && bg->group[idx].tasks) {
			bg->boost_max = boost;
			trace_sched_tune_boostgroup_update(cpu, 1, bg->boost_max);
			continue;
		}

		/* Check if this update has decreased current max */
		if (cur_boost_max == old_boost && old_boost > boost) {
			schedtune_cpu_update(cpu);
			trace_sched_tune_boostgroup_update(cpu, -1, bg->boost_max);
			continue;
		}

		trace_sched_tune_boostgroup_update(cpu, 0, bg->boost_max);
	}

	return 0;
}

#define ENQUEUE_TASK  1
#define DEQUEUE_TASK -1

static inline void
schedtune_tasks_update(struct task_struct *p, int cpu, int idx, int task_count)
{
	struct boost_groups *bg = &per_cpu(cpu_boost_groups, cpu);
	int tasks = bg->group[idx].tasks + task_count;

	/* Update boosted tasks count while avoiding to make it negative */
	bg->group[idx].tasks = max(0, tasks);

	trace_sched_tune_tasks_update(p, cpu, tasks, idx,
			bg->group[idx].boost, bg->boost_max);

	/* Boost group activation or deactivation on that RQ */
	if (tasks == 1 || tasks == 0)
		schedtune_cpu_update(cpu);
}

/*
 * NOTE: This function must be called while holding the lock on the CPU RQ
 */
void schedtune_enqueue_task(struct task_struct *p, int cpu)
{
	struct schedtune *st;
	int idx;

	/*
	 * When a task is marked PF_EXITING by do_exit() it's going to be
	 * dequeued and enqueued multiple times in the exit path.
	 * Thus we avoid any further update, since we do not want to change
	 * CPU boosting while the task is exiting.
	 */
	if (p->flags & PF_EXITING)
		return;

	/* Get task boost group */
	rcu_read_lock();
	st = task_schedtune(p);
	idx = st->idx;
	rcu_read_unlock();

	schedtune_tasks_update(p, cpu, idx, ENQUEUE_TASK);
}

/*
 * NOTE: This function must be called while holding the lock on the CPU RQ
 */
void schedtune_dequeue_task(struct task_struct *p, int cpu)
{
	struct schedtune *st;
	int idx;

	/*
	 * When a task is marked PF_EXITING by do_exit() it's going to be
	 * dequeued and enqueued multiple times in the exit path.
	 * Thus we avoid any further update, since we do not want to change
	 * CPU boosting while the task is exiting.
	 * The last dequeue will be done by cgroup exit() callback.
	 */
	if (p->flags & PF_EXITING)
		return;

	/* Get task boost group */
	rcu_read_lock();
	st = task_schedtune(p);
	idx = st->idx;
	rcu_read_unlock();

	schedtune_tasks_update(p, cpu, idx, DEQUEUE_TASK);
}

int schedtune_cpu_boost(int cpu)
{
	struct boost_groups *bg;

	bg = &per_cpu(cpu_boost_groups, cpu);
	return bg->boost_max;
}

int schedtune_task_boost(struct task_struct *p)
{
	struct schedtune *st;
	int task_boost;

	/* Get task boost value */
	rcu_read_lock();
	st = task_schedtune(p);
	task_boost = st->boost;
	rcu_read_unlock();

	return task_boost;
}

int boost_value_for_GED_pid(int pid, int boost_value)
{
	struct task_struct *boost_task;
	struct schedtune *ct;
	unsigned threshold_idx;
	int boost_pct;

	if (boost_value < 0 || boost_value > 100)
		printk_deferred("warning: GED boost value should be 0~100\n");

	if (boost_value < 0)
		boost_value = 0;

	if (boost_value >= 100)
		boost_value = 99;

	rcu_read_lock();

	boost_task = find_task_by_vpid(pid);
	if (boost_task) {
		ct = task_schedtune(boost_task);

		if (ct->idx == 0) {
			printk_deferred("error: don't boost GED task at root idx=%d, pid:%d\n", ct->idx, pid);
			rcu_read_unlock();
			return -EINVAL;
		}

		boost_pct = boost_value;

		/*
		 * Update threshold params for Performance Boost (B)
		 * and Performance Constraint (C) regions.
		 * The current implementatio uses the same cuts for both
		 * B and C regions.
		 */
		threshold_idx = clamp(boost_pct, 0, 99) / 10;
		ct->perf_boost_idx = threshold_idx;
		ct->perf_constrain_idx = threshold_idx;

		ct->boost = boost_value;

		/* Update CPU boost */
		schedtune_boostgroup_update(ct->idx, ct->boost);
	} else {
		printk_deferred("error: GED task no exist: pid=%d, boost=%d\n", pid, boost_value);
		rcu_read_unlock();
		return -EINVAL;
	}

	trace_sched_tune_config(ct->boost);

#if MET_SCHED_DEBUG
	met_stune_boost(ct->idx, ct->boost);
#endif

	rcu_read_unlock();
	return 0;
}

int boost_value_for_GED_idx(int group_idx, int boost_value)
{
	struct schedtune *ct;
	unsigned threshold_idx;
	int boost_pct;

	if (group_idx == 0) {
		printk_deferred("error: don't boost GED task at root: idx=%d\n", group_idx);
		return -EINVAL;
	}

	if (boost_value < 0 || boost_value > 100)
		printk_deferred("warning: GED boost value should be 0~100\n");

	if (boost_value < 0)
		boost_value = 0;

	if (boost_value >= 100)
		boost_value = 99;

	ct = allocated_group[group_idx];

	if (ct) {
		rcu_read_lock();

		boost_pct = boost_value;

		/*
		 * Update threshold params for Performance Boost (B)
		 * and Performance Constraint (C) regions.
		 * The current implementatio uses the same cuts for both
		 * B and C regions.
		 */
		threshold_idx = clamp(boost_pct, 0, 99) / 10;
		ct->perf_boost_idx = threshold_idx;
		ct->perf_constrain_idx = threshold_idx;

		ct->boost = boost_value;

		/* Update CPU boost */
		schedtune_boostgroup_update(ct->idx, ct->boost);
		rcu_read_unlock();

	} else {
		printk_deferred("error: GED boost for stune group no exist: idx=%d\n", group_idx);
		return -EINVAL;
	}

	trace_sched_tune_config(ct->boost);

#if MET_SCHED_DEBUG
	met_stune_boost(ct->idx, ct->boost);
#endif

	return 0;
}

int group_boost_read(int group_idx)
{
	struct schedtune *ct;
	int boost = 0;

	ct = allocated_group[group_idx];
	if (ct) {
		rcu_read_lock();
		boost = ct->boost;
		rcu_read_unlock();
	}

	return boost;
}
EXPORT_SYMBOL(group_boost_read);

/* mtk: a linear bosot value for tuning */
int linear_real_boost(int linear_boost)
{
	int target_cpu, usage;
	int boost;

	sched_max_util_task(&target_cpu, NULL, &usage, NULL);

	/* margin = (usage*linear_boost)/100; */
	/* (original_cap - usage)*boost/100 = margin; */
	boost = (usage*linear_boost)/(capacity_orig_of(target_cpu) - usage);

#if 0
	printk_deferred("Michael: (%d->%d) target_cpu=%d orig_cap=%ld usage=%ld\n",
			linear_boost, boost, target_cpu, capacity_orig_of(target_cpu), usage);
#endif

	return boost;
}
EXPORT_SYMBOL(linear_real_boost);

/* mtk: a linear bosot value for tuning */
int linear_real_boost_pid(int linear_boost, int pid)
{
	struct task_struct *boost_task = find_task_by_vpid(pid);
	int target_cpu;
	unsigned long usage;
	int boost;

	if (!boost_task)
		return linear_real_boost(linear_boost);

	usage = boost_task->se.avg.util_avg;
	target_cpu = task_cpu(boost_task);

	boost = (usage*linear_boost)/(capacity_orig_of(target_cpu) - usage);

#if 0
	printk_deferred("Michael2: (%d->%d) target_cpu=%d orig_cap=%ld usage=%ld pid=%d\n",
			linear_boost, boost, target_cpu, capacity_orig_of(target_cpu), usage, pid);
#endif

	return boost;
}
EXPORT_SYMBOL(linear_real_boost_pid);

static u64
boost_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->boost;
}

static int
boost_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 boost)
{
	struct schedtune *st = css_st(css);
	unsigned threshold_idx;
	int boost_pct;

	if (boost < 0 || boost > 100)
		return -EINVAL;
	boost_pct = boost;

	/*
	 * Update threshold params for Performance Boost (B)
	 * and Performance Constraint (C) regions.
	 * The current implementatio uses the same cuts for both
	 * B and C regions.
	 */
	threshold_idx = clamp(boost_pct, 0, 99) / 10;
	st->perf_boost_idx = threshold_idx;
	st->perf_constrain_idx = threshold_idx;

	st->boost = boost;
	if (css == &root_schedtune.css) {
		sysctl_sched_cfs_boost = boost;
		perf_boost_idx  = threshold_idx;
		perf_constrain_idx  = threshold_idx;
	}

	/* Update CPU boost */
	schedtune_boostgroup_update(st->idx, st->boost);

	trace_sched_tune_config(st->boost);

#if MET_SCHED_DEBUG
	met_stune_boost(st->idx, st->boost);
#endif

	return 0;
}

static struct cftype files[] = {
	{
		.name = "boost",
		.read_u64 = boost_read,
		.write_u64 = boost_write,
	},
	{ }	/* terminate */
};

static int
schedtune_boostgroup_init(struct schedtune *st)
{
	struct boost_groups *bg;
	int cpu;

	/* Keep track of allocated boost groups */
	allocated_group[st->idx] = st;

	/* Initialize the per CPU boost groups */
	for_each_possible_cpu(cpu) {
		bg = &per_cpu(cpu_boost_groups, cpu);
		bg->group[st->idx].boost = 0;
		bg->group[st->idx].tasks = 0;
	}

	return 0;
}

static struct cgroup_subsys_state *
schedtune_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct schedtune *st;
	int idx;

	if (!parent_css)
		return &root_schedtune.css;

	/* Allow only single level hierachies */
	if (parent_css != &root_schedtune.css) {
		pr_err("Nested SchedTune boosting groups not allowed\n");
		return ERR_PTR(-ENOMEM);
	}

	/* Allow only a limited number of boosting groups */
	for (idx = 1; idx < BOOSTGROUPS_COUNT; ++idx)
		if (!allocated_group[idx])
			break;
	if (idx == BOOSTGROUPS_COUNT) {
		pr_err("Trying to create more than %d SchedTune boosting groups\n",
		       BOOSTGROUPS_COUNT);
		return ERR_PTR(-ENOSPC);
	}

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		goto out;

	/* Initialize per CPUs boost group support */
	st->idx = idx;
	if (schedtune_boostgroup_init(st))
		goto release;

	return &st->css;

release:
	kfree(st);
out:
	return ERR_PTR(-ENOMEM);
}

static void
schedtune_boostgroup_release(struct schedtune *st)
{
	/* Reset this boost group */
	schedtune_boostgroup_update(st->idx, 0);

	/* Keep track of allocated boost groups */
	allocated_group[st->idx] = NULL;
}

static void
schedtune_css_free(struct cgroup_subsys_state *css)
{
	struct schedtune *st = css_st(css);

	schedtune_boostgroup_release(st);
	kfree(st);
}

struct cgroup_subsys schedtune_cgrp_subsys = {
	.css_alloc	= schedtune_css_alloc,
	.css_free	= schedtune_css_free,
	.legacy_cftypes	= files,
	.early_init	= 1,
};

static inline void
schedtune_init_cgroups(void)
{
	struct boost_groups *bg;
	int cpu;

	/* Initialize the per CPU boost groups */
	for_each_possible_cpu(cpu) {
		bg = &per_cpu(cpu_boost_groups, cpu);
		memset(bg, 0, sizeof(struct boost_groups));
	}

	pr_info("schedtune: configured to support %d boost groups\n",
		BOOSTGROUPS_COUNT);
}

#else /* CONFIG_CGROUP_SCHEDTUNE */

int
schedtune_accept_deltas(int nrg_delta, int cap_delta,
			struct task_struct *task)
{
	/* Optimal (O) region */
	if (nrg_delta < 0 && cap_delta > 0) {
		trace_sched_tune_filter(nrg_delta, cap_delta, 0, 0, 1, 0);
		return INT_MAX;
	}

	/* Suboptimal (S) region */
	if (nrg_delta > 0 && cap_delta < 0) {
		trace_sched_tune_filter(nrg_delta, cap_delta, 0, 0, -1, 5);
		return -INT_MAX;
	}

	return __schedtune_accept_deltas(nrg_delta, cap_delta,
			perf_boost_idx, perf_constrain_idx);
}

#endif /* CONFIG_CGROUP_SCHEDTUNE */

int
sysctl_sched_cfs_boost_handler(struct ctl_table *table, int write,
			       void __user *buffer, size_t *lenp,
			       loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	unsigned threshold_idx;
	int boost_pct;

	if (ret || !write)
		return ret;

	if (sysctl_sched_cfs_boost < -100 || sysctl_sched_cfs_boost > 100)
		return -EINVAL;
	boost_pct = sysctl_sched_cfs_boost;

	/*
	 * Update threshold params for Performance Boost (B)
	 * and Performance Constraint (C) regions.
	 * The current implementatio uses the same cuts for both
	 * B and C regions.
	 */
	threshold_idx = clamp(boost_pct, 0, 99) / 10;
	perf_boost_idx = threshold_idx;
	perf_constrain_idx = threshold_idx;

	return 0;
}

/*
 * System energy normalization
 * Returns the normalized value, in the range [0..SCHED_LOAD_SCALE],
 * corresponding to the specified energy variation.
 */
int
schedtune_normalize_energy(int energy_diff)
{
	u32 normalized_nrg;
	int max_delta;

#ifdef CONFIG_SCHED_DEBUG
	/* Check for boundaries */
	max_delta  = schedtune_target_nrg.max_power;
	max_delta -= schedtune_target_nrg.min_power;
	WARN_ON(abs(energy_diff) >= max_delta);
#endif

	/* Do scaling using positive numbers to increase the range */
	normalized_nrg = (energy_diff < 0) ? -energy_diff : energy_diff;

	/* Scale by energy magnitude */
	normalized_nrg <<= SCHED_LOAD_SHIFT;

	/* Normalize on max energy for target platform */
	normalized_nrg = reciprocal_divide(
			normalized_nrg, schedtune_target_nrg.rdiv);

	return (energy_diff < 0) ? -normalized_nrg : normalized_nrg;
}

#ifdef CONFIG_SCHED_DEBUG
static void
schedtune_test_nrg(unsigned long delta_pwr)
{
	unsigned long test_delta_pwr;
	unsigned long test_norm_pwr;
	int idx;

	/*
	 * Check normalization constants using some constant system
	 * energy values
	 */
	pr_info("schedtune: verify normalization constants...\n");
	for (idx = 0; idx < 6; ++idx) {
		test_delta_pwr = delta_pwr >> idx;

		/* Normalize on max energy for target platform */
		test_norm_pwr = reciprocal_divide(
					test_delta_pwr << SCHED_LOAD_SHIFT,
					schedtune_target_nrg.rdiv);

		pr_info("schedtune: max_pwr/2^%d: %4lu => norm_pwr: %5lu\n",
			idx, test_delta_pwr, test_norm_pwr);
	}
}
#else
#define schedtune_test_nrg(delta_pwr)
#endif

#ifndef CONFIG_MTK_ACAO
/*
 * mtk: Because system only eight cores online when init, we compute
 * the min/max power consumption of all possible clusters and CPUs.
 */
static void
schedtune_add_cluster_nrg_hotplug(struct target_nrg *ste, struct sched_group *sg)
{
	struct cpumask *cluster_cpus;
	char str[32];
	unsigned long min_pwr;
	unsigned long max_pwr;
	const struct sched_group_energy *cluster_energy, *core_energy;
	int cpu;
	int cluster_id = 0;
	int i = 0;
	int cluster_first_cpu = 0;

	cluster_cpus = sched_group_cpus(sg);

	/* Get num of all clusters */
	cluster_id = arch_get_nr_clusters();
	for (i = 0; i < cluster_id ; i++) {
		arch_get_cluster_cpus(cluster_cpus, i);
		cluster_first_cpu = cpumask_first(cluster_cpus);

		snprintf(str, 32, "CLUSTER[%*pbl]",
			cpumask_pr_args(cluster_cpus));

		/* Get Cluster energy using EM data of first CPU in this cluster */
		cluster_energy = cpu_cluster_energy(cluster_first_cpu);
		min_pwr = cluster_energy->idle_states[cluster_energy->nr_idle_states - 1].power;
		max_pwr = cluster_energy->cap_states[cluster_energy->nr_cap_states - 1].dyn_pwr;
		pr_info("schedtune: %-17s min_pwr: %5lu max_pwr: %5lu\n",
			str, min_pwr, max_pwr);

		ste->min_power += min_pwr;
		ste->max_power += max_pwr;

		/* Get CPU energy using EM data for each CPU in this cluster */
		for_each_cpu(cpu, cluster_cpus) {
			core_energy = cpu_core_energy(cpu);
			min_pwr = core_energy->idle_states[core_energy->nr_idle_states - 1].power;
			max_pwr = core_energy->cap_states[core_energy->nr_cap_states - 1].dyn_pwr;

			ste->min_power += min_pwr;
			ste->max_power += max_pwr;

			snprintf(str, 32, "CPU[%d]", cpu);
			pr_info("schedtune: %-17s min_pwr: %5lu max_pwr: %5lu\n",
				str, min_pwr, max_pwr);
		}
	}
}
#else
/*
 * Compute the min/max power consumption of a cluster and all its CPUs
 */
static void
schedtune_add_cluster_nrg(
		struct sched_domain *sd,
		struct sched_group *sg,
		struct target_nrg *ste)
{
	struct sched_domain *sd2;
	struct sched_group *sg2;

	struct cpumask *cluster_cpus;
	char str[32];

	unsigned long min_pwr;
	unsigned long max_pwr;
	int cpu;

	/* Get Cluster energy using EM data for the first CPU */
	cluster_cpus = sched_group_cpus(sg);
	snprintf(str, 32, "CLUSTER[%*pbl]",
		 cpumask_pr_args(cluster_cpus));

	min_pwr = sg->sge->idle_states[sg->sge->nr_idle_states - 1].power;
	max_pwr = sg->sge->cap_states[sg->sge->nr_cap_states - 1].dyn_pwr;
	pr_info("schedtune: %-17s min_pwr: %5lu max_pwr: %5lu\n",
		str, min_pwr, max_pwr);

	/*
	 * Keep track of this cluster's energy in the computation of the
	 * overall system energy
	 */
	ste->min_power += min_pwr;
	ste->max_power += max_pwr;

	/* Get CPU energy using EM data for each CPU in the group */
	for_each_cpu(cpu, cluster_cpus) {
		/* Get a SD view for the specific CPU */
		for_each_domain(cpu, sd2) {
			/* Get the CPU group */
			sg2 = sd2->groups;
			min_pwr = sg2->sge->idle_states[sg2->sge->nr_idle_states - 1].power;
			max_pwr = sg2->sge->cap_states[sg2->sge->nr_cap_states - 1].dyn_pwr;

			ste->min_power += min_pwr;
			ste->max_power += max_pwr;

			snprintf(str, 32, "CPU[%d]", cpu);
			pr_info("schedtune: %-17s min_pwr: %5lu max_pwr: %5lu\n",
				str, min_pwr, max_pwr);

			/*
			 * Assume we have EM data only at the CPU and
			 * the upper CLUSTER level
			 */
			BUG_ON(!cpumask_equal(
				sched_group_cpus(sg),
				sched_group_cpus(sd2->parent->groups)
				));
			break;
		}
	}
}
#endif /* !define CONFIG_MTK_ACAO */


/*
 * Initialize the constants required to compute normalized energy.
 * The values of these constants depends on the EM data for the specific
 * target system and topology.
 * Thus, this function is expected to be called by the code
 * that bind the EM to the topology information.
 */
static int
schedtune_init(void)
{
	struct target_nrg *ste = &schedtune_target_nrg;
	unsigned long delta_pwr = 0;
	struct sched_domain *sd;
	struct sched_group *sg;

	pr_info("schedtune: init normalization constants...\n");
	ste->max_power = 0;
	ste->min_power = 0;

	rcu_read_lock();

	/*
	 * When EAS is in use, we always have a pointer to the highest SD
	 * which provides EM data.
	 */
	sd = rcu_dereference(per_cpu(sd_ea, cpumask_first(cpu_online_mask)));
	if (!sd) {
		pr_info("schedtune: no energy model data\n");
		goto nodata;
	}

	sg = sd->groups;
#ifndef CONFIG_MTK_ACAO
	/* mtk: compute max_power & min_power of all possible cores, not only online cores. */
	schedtune_add_cluster_nrg_hotplug(ste, sg);
#else
	do {
		schedtune_add_cluster_nrg(sd, sg, ste);
	} while (sg = sg->next, sg != sd->groups);
#endif
	rcu_read_unlock();

	pr_info("schedtune: %-17s min_pwr: %5lu max_pwr: %5lu\n",
		"SYSTEM", ste->min_power, ste->max_power);

	/* Compute normalization constants */
	delta_pwr = ste->max_power - ste->min_power;
	ste->rdiv = reciprocal_value(delta_pwr);
	pr_info("schedtune: using normalization constants mul: %u sh1: %u sh2: %u\n",
		ste->rdiv.m, ste->rdiv.sh1, ste->rdiv.sh2);

	schedtune_test_nrg(delta_pwr);

#ifdef CONFIG_CGROUP_SCHEDTUNE
	schedtune_init_cgroups();
#else
	pr_info("schedtune: configured to support global boosting only\n");
#endif

	return 0;

nodata:
	pr_warn("schedtune: disabled!\n");
	rcu_read_unlock();
	return -EINVAL;
}
late_initcall(schedtune_init);

