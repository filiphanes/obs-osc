#include "osc-ui-task.h"

#include <obs.h>

#include <memory>

static void run_on_ui(std::function<void()> fn, bool wait)
{
	auto *heap = new std::function<void()>(std::move(fn));

	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			const std::unique_ptr<std::function<void()>> f((std::function<void()> *)param);
			(*f)();
		},
		heap, wait);
}

void osc_ui_post(std::function<void()> fn)
{
	run_on_ui(std::move(fn), false);
}

void osc_ui_run(std::function<void()> fn)
{
	run_on_ui(std::move(fn), true);
}
