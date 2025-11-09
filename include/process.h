#ifndef PROCESS_H
#define PROCESS_H

#include <string>

struct ProcessInfo {
    int pid;
    std::string user;
    std::string cmd;
    unsigned long utime;   // user jiffies
    unsigned long stime;   // system jiffies
    unsigned long total_time; // utime + stime
    long rss;              // resident set size in kB
    double cpu_percent;    // computed at update
    double mem_percent;    // computed at update
};

#endif // PROCESS_H
