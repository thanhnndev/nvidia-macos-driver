# fork_linux_current.py <override/src/linux_current.c>: macOS forms of LinuxKPI's per-thread task_struct code.
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
def replace_func(sig_line, new_body):
    global s
    anchored = "\n" + sig_line
    assert s.count(anchored) == 1, (sig_line, s.count(anchored))
    i = s.index(anchored) + 1
    start = s.rfind("\n\n", 0, i) + 2
    end = s.index("\n}\n", i) + 3
    s = s[:start] + new_body.strip("\n") + "\n" + s[end:]
def replace_once(old, new):
    global s
    assert s.count(old) == 1, (old[:60], s.count(old))
    s = s.replace(old, new)

replace_func("find_other_mm(struct proc *p)\n", r"""
/* macOS port: the mm -> Mach task record (nmx_mm.cpp), declared here because this file sees no IOKit headers */
void nmx_xnu_mm_note_task(void *mm);
void nmx_xnu_mm_forget(void *mm);

/*
 * macOS port: one mm_struct per process, kept on the port's process record (FreeBSD finds it by walking the
 * process's threads, which xnu does not let a kext do). The record holds one mm_users reference from the moment the
 * mm is published until the port retires the record (the process is gone, nmx_proc_reap), so a published mm is
 * never freed under the pointer and every thread of the process gets the same one.
 */
static struct mm_struct *
find_other_mm(struct proc *p)
{
	struct mm_struct *mm;

	mm = p->p_lkpi_mm;
	if (mm != NULL && atomic_inc_not_zero(&mm->mm_users))
		return (mm);
	return (NULL);
}
""")
# record a new mm on the process so later threads share it (37bac65): PROC_LOCK is no lock here, so the mm is looked up
# and published (compare-and-swap) and the task stored under the port's registry lock while td_proc is still the
# process read (nmx_thread_proc_locked, with the two helpers below); a thread moved to kernproc meanwhile looks again.
# Two first threads of one process must not each install their own mm (a userptr BO made on one thread would then
# fail the mm check of a submit from the other), and nothing may be published into a retired record.
replace_once("\tstruct mm_struct *mm, *mm_other;\n", "\tstruct mm_struct *mm;\n")
replace_once("int\nlinux_alloc_current(", r"""/* macOS port: run under the registry lock by nmx_thread_proc_locked (a spin lock: nothing here may sleep) */
struct linux_current_find {
	struct proc *proc;
	struct task_struct *ts;
	struct mm_struct *mm;
};

static int
linux_current_find_locked(void *arg)
{
	struct linux_current_find *a = arg;

	a->mm = find_other_mm(a->proc);
	if (a->mm != NULL) {
		a->ts->mm = a->mm;
		/* store pointer to task struct */
		a->ts->task_thread->td_lkpi_task = a->ts;
	}
	return (0);
}

struct linux_current_pub {
	struct proc *proc;
	struct task_struct *ts;
	struct mm_struct *mm;
	int published;
};

static int
linux_current_publish_locked(void *arg)
{
	struct linux_current_pub *a = arg;

	if (!atomic_cmpset_ptr((volatile uintptr_t *)&a->proc->p_lkpi_mm, 0, (uintptr_t)a->mm)) {
		a->published = -1;
		return (0);
	}
	a->ts->mm = a->mm;
	a->ts->task_thread->td_lkpi_task = a->ts;
	a->published = 1;
	return (0);
}

""" + "int\nlinux_alloc_current(")
i = s.index("\tproc = td->td_proc;\n\n\tPROC_LOCK(proc);\n\tmm_other = find_other_mm(proc);\n")
j = s.index("\treturn (0);\n}", i)
s = s[:i] + r"""	/* macOS port: release what exited processes and recycled thread records left (it may sleep). It runs before
	 * td_proc is read: it can retire this thread's own process (an exit-time close, when proc_find already fails)
	 * and move the thread to kernproc (review of fdffd32) */
	if ((flags & M_WAITOK) != 0)
		nmx_proc_reap();

	/*
	 * macOS port: the mm is looked up and published, and the task stored, under the port's registry lock while
	 * td_proc is still the process read here (nmx_thread_proc_locked). nmx_proc_reap moves a retired process's threads
	 * to kernproc under that lock and drops the record's mm after that walk, so nothing is published into a retired
	 * record (it would be freed without mmput, leaking the mm and its Mach task reference) and a record freed during
	 * the allocation below is never written: the check fails and the lookup starts over with the new td_proc.
	 */
again:
	proc = td->td_proc;
	struct linux_current_find fa = { .proc = proc, .ts = ts, .mm = NULL };
	if (nmx_thread_proc_locked(td, proc, linux_current_find_locked, &fa) < 0)
		goto again;
	if (fa.mm == NULL) {
		if (mm == NULL) {
			mm = uma_zalloc(linux_mm_zone, flags | M_ZERO);
			if (mm == NULL) {
				uma_zfree(linux_current_zone, ts);
				return (ENOMEM);
			}
			/* setup new mm_struct: one user for this task, one for the process record */
			init_rwsem(&mm->mmap_sem);
			atomic_set(&mm->mm_count, 1);
			atomic_set(&mm->mm_users, 2);
			/* macOS port: remember the Mach task, for driver workers that copy to this process. Noted before
			 * it is published: once it is, the reaper can drop the last reference (linux_mm_dtor forgets it) */
			nmx_xnu_mm_note_task(mm);
		}
		struct linux_current_pub pa = { .proc = proc, .ts = ts, .mm = mm, .published = 0 };
		if (nmx_thread_proc_locked(td, proc, linux_current_publish_locked, &pa) < 0 || pa.published < 0)
			goto again;	/* reparented, or another thread of this process published first: look again */
		mm = NULL;
	}

	/* free the mm_struct another thread's made unneeded, if any */
	if (mm != NULL) {
		nmx_xnu_mm_forget(mm);
		sx_destroy(&mm->mmap_sem.sx);
		uma_zfree(linux_mm_zone, mm);
	}

""" + s[j:]

