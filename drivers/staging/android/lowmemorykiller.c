// SPDX-License-Identifier: GPL-2.0
/*
 * Android Adaptive Low Memory Killer.
 *
 * Minfree/oom_score_adj selection follows the Android in-kernel LMK design.
 * ALMK widens the eligible oom_score_adj range when vmpressure indicates
 * severe reclaim inefficiency.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/oom.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/vmpressure.h>
#include <linux/vmstat.h>

static short lowmem_adj[6] = { 0, 58, 352, 705 };
static int lowmem_adj_size = 4;
static unsigned int lowmem_minfree[6] = {
	3 * 512,	/* 6 MiB */
	2 * 1024,	/* 8 MiB */
	4 * 1024,	/* 16 MiB */
	16 * 1024,	/* 64 MiB */
};
static int lowmem_minfree_size = 4;
static unsigned long lowmem_deathpending_timeout;
static DEFINE_MUTEX(scan_mutex);

static atomic_t shift_adj = ATOMIC_INIT(0);
static short adj_max_shift = 353;
static int enable_adaptive_lmk = 1;
static int vmpressure_file_min = 32768; /* 128 MiB, in pages */

static int adjust_minadj(short *min_score_adj)
{
	if (!READ_ONCE(enable_adaptive_lmk))
		return 0;

	if (atomic_xchg(&shift_adj, 0) &&
	    *min_score_adj > adj_max_shift) {
		*min_score_adj = adj_max_shift;
		return 1;
	}

	return 0;
}

static int lmk_vmpressure_notifier(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	long other_free, other_file;
	int array_size = min(lowmem_adj_size, lowmem_minfree_size);

	if (!READ_ONCE(enable_adaptive_lmk))
		return NOTIFY_DONE;

	if (action >= 95) {
		atomic_set(&shift_adj, 1);
	} else if (action >= 90 && array_size > 0) {
		other_free = (long)global_zone_page_state(NR_FREE_PAGES) -
			     (long)totalreserve_pages;
		other_file = (long)global_node_page_state(NR_FILE_PAGES) -
			     (long)global_node_page_state(NR_SHMEM) -
			     (long)global_node_page_state(NR_UNEVICTABLE) -
			     (long)total_swapcache_pages();

		if (other_free < 0)
			other_free = 0;
		if (other_file < 0)
			other_file = 0;

		if (other_free < lowmem_minfree[array_size - 1] &&
		    other_file < READ_ONCE(vmpressure_file_min))
			atomic_set(&shift_adj, 1);
	} else if (action < 90) {
		atomic_set(&shift_adj, 0);
	}

	return NOTIFY_OK;
}

static struct notifier_block lmk_vmpr_nb = {
	.notifier_call = lmk_vmpressure_notifier,
};

static int test_task_flag(struct task_struct *p, int flag)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (test_tsk_thread_flag(t, flag)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	}

	return 0;
}

static int test_task_state(struct task_struct *p, int state)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (t->state & state) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	}

	return 0;
}

static unsigned long lowmem_count(struct shrinker *s,
				  struct shrink_control *sc)
{
	return global_node_page_state(NR_ACTIVE_ANON) +
	       global_node_page_state(NR_ACTIVE_FILE) +
	       global_node_page_state(NR_INACTIVE_ANON) +
	       global_node_page_state(NR_INACTIVE_FILE);
}

