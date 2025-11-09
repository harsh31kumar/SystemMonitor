#include "system_monitor.h"
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <cstring>

SystemMonitor::SystemMonitor(int refresh_seconds)
: refresh_seconds_(refresh_seconds),
  prev_total_jiffies_(0)
{
    clock_ticks_per_sec_ = sysconf(_SC_CLK_TCK);
    page_size_kb_ = sysconf(_SC_PAGESIZE) / 1024; // pagesize in kB
}

unsigned long SystemMonitor::read_total_jiffies() {
    std::ifstream f("/proc/stat");
    if (!f) return 0;
    std::string line;
    std::getline(f, line);
    // first line: cpu  user nice system idle iowait irq softirq steal guest guest_nice
    std::istringstream ss(line);
    std::string cpu_label;
    ss >> cpu_label;
    unsigned long v, total = 0;
    while (ss >> v) total += v;
    return total;
}

bool SystemMonitor::parse_meminfo(unsigned long &total_kb, unsigned long &free_kb, unsigned long &avail_kb) {
    std::ifstream f("/proc/meminfo");
    if (!f) return false;
    std::string line;
    total_kb = free_kb = avail_kb = 0;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string key;
        unsigned long val;
        std::string unit;
        ss >> key >> val >> unit;
        if (key == "MemTotal:") total_kb = val;
        else if (key == "MemFree:") free_kb = val;
        else if (key == "MemAvailable:") avail_kb = val;
        if (total_kb && free_kb && avail_kb) break;
    }
    return total_kb != 0;
}

double SystemMonitor::read_uptime_seconds() {
    std::ifstream f("/proc/uptime");
    if (!f) return 0.0;
    double up = 0.0;
    f >> up;
    return up;
}

bool SystemMonitor::read_process_stat(int pid, ProcessInfo &p) {
    p.pid = pid;
    // read /proc/[pid]/stat
    std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream fs(stat_path);
    if (!fs) return false;
    // stat file format: pid (comm) state ppid ... utime stime ...
    std::string content;
    std::getline(fs, content);
    if (content.empty()) return false;

    // Extract command properly (between parentheses)
    size_t lparen = content.find('(');
    size_t rparen = content.rfind(')');
    if (lparen == std::string::npos || rparen == std::string::npos || rparen <= lparen) return false;
    p.cmd = content.substr(lparen + 1, rparen - lparen - 1);

    // tokenize after rparen
    std::istringstream ss(content.substr(rparen + 2));
    // fields start from state as first token now
    std::string token;
    // We need to skip fields to reach utime (14th and 15th fields in original numbering)
    // After rparen, the next tokens are: state (1), ppid (2), pgrp (3), session (4),
    // tty_nr (5), tpgid (6), flags (7), minflt (8), cminflt (9), majflt (10),
    // cmajflt (11), utime (12), stime (13) ... But because we cut after rparen, indices shift.
    // We'll read tokens up to utime & stime by reading tokens into a vector.
    std::vector<std::string> toks;
    while (ss >> token) toks.push_back(token);
    if (toks.size() < 15) return false; // need enough tokens
    // utime is token[11] (0-based), stime token[12]
    unsigned long utime = std::stoul(toks[11]);
    unsigned long stime = std::stoul(toks[12]);
    long rss_pages = 0;
    // rss typically at field index 21 (0-based 20) relative to tokens after rparen.
    if (toks.size() > 21) {
        try { rss_pages = std::stol(toks[21]); } catch(...) { rss_pages = 0; }
    }
    p.utime = utime;
    p.stime = stime;
    p.total_time = utime + stime;
    p.rss = rss_pages * page_size_kb_;
    // get uid from /proc/[pid]/status
    std::string status_path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream stf(status_path);
    if (stf) {
        std::string line2;
        while (std::getline(stf, line2)) {
            if (line2.rfind("Uid:", 0) == 0) {
                std::istringstream s2(line2);
                std::string k; int uid;
                s2 >> k >> uid;
                p.user = uid_to_username(uid);
                break;
            }
        }
    } else {
        p.user = "?";
    }
    return true;
}

std::string SystemMonitor::uid_to_username(int uid) {
    struct passwd *pw = getpwuid(uid);
    if (pw) return std::string(pw->pw_name);
    return std::to_string(uid);
}

