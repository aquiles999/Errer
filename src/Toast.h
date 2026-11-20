#pragma once
#include <windows.h>

// Taskbar balloon (Windows 7) / real WinRT toast (Windows 8+), choosing the
// path automatically. Falls back to the balloon on 8+ if WinRT can't activate.
void ShowToast(const wchar_t* title, const wchar_t* body, DWORD timeoutMs = 6000);
void ShowRandomErrorToast(DWORD timeoutMs = 6000);
void DismissToast();

// Background ticker that summons one random error toast every intervalMs
// (default 1000). One thread per payload; it lives for the payload's lifetime
// and dies with the process, so no explicit stop is required.
void StartRandomErrorToasts(DWORD intervalMs = 1000);

// Toast timeline: waits delays[i] ms (relative to the previous toast), then
// shows a random error toast, dismisses it, and repeats. The array must have
// static storage duration (it is read until the payload ends).
void StartToastTimeline(const DWORD* delays, int count);

// Dismisses every live toast in this session, including ones owned by other
// errer processes. Called automatically when a payload starts its toasts.
void DismissAllToasts();

// Stops the toast timeline, hides any live toast and clears the queue. Call
// right before a payload process terminates so nothing lingers into the next
// payload.
void ToastShutdown();