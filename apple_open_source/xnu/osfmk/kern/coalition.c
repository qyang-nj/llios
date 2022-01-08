/*
 * Copyright (c) 2019-2020 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#include <kern/kern_types.h>
#include <mach/mach_types.h>
#include <mach/boolean.h>

#include <kern/coalition.h>
#include <kern/exc_resource.h>
#include <kern/host.h>
#include <kern/ledger.h>
#include <kern/mach_param.h> /* for TASK_CHUNK */
#if MONOTONIC
#include <kern/monotonic.h>
#endif /* MONOTONIC */
#include <kern/policy_internal.h>
#include <kern/task.h>
#include <kern/thread_group.h>
#include <kern/zalloc.h>

#include <libkern/OSAtomic.h>

#include <mach/coalition_notification_server.h>
#include <mach/host_priv.h>
#include <mach/host_special_ports.h>

#include <os/log.h>

#include <sys/errno.h>

/*
 * BSD interface functions
 */
int coalitions_get_list(int type, struct procinfo_coalinfo *coal_list, int list_sz);
coalition_t task_get_coalition(task_t task, int type);
boolean_t coalition_is_leader(task_t task, coalition_t coal);
task_t coalition_get_leader(coalition_t coal);
int coalition_get_task_count(coalition_t coal);
uint64_t coalition_get_page_count(coalition_t coal, int *ntasks);
int coalition_get_pid_list(coalition_t coal, uint32_t rolemask, int sort_order,
    int *pid_list, int list_sz);

/* defined in task.c */
extern ledger_template_t task_ledger_template;

/*
 * Templates; task template is copied due to potential allocation limits on
 * task ledgers.
 */
ledger_template_t coalition_task_ledger_template = NULL;
ledger_template_t coalition_ledger_template = NULL;

extern int      proc_selfpid(void);
/*
 * Coalition zone needs limits. We expect there will be as many coalitions as
 * tasks (same order of magnitude), so use the task zone's limits.
 * */
#define CONFIG_COALITION_MAX CONFIG_TASK_MAX
#define COALITION_CHUNK TASK_CHUNK

int unrestrict_coalition_syscalls;
int merge_adaptive_coalitions;

LCK_GRP_DECLARE(coalitions_lck_grp, "coalition");

/* coalitions_list_lock protects coalition_count, coalitions queue, next_coalition_id. */
static LCK_RW_DECLARE(coalitions_list_lock, &coalitions_lck_grp);
static uint64_t coalition_count;
static uint64_t coalition_next_id = 1;
static queue_head_t coalitions_q;

coalition_t init_coalition[COALITION_NUM_TYPES];
coalition_t corpse_coalition[COALITION_NUM_TYPES];

static const char *
coal_type_str(int type)
{
	switch (type) {
	case COALITION_TYPE_RESOURCE:
		return "RESOURCE";
	case COALITION_TYPE_JETSAM:
		return "JETSAM";
	default:
		return "<unknown>";
	}
}

struct coalition_type {
	int type;
	int has_default;
	/*
	 * init
	 * pre-condition: coalition just allocated (unlocked), unreferenced,
	 *                type field set
	 */
	kern_return_t (*init)(coalition_t coal, boolean_t privileged);

	/*
	 * dealloc
	 * pre-condition: coalition unlocked
	 * pre-condition: coalition refcount=0, active_count=0,
	 *                termrequested=1, terminated=1, reaped=1
	 */
	void          (*dealloc)(coalition_t coal);

	/*
	 * adopt_task
	 * pre-condition: coalition locked
	 * pre-condition: coalition !repead and !terminated
	 */
	kern_return_t (*adopt_task)(coalition_t coal, task_t task);

	/*
	 * remove_task
	 * pre-condition: coalition locked
	 * pre-condition: task has been removed from coalition's task list
	 */
	kern_return_t (*remove_task)(coalition_t coal, task_t task);

	/*
	 * set_taskrole
	 * pre-condition: coalition locked
	 * pre-condition: task added to coalition's task list,
	 *                active_count >= 1 (at least the given task is active)
	 */
	kern_return_t (*set_taskrole)(coalition_t coal, task_t task, int role);

	/*
	 * get_taskrole
	 * pre-condition: coalition locked
	 * pre-condition: task added to coalition's task list,
	 *                active_count >= 1 (at least the given task is active)
	 */
	int (*get_taskrole)(coalition_t coal, task_t task);

	/*
	 * iterate_tasks
	 * pre-condition: coalition locked
	 */
	void (*iterate_tasks)(coalition_t coal, void *ctx, void (*callback)(coalition_t, void *, task_t));
};

/*
 * COALITION_TYPE_RESOURCE
 */

static kern_return_t i_coal_resource_init(coalition_t coal, boolean_t privileged);
static void          i_coal_resource_dealloc(coalition_t coal);
static kern_return_t i_coal_resource_adopt_task(coalition_t coal, task_t task);
static kern_return_t i_coal_resource_remove_task(coalition_t coal, task_t task);
static kern_return_t i_coal_resource_set_taskrole(coalition_t coal,
    task_t task, int role);
static int           i_coal_resource_get_taskrole(coalition_t coal, task_t task);
static void          i_coal_resource_iterate_tasks(coalition_t coal, void *ctx,
    void (*callback)(coalition_t, void *, task_t));

/*
 * Ensure COALITION_NUM_THREAD_QOS_TYPES defined in mach/coalition.h still
 * matches THREAD_QOS_LAST defined in mach/thread_policy.h
 */
static_assert(COALITION_NUM_THREAD_QOS_TYPES == THREAD_QOS_LAST);

struct i_resource_coalition {
	/*
	 * This keeps track of resource utilization of tasks that are no longer active
	 * in the coalition and is updated when a task is removed from the coalition.
	 */
	ledger_t ledger;
	uint64_t bytesread;
	uint64_t byteswritten;
	uint64_t energy;
	uint64_t gpu_time;
	uint64_t logical_immediate_writes;
	uint64_t logical_deferred_writes;
	uint64_t logical_invalidated_writes;
	uint64_t logical_metadata_writes;
	uint64_t logical_immediate_writes_to_external;
	uint64_t logical_deferred_writes_to_external;
	uint64_t logical_invalidated_writes_to_external;
	uint64_t logical_metadata_writes_to_external;
	uint64_t cpu_ptime;
	uint64_t cpu_time_eqos[COALITION_NUM_THREAD_QOS_TYPES];      /* cpu time per effective QoS class */
	uint64_t cpu_time_rqos[COALITION_NUM_THREAD_QOS_TYPES];      /* cpu time per requested QoS class */
	uint64_t cpu_instructions;
	uint64_t cpu_cycles;

	uint64_t task_count;      /* tasks that have started in this coalition */
	uint64_t dead_task_count; /* tasks that have exited in this coalition;
	                           *  subtract from task_count to get count
	                           *  of "active" tasks */
	/*
	 * Count the length of time this coalition had at least one active task.
	 * This can be a 'denominator' to turn e.g. cpu_time to %cpu.
	 * */
	uint64_t last_became_nonempty_time;
	uint64_t time_nonempty;

	queue_head_t tasks;         /* List of active tasks in the coalition */
	/*
	 * This ledger is used for triggering resource exception. For the tracked resources, this is updated
	 * when the member tasks' resource usage changes.
	 */
	ledger_t resource_monitor_ledger;
#if CONFIG_PHYS_WRITE_ACCT
	uint64_t fs_metadata_writes;
#endif /* CONFIG_PHYS_WRITE_ACCT */
};

/*
 * COALITION_TYPE_JETSAM
 */

static kern_return_t i_coal_jetsam_init(coalition_t coal, boolean_t privileged);
static void          i_coal_jetsam_dealloc(coalition_t coal);
static kern_return_t i_coal_jetsam_adopt_task(coalition_t coal, task_t task);
static kern_return_t i_coal_jetsam_remove_task(coalition_t coal, task_t task);
static kern_return_t i_coal_jetsam_set_taskrole(coalition_t coal,
    task_t task, int role);
int           i_coal_jetsam_get_taskrole(coalition_t coal, task_t task);
static void          i_coal_jetsam_iterate_tasks(coalition_t coal, void *ctx,
    void (*callback)(coalition_t, void *, task_t));

struct i_jetsam_coalition {
	task_t       leader;
	queue_head_t extensions;
	queue_head_t services;
	queue_head_t other;
	struct thread_group *thread_group;
};


/*
 * main coalition structure
 */
struct coalition {
	uint64_t id;                /* monotonically increasing */
	uint32_t type;
	uint32_t role;              /* default task role (background, adaptive, interactive, etc) */
	uint32_t ref_count;         /* Number of references to the memory containing this struct */
	uint32_t active_count;      /* Number of members of (tasks in) the
	                             *  coalition, plus vouchers referring
	                             *  to the coalition */
	uint32_t focal_task_count;   /* Number of TASK_FOREGROUND_APPLICATION tasks in the coalition */
	uint32_t nonfocal_task_count; /* Number of TASK_BACKGROUND_APPLICATION tasks in the coalition */

	/* coalition flags */
	uint32_t privileged : 1;    /* Members of this coalition may create
	                             *  and manage coalitions and may posix_spawn
	                             *  processes into selected coalitions */
	/* ast? */
	/* voucher */
	uint32_t termrequested : 1; /* launchd has requested termination when coalition becomes empty */
	uint32_t terminated : 1;    /* coalition became empty and spawns are now forbidden */
	uint32_t reaped : 1;        /* reaped, invisible to userspace, but waiting for ref_count to go to zero */
	uint32_t notified : 1;      /* no-more-processes notification was sent via special port */
	uint32_t efficient : 1;     /* launchd has marked the coalition as efficient */
#if DEVELOPMENT || DEBUG
	uint32_t should_notify : 1; /* should this coalition send notifications (default: yes) */
#endif

	queue_chain_t coalitions;   /* global list of coalitions */

	decl_lck_mtx_data(, lock);    /* Coalition lock. */

	/* put coalition type-specific structures here */
	union {
		struct i_resource_coalition  r;
		struct i_jetsam_coalition    j;
	};
};

/*
 * register different coalition types:
 * these must be kept in the order specified in coalition.h
 */
