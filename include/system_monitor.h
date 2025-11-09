#ifndef SYSTEM_MONITOR_H
#define SYSTEM_MONITOR_H

#include "process.h"
#include <string>
#include <vector>
#include <unordered_map>

class SystemMonitor {
public:
    SystemMonitor(int refresh_seconds = 2);
    void run(); // start monitoring loop

private:
    int refresh_seconds_;
    unsigned long prev_total_jiffies_;
    std::unordered_map<int, unsigned long> prev_proc_jiffies_; // pid -> total jiffies at previous sample
    unsigned long clock_ticks_per_sec_;
    long page_size_kb_;

    // helpers
    unsigned long read_total_jiffies();
    bool parse_meminfo(unsigned long &total_kb, unsigned long &free_kb, unsigned long &avail_kb);
    double read_uptime_seconds();
    std::vector<ProcessInfo> collect_processes(unsigned long total_jiffies, unsigned long total_ram_kb);
    bool read_process_stat(int pid, ProcessInfo &p);
    std::string uid_to_username(int uid);
    void print_header(double cpu_usage_percent, unsigned long total_kb, unsigned long free_kb, unsigned long avail_kb, double uptime);
    void clear_screen();
};

#endif // SYSTEM_MONITOR_H