replace_func("linux_mm_dtor(struct mm_struct *mm)\n", r"""
/*
 * macOS port: the last reference is gone. No process record points at the mm any more (a record holds a reference
 * while it does), so there is nothing to clear; the Mach task record goes, and the lock the rwsem allocated.
 */
void
linux_mm_dtor(struct mm_struct *mm)
{
	nmx_xnu_mm_forget(mm);
	sx_destroy(&mm->mmap_sem.sx);
	uma_zfree(linux_mm_zone, mm);
}
""")

replace_func("linux_get_pid_task_int(pid_t pid, const bool do_get)\n", r"""
/* macOS port: threads and processes are found through the port's records (nmx_kernel.c) */
static struct task_struct *
linux_get_pid_task_int(pid_t pid, const bool do_get)
{
	struct thread *td;
	struct task_struct *ts;
	struct proc *p;

	if (pid > PID_MAX) {
		td = tdfind(pid, -1);
		if (td == NULL)
			return (NULL);
		ts = td->td_lkpi_task;
		if (do_get && ts != NULL)
			get_task_struct(ts);
		return (ts);
	}
	p = nmx_proc_find(pid);
	if (p == NULL)
		return (NULL);
	return (nmx_proc_any_task(p, do_get));
}
""")

replace_func("linux_task_exiting(struct task_struct *task)\n", r"""
/* macOS port: whether the task's process is exiting comes from xnu (proc_exiting) */
bool
linux_task_exiting(struct task_struct *task)
{
	struct thread *td;
	struct proc *p;

	td = tdfind(task->pid, -1);
	p = (td != NULL) ? td->td_proc : nmx_proc_find(task->pid);
	return (p != NULL && nmx_proc_exiting(p));
}
""")

replace_func("linux_current_init(void *arg __unused)\n", r"""
/*
 * macOS port: task and mm structures come from plain zones (xnu has no reserve to pre-fill), and FreeBSD's
 * thread-destructor event is the port's release hook, run when a thread's record is dropped (end of a system call,
 * exit of a kernel thread the port started).
 */
static void
linux_current_init(void *arg __unused)
{
	linux_current_zone = uma_zcreate("lkpicurr",
	    sizeof(struct task_struct), NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, 0);
	linux_mm_zone = uma_zcreate("lkpimm",
	    sizeof(struct mm_struct), NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, 0);

	atomic_thread_fence_seq_cst();

	nmx_thread_release_hook = linuxkpi_thread_dtor_hook;
	nmx_task_release_hook = linuxkpi_task_release_hook;
	nmx_proc_release_hook = linuxkpi_proc_release_hook;
	lkpi_alloc_current = linux_alloc_current;
}
""")

replace_func("linux_current_uninit(void *arg __unused)\n", r"""
static void
linux_current_uninit(void *arg __unused)
{
	lkpi_alloc_current = linux_alloc_current_noop;
	nmx_thread_release_hook = NULL;

	atomic_thread_fence_seq_cst();

	nmx_threads_release_all(linuxkpi_thread_dtor_hook);
	/* what is still pending, then the hooks (the records themselves go with the layer) */
	nmx_proc_reap();
	nmx_task_release_hook = NULL;
	nmx_proc_release_hook = NULL;

	uma_zdestroy(linux_current_zone);
	uma_zdestroy(linux_mm_zone);
}
""")

# the release hook: FreeBSD's destructor body, under the hook's signature
replace_func("linuxkpi_thread_dtor(void *arg __unused, struct thread *td)\n", r"""
static void
linuxkpi_thread_dtor_hook(struct thread *td)
{
	struct task_struct *ts;

	ts = td->td_lkpi_task;
	if (ts == NULL)
		return;

	td->td_lkpi_task = NULL;
	put_task_struct(ts);
}

/* macOS port: a task taken from a recycled thread record (nmx_proc_reap) */
static void
linuxkpi_task_release_hook(struct task_struct *ts)
{
	put_task_struct(ts);
}

/* macOS port: a retired process record gives up its mm reference (nmx_proc_reap) */
static void
linuxkpi_proc_release_hook(struct proc *p)
{
	struct mm_struct *mm;

	mm = (struct mm_struct *)atomic_readandclear_ptr((volatile uintptr_t *)&p->p_lkpi_mm);
	if (mm != NULL)
		mmput(mm);
}
""")
replace_once("static eventhandler_tag linuxkpi_thread_dtor_tag;\n", "")
# uma reserve tunable: xnu allocator has no reserve
i = s.index("static int lkpi_task_resrv;\n")
j = s.index("\n\n", s.index('"allocations");', i)) + 2
s = s[:i] + s[j:]

# get_task_struct for the port's lookup helper (it takes the reference under the registry lock)
s += """
/* macOS port: reference a task from the port's registry walk (nmx_proc_any_task) */
void nmx_task_get(struct task_struct *ts);
void
nmx_task_get(struct task_struct *ts)
{
	get_task_struct(ts);
}
"""
p.write_text(s)
print("linux_current.c fork applied")
