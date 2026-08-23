#pragma once

/*
 * Helpers for hopping onto libobs' UI thread. Centralizes the two
 * patterns the plugin needs - fire-and-forget and blocking - instead
 * of hand-rolling obs_queue_task() plumbing at each call site.
 */

#include <functional>

/* Runs fn on the UI thread and returns immediately. Safe to call from
 * any thread. */
void osc_ui_post(std::function<void()> fn);

/* Runs fn on the UI thread and waits for it to complete. The calling
 * thread must not be the UI thread. */
void osc_ui_run(std::function<void()> fn);
