#ifndef _NUVIX_PID_INTERNAL_H
#define _NUVIX_PID_INTERNAL_H

#include <nuvix/pid.h>
#include <nuvix/task.h>

typedef bool (*pid_task_visit_fn)(struct task_struct *task, void *arg);

void pid_attach_task(pid_t pid, struct task_struct *task);
void pid_detach_task(pid_t pid, struct task_struct *task);
struct task_struct *pid_task_get(pid_t pid) __must_check;
bool pid_visit_published(pid_task_visit_fn visit, void *arg) __must_check;

#endif
