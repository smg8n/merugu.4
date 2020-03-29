#ifndef PROCESS_CTRL_BLOCK_H
#define PROCESS_CTRL_BLOCK_H

#define ONE_BILLION 1000000000
#define TWO_BILLION 2000000000
#define TEN_MILLION 10000000
#define NUM_QUEUES 4
#define PROC_CTRL_TBL_SZE 18
#define QUEUESIZE PROC_CTRL_TBL_SZE
#define MSGSZ 50
const unsigned int EXECV_SIZE = 6;
const unsigned int SYSCLOCK_ID_IDX = 1;
const unsigned int PCT_ID_IDX = 2;
const unsigned int PID_IDX = 3;
const unsigned int SCHEDULER_IDX = 4;

struct clock {
    unsigned long seconds;
    unsigned long nanoseconds;
};

struct msgbuf {
    long mtype;
    char mtext[MSGSZ];
};

enum Status { 
    RUNNING,
    READY,
    TERMINATED, 
    BLOCKED,
};

struct process_ctrl_block {
    int pid;
    enum Status status;
    bool is_realtime;
    unsigned int time_quantum;
    struct clock cpu_time_used;     // total CPU time used
    struct clock sys_time_used;     // total system time used
    unsigned long last_run;          // length of time of last run in nanoseconds
    struct clock time_blocked;      // total time process is blocked for
    struct clock time_unblocked;    // time when the process is unblocked
    struct clock time_scheduled;    // time when process is scheduled
    struct clock time_finished;     // time when process terminates
};

struct process_ctrl_table {
    struct process_ctrl_block pcbs[PROC_CTRL_TBL_SZE];
};

struct Queue {
        int q[QUEUESIZE+1];		/* body of queue */
        int first;                      /* position of first element */
        int last;                       /* position of last element */
        int count;                      /* number of queue elements */
};



struct Statistics {
    unsigned int turnaround_time;   // time it took to schedule a process in nanoseconds
    struct clock wait_time;         // how long a process waited to be scheduled
    struct clock sleep_time;        // how long processes were blocked
    struct clock idle_time;         // how long CPU was not running a user process: equals (sys time - CPU time)
    struct clock total_cpu_time;
    struct clock total_sys_time; 
};
#endif // PROCESS_CTRL_BLOCK_H
