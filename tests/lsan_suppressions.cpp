// LeakSanitizer suppressions for the test binary: one-time allocations made
// by system libraries during device enumeration (ALSA, NVIDIA, X11) are not
// rendy leaks. Only consulted when LSan is active (asan preset).

extern "C" const char* __lsan_default_suppressions();
extern "C" const char* __lsan_default_suppressions() {
    return "leak:libasound\n"
           "leak:libpulse\n"
           "leak:hotplug_device_process\n" // SDL ALSA device-name strdups
           "leak:libnvidia\n"
           "leak:libGLX\n"
           "leak:libX11\n"
           "leak:_dl_catch_exception\n";
}