static const struct coalition_type
    s_coalition_types[COALITION_NUM_TYPES] = {
	{
		COALITION_TYPE_RESOURCE,
		1,
		i_coal_resource_init,
		i_coal_resource_dealloc,
		i_coal_resource_adopt_task,
		i_coal_resource_remove_task,
		i_coal_resource_set_taskrole,
		i_coal_resource_get_taskrole,
		i_coal_resource_iterate_tasks,
	},
	{
		COALITION_TYPE_JETSAM,
		1,
		i_coal_jetsam_init,
		i_coal_jetsam_dealloc,
		i_coal_jetsam_adopt_task,
		i_coal_jetsam_remove_task,
		i_coal_jetsam_set_taskrole,
		i_coal_jetsam_get_taskrole,
		i_coal_jetsam_iterate_tasks,
	},
};

ZONE_DECLARE(coalition_zone, "coalitions",
    sizeof(struct coalition), ZC_NOENCRYPT | ZC_ZFREE_CLEARMEM);

#define coal_call(coal, func, ...) \
	(s_coalition_types[(coal)->type].func)(coal, ## __VA_ARGS__)


#define coalition_lock(c) do{ lck_mtx_lock(&c->lock); }while(0)
#define coalition_unlock(c) do{ lck_mtx_unlock(&c->lock); }while(0)

/*
 * Define the coalition type to track focal tasks.
 * On embedded, track them using jetsam coalitions since they have associated thread
 * groups which reflect this property as a flag (and pass it down to CLPC).
 * On non-embedded platforms, since not all coalitions have jetsam coalitions
 * track focal counts on the resource coalition.
 */
#if !XNU_TARGET_OS_OSX
#define COALITION_FOCAL_TASKS_ACCOUNTING  COALITION_TYPE_JETSAM
#else /* !XNU_TARGET_OS_OSX */
#define COALITION_FOCAL_TASKS_ACCOUNTING  COALITION_TYPE_RESOURCE
#endif /* !XNU_TARGET_OS_OSX */


/*
 *
 * Coalition ledger implementation
 *
 */

struct coalition_ledger_indices coalition_ledgers =
{.logical_writes = -1, };
void __attribute__((noinline)) SENDING_NOTIFICATION__THIS_COALITION_IS_CAUSING_TOO_MUCH_IO(int flavor);

ledger_t
coalition_ledger_get_from_task(task_t task)
{
	ledger_t ledger = LEDGER_NULL;
	coalition_t coal = task->coalition[COALITION_TYPE_RESOURCE];

	if (coal != NULL && (!queue_empty(&task->task_coalition[COALITION_TYPE_RESOURCE]))) {
		ledger = coal->r.resource_monitor_ledger;
		ledger_reference(ledger);
	}
	return ledger;
}


enum {
	COALITION_IO_LEDGER_ENABLE,
	COALITION_IO_LEDGER_DISABLE
};

void
coalition_io_monitor_ctl(struct coalition *coalition, uint32_t flags, int64_t limit)
{
	ledger_t ledger = coalition->r.resource_monitor_ledger;

	if (flags == COALITION_IO_LEDGER_ENABLE) {
		/* Configure the logical I/O ledger */
		ledger_set_limit(ledger, coalition_ledgers.logical_writes, (limit * 1024 * 1024), 0);
		ledger_set_period(ledger, coalition_ledgers.logical_writes, (COALITION_LEDGER_MONITOR_INTERVAL_SECS * NSEC_PER_SEC));
	} else if (flags == COALITION_IO_LEDGER_DISABLE) {
		ledger_disable_refill(ledger, coalition_ledgers.logical_writes);
		ledger_disable_callback(ledger, coalition_ledgers.logical_writes);
	}
}

int
coalition_ledger_set_logical_writes_limit(struct coalition *coalition, int64_t limit)
{
	int error = 0;

	/*  limit = -1 will be used to disable the limit and the callback */
	if (limit > COALITION_MAX_LOGICAL_WRITES_LIMIT || limit == 0 || limit < -1) {
		error = EINVAL;
		goto out;
	}

	coalition_lock(coalition);
	if (limit == -1) {
		coalition_io_monitor_ctl(coalition, COALITION_IO_LEDGER_DISABLE, limit);
	} else {
		coalition_io_monitor_ctl(coalition, COALITION_IO_LEDGER_ENABLE, limit);
	}
	coalition_unlock(coalition);
out:
	return error;
}

void __attribute__((noinline))
SENDING_NOTIFICATION__THIS_COALITION_IS_CAUSING_TOO_MUCH_IO(int flavor)
{
	int pid = proc_selfpid();
	ledger_amount_t new_limit;
	task_t task = current_task();
	struct ledger_entry_info lei;
	kern_return_t kr;
	ledger_t ledger;
	struct coalition *coalition = task->coalition[COALITION_TYPE_RESOURCE];

	assert(coalition != NULL);
	ledger = coalition->r.resource_monitor_ledger;

	switch (flavor) {
	case FLAVOR_IO_LOGICAL_WRITES:
		ledger_get_entry_info(ledger, coalition_ledgers.logical_writes, &lei);
		trace_resource_violation(RMON_LOGWRITES_VIOLATED, &lei);
		break;
	default:
		goto Exit;
	}

	os_log(OS_LOG_DEFAULT, "Coalition [%lld] caught causing excessive I/O (flavor: %d). Task I/O: %lld MB. [Limit : %lld MB per %lld secs]. Triggered by process [%d]\n",
	    coalition->id, flavor, (lei.lei_balance / (1024 * 1024)), (lei.lei_limit / (1024 * 1024)),
	    (lei.lei_refill_period / NSEC_PER_SEC), pid);

	kr = send_resource_violation(send_disk_writes_violation, task, &lei, kRNFlagsNone);
	if (kr) {
		os_log(OS_LOG_DEFAULT, "ERROR %#x returned from send_resource_violation(disk_writes, ...)\n", kr);
	}

	/*
	 * Continue to monitor the coalition after it hits the initital limit, but increase
	 * the limit exponentially so that we don't spam the listener.
	 */
	new_limit = (lei.lei_limit / 1024 / 1024) * 4;
	coalition_lock(coalition);
	if (new_limit > COALITION_MAX_LOGICAL_WRITES_LIMIT) {
		coalition_io_monitor_ctl(coalition, COALITION_IO_LEDGER_DISABLE, -1);
	} else {
		coalition_io_monitor_ctl(coalition, COALITION_IO_LEDGER_ENABLE, new_limit);
	}
	coalition_unlock(coalition);

Exit:
	return;
}

void
coalition_io_rate_exceeded(int warning, const void *param0, __unused const void *param1)
{
	if (warning == 0) {
		SENDING_NOTIFICATION__THIS_COALITION_IS_CAUSING_TOO_MUCH_IO((int)param0);
	}
}

void
init_coalition_ledgers(void)
{
	ledger_template_t t;
	assert(coalition_ledger_template == NULL);

	if ((t = ledger_template_create("Per-coalition ledgers")) == NULL) {
		panic("couldn't create coalition ledger template");
	}

	coalition_ledgers.logical_writes = ledger_entry_add(t, "logical_writes", "res", "bytes");

	if (coalition_ledgers.logical_writes < 0) {
		panic("couldn't create entries for coaliton ledger template");
	}

	ledger_set_callback(t, coalition_ledgers.logical_writes, coalition_io_rate_exceeded, (void *)FLAVOR_IO_LOGICAL_WRITES, NULL);
	ledger_template_complete(t);

	coalition_task_ledger_template = ledger_template_copy(task_ledger_template, "Coalition task ledgers");

	if (coalition_task_ledger_template == NULL) {
		panic("couldn't create coalition task ledger template");
	}

	ledger_template_complete(coalition_task_ledger_template);

	coalition_ledger_template = t;
}

void
coalition_io_ledger_update(task_t task, int32_t flavor, boolean_t is_credit, uint32_t io_size)
{
	ledger_t ledger;
	coalition_t coal = task->coalition[COALITION_TYPE_RESOURCE];

	assert(coal != NULL);
	ledger = coal->r.resource_monitor_ledger;
	if (LEDGER_VALID(ledger)) {
		if (flavor == FLAVOR_IO_LOGICAL_WRITES) {
			if (is_credit) {
				ledger_credit(ledger, coalition_ledgers.logical_writes, io_size);
			} else {
				ledger_debit(ledger, coalition_ledgers.logical_writes, io_size);
			}
		}
	}
}

static void
coalition_notify_user(uint64_t id, uint32_t flags)
{
	mach_port_t user_port;
	kern_return_t kr;

	kr = host_get_coalition_port(host_priv_self(), &user_port);
	if ((kr != KERN_SUCCESS) || !IPC_PORT_VALID(user_port)) {
		return;
	}

	coalition_notification(user_port, id, flags);
	ipc_port_release_send(user_port);
}

/*
 *
 * COALITION_TYPE_RESOURCE
 *
 */
static kern_return_t
i_coal_resource_init(coalition_t coal, boolean_t privileged)
{
	(void)privileged;
	assert(coal && coal->type == COALITION_TYPE_RESOURCE);
	coal->r.ledger = ledger_instantiate(coalition_task_ledger_template,
	    LEDGER_CREATE_ACTIVE_ENTRIES);
	if (coal->r.ledger == NULL) {
		return KERN_RESOURCE_SHORTAGE;
	}

	coal->r.resource_monitor_ledger = ledger_instantiate(coalition_ledger_template,
	    LEDGER_CREATE_ACTIVE_ENTRIES);
	if (coal->r.resource_monitor_ledger == NULL) {
		return KERN_RESOURCE_SHORTAGE;
	}

	queue_init(&coal->r.tasks);

	return KERN_SUCCESS;
}

static void
i_coal_resource_dealloc(coalition_t coal)
{
	assert(coal && coal->type == COALITION_TYPE_RESOURCE);

	ledger_dereference(coal->r.ledger);
	ledger_dereference(coal->r.resource_monitor_ledger);
}

static kern_return_t
i_coal_resource_adopt_task(coalition_t coal, task_t task)
{
	struct i_resource_coalition *cr;

	assert(coal && coal->type == COALITION_TYPE_RESOURCE);
	assert(queue_empty(&task->task_coalition[COALITION_TYPE_RESOURCE]));

	cr = &coal->r;
	cr->task_count++;

	if (cr->task_count < cr->dead_task_count) {
		panic("%s: coalition %p id:%llu type:%s task_count(%llu) < dead_task_count(%llu)",
		    __func__, coal, coal->id, coal_type_str(coal->type),
		    cr->task_count, cr->dead_task_count);
	}

	/* If moving from 0->1 active tasks */
	if (cr->task_count - cr->dead_task_count == 1) {
		cr->last_became_nonempty_time = mach_absolute_time();
	}

	/* put the task on the coalition's list of tasks */
	enqueue_tail(&cr->tasks, &task->task_coalition[COALITION_TYPE_RESOURCE]);

	coal_dbg("Added PID:%d to id:%llu, task_count:%llu, dead_count:%llu, nonempty_time:%llu",
	    task_pid(task), coal->id, cr->task_count, cr->dead_task_count,
	    cr->last_became_nonempty_time);

	return KERN_SUCCESS;
}

static kern_return_t
i_coal_resource_remove_task(coalition_t coal, task_t task)
{
	struct i_resource_coalition *cr;

	assert(coal && coal->type == COALITION_TYPE_RESOURCE);
	assert(task->coalition[COALITION_TYPE_RESOURCE] == coal);
	assert(!queue_empty(&task->task_coalition[COALITION_TYPE_RESOURCE]));

	/*
	 * handle resource coalition accounting rollup for dead tasks
	 */
	cr = &coal->r;

	cr->dead_task_count++;

	if (cr->task_count < cr->dead_task_count) {
		panic("%s: coalition %p id:%llu type:%s task_count(%llu) < dead_task_count(%llu)",
		    __func__, coal, coal->id, coal_type_str(coal->type), cr->task_count, cr->dead_task_count);
	}

	/* If moving from 1->0 active tasks */
	if (cr->task_count - cr->dead_task_count == 0) {
		uint64_t last_time_nonempty = mach_absolute_time() - cr->last_became_nonempty_time;
		cr->last_became_nonempty_time = 0;
		cr->time_nonempty += last_time_nonempty;
	}

	/* Do not roll up for exec'd task or exec copy task */
	if (!task_is_exec_copy(task) && !task_did_exec(task)) {
		ledger_rollup(cr->ledger, task->ledger);
		cr->bytesread += task->task_io_stats->disk_reads.size;
		cr->byteswritten += task->task_io_stats->total_io.size - task->task_io_stats->disk_reads.size;
#if defined(__x86_64__)
		cr->gpu_time += task_gpu_utilisation(task);
#endif /* defined(__x86_64__) */

#if defined(__arm__) || defined(__arm64__)
		cr->energy += task_energy(task);
#endif /* defined(__arm__) || defined(__arm64__) */

		cr->logical_immediate_writes += task->task_writes_counters_internal.task_immediate_writes;
		cr->logical_deferred_writes += task->task_writes_counters_internal.task_deferred_writes;
		cr->logical_invalidated_writes += task->task_writes_counters_internal.task_invalidated_writes;
		cr->logical_metadata_writes += task->task_writes_counters_internal.task_metadata_writes;
		cr->logical_immediate_writes_to_external += task->task_writes_counters_external.task_immediate_writes;
		cr->logical_deferred_writes_to_external += task->task_writes_counters_external.task_deferred_writes;
		cr->logical_invalidated_writes_to_external += task->task_writes_counters_external.task_invalidated_writes;
		cr->logical_metadata_writes_to_external += task->task_writes_counters_external.task_metadata_writes;
#if CONFIG_PHYS_WRITE_ACCT
		cr->fs_metadata_writes += task->task_fs_metadata_writes;
#endif /* CONFIG_PHYS_WRITE_ACCT */
		cr->cpu_ptime += task_cpu_ptime(task);
		task_update_cpu_time_qos_stats(task, cr->cpu_time_eqos, cr->cpu_time_rqos);
#if MONOTONIC
		uint64_t counts[MT_CORE_NFIXED] = {};
		(void)mt_fixed_task_counts(task, counts);
		cr->cpu_cycles += counts[MT_CORE_CYCLES];
#if defined(MT_CORE_INSTRS)
		cr->cpu_instructions += counts[MT_CORE_INSTRS];
#endif /* defined(MT_CORE_INSTRS) */
#endif /* MONOTONIC */
	}

	/* remove the task from the coalition's list */
	remqueue(&task->task_coalition[COALITION_TYPE_RESOURCE]);
	queue_chain_init(task->task_coalition[COALITION_TYPE_RESOURCE]);

	coal_dbg("removed PID:%d from id:%llu, task_count:%llu, dead_count:%llu",
	    task_pid(task), coal->id, cr->task_count, cr->dead_task_count);

	return KERN_SUCCESS;
}

static kern_return_t
i_coal_resource_set_taskrole(__unused coalition_t coal,
    __unused task_t task, __unused int role)
{
	return KERN_SUCCESS;
}

static int
i_coal_resource_get_taskrole(__unused coalition_t coal, __unused task_t task)
{
	task_t t;

	assert(coal && coal->type == COALITION_TYPE_RESOURCE);

	qe_foreach_element(t, &coal->r.tasks, task_coalition[COALITION_TYPE_RESOURCE]) {
		if (t == task) {
			return COALITION_TASKROLE_UNDEF;
		}
	}

	return -1;
}

static void
i_coal_resource_iterate_tasks(coalition_t coal, void *ctx, void (*callback)(coalition_t, void *, task_t))
{
	task_t t;
	assert(coal && coal->type == COALITION_TYPE_RESOURCE);

	qe_foreach_element(t, &coal->r.tasks, task_coalition[COALITION_TYPE_RESOURCE])
	callback(coal, ctx, t);
}

#if CONFIG_PHYS_WRITE_ACCT
extern uint64_t kernel_pm_writes;
#endif /* CONFIG_PHYS_WRITE_ACCT */

kern_return_t
coalition_resource_usage_internal(coalition_t coal, struct coalition_resource_usage *cru_out)
{
	kern_return_t kr;
	ledger_amount_t credit, debit;
	int i;

	if (coal->type != COALITION_TYPE_RESOURCE) {
		return KERN_INVALID_ARGUMENT;
	}

	/* Return KERN_INVALID_ARGUMENT for Corpse coalition */
	for (i = 0; i < COALITION_NUM_TYPES; i++) {
		if (coal == corpse_coalition[i]) {
			return KERN_INVALID_ARGUMENT;
		}
	}

	ledger_t sum_ledger = ledger_instantiate(coalition_task_ledger_template, LEDGER_CREATE_ACTIVE_ENTRIES);
	if (sum_ledger == LEDGER_NULL) {
		return KERN_RESOURCE_SHORTAGE;
	}

	coalition_lock(coal);

	/*
	 * Start with the coalition's ledger, which holds the totals from all
	 * the dead tasks.
	 */
	ledger_rollup(sum_ledger, coal->r.ledger);
	uint64_t bytesread = coal->r.bytesread;
	uint64_t byteswritten = coal->r.byteswritten;
	uint64_t gpu_time = coal->r.gpu_time;
	uint64_t energy = coal->r.energy;
	uint64_t logical_immediate_writes = coal->r.logical_immediate_writes;
	uint64_t logical_deferred_writes = coal->r.logical_deferred_writes;
	uint64_t logical_invalidated_writes = coal->r.logical_invalidated_writes;
	uint64_t logical_metadata_writes = coal->r.logical_metadata_writes;
	uint64_t logical_immediate_writes_to_external = coal->r.logical_immediate_writes_to_external;
	uint64_t logical_deferred_writes_to_external = coal->r.logical_deferred_writes_to_external;
	uint64_t logical_invalidated_writes_to_external = coal->r.logical_invalidated_writes_to_external;
	uint64_t logical_metadata_writes_to_external = coal->r.logical_metadata_writes_to_external;
#if CONFIG_PHYS_WRITE_ACCT
	uint64_t fs_metadata_writes = coal->r.fs_metadata_writes;
#endif /* CONFIG_PHYS_WRITE_ACCT */
	int64_t cpu_time_billed_to_me = 0;
	int64_t cpu_time_billed_to_others = 0;
	int64_t energy_billed_to_me = 0;
	int64_t energy_billed_to_others = 0;
	uint64_t cpu_ptime = coal->r.cpu_ptime;
	uint64_t cpu_time_eqos[COALITION_NUM_THREAD_QOS_TYPES];
	memcpy(cpu_time_eqos, coal->r.cpu_time_eqos, sizeof(cpu_time_eqos));
	uint64_t cpu_time_rqos[COALITION_NUM_THREAD_QOS_TYPES];
	memcpy(cpu_time_rqos, coal->r.cpu_time_rqos, sizeof(cpu_time_rqos));
	uint64_t cpu_instructions = coal->r.cpu_instructions;
	uint64_t cpu_cycles = coal->r.cpu_cycles;

	/*
	 * Add to that all the active tasks' ledgers. Tasks cannot deallocate
	 * out from under us, since we hold the coalition lock.
	 */
	task_t task;
	qe_foreach_element(task, &coal->r.tasks, task_coalition[COALITION_TYPE_RESOURCE]) {
		/*
		 * Rolling up stats for exec copy task or exec'd task will lead to double accounting.
		 * Cannot take task lock after taking coaliton lock
		 */
		if (task_is_exec_copy(task) || task_did_exec(task)) {
			continue;
		}

		ledger_rollup(sum_ledger, task->ledger);
		bytesread += task->task_io_stats->disk_reads.size;
		byteswritten += task->task_io_stats->total_io.size - task->task_io_stats->disk_reads.size;
#if defined(__x86_64__)
		gpu_time += task_gpu_utilisation(task);
#endif /* defined(__x86_64__) */

#if defined(__arm__) || defined(__arm64__)
		energy += task_energy(task);
#endif /* defined(__arm__) || defined(__arm64__) */

		logical_immediate_writes += task->task_writes_counters_internal.task_immediate_writes;
		logical_deferred_writes += task->task_writes_counters_internal.task_deferred_writes;
		logical_invalidated_writes += task->task_writes_counters_internal.task_invalidated_writes;
		logical_metadata_writes += task->task_writes_counters_internal.task_metadata_writes;
		logical_immediate_writes_to_external += task->task_writes_counters_external.task_immediate_writes;
		logical_deferred_writes_to_external += task->task_writes_counters_external.task_deferred_writes;
		logical_invalidated_writes_to_external += task->task_writes_counters_external.task_invalidated_writes;
		logical_metadata_writes_to_external += task->task_writes_counters_external.task_metadata_writes;
#if CONFIG_PHYS_WRITE_ACCT
		fs_metadata_writes += task->task_fs_metadata_writes;
#endif /* CONFIG_PHYS_WRITE_ACCT */

		cpu_ptime += task_cpu_ptime(task);
		task_update_cpu_time_qos_stats(task, cpu_time_eqos, cpu_time_rqos);
#if MONOTONIC
		uint64_t counts[MT_CORE_NFIXED] = {};
		(void)mt_fixed_task_counts(task, counts);
		cpu_cycles += counts[MT_CORE_CYCLES];
#if defined(MT_CORE_INSTRS)
		cpu_instructions += counts[MT_CORE_INSTRS];
#endif /* defined(MT_CORE_INSTRS) */
#endif /* MONOTONIC */
	}

	kr = ledger_get_balance(sum_ledger, task_ledgers.cpu_time_billed_to_me, (int64_t *)&cpu_time_billed_to_me);
	if (kr != KERN_SUCCESS || cpu_time_billed_to_me < 0) {
		cpu_time_billed_to_me = 0;
	}

	kr = ledger_get_balance(sum_ledger, task_ledgers.cpu_time_billed_to_others, (int64_t *)&cpu_time_billed_to_others);
	if (kr != KERN_SUCCESS || cpu_time_billed_to_others < 0) {
		cpu_time_billed_to_others = 0;
	}

	kr = ledger_get_balance(sum_ledger, task_ledgers.energy_billed_to_me, (int64_t *)&energy_billed_to_me);
	if (kr != KERN_SUCCESS || energy_billed_to_me < 0) {
		energy_billed_to_me = 0;
	}

	kr = ledger_get_balance(sum_ledger, task_ledgers.energy_billed_to_others, (int64_t *)&energy_billed_to_others);
	if (kr != KERN_SUCCESS || energy_billed_to_others < 0) {
		energy_billed_to_others = 0;
	}

	/* collect information from the coalition itself */
	cru_out->tasks_started = coal->r.task_count;
	cru_out->tasks_exited = coal->r.dead_task_count;

	uint64_t time_nonempty = coal->r.time_nonempty;
	uint64_t last_became_nonempty_time = coal->r.last_became_nonempty_time;

	coalition_unlock(coal);

	/* Copy the totals out of sum_ledger */
	kr = ledger_get_entries(sum_ledger, task_ledgers.cpu_time,
	    &credit, &debit);
	if (kr != KERN_SUCCESS) {
		credit = 0;
	}
	cru_out->cpu_time = credit;
	cru_out->cpu_time_billed_to_me = (uint64_t)cpu_time_billed_to_me;
	cru_out->cpu_time_billed_to_others = (uint64_t)cpu_time_billed_to_others;
	cru_out->energy_billed_to_me = (uint64_t)energy_billed_to_me;
	cru_out->energy_billed_to_others = (uint64_t)energy_billed_to_others;

	kr = ledger_get_entries(sum_ledger, task_ledgers.interrupt_wakeups,
	    &credit, &debit);
	if (kr != KERN_SUCCESS) {
		credit = 0;
	}
	cru_out->interrupt_wakeups = credit;

	kr = ledger_get_entries(sum_ledger, task_ledgers.platform_idle_wakeups,
	    &credit, &debit);
	if (kr != KERN_SUCCESS) {
		credit = 0;
	}
	cru_out->platform_idle_wakeups = credit;

	cru_out->bytesread = bytesread;
	cru_out->byteswritten = byteswritten;
	cru_out->gpu_time = gpu_time;
	cru_out->energy = energy;
	cru_out->logical_immediate_writes = logical_immediate_writes;
	cru_out->logical_deferred_writes = logical_deferred_writes;
	cru_out->logical_invalidated_writes = logical_invalidated_writes;
	cru_out->logical_metadata_writes = logical_metadata_writes;
	cru_out->logical_immediate_writes_to_external = logical_immediate_writes_to_external;
	cru_out->logical_deferred_writes_to_external = logical_deferred_writes_to_external;
	cru_out->logical_invalidated_writes_to_external = logical_invalidated_writes_to_external;
	cru_out->logical_metadata_writes_to_external = logical_metadata_writes_to_external;
#if CONFIG_PHYS_WRITE_ACCT
	cru_out->fs_metadata_writes = fs_metadata_writes;
#else
	cru_out->fs_metadata_writes = 0;
#endif /* CONFIG_PHYS_WRITE_ACCT */
	cru_out->cpu_ptime = cpu_ptime;
	cru_out->cpu_time_eqos_len = COALITION_NUM_THREAD_QOS_TYPES;
	memcpy(cru_out->cpu_time_eqos, cpu_time_eqos, sizeof(cru_out->cpu_time_eqos));
	cru_out->cpu_cycles = cpu_cycles;
	cru_out->cpu_instructions = cpu_instructions;
	ledger_dereference(sum_ledger);
	sum_ledger = LEDGER_NULL;

#if CONFIG_PHYS_WRITE_ACCT
	// kernel_pm_writes are only recorded under kernel_task coalition
	if (coalition_id(coal) == COALITION_ID_KERNEL) {
		cru_out->pm_writes = kernel_pm_writes;
	} else {
		cru_out->pm_writes = 0;
	}
#else
	cru_out->pm_writes = 0;
#endif /* CONFIG_PHYS_WRITE_ACCT */

	if (last_became_nonempty_time) {
		time_nonempty += mach_absolute_time() - last_became_nonempty_time;
	}
	absolutetime_to_nanoseconds(time_nonempty, &cru_out->time_nonempty);

	return KERN_SUCCESS;
}

/*
 *
 * COALITION_TYPE_JETSAM
 *
 */
static kern_return_t
i_coal_jetsam_init(coalition_t coal, boolean_t privileged)
{
	assert(coal && coal->type == COALITION_TYPE_JETSAM);
	(void)privileged;

	coal->j.leader = TASK_NULL;
	queue_head_init(coal->j.extensions);
	queue_head_init(coal->j.services);
	queue_head_init(coal->j.other);

#if CONFIG_THREAD_GROUPS
	switch (coal->role) {
	case COALITION_ROLE_SYSTEM:
		coal->j.thread_group = thread_group_find_by_id_and_retain(THREAD_GROUP_SYSTEM);
		break;
	case COALITION_ROLE_BACKGROUND:
		coal->j.thread_group = thread_group_find_by_id_and_retain(THREAD_GROUP_BACKGROUND);
		break;
	case COALITION_ROLE_ADAPTIVE:
		if (merge_adaptive_coalitions) {
			coal->j.thread_group = thread_group_find_by_id_and_retain(THREAD_GROUP_ADAPTIVE);
		} else {
			coal->j.thread_group = thread_group_create_and_retain();
		}
		break;
	default:
		coal->j.thread_group = thread_group_create_and_retain();
	}
	assert(coal->j.thread_group != NULL);
#endif
	return KERN_SUCCESS;
}

static void
i_coal_jetsam_dealloc(__unused coalition_t coal)
{
	assert(coal && coal->type == COALITION_TYPE_JETSAM);

	/* the coalition should be completely clear at this point */
	assert(queue_empty(&coal->j.extensions));
	assert(queue_empty(&coal->j.services));
	assert(queue_empty(&coal->j.other));
	assert(coal->j.leader == TASK_NULL);

#if CONFIG_THREAD_GROUPS
	/* disassociate from the thread group */
	assert(coal->j.thread_group != NULL);
	thread_group_release(coal->j.thread_group);
	coal->j.thread_group = NULL;
#endif
}

static kern_return_t
i_coal_jetsam_adopt_task(coalition_t coal, task_t task)
{
	struct i_jetsam_coalition *cj;
	assert(coal && coal->type == COALITION_TYPE_JETSAM);

	cj = &coal->j;

	assert(queue_empty(&task->task_coalition[COALITION_TYPE_JETSAM]));

	/* put each task initially in the "other" list */
	enqueue_tail(&cj->other, &task->task_coalition[COALITION_TYPE_JETSAM]);
	coal_dbg("coalition %lld adopted PID:%d as UNDEF",
	    coal->id, task_pid(task));

	return KERN_SUCCESS;
}

static kern_return_t
i_coal_jetsam_remove_task(coalition_t coal, task_t task)
{
	assert(coal && coal->type == COALITION_TYPE_JETSAM);
	assert(task->coalition[COALITION_TYPE_JETSAM] == coal);

	coal_dbg("removing PID:%d from coalition id:%lld",
	    task_pid(task), coal->id);

	if (task == coal->j.leader) {
		coal->j.leader = NULL;
		coal_dbg("    PID:%d was the leader!", task_pid(task));
	} else {
		assert(!queue_empty(&task->task_coalition[COALITION_TYPE_JETSAM]));
	}

	/* remove the task from the specific coalition role queue */
	remqueue(&task->task_coalition[COALITION_TYPE_JETSAM]);
	queue_chain_init(task->task_coalition[COALITION_TYPE_RESOURCE]);

	return KERN_SUCCESS;
}

static kern_return_t
i_coal_jetsam_set_taskrole(coalition_t coal, task_t task, int role)
{
	struct i_jetsam_coalition *cj;
	queue_t q = NULL;
	assert(coal && coal->type == COALITION_TYPE_JETSAM);
	assert(task->coalition[COALITION_TYPE_JETSAM] == coal);

	cj = &coal->j;

	switch (role) {
	case COALITION_TASKROLE_LEADER:
		coal_dbg("setting PID:%d as LEADER of %lld",
		    task_pid(task), coal->id);
		if (cj->leader != TASK_NULL) {
			/* re-queue the exiting leader onto the "other" list */
			coal_dbg("    re-queue existing leader (%d) as OTHER",
			    task_pid(cj->leader));
			re_queue_tail(&cj->other, &cj->leader->task_coalition[COALITION_TYPE_JETSAM]);
		}
		/*
		 * remove the task from the "other" list
		 * (where it was put by default)
		 */
		remqueue(&task->task_coalition[COALITION_TYPE_JETSAM]);
		queue_chain_init(task->task_coalition[COALITION_TYPE_JETSAM]);

		/* set the coalition leader */
		cj->leader = task;
		break;
	case COALITION_TASKROLE_XPC:
		coal_dbg("setting PID:%d as XPC in %lld",
		    task_pid(task), coal->id);
		q = (queue_t)&cj->services;
		break;
	case COALITION_TASKROLE_EXT:
		coal_dbg("setting PID:%d as EXT in %lld",
		    task_pid(task), coal->id);
		q = (queue_t)&cj->extensions;
		break;
	case COALITION_TASKROLE_NONE:
		/*
		 * Tasks with a role of "none" should fall through to an
		 * undefined role so long as the task is currently a member
		 * of the coalition. This scenario can happen if a task is
		 * killed (usually via jetsam) during exec.
		 */
		if (task->coalition[COALITION_TYPE_JETSAM] != coal) {
			panic("%s: task %p attempting to set role %d "
			    "in coalition %p to which it does not belong!", __func__, task, role, coal);
		}
		OS_FALLTHROUGH;
	case COALITION_TASKROLE_UNDEF:
		coal_dbg("setting PID:%d as UNDEF in %lld",
		    task_pid(task), coal->id);
		q = (queue_t)&cj->other;
		break;
	default:
		panic("%s: invalid role(%d) for task", __func__, role);
		return KERN_INVALID_ARGUMENT;
	}

	if (q != NULL) {
		re_queue_tail(q, &task->task_coalition[COALITION_TYPE_JETSAM]);
	}

	return KERN_SUCCESS;
}

int
i_coal_jetsam_get_taskrole(coalition_t coal, task_t task)
{
	struct i_jetsam_coalition *cj;
	task_t t;

	assert(coal && coal->type == COALITION_TYPE_JETSAM);
	assert(task->coalition[COALITION_TYPE_JETSAM] == coal);

	cj = &coal->j;

	if (task == cj->leader) {
		return COALITION_TASKROLE_LEADER;
	}

	qe_foreach_element(t, &cj->services, task_coalition[COALITION_TYPE_JETSAM]) {
		if (t == task) {
			return COALITION_TASKROLE_XPC;
		}
	}

	qe_foreach_element(t, &cj->extensions, task_coalition[COALITION_TYPE_JETSAM]) {
		if (t == task) {
			return COALITION_TASKROLE_EXT;
		}
	}

	qe_foreach_element(t, &cj->other, task_coalition[COALITION_TYPE_JETSAM]) {
		if (t == task) {
			return COALITION_TASKROLE_UNDEF;
		}
	}

	/* task not in the coalition?! */
	return COALITION_TASKROLE_NONE;
}

static void
i_coal_jetsam_iterate_tasks(coalition_t coal, void *ctx, void (*callback)(coalition_t, void *, task_t))
{
	struct i_jetsam_coalition *cj;
	task_t t;

	assert(coal && coal->type == COALITION_TYPE_JETSAM);

	cj = &coal->j;

	if (cj->leader) {
		callback(coal, ctx, cj->leader);
	}

	qe_foreach_element(t, &cj->services, task_coalition[COALITION_TYPE_JETSAM])
	callback(coal, ctx, t);

	qe_foreach_element(t, &cj->extensions, task_coalition[COALITION_TYPE_JETSAM])
	callback(coal, ctx, t);

	qe_foreach_element(t, &cj->other, task_coalition[COALITION_TYPE_JETSAM])
	callback(coal, ctx, t);
}


/*
 *
 * Main Coalition implementation
 *
 */

/*
 * coalition_create_internal
 * Returns: New coalition object, referenced for the caller and unlocked.
 * Condition: coalitions_list_lock must be UNLOCKED.
 */
kern_return_t
coalition_create_internal(int type, int role, boolean_t privileged, coalition_t *out, uint64_t *coalition_id)
{
	kern_return_t kr;
	struct coalition *new_coal;
	uint64_t cid;
	uint32_t ctype;

	if (type < 0 || type > COALITION_TYPE_MAX) {
		return KERN_INVALID_ARGUMENT;
	}

	new_coal = (struct coalition *)zalloc(coalition_zone);
	if (new_coal == COALITION_NULL) {
		return KERN_RESOURCE_SHORTAGE;
	}
	bzero(new_coal, sizeof(*new_coal));

	new_coal->type = type;
	new_coal->role = role;

	/* initialize type-specific resources */
	kr = coal_call(new_coal, init, privileged);
	if (kr != KERN_SUCCESS) {
		zfree(coalition_zone, new_coal);
		return kr;
	}

	/* One for caller, one for coalitions list */
	new_coal->ref_count = 2;

	new_coal->privileged = privileged ? TRUE : FALSE;
#if DEVELOPMENT || DEBUG
	new_coal->should_notify = 1;
#endif

	lck_mtx_init(&new_coal->lock, &coalitions_lck_grp, LCK_ATTR_NULL);

	lck_rw_lock_exclusive(&coalitions_list_lock);
	new_coal->id = coalition_next_id++;
	coalition_count++;
	enqueue_tail(&coalitions_q, &new_coal->coalitions);

#if CONFIG_THREAD_GROUPS
	KDBG_RELEASE(MACHDBG_CODE(DBG_MACH_COALITION, MACH_COALITION_NEW),
	    new_coal->id, new_coal->type,
	    (new_coal->type == COALITION_TYPE_JETSAM && new_coal->j.thread_group) ?
	    thread_group_get_id(new_coal->j.thread_group) : 0);

#else
	KDBG_RELEASE(MACHDBG_CODE(DBG_MACH_COALITION, MACH_COALITION_NEW),
	    new_coal->id, new_coal->type);
#endif
	cid = new_coal->id;
	ctype = new_coal->type;
	lck_rw_unlock_exclusive(&coalitions_list_lock);

	coal_dbg("id:%llu, type:%s", cid, coal_type_str(ctype));

	if (coalition_id != NULL) {
		*coalition_id = cid;
	}

	*out = new_coal;
	return KERN_SUCCESS;
}

/*
 * coalition_release
 * Condition: coalition must be UNLOCKED.
 * */
void
coalition_release(coalition_t coal)
{
	/* TODO: This can be done with atomics. */
	coalition_lock(coal);
	coal->ref_count--;

#if COALITION_DEBUG
	uint32_t rc = coal->ref_count;
	uint32_t ac = coal->active_count;
#endif /* COALITION_DEBUG */

	coal_dbg("id:%llu type:%s ref_count:%u active_count:%u%s",
	    coal->id, coal_type_str(coal->type), rc, ac,
	    rc <= 0 ? ", will deallocate now" : "");

	if (coal->ref_count > 0) {
		coalition_unlock(coal);
		return;
	}

	assert(coal->termrequested);
	assert(coal->terminated);
	assert(coal->active_count == 0);
	assert(coal->reaped);
	assert(coal->focal_task_count == 0);
	assert(coal->nonfocal_task_count == 0);
#if CONFIG_THREAD_GROUPS
	KDBG_RELEASE(MACHDBG_CODE(DBG_MACH_COALITION, MACH_COALITION_FREE),
	    coal->id, coal->type,
	    coal->type == COALITION_TYPE_JETSAM ?
	    coal->j.thread_group : 0);
#else
	KDBG_RELEASE(MACHDBG_CODE(DBG_MACH_COALITION, MACH_COALITION_FREE),
	    coal->id, coal->type);
#endif

	coal_call(coal, dealloc);

	coalition_unlock(coal);

	lck_mtx_destroy(&coal->lock, &coalitions_lck_grp);

	zfree(coalition_zone, coal);
}

/*
 * coalition_find_by_id_internal
 * Returns: Coalition object with specified id, NOT referenced.
 *          If not found, returns COALITION_NULL.
 *          If found, returns a locked coalition.
 *
 * Condition: No locks held
 */
static coalition_t
coalition_find_by_id_internal(uint64_t coal_id)
{
	coalition_t coal;

	if (coal_id == 0) {
		return COALITION_NULL;
	}

	lck_rw_lock_shared(&coalitions_list_lock);
	qe_foreach_element(coal, &coalitions_q, coalitions) {
		if (coal->id == coal_id) {
			coalition_lock(coal);
			lck_rw_unlock_shared(&coalitions_list_lock);
			return coal;
		}
	}
	lck_rw_unlock_shared(&coalitions_list_lock);

	return COALITION_NULL;
}

/*
 * coalition_find_by_id
 * Returns: Coalition object with specified id, referenced.
 * Condition: coalitions_list_lock must be UNLOCKED.
 */
coalition_t
coalition_find_by_id(uint64_t cid)
{
	coalition_t coal = coalition_find_by_id_internal(cid);

	if (coal == COALITION_NULL) {
		return COALITION_NULL;
	}

	/* coal is locked */

	if (coal->reaped) {
		coalition_unlock(coal);
		return COALITION_NULL;
	}

	if (coal->ref_count == 0) {
		panic("resurrecting coalition %p id:%llu type:%s, active_count:%u\n",
		    coal, coal->id, coal_type_str(coal->type), coal->active_count);
	}
	coal->ref_count++;
#if COALITION_DEBUG
	uint32_t rc = coal->ref_count;
#endif

	coalition_unlock(coal);

	coal_dbg("id:%llu type:%s ref_count:%u",
	    coal->id, coal_type_str(coal->type), rc);

	return coal;
}

/*
 * coalition_find_and_activate_by_id
 * Returns: Coalition object with specified id, referenced, and activated.
 * Condition: coalitions_list_lock must be UNLOCKED.
 * This is the function to use when putting a 'new' thing into a coalition,
 * like posix_spawn of an XPC service by launchd.
 * See also coalition_extend_active.
 */
coalition_t
coalition_find_and_activate_by_id(uint64_t cid)
{
	coalition_t coal = coalition_find_by_id_internal(cid);

	if (coal == COALITION_NULL) {
		return COALITION_NULL;
	}

	/* coal is locked */

	if (coal->reaped || coal->terminated) {
		/* Too late to put something new into this coalition, it's
		 * already on its way out the door */
		coalition_unlock(coal);
		return COALITION_NULL;
	}

	if (coal->ref_count == 0) {
		panic("resurrecting coalition %p id:%llu type:%s, active_count:%u\n",
		    coal, coal->id, coal_type_str(coal->type), coal->active_count);
	}

	coal->ref_count++;
	coal->active_count++;

#if COALITION_DEBUG
	uint32_t rc = coal->ref_count;
	uint32_t ac = coal->active_count;
#endif

	coalition_unlock(coal);

	coal_dbg("id:%llu type:%s ref_count:%u, active_count:%u",
	    coal->id, coal_type_str(coal->type), rc, ac);

	return coal;
}

uint64_t
coalition_id(coalition_t coal)
{
	assert(coal != COALITION_NULL);
	return coal->id;
}

void
task_coalition_ids(task_t task, uint64_t ids[COALITION_NUM_TYPES])
{
	int i;
	for (i = 0; i < COALITION_NUM_TYPES; i++) {
		if (task->coalition[i]) {
			ids[i] = task->coalition[i]->id;
		} else {
			ids[i] = 0;
		}
	}
}

void
task_coalition_roles(task_t task, int roles[COALITION_NUM_TYPES])
{
	int i;
	memset(roles, 0, COALITION_NUM_TYPES * sizeof(roles[0]));

	for (i = 0; i < COALITION_NUM_TYPES; i++) {
		if (task->coalition[i]) {
			coalition_lock(task->coalition[i]);
			roles[i] = coal_call(task->coalition[i],
			    get_taskrole, task);
			coalition_unlock(task->coalition[i]);
		} else {
			roles[i] = COALITION_TASKROLE_NONE;
		}
	}
}


int
coalition_type(coalition_t coal)
{
	return coal->type;
}

boolean_t
coalition_term_requested(coalition_t coal)
{
	return coal->termrequested;
}

boolean_t
coalition_is_terminated(coalition_t coal)
{
	return coal->terminated;
}

boolean_t
coalition_is_reaped(coalition_t coal)
{
	return coal->reaped;
}

boolean_t
coalition_is_privileged(coalition_t coal)
{
	return coal->privileged || unrestrict_coalition_syscalls;
}

boolean_t
task_is_in_privileged_coalition(task_t task, int type)
{
	if (type < 0 || type > COALITION_TYPE_MAX) {
		return FALSE;
	}
	if (unrestrict_coalition_syscalls) {
		return TRUE;
	}
	if (!task->coalition[type]) {
		return FALSE;
	}
	return task->coalition[type]->privileged;
}

void
task_coalition_update_gpu_stats(task_t task, uint64_t gpu_ns_delta)
{
	coalition_t coal;

	assert(task != TASK_NULL);
	if (gpu_ns_delta == 0) {
		return;
	}

	coal = task->coalition[COALITION_TYPE_RESOURCE];
	assert(coal != COALITION_NULL);

	coalition_lock(coal);
	coal->r.gpu_time += gpu_ns_delta;
	coalition_unlock(coal);
}

boolean_t
task_coalition_adjust_focal_count(task_t task, int count, uint32_t *new_count)
{
	coalition_t coal = task->coalition[COALITION_FOCAL_TASKS_ACCOUNTING];
	if (coal == COALITION_NULL) {
		return FALSE;
	}

	*new_count = os_atomic_add(&coal->focal_task_count, count, relaxed);
	assert(*new_count != UINT32_MAX);
	return TRUE;
}

uint32_t
task_coalition_focal_count(task_t task)
{
	coalition_t coal = task->coalition[COALITION_FOCAL_TASKS_ACCOUNTING];
	if (coal == COALITION_NULL) {
		return 0;
	}

	return coal->focal_task_count;
}

boolean_t
task_coalition_adjust_nonfocal_count(task_t task, int count, uint32_t *new_count)
{
	coalition_t coal = task->coalition[COALITION_FOCAL_TASKS_ACCOUNTING];
	if (coal == COALITION_NULL) {
		return FALSE;
	}

	*new_count = os_atomic_add(&coal->nonfocal_task_count, count, relaxed);
	assert(*new_count != UINT32_MAX);
	return TRUE;
}

uint32_t
task_coalition_nonfocal_count(task_t task)
{
	coalition_t coal = task->coalition[COALITION_FOCAL_TASKS_ACCOUNTING];
	if (coal == COALITION_NULL) {
		return 0;
	}

	return coal->nonfocal_task_count;
}

void
coalition_set_efficient(coalition_t coal)
{
	coalition_lock(coal);
	coal->efficient = TRUE;
	coalition_unlock(coal);
}

#if CONFIG_THREAD_GROUPS
struct thread_group *
task_coalition_get_thread_group(task_t task)
{
	coalition_t coal = task->coalition[COALITION_TYPE_JETSAM];
	/* return system thread group for non-jetsam coalitions */
	if (coal == COALITION_NULL) {
		return init_coalition[COALITION_TYPE_JETSAM]->j.thread_group;
	}
	return coal->j.thread_group;
}


struct thread_group *
kdp_coalition_get_thread_group(coalition_t coal)
{
	if (coal->type != COALITION_TYPE_JETSAM) {
		return NULL;
	}
	assert(coal->j.thread_group != NULL);
	return coal->j.thread_group;
}

struct thread_group *
coalition_get_thread_group(coalition_t coal)
{
	if (coal->type != COALITION_TYPE_JETSAM) {
		return NULL;
	}
	assert(coal->j.thread_group != NULL);
	return thread_group_retain(coal->j.thread_group);
}

void
coalition_set_thread_group(coalition_t coal, struct thread_group *tg)
{
	assert(coal != COALITION_NULL);
	assert(tg != NULL);

	if (coal->type != COALITION_TYPE_JETSAM) {
		return;
	}
	struct thread_group *old_tg = coal->j.thread_group;
	assert(old_tg != NULL);
	coal->j.thread_group = tg;

	KDBG_RELEASE(MACHDBG_CODE(DBG_MACH_COALITION, MACH_COALITION_THREAD_GROUP_SET),
	    coal->id, coal->type, thread_group_get_id(tg));

	thread_group_release(old_tg);
}

void
task_coalition_thread_group_focal_update(task_t task)
{
	assert(task->coalition[COALITION_FOCAL_TASKS_ACCOUNTING] != COALITION_NULL);
	thread_group_flags_update_lock();
	uint32_t focal_count = task_coalition_focal_count(task);
	if (focal_count) {
		thread_group_set_flags_locked(task_coalition_get_thread_group(task), THREAD_GROUP_FLAGS_UI_APP);
	} else {
		thread_group_clear_flags_locked(task_coalition_get_thread_group(task), THREAD_GROUP_FLAGS_UI_APP);
	}
	thread_group_flags_update_unlock();
}

#endif

void
coalition_for_each_task(coalition_t coal, void *ctx,
    void (*callback)(coalition_t, void *, task_t))
{
	assert(coal != COALITION_NULL);

	coal_dbg("iterating tasks in coalition %p id:%llu type:%s, active_count:%u",
	    coal, coal->id, coal_type_str(coal->type), coal->active_count);

	coalition_lock(coal);

	coal_call(coal, iterate_tasks, ctx, callback);

	coalition_unlock(coal);
}


void
coalition_remove_active(coalition_t coal)
{
	coalition_lock(coal);

	assert(!coal->reaped);
	assert(coal->active_count > 0);

	coal->active_count--;

	boolean_t do_notify = FALSE;
	uint64_t notify_id = 0;
	uint32_t notify_flags = 0;
	if (coal->termrequested && coal->active_count == 0) {
		/* We only notify once, when active_count reaches zero.
		 * We just decremented, so if it reached zero, we mustn't have
		 * notified already.
		 */
		assert(!coal->terminated);
		coal->terminated = TRUE;

		assert(!coal->notified);

		coal->notified = TRUE;
#if DEVELOPMENT || DEBUG
		do_notify = coal->should_notify;
#else
		do_notify = TRUE;
#endif
		notify_id = coal->id;
		notify_flags = 0;
	}

#if COALITION_DEBUG
	uint64_t cid = coal->id;
	uint32_t rc = coal->ref_count;
	int      ac = coal->active_count;
	int      ct = coal->type;
#endif
	coalition_unlock(coal);

	coal_dbg("id:%llu type:%s ref_count:%u, active_count:%u,%s",
	    cid, coal_type_str(ct), rc, ac, do_notify ? " NOTIFY" : " ");

	if (do_notify) {
		coalition_notify_user(notify_id, notify_flags);
	}
}

/* Used for kernel_task, launchd, launchd's early boot tasks... */
kern_return_t
coalitions_adopt_init_task(task_t task)
{
	kern_return_t kr;
	kr = coalitions_adopt_task(init_coalition, task);
	if (kr != KERN_SUCCESS) {
		panic("failed to adopt task %p into default coalition: %d", task, kr);
	}
	return kr;
}

/* Used for forked corpses. */
kern_return_t
coalitions_adopt_corpse_task(task_t task)
{
	kern_return_t kr;
	kr = coalitions_adopt_task(corpse_coalition, task);
	if (kr != KERN_SUCCESS) {
		panic("failed to adopt task %p into corpse coalition: %d", task, kr);
	}
	return kr;
}

/*
 * coalition_adopt_task_internal
 * Condition: Coalition must be referenced and unlocked. Will fail if coalition
 * is already terminated.
 */
static kern_return_t
coalition_adopt_task_internal(coalition_t coal, task_t task)
{
	kern_return_t kr;

	if (task->coalition[coal->type]) {
		return KERN_ALREADY_IN_SET;
	}

	coalition_lock(coal);

	if (coal->reaped || coal->terminated) {
		coalition_unlock(coal);
		return KERN_TERMINATED;
	}

	kr = coal_call(coal, adopt_task, task);
	if (kr != KERN_SUCCESS) {
		goto out_unlock;
	}

	coal->active_count++;

	coal->ref_count++;

	task->coalition[coal->type] = coal;

out_unlock:
#if COALITION_DEBUG
	(void)coal; /* need expression after label */
	uint64_t cid = coal->id;
	uint32_t rc = coal->ref_count;
	uint32_t ct = coal->type;
#endif
	if (get_task_uniqueid(task) != UINT64_MAX) {
		/* On 32-bit targets, uniqueid will get truncated to 32 bits */
		KDBG_RELEASE(MACHDBG_CODE(DBG_MACH_COALITION, MACH_COALITION_ADOPT),
		    coal->id, get_task_uniqueid(task));
	}

	coalition_unlock(coal);

	coal_dbg("task:%d, id:%llu type:%s ref_count:%u, kr=%d",
	    task_pid(task), cid, coal_type_str(ct), rc, kr);
	return kr;
}

static kern_return_t
coalition_remove_task_internal(task_t task, int type)
{
	kern_return_t kr;

	coalition_t coal = task->coalition[type];

	if (!coal) {
		return KERN_SUCCESS;
	}

	assert(coal->type == (uint32_t)type);

	coalition_lock(coal);

	kr = coal_call(coal, remove_task, task);

#if COALITION_DEBUG
	uint64_t cid = coal->id;
	uint32_t rc = coal->ref_count;
	int      ac = coal->active_count;
	int      ct = coal->type;
#endif
	KDBG_RELEASE(MACHDBG_CODE(DBG_MACH_COALITION, MACH_COALITION_REMOVE),
	    coal->id, get_task_uniqueid(task));
	coalition_unlock(coal);

	coal_dbg("id:%llu type:%s ref_count:%u, active_count:%u, kr=%d",
	    cid, coal_type_str(ct), rc, ac, kr);

	coalition_remove_active(coal);

	return kr;
}

/*
 * coalitions_adopt_task
 * Condition: All coalitions must be referenced and unlocked.
 * Will fail if any coalition is already terminated.
 */
kern_return_t
coalitions_adopt_task(coalition_t *coals, task_t task)
{
	int i;
	kern_return_t kr;

	if (!coals || coals[COALITION_TYPE_RESOURCE] == COALITION_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	/* verify that the incoming coalitions are what they say they are */
	for (i = 0; i < COALITION_NUM_TYPES; i++) {
		if (coals[i] && coals[i]->type != (uint32_t)i) {
			return KERN_INVALID_ARGUMENT;
		}
	}

	for (i = 0; i < COALITION_NUM_TYPES; i++) {
		kr = KERN_SUCCESS;
		if (coals[i]) {
			kr = coalition_adopt_task_internal(coals[i], task);
		}
		if (kr != KERN_SUCCESS) {
			/* dis-associate any coalitions that just adopted this task */
			while (--i >= 0) {
				if (task->coalition[i]) {
					coalition_remove_task_internal(task, i);
				}
			}
			break;
		}
	}
	return kr;
}

/*
 * coalitions_remove_task
 * Condition: task must be referenced and UNLOCKED; all task's coalitions must be UNLOCKED
 */
kern_return_t
coalitions_remove_task(task_t task)
{
	kern_return_t kr;
	int i;

	task_lock(task);
	if (!task_is_coalition_member(task)) {
		task_unlock(task);
		return KERN_SUCCESS;
	}

	task_clear_coalition_member(task);
	task_unlock(task);

	for (i = 0; i < COALITION_NUM_TYPES; i++) {
		kr = coalition_remove_task_internal(task, i);
		assert(kr == KERN_SUCCESS);
	}

	return kr;
}

/*
 * task_release_coalitions
 * helper function to release references to all coalitions in which
 * 'task' is a member.
 */
void
task_release_coalitions(task_t task)
{
	int i;
	for (i = 0; i < COALITION_NUM_TYPES; i++) {
		if (task->coalition[i]) {
			coalition_release(task->coalition[i]);
		} else if (i == COALITION_TYPE_RESOURCE) {
			panic("deallocating task %p was not a member of a resource coalition", task);
		}
	}
}

/*
 * coalitions_set_roles
 * for each type of coalition, if the task is a member of a coalition of
 * that type (given in the coalitions parameter) then set the role of
 * the task within that that coalition.
 */
kern_return_t
coalitions_set_roles(coalition_t coalitions[COALITION_NUM_TYPES],
    task_t task, int roles[COALITION_NUM_TYPES])
{
	kern_return_t kr = KERN_SUCCESS;
	int i;

	for (i = 0; i < COALITION_NUM_TYPES; i++) {
		if (!coalitions[i]) {
			continue;
		}
		coalition_lock(coalitions[i]);
		kr = coal_call(coalitions[i], set_taskrole, task, roles[i]);
		coalition_unlock(coalitions[i]);
		assert(kr == KERN_SUCCESS);
	}

	return kr;
}

/*
 * coalition_terminate_internal
 * Condition: Coalition must be referenced and UNLOCKED.
 */
kern_return_t
coalition_request_terminate_internal(coalition_t coal)
{
	assert(coal->type >= 0 && coal->type <= COALITION_TYPE_MAX);

	if (coal == init_coalition[coal->type]) {
		return KERN_DEFAULT_SET;
	}

	coalition_lock(coal);

	if (coal->reaped) {
		coalition_unlock(coal);
		return KERN_INVALID_NAME;
	}

	if (coal->terminated || coal->termrequested) {
		coalition_unlock(coal);
		return KERN_TERMINATED;
	}

	coal->termrequested = TRUE;

	boolean_t do_notify = FALSE;
	uint64_t note_id = 0;
	uint32_t note_flags = 0;

	if (coal->active_count == 0) {
		/*
		 * We only notify once, when active_count reaches zero.
		 * We just set termrequested to zero. If the active count
		 * was already at zero (tasks died before we could request
		 * a termination notification), we should notify.
		 */
		assert(!coal->terminated);
		coal->terminated = TRUE;

		assert(!coal->notified);

		coal->notified = TRUE;
#if DEVELOPMENT || DEBUG
		do_notify = coal->should_notify;
#else
		do_notify = TRUE;
#endif
		note_id = coal->id;
		note_flags = 0;
	}

	coalition_unlock(coal);

	if (do_notify) {
		coalition_notify_user(note_id, note_flags);
	}

	return KERN_SUCCESS;
}

/*
 * coalition_reap_internal
 * Condition: Coalition must be referenced and UNLOCKED.
 */
kern_return_t
coalition_reap_internal(coalition_t coal)
{
	assert(coal->type <= COALITION_TYPE_MAX);

	if (coal == init_coalition[coal->type]) {
		return KERN_DEFAULT_SET;
	}

	coalition_lock(coal);
	if (coal->reaped) {
		coalition_unlock(coal);
		return KERN_TERMINATED;
	}
	if (!coal->terminated) {
		coalition_unlock(coal);
		return KERN_FAILURE;
	}
	assert(coal->termrequested);
	if (coal->active_count > 0) {
		coalition_unlock(coal);
		return KERN_FAILURE;
	}

	coal->reaped = TRUE;

	/* Caller, launchd, and coalitions list should each have a reference */
	assert(coal->ref_count > 2);

	coalition_unlock(coal);

	lck_rw_lock_exclusive(&coalitions_list_lock);
	coalition_count--;
	remqueue(&coal->coalitions);
	lck_rw_unlock_exclusive(&coalitions_list_lock);

	/* Release the list's reference and launchd's reference. */
	coalition_release(coal);
	coalition_release(coal);

	return KERN_SUCCESS;
}

#if DEVELOPMENT || DEBUG
int
coalition_should_notify(coalition_t coal)
{
	int should;
	if (!coal) {
		return -1;
	}
	coalition_lock(coal);
	should = coal->should_notify;
	coalition_unlock(coal);

	return should;
}

void
coalition_set_notify(coalition_t coal, int notify)
{
	if (!coal) {
		return;
	}
	coalition_lock(coal);
	coal->should_notify = !!notify;
	coalition_unlock(coal);
}
#endif

void
coalitions_init(void)
{
	kern_return_t kr;
	int i;
	const struct coalition_type *ctype;

	queue_head_init(coalitions_q);

	if (!PE_parse_boot_argn("unrestrict_coalition_syscalls", &unrestrict_coalition_syscalls,
	    sizeof(unrestrict_coalition_syscalls))) {
		unrestrict_coalition_syscalls = 0;
	}

	if (!PE_parse_boot_argn("tg_adaptive", &merge_adaptive_coalitions,
	    sizeof(merge_adaptive_coalitions))) {
		merge_adaptive_coalitions = 0;
	}

	init_task_ledgers();

	init_coalition_ledgers();

	for (i = 0, ctype = &s_coalition_types[0]; i < COALITION_NUM_TYPES; ctype++, i++) {
		/* verify the entry in the global coalition types array */
		if (ctype->type != i ||
		    !ctype->init ||
		    !ctype->dealloc ||
		    !ctype->adopt_task ||
		    !ctype->remove_task) {
			panic("%s: Malformed coalition type %s(%d) in slot for type:%s(%d)",
			    __func__, coal_type_str(ctype->type), ctype->type, coal_type_str(i), i);
		}
		if (!ctype->has_default) {
			continue;
		}
		kr = coalition_create_internal(ctype->type, COALITION_ROLE_SYSTEM, TRUE, &init_coalition[ctype->type], NULL);
		if (kr != KERN_SUCCESS) {
			panic("%s: could not create init %s coalition: kr:%d",
			    __func__, coal_type_str(i), kr);
		}
		if (i == COALITION_TYPE_RESOURCE) {
			assert(COALITION_ID_KERNEL == init_coalition[ctype->type]->id);
		}
		kr = coalition_create_internal(ctype->type, COALITION_ROLE_SYSTEM, FALSE, &corpse_coalition[ctype->type], NULL);
		if (kr != KERN_SUCCESS) {
			panic("%s: could not create corpse %s coalition: kr:%d",
			    __func__, coal_type_str(i), kr);
		}
	}

	/* "Leak" our reference to the global object */
}

/*
 * BSD Kernel interface functions
 *
 */
static void
coalition_fill_procinfo(struct coalition *coal,
    struct procinfo_coalinfo *coalinfo)
{
	coalinfo->coalition_id = coal->id;
	coalinfo->coalition_type = coal->type;
	coalinfo->coalition_tasks = coalition_get_task_count(coal);
}


int
coalitions_get_list(int type, struct procinfo_coalinfo *coal_list, int list_sz)
{
	int ncoals = 0;
	struct coalition *coal;

	lck_rw_lock_shared(&coalitions_list_lock);
	qe_foreach_element(coal, &coalitions_q, coalitions) {
		if (!coal->reaped && (type < 0 || type == (int)coal->type)) {
			if (coal_list && ncoals < list_sz) {
				coalition_fill_procinfo(coal, &coal_list[ncoals]);
			}
			++ncoals;
		}
	}
	lck_rw_unlock_shared(&coalitions_list_lock);

	return ncoals;
}

/*
 * Return the coaltion of the given type to which the task belongs.
 */
coalition_t
task_get_coalition(task_t task, int coal_type)
{
	coalition_t c;

	if (task == NULL || coal_type > COALITION_TYPE_MAX) {
		return COALITION_NULL;
	}

	c = task->coalition[coal_type];
	assert(c == COALITION_NULL || (int)c->type == coal_type);
	return c;
}

/*
 * Report if the given task is the leader of the given jetsam coalition.
 */
boolean_t
coalition_is_leader(task_t task, coalition_t coal)
{
	boolean_t ret = FALSE;

	if (coal != COALITION_NULL) {
		coalition_lock(coal);

		ret = (coal->type == COALITION_TYPE_JETSAM && coal->j.leader == task);

		coalition_unlock(coal);
	}

	return ret;
}

kern_return_t
coalition_iterate_stackshot(coalition_iterate_fn_t callout, void *arg, uint32_t coalition_type)
{
	coalition_t coal;
	int i = 0;

	qe_foreach_element(coal, &coalitions_q, coalitions) {
		if (coal == NULL || !ml_validate_nofault((vm_offset_t)coal, sizeof(struct coalition))) {
			return KERN_FAILURE;
		}

		if (coalition_type == coal->type) {
			callout(arg, i++, coal);
		}
	}

	return KERN_SUCCESS;
}

task_t
kdp_coalition_get_leader(coalition_t coal)
{
	if (!coal) {
		return TASK_NULL;
	}

	if (coal->type == COALITION_TYPE_JETSAM) {
		return coal->j.leader;
	}
	return TASK_NULL;
}

task_t
coalition_get_leader(coalition_t coal)
{
	task_t leader = TASK_NULL;

	if (!coal) {
		return TASK_NULL;
	}

	coalition_lock(coal);
	if (coal->type != COALITION_TYPE_JETSAM) {
		goto out_unlock;
	}

	leader = coal->j.leader;
	if (leader != TASK_NULL) {
		task_reference(leader);
	}

out_unlock:
	coalition_unlock(coal);
	return leader;
}


int
coalition_get_task_count(coalition_t coal)
{
	int ntasks = 0;
	struct queue_entry *qe;
	if (!coal) {
		return 0;
	}

	coalition_lock(coal);
	switch (coal->type) {
	case COALITION_TYPE_RESOURCE:
		qe_foreach(qe, &coal->r.tasks)
		ntasks++;
		break;
	case COALITION_TYPE_JETSAM:
		if (coal->j.leader) {
			ntasks++;
		}
		qe_foreach(qe, &coal->j.other)
		ntasks++;
		qe_foreach(qe, &coal->j.extensions)
		ntasks++;
		qe_foreach(qe, &coal->j.services)
		ntasks++;
		break;
	default:
		break;
	}
	coalition_unlock(coal);

	return ntasks;
}


static uint64_t
i_get_list_footprint(queue_t list, int type, int *ntasks)
{
	task_t task;
	uint64_t bytes = 0;

	qe_foreach_element(task, list, task_coalition[type]) {
		bytes += get_task_phys_footprint(task);
		coal_dbg("    [%d] task_pid:%d, type:%d, footprint:%lld",
		    *ntasks, task_pid(task), type, bytes);
		*ntasks += 1;
	}

	return bytes;
}

uint64_t
coalition_get_page_count(coalition_t coal, int *ntasks)
{
	uint64_t bytes = 0;
	int num_tasks = 0;

	if (ntasks) {
		*ntasks = 0;
	}
	if (!coal) {
		return bytes;
	}

	coalition_lock(coal);

	switch (coal->type) {
	case COALITION_TYPE_RESOURCE:
		bytes += i_get_list_footprint(&coal->r.tasks, COALITION_TYPE_RESOURCE, &num_tasks);
		break;
	case COALITION_TYPE_JETSAM:
		if (coal->j.leader) {
			bytes += get_task_phys_footprint(coal->j.leader);
			num_tasks = 1;
		}
		bytes += i_get_list_footprint(&coal->j.extensions, COALITION_TYPE_JETSAM, &num_tasks);
		bytes += i_get_list_footprint(&coal->j.services, COALITION_TYPE_JETSAM, &num_tasks);
		bytes += i_get_list_footprint(&coal->j.other, COALITION_TYPE_JETSAM, &num_tasks);
		break;
	default:
		break;
	}

	coalition_unlock(coal);

	if (ntasks) {
		*ntasks = num_tasks;
	}

	return bytes / PAGE_SIZE_64;
}

struct coal_sort_s {
	int pid;
	int usr_order;
	uint64_t bytes;
};

/*
 * return < 0 for a < b
 *          0 for a == b
 *        > 0 for a > b
 */
typedef int (*cmpfunc_t)(const void *a, const void *b);

extern void
qsort(void *a, size_t n, size_t es, cmpfunc_t cmp);

static int
dflt_cmp(const void *a, const void *b)
{
	const struct coal_sort_s *csA = (const struct coal_sort_s *)a;
	const struct coal_sort_s *csB = (const struct coal_sort_s *)b;

	/*
	 * if both A and B are equal, use a memory descending sort
	 */
	if (csA->usr_order == csB->usr_order) {
		return (int)((int64_t)csB->bytes - (int64_t)csA->bytes);
	}

	/* otherwise, return the relationship between user specified orders */
	return csA->usr_order - csB->usr_order;
}

static int
mem_asc_cmp(const void *a, const void *b)
{
	const struct coal_sort_s *csA = (const struct coal_sort_s *)a;
	const struct coal_sort_s *csB = (const struct coal_sort_s *)b;

	return (int)((int64_t)csA->bytes - (int64_t)csB->bytes);
}

static int
mem_dec_cmp(const void *a, const void *b)
{
	const struct coal_sort_s *csA = (const struct coal_sort_s *)a;
	const struct coal_sort_s *csB = (const struct coal_sort_s *)b;

	return (int)((int64_t)csB->bytes - (int64_t)csA->bytes);
}

static int
usr_asc_cmp(const void *a, const void *b)
{
	const struct coal_sort_s *csA = (const struct coal_sort_s *)a;
	const struct coal_sort_s *csB = (const struct coal_sort_s *)b;

	return csA->usr_order - csB->usr_order;
}

static int
usr_dec_cmp(const void *a, const void *b)
{
	const struct coal_sort_s *csA = (const struct coal_sort_s *)a;
	const struct coal_sort_s *csB = (const struct coal_sort_s *)b;

	return csB->usr_order - csA->usr_order;
}

/* avoid dynamic allocation in this path */
#define MAX_SORTED_PIDS  80

static int
coalition_get_sort_list(coalition_t coal, int sort_order, queue_t list,
    struct coal_sort_s *sort_array, int array_sz)
{
	int ntasks = 0;
	task_t task;

	assert(sort_array != NULL);

	if (array_sz <= 0) {
		return 0;
	}

	if (!list) {
		/*
		 * this function will only be called with a NULL
		 * list for JETSAM-type coalitions, and is intended
		 * to investigate the leader process
		 */
		if (coal->type != COALITION_TYPE_JETSAM ||
		    coal->j.leader == TASK_NULL) {
			return 0;
		}
		sort_array[0].pid = task_pid(coal->j.leader);
		switch (sort_order) {
		case COALITION_SORT_DEFAULT:
			sort_array[0].usr_order = 0;
			OS_FALLTHROUGH;
		case COALITION_SORT_MEM_ASC:
		case COALITION_SORT_MEM_DEC:
			sort_array[0].bytes = get_task_phys_footprint(coal->j.leader);
			break;
		case COALITION_SORT_USER_ASC:
		case COALITION_SORT_USER_DEC:
			sort_array[0].usr_order = 0;
			break;
		default:
			break;
		}
		return 1;
	}

	qe_foreach_element(task, list, task_coalition[coal->type]) {
		if (ntasks >= array_sz) {
			printf("WARNING: more than %d pids in coalition %llu\n",
			    MAX_SORTED_PIDS, coal->id);
			break;
		}

		sort_array[ntasks].pid = task_pid(task);

		switch (sort_order) {
		case COALITION_SORT_DEFAULT:
			sort_array[ntasks].usr_order = 0;
			OS_FALLTHROUGH;
		case COALITION_SORT_MEM_ASC:
		case COALITION_SORT_MEM_DEC:
			sort_array[ntasks].bytes = get_task_phys_footprint(task);
			break;
		case COALITION_SORT_USER_ASC:
		case COALITION_SORT_USER_DEC:
			sort_array[ntasks].usr_order = 0;
			break;
		default:
			break;
		}

		ntasks++;
	}

	return ntasks;
}

int
coalition_get_pid_list(coalition_t coal, uint32_t rolemask, int sort_order,
    int *pid_list, int list_sz)
{
	struct i_jetsam_coalition *cj;
	int ntasks = 0;
	cmpfunc_t cmp_func = NULL;
	struct coal_sort_s sort_array[MAX_SORTED_PIDS] = { {0, 0, 0} }; /* keep to < 2k */

	if (!coal ||
	    !(rolemask & COALITION_ROLEMASK_ALLROLES) ||
	    !pid_list || list_sz < 1) {
		coal_dbg("Invalid parameters: coal:%p, type:%d, rolemask:0x%x, "
		    "pid_list:%p, list_sz:%d", coal, coal ? coal->type : -1,
		    rolemask, pid_list, list_sz);
		return -EINVAL;
	}

	switch (sort_order) {
	case COALITION_SORT_NOSORT:
		cmp_func = NULL;
		break;
	case COALITION_SORT_DEFAULT:
		cmp_func = dflt_cmp;
		break;
	case COALITION_SORT_MEM_ASC:
		cmp_func = mem_asc_cmp;
		break;
	case COALITION_SORT_MEM_DEC:
		cmp_func = mem_dec_cmp;
		break;
	case COALITION_SORT_USER_ASC:
		cmp_func = usr_asc_cmp;
		break;
	case COALITION_SORT_USER_DEC:
		cmp_func = usr_dec_cmp;
		break;
	default:
		return -ENOTSUP;
	}

	coalition_lock(coal);

	if (coal->type == COALITION_TYPE_RESOURCE) {
		ntasks += coalition_get_sort_list(coal, sort_order, &coal->r.tasks,
		    sort_array, MAX_SORTED_PIDS);
		goto unlock_coal;
	}

	cj = &coal->j;

	if (rolemask & COALITION_ROLEMASK_UNDEF) {
		ntasks += coalition_get_sort_list(coal, sort_order, &cj->other,
		    sort_array + ntasks,
		    MAX_SORTED_PIDS - ntasks);
	}

	if (rolemask & COALITION_ROLEMASK_XPC) {
		ntasks += coalition_get_sort_list(coal, sort_order, &cj->services,
		    sort_array + ntasks,
		    MAX_SORTED_PIDS - ntasks);
	}

	if (rolemask & COALITION_ROLEMASK_EXT) {
		ntasks += coalition_get_sort_list(coal, sort_order, &cj->extensions,
		    sort_array + ntasks,
		    MAX_SORTED_PIDS - ntasks);
	}

	if (rolemask & COALITION_ROLEMASK_LEADER) {
		ntasks += coalition_get_sort_list(coal, sort_order, NULL,
		    sort_array + ntasks,
		    MAX_SORTED_PIDS - ntasks);
	}

unlock_coal:
	coalition_unlock(coal);

	/* sort based on the chosen criterion (no sense sorting 1 item) */
	if (cmp_func && ntasks > 1) {
		qsort(sort_array, ntasks, sizeof(struct coal_sort_s), cmp_func);
	}

	for (int i = 0; i < ntasks; i++) {
		if (i >= list_sz) {
			break;
		}
		coal_dbg(" [%d] PID:%d, footprint:%lld, usr_order:%d",
		    i, sort_array[i].pid, sort_array[i].bytes,
		    sort_array[i].usr_order);
		pid_list[i] = sort_array[i].pid;
	}

	return ntasks;
}