std::vector<ProcessInfo> SystemMonitor::collect_processes(unsigned long total_jiffies, unsigned long total_ram_kb) {
    std::vector<ProcessInfo> list;
    DIR *d = opendir("/proc");
    if (!d) return list;
    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            const char *name = entry->d_name;
            bool is_num = true;
            for (const char *c = name; *c; ++c) if (!isdigit(*c)) { is_num = false; break; }
            if (!is_num) continue;
            int pid = atoi(name);
            ProcessInfo p;
            if (!read_process_stat(pid, p)) continue;
            unsigned long prev = 0;
            auto it = prev_proc_jiffies_.find(pid);
            if (it != prev_proc_jiffies_.end()) prev = it->second;
            unsigned long delta_proc = 0;
            if (prev != 0 && p.total_time >= prev) delta_proc = p.total_time - prev;
            unsigned long delta_total = 0;
            if (prev_total_jiffies_ != 0 && total_jiffies >= prev_total_jiffies_)
                delta_total = total_jiffies - prev_total_jiffies_;
            double cpu = 0.0;
            if (delta_total > 0) {
                cpu = (double)delta_proc / (double)delta_total * 100.0;
            }
            p.cpu_percent = cpu;
            if (total_ram_kb > 0)
                p.mem_percent = (double)p.rss / (double)total_ram_kb * 100.0;
            else p.mem_percent = 0.0;
            // save current jiffies for next iteration
            prev_proc_jiffies_[pid] = p.total_time;
            list.push_back(p);
        }
    }
    closedir(d);
    return list;
}

void SystemMonitor::clear_screen() {
    // ANSI clear screen
    std::cout << "\033[2J\033[1;1H";
}

void SystemMonitor::print_header(double cpu_usage_percent, unsigned long total_kb, unsigned long free_kb, unsigned long avail_kb, double uptime) {
    std::cout << "Simple System Monitor (refresh every " << refresh_seconds_ << "s)\n";
    std::cout << "Uptime: " << std::fixed << std::setprecision(0) << uptime << "s"
              << " | CPU usage: " << std::fixed << std::setprecision(2) << cpu_usage_percent << "%\n";
    std::cout << "Memory: total " << total_kb/1024 << "MB"
              << " free " << free_kb/1024 << "MB"
              << " avail " << avail_kb/1024 << "MB\n";
    std::cout << "-------------------------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(7) << "PID" << std::setw(10) << "USER" << std::setw(7) << "CPU%" << std::setw(7) << "MEM%" << std::setw(10) << "RSS(kB)" << "CMD\n";
    std::cout << "-------------------------------------------------------------------------------------------------\n";
}

void SystemMonitor::run() {
    // initial sample
    prev_total_jiffies_ = read_total_jiffies();
    unsigned long total_kb=0, free_kb=0, avail_kb=0;
    parse_meminfo(total_kb, free_kb, avail_kb);
    double uptime = read_uptime_seconds();

    while (true) {
        sleep(refresh_seconds_);
        unsigned long curr_total = read_total_jiffies();
        unsigned long total_jiffies_delta = 0;
        if (curr_total >= prev_total_jiffies_) total_jiffies_delta = curr_total - prev_total_jiffies_;
        double cpu_percent_overall = 0.0;
        if (prev_total_jiffies_ != 0 && curr_total >= prev_total_jiffies_) {
            // approximate CPU usage as 1 - idle/total, but we did not parse idle separately.
            // For simplicity we compute CPU usage by reading /proc/stat second time and computing (1 - idle_delta/total_delta)
            std::ifstream f("/proc/stat");
            std::string line;
            if (f && std::getline(f, line)) {
                std::istringstream ss(line);
                std::string cpu;
                ss >> cpu;
                unsigned long user=0, nice=0, system=0, idle=0, iowait=0, irq=0, softirq=0, steal=0;
                ss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
                static unsigned long prev_idle = 0;
                static unsigned long prev_total = 0;
                unsigned long idle_now = idle + iowait;
                unsigned long total_now = user + nice + system + idle + iowait + irq + softirq + steal;
                if (prev_total != 0 && total_now >= prev_total) {
                    unsigned long totald = total_now - prev_total;
                    unsigned long idled = idle_now - prev_idle;
                    if (totald > 0) cpu_percent_overall = (double)(totald - idled) / (double)totald * 100.0;
                }
                prev_idle = idle_now;
                prev_total = total_now;
            }
        }

        // meminfo & uptime
        parse_meminfo(total_kb, free_kb, avail_kb);
        uptime = read_uptime_seconds();

        // collect processes
        auto procs = collect_processes(curr_total, total_kb);
        // sort by cpu desc
        std::sort(procs.begin(), procs.end(), [](const ProcessInfo &a, const ProcessInfo &b){
            if (a.cpu_percent == b.cpu_percent) return a.rss > b.rss;
            return a.cpu_percent > b.cpu_percent;
        });

        // display top N
        clear_screen();
        print_header(cpu_percent_overall, total_kb, free_kb, avail_kb, uptime);
        int show = std::min<int>(10, procs.size());
        for (int i = 0; i < show; ++i) {
            const auto &p = procs[i];
            std::cout << std::left << std::setw(7) << p.pid
                      << std::setw(10) << (p.user.size() > 9 ? p.user.substr(0,9) : p.user)
                      << std::setw(7) << std::fixed << std::setprecision(2) << p.cpu_percent
                      << std::setw(7) << std::fixed << std::setprecision(2) << p.mem_percent
                      << std::setw(10) << p.rss
                      << p.cmd << "\n";
        }
        std::cout << "-------------------------------------------------------------------------------------------------\n";
        std::cout << "Total processes: " << procs.size() << " | Press Ctrl+C to quit\n";

        // save for next iteration
        prev_total_jiffies_ = curr_total;
    }
}
