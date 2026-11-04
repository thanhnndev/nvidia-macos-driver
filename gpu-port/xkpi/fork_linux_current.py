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
# record a new mm on the process so later threads share it. PROC_LOCK is no lock here, so the mm is published with
# a compare-and-swap: two first threads of one process must not each install their own (a userptr BO made on one
# thread would then fail the mm check of a submit from the other).
i = s.index("\tproc = td->td_proc;\n\n\tPROC_LOCK(proc);\n\tmm_other = find_other_mm(proc);\n")
j = s.index("\t/* store pointer to task struct */\n", i)
s = s[:i] + """	proc = td->td_proc;

	/* macOS port: release what exited processes and recycled thread records left (it may sleep) */
	if ((flags & M_WAITOK) != 0)
		nmx_proc_reap();

	PROC_LOCK(proc);
	mm_other = find_other_mm(proc);

	/* use allocated mm_struct as a fallback */
	if (mm_other == NULL) {
		PROC_UNLOCK(proc);
		mm = uma_zalloc(linux_mm_zone, flags | M_ZERO);
		if (mm == NULL) {
			uma_zfree(linux_current_zone, ts);
			return (ENOMEM);
		}
		/* setup new mm_struct: one user for this task, one for the process record */
		init_rwsem(&mm->mmap_sem);
		atomic_set(&mm->mm_count, 1);
		atomic_set(&mm->mm_users, 2);

		PROC_LOCK(proc);
		for (;;) {
			if (atomic_cmpset_ptr((volatile uintptr_t *)&proc->p_lkpi_mm, 0, (uintptr_t)mm)) {
				/* set mm_struct pointer */
				ts->mm = mm;
				/* macOS port: remember the Mach task, for driver workers that copy to this process */
				nmx_xnu_mm_note_task(mm);
				/* clear pointer to not free memory */
				mm = NULL;
				break;
			}
			/* another thread of this process published one first: share it */
			mm_other = find_other_mm(proc);
			if (mm_other != NULL) {
				ts->mm = mm_other;
				sx_destroy(&mm->mmap_sem.sx);
				break;
			}
		}
	} else {
		ts->mm = mm_other;
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