static unsigned long lowmem_scan(struct shrinker *s, struct shrink_control *sc)
{
	struct task_struct *tsk, *selected = NULL;
	unsigned long rem = 0;
	long other_free, other_file;
	int i, array_size;
	int selected_tasksize = 0;
	short min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	short selected_oom_score_adj = 0;

	if (!mutex_trylock(&scan_mutex))
		return 0;

	other_free = (long)global_zone_page_state(NR_FREE_PAGES) -
		     (long)totalreserve_pages;
	other_file = (long)global_node_page_state(NR_FILE_PAGES) -
		     (long)global_node_page_state(NR_SHMEM) -
		     (long)global_node_page_state(NR_UNEVICTABLE) -
		     (long)total_swapcache_pages();

	if (other_free < 0)
		other_free = 0;
	if (other_file < 0)
		other_file = 0;

	array_size = min(lowmem_adj_size, lowmem_minfree_size);
	for (i = 0; i < array_size; i++) {
		if (other_free < lowmem_minfree[i] &&
		    other_file < lowmem_minfree[i]) {
			min_score_adj = lowmem_adj[i];
			break;
		}
	}

	adjust_minadj(&min_score_adj);
	if (min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		mutex_unlock(&scan_mutex);
		return 0;
	}

	if (time_before_eq(jiffies, lowmem_deathpending_timeout)) {
		rcu_read_lock();
		for_each_process(tsk) {
			if (test_task_flag(tsk, TIF_MEMDIE)) {
				rcu_read_unlock();
				mutex_unlock(&scan_mutex);
				return 0;
			}
		}
		rcu_read_unlock();
	}

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		short oom_score_adj;
		int tasksize;

		if (tsk->flags & PF_KTHREAD)
			continue;

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		oom_score_adj = READ_ONCE(p->signal->oom_score_adj);
		if (oom_score_adj < min_score_adj) {
			task_unlock(p);
			continue;
		}

		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;

		if (selected &&
		    (oom_score_adj < selected_oom_score_adj ||
		     (oom_score_adj == selected_oom_score_adj &&
		      tasksize <= selected_tasksize)))
			continue;

		selected = p;
		selected_tasksize = tasksize;
		selected_oom_score_adj = oom_score_adj;
	}

	if (!selected) {
		rcu_read_unlock();
		mutex_unlock(&scan_mutex);
		return 0;
	}

	if (test_task_flag(selected, TIF_MEMDIE) &&
	    test_task_state(selected, TASK_UNINTERRUPTIBLE)) {
		rcu_read_unlock();
		mutex_unlock(&scan_mutex);
		return 0;
	}

	task_lock(selected);
	get_task_struct(selected);
	send_sig(SIGKILL, selected, 0);
	task_unlock(selected);

	lowmem_deathpending_timeout = jiffies + HZ;
	rem = selected_tasksize;
	rcu_read_unlock();

	pr_info("lowmemorykiller: killed %s (%d), adj %hd, rss %d pages, threshold %hd\n",
		selected->comm, selected->pid, selected_oom_score_adj,
		selected_tasksize, min_score_adj);
	msleep_interruptible(20);
	put_task_struct(selected);
	mutex_unlock(&scan_mutex);

	return rem;
}

static struct shrinker lowmem_shrinker = {
	.scan_objects = lowmem_scan,
	.count_objects = lowmem_count,
	.seeks = DEFAULT_SEEKS * 16,
};

static int __init lowmem_init(void)
{
	int ret;

	ret = register_shrinker(&lowmem_shrinker);
	if (ret)
		return ret;

	ret = vmpressure_notifier_register(&lmk_vmpr_nb);
	if (ret)
		unregister_shrinker(&lowmem_shrinker);

	return ret;
}

static void __exit lowmem_exit(void)
{
	vmpressure_notifier_unregister(&lmk_vmpr_nb);
	unregister_shrinker(&lowmem_shrinker);
}

module_param_array_named(adj, lowmem_adj, short, &lowmem_adj_size, 0644);
module_param_array_named(minfree, lowmem_minfree, uint,
			 &lowmem_minfree_size, 0644);
module_param_named(enable_adaptive_lmk, enable_adaptive_lmk, int, 0644);
module_param_named(adj_max_shift, adj_max_shift, short, 0644);
module_param_named(vmpressure_file_min, vmpressure_file_min, int, 0644);
module_param_named(cost, lowmem_shrinker.seeks, int, 0644);

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Android Adaptive Low Memory Killer");
