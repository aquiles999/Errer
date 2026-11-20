#pragma once

void InstallCrashLog(const char* who);
void CrashLog(const char* what);
void CrashLogf(const char* fmt, ...);