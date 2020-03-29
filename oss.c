#include <stdio.h>
#include <sys/wait.h>
#include <getopt.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <string.h>
#include <locale.h>
#include <sys/time.h>
#include <time.h>
#include <sys/queue.h>
#include <math.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/shm.h>
#include <stdio.h>
#include <signal.h>

#define ONE_BILLION 1000000000
#define TWO_BILLION 2000000000
#define TEN_MILLION 10000000
#define NUM_QUEUES 4
#define PROC_CTRL_TBL_SZE 18
#define QUEUESIZE PROC_CTRL_TBL_SZE
#define MSGSZ 50
#define ONE_BILLION 1000000000

int pcap;
const unsigned int EXECV_SIZE = 6;
const unsigned int SYSCLOCK_ID_IDX = 1;
const unsigned int PCT_ID_IDX = 2;
const unsigned int PID_IDX = 3;
const unsigned int SCHEDULER_IDX = 4;

const unsigned int TOTAL_PROC_LIMIT = 100;

const unsigned int MAX_RUNTIME = 20; // In seconds

const unsigned int MAX_NS_BEFORE_NEW_PROC = TWO_BILLION;
const unsigned int PCT_REALTIME = 3;
const unsigned int BASE_TIME_QUANTUM = TEN_MILLION; // 10 ms == 10 million nanoseconds

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

void wait_for_all_children();
void add_signal_handlers();
void handle_sigint(int sig);
void handle_sigalrm(int sig);
void cleanup_and_exit();
void fork_child(char** execv_arr, int child_idx, int pid);
struct clock convertToClockTime(int nanoseconds);
bool process_is_realtime();
unsigned int get_amt_time_to_schedule();
void print_and_write(char* str);
struct Statistics get_new_stats();

int get_shared_memory();
void* attach_to_shared_memory(int shmemid, unsigned int readonly);
void cleanup_shared_memory(int shmemid, void* p);
void detach_from_shared_memory(void* p);
void deallocate_shared_memory(int shmemid);

void init_queue(struct Queue *q);
void enqueue(struct Queue *q, int x);
int dequeue(struct Queue *q);
bool empty(struct Queue *q);
void print_queue(struct Queue *q);

int get_message_queue();
void remove_message_queue(int msgqid);
void receive_msg(int msgqid, struct msgbuf* mbuf, int mtype);
void send_msg(int msgqid, struct msgbuf* mbuf, int mtype);

char** split_string(char* str, char* delimeter);
char* get_timestamp();
void print_usage();
void parse_cmd_line_args(int argc, char* argv[]);
void set_timer(int duration);
bool event_occured(unsigned int pct_chance);

void increment_clock(struct clock* clock, int increment);
struct clock add_clocks(struct clock c1, struct clock c2);
int compare_clocks(struct clock c1, struct clock c2);
long double clock_to_seconds(struct clock c);
struct clock seconds_to_clock(long double seconds);
struct clock calculate_avg_time(struct clock clk, int divisor);
struct clock subtract_clocks(struct clock c1, struct clock c2);
void print_clock(char* name, struct clock clk);
void  optset(argc, argv);


// Globals used in signal handler
int simulated_clock_id, proc_ctrl_tbl_id, scheduler_id;
struct clock* sysclock;                                 
struct process_ctrl_table* pct;
int cleaning_up = 0;
pid_t* childpids;
FILE* fp;

int main (int argc, char* argv[]) {
  optset(argc, argv);
    /*
     *  Setup program before entering main loop
     */
    set_timer(MAX_RUNTIME);         // Set timer that triggers SIGALRM
    add_signal_handlers();
    setlocale(LC_NUMERIC, "");      // For comma separated integers in printf
    srand(time(NULL) ^ getpid());

    int i, pid, q_idx, times_blocked, times_scheduled;
    i = pid = q_idx = times_blocked = times_scheduled = 0;
    bool all_queues_empty = 1;
    struct Statistics stats = get_new_stats();
    char buffer[255];
    const unsigned int TOTAL_RUNTIME = 3;       // Max seconds oss should run for
    bool pcb_in_use[PROC_CTRL_TBL_SZE];         // Bit vector used to determine if process ctrl block is in use
    for (i = 0; i < PROC_CTRL_TBL_SZE; i++) {
        pcb_in_use[i] = 0;
    }
    unsigned int num_procs_spawned = 0;         // Total number of children spawned
    unsigned int nanosecs = 0;                  // Holds total time it took to schedule a process
    unsigned int ns_before_next_proc = 0;       // Holds nanoseconds before next processes is scheduled
    struct clock time_to_fork =                 // Holds time to schedule new process
        { .seconds = 0, .nanoseconds = 0 };
    unsigned int elapsed_seconds = 0;           // Holds total real-time seconds the program has run      
    struct timeval tv_start, tv_stop;           // Used to calculated real elapsed time
    gettimeofday(&tv_start, NULL);

    char* execv_arr[EXECV_SIZE];                // Used to exec and pass data to user processes
    execv_arr[0] = "./user";
    execv_arr[EXECV_SIZE - 1] = NULL;
    
    // Setup shared memory
    simulated_clock_id = get_shared_memory();
    sysclock = (struct clock*) attach_to_shared_memory(simulated_clock_id, 0);
    sysclock->seconds = 0;
    sysclock->nanoseconds = 0;
    proc_ctrl_tbl_id = get_shared_memory();
    pct = (struct process_ctrl_table*) attach_to_shared_memory(proc_ctrl_tbl_id, 0);
    struct process_ctrl_block* pcb; 
    scheduler_id = get_message_queue();
    struct msgbuf scheduler;
    sprintf(scheduler.mtext, "You've been scheduled!");

    childpids = malloc(sizeof(pid_t) * TOTAL_PROC_LIMIT);
    for (i = 0; i < TOTAL_PROC_LIMIT; i++) {
        childpids[i] = 0;
    }

    if ((fp = fopen("./log.txt", "w")) == NULL) {
        perror("fopen");
        exit(1);
    }

    // Round robin queue and Multi-level feedback queues
    struct Queue roundRobin1, roundRobin2, level1, level2;
    // Blocked queue
    unsigned int blocked[PROC_CTRL_TBL_SZE];
    for (i = 0; i < PROC_CTRL_TBL_SZE; i++) {
        blocked[i] = 0;
    }

    struct Queue queue_arr[NUM_QUEUES] = {
        roundRobin1,
        roundRobin2,
        level1, 
        level2
    };

    for (i = 0; i < NUM_QUEUES; i++) {
        init_queue(&queue_arr[i]);
    }

    // Get a time to fork first process at
    ns_before_next_proc = rand() % MAX_NS_BEFORE_NEW_PROC; 
    time_to_fork = convertToClockTime(ns_before_next_proc); // Will be 0-2 seconds
    
    // Increment current time so it is time to fork a user process
    sysclock->seconds = time_to_fork.seconds;
    sysclock->nanoseconds = time_to_fork.nanoseconds;

    /*
     *  Main loop
     */
    while ( (num_procs_spawned < TOTAL_PROC_LIMIT) && (elapsed_seconds < TOTAL_RUNTIME) ) {
        
        // Check if it is time to fork a new user process
        if (compare_clocks(*sysclock, time_to_fork) >= 0) {
            // Fork 1 process if there is an empty process control block
            for (i = 1; i < PROC_CTRL_TBL_SZE + 1; i++) {
                if (pcb_in_use[i]) {
                    continue;
                }
                // PCB is empty
                // Create process control block
                struct process_ctrl_block proc_ctrl_blk = {
                    .pid = i,
                    .status = READY,
                    .is_realtime = process_is_realtime(),
                    .time_quantum = BASE_TIME_QUANTUM, 
                    .cpu_time_used.seconds = 0, .cpu_time_used.nanoseconds = 0,
                    .sys_time_used.seconds = 0, .sys_time_used.nanoseconds = 0,
                    .last_run = 0,
                    .time_blocked.seconds = 0, .time_blocked.nanoseconds = 0,
                    .time_unblocked.seconds = 0, .time_unblocked.nanoseconds = 0,
                    .time_scheduled.seconds = sysclock->seconds, .time_scheduled.nanoseconds = sysclock->nanoseconds,
                    .time_finished.seconds = 0, .time_finished.nanoseconds = 0
                };

                // Add proc_ctrl_blk to process control table
                pct->pcbs[i] = proc_ctrl_blk;

                // Mark PCB in use
                pcb_in_use[i] = 1;
            
                // Determine which queue to insert process into
                if (proc_ctrl_blk.is_realtime) {            
                    q_idx = 0; // To insert into round robin queue
                    proc_ctrl_blk.time_quantum = BASE_TIME_QUANTUM;
                }
                else {
                    q_idx = 1; // To insert into level 1 queue
                    proc_ctrl_blk.time_quantum = (int)(pow(BASE_TIME_QUANTUM, (q_idx + 1)) + 0.5);
                }
                
                // Fork and place in queue
                fork_child(execv_arr, num_procs_spawned, proc_ctrl_blk.pid);
                enqueue(&queue_arr[q_idx], proc_ctrl_blk.pid);
                sprintf(buffer, "OSS: Generating process with PID %d at putting it in queue %d at time %ld:%ld\n",
                    proc_ctrl_blk.pid, q_idx, sysclock->seconds, sysclock->nanoseconds);
                print_and_write(buffer);

                num_procs_spawned++;

                // Get a time to fork next proc
                ns_before_next_proc = rand() % MAX_NS_BEFORE_NEW_PROC; 
                time_to_fork = convertToClockTime(ns_before_next_proc); // Will be 0-2 seconds
                time_to_fork.seconds += sysclock->seconds;              // Increment to current time
                increment_clock(&time_to_fork, sysclock->nanoseconds);
                
                break;
            }
        }

        // Unblock any processes that can be unblocked
        for (i = 0; i < PROC_CTRL_TBL_SZE; i++) {
            if (blocked[i] == 0) {
                continue;
            }
            // Process is blocked
            pcb = &pct->pcbs[blocked[i]];
            if (compare_clocks(*sysclock, pcb->time_unblocked) >= 0) {
                // Unblock process
                sprintf(buffer, "OSS: Removing process %d from the blocked queue\n", 
                    blocked[i]);
                print_and_write(buffer);
                blocked[i] = 0;
                pcb->status = READY;
                if (pcb->is_realtime) {            
                    q_idx = 0; // To insert into round robin queue
                    pcb->time_quantum = BASE_TIME_QUANTUM;
                }
                else {
                    q_idx = 1; // To insert into level 1 queue
                    pcb->time_quantum = (int) BASE_TIME_QUANTUM * pow(2, q_idx);
                }
                enqueue(&queue_arr[q_idx], pcb->pid);
            }
            
        }

        // Dequeue process from queue with highest priority
        for (i = 0; i < NUM_QUEUES; i++) {
            if (empty(&queue_arr[i])) {
                all_queues_empty = 1;
                continue;
            }
            // Queue is not empty so...
            // Dequeue and store ptr to process control block
            all_queues_empty = 0;
            pid = dequeue(&queue_arr[i]);
            pcb = &pct->pcbs[pid];
            q_idx = i;
            break;
        }

        // Calculate amount of time it took to schedule
        nanosecs = get_amt_time_to_schedule();
        increment_clock(sysclock, nanosecs);

        if (all_queues_empty) {
            increment_clock(&stats.idle_time, nanosecs);
            continue;
        }

        // Schedule by sending message
        send_msg(scheduler_id, &scheduler, pcb->pid);
        sprintf(buffer, "OSS: Dispatching process with PID %d from queue %d at time %ld:%ld\n", 
            pcb->pid, (q_idx), sysclock->seconds, sysclock->nanoseconds);
        print_and_write(buffer);

        sprintf(buffer, "OSS: Total time this dispatch was %d nanoseconds\n", 
            nanosecs);
        print_and_write(buffer);

        // Keep track of time it took to scheduled for statistics
        stats.turnaround_time += nanosecs;
        times_scheduled++;

        // Receive
        receive_msg(scheduler_id, &scheduler, (pcb->pid + PROC_CTRL_TBL_SZE)); // Add PROC_CTRL_TBL_SZE to message type
        sprintf(buffer, "OSS: Receiving that process with PID %d ran for %ld nanoseconds\n", 
            pcb->pid, pcb->last_run);
        print_and_write(buffer);

        increment_clock(sysclock, pcb->last_run);

        if (pcb->status == TERMINATED) {
            // Store off statistical information
            // And do not put back in queue
            pcb_in_use[pcb->pid] = 0;
            sprintf(buffer, "OSS: Process %d terminated\n", 
                pcb->pid);
            print_and_write(buffer);
            stats.total_cpu_time = add_clocks(stats.total_cpu_time, pcb->cpu_time_used);
            stats.total_sys_time = add_clocks(stats.total_sys_time, pcb->sys_time_used);
            struct clock idle = subtract_clocks(stats.total_sys_time, stats.total_cpu_time);
            stats.wait_time = add_clocks(stats.wait_time, idle);
        }
        else if (pcb->status == BLOCKED) {
            // Place in blocked queue
            sprintf(buffer, "OSS: Process %d is blocked and did not use its entire timeslice", 
                pcb->pid);
            print_and_write(buffer);
            for (i = 0; i < PROC_CTRL_TBL_SZE; i++) {
                if (blocked[i] > 0) {
                    continue;
                }
                // Blocked queue index is empty
                blocked[i] = pcb->pid;
                stats.sleep_time = add_clocks(stats.sleep_time, pcb->time_blocked);
                times_blocked++;
                break;
            }
        }
        else {
            sprintf(buffer, "OSS: Process %d used its entire timeslice and is not blocked\n", 
                pcb->pid);
            print_and_write(buffer);
            if ( (q_idx == 0) || (q_idx == (NUM_QUEUES - 1)) ) {
                // Process is a realtime process
                // OR process was dequeued from level 3 queue
                // So decement queue idx to insert the process back into the same queue
                q_idx--;
            }

            // Insert process into queue
            enqueue(&queue_arr[q_idx + 1], pcb->pid);
            sprintf(buffer, "OSS: Putting process with PID %d into queue %d", 
                pcb->pid, q_idx + 1);
            print_and_write(buffer);
            
            // Update processes' time quantum
            pcb->time_quantum = (int) BASE_TIME_QUANTUM * pow(2, q_idx + 1);
        }
        
        sprintf(buffer, "\n");
        print_and_write(buffer);
        
        waitpid(-1, NULL, WNOHANG); // Cleanup any zombies as we go
        
        // Calculate total elapsed realtime seconds
        gettimeofday(&tv_stop, NULL);
        elapsed_seconds = tv_stop.tv_sec - tv_start.tv_sec;
    }

    // Print information before exiting
   /* sprintf(buffer, "OSS: Exiting because 100 processes have been spawned or because %d seconds have been passed\n", TOTAL_RUNTIME);
    print_and_write(buffer);

    char formatstr[50] = "%-24s: %'10ld:%'12ld\n";

    sprintf(buffer, "\n%s\n", "==================== Statistics ====================");
    print_and_write(buffer);

    sprintf(buffer, "%-24s: %10d\n", "  Processes generated", num_procs_spawned);
    print_and_write(buffer);

    sprintf(buffer, formatstr, "  Simulated clock time",
            sysclock->seconds, sysclock->nanoseconds);
    print_and_write(buffer);

    sprintf(buffer, "\n");
    print_and_write(buffer);
    
    stats.wait_time = calculate_avg_time(stats.wait_time, times_scheduled);
    sprintf(buffer, formatstr, "  Avg wait time",
        stats.wait_time.seconds, stats.wait_time.nanoseconds);
    print_and_write(buffer);

    stats.sleep_time = calculate_avg_time(stats.sleep_time, times_blocked);
    sprintf(buffer, formatstr, "  Avg sleep time",
        stats.sleep_time.seconds, stats.sleep_time.nanoseconds);
    print_and_write(buffer);
    
    sprintf(buffer, formatstr, "  Avg turn-around time",
        0, stats.turnaround_time / times_scheduled);
    print_and_write(buffer);
    
    stats.idle_time = subtract_clocks(stats.total_sys_time, stats.total_cpu_time);
    sprintf(buffer, formatstr, "  Total idle time",
        stats.idle_time.seconds, stats.idle_time.nanoseconds);
    print_and_write(buffer);
    
    sprintf(buffer, "%s\n", "====================================================");*/
    print_and_write(buffer);

    cleanup_and_exit();

    return 0;
}

bool process_is_realtime() {
    return event_occured(PCT_REALTIME);
}

unsigned int get_amt_time_to_schedule() {
    return (rand() % 10000) + 100; // In nanoseconds
}

struct clock convertToClockTime(int nanoseconds) {
    struct clock clk = { .seconds = 0, .nanoseconds = 0 };
    if (nanoseconds >= ONE_BILLION) {
        nanoseconds -= ONE_BILLION;
        clk.seconds = 1;
    }
    clk.nanoseconds = nanoseconds;
    return clk;
}

void fork_child(char** execv_arr, int child_idx, int pid) {
    if ((childpids[child_idx] = fork()) == 0) {
        // Child so...
        char sysclock_id[10];
        char pct_id[10];
        char pid_str[5];
        char schedulerid[10];
        sprintf(sysclock_id, "%d", simulated_clock_id);
        sprintf(pct_id, "%d", proc_ctrl_tbl_id);
        sprintf(pid_str, "%d", pid);
        sprintf(schedulerid, "%d", scheduler_id);
        execv_arr[SYSCLOCK_ID_IDX] = sysclock_id;
        execv_arr[PCT_ID_IDX] = pct_id;
        execv_arr[PID_IDX] = pid_str;
        execv_arr[SCHEDULER_IDX] = schedulerid;

        execvp(execv_arr[0], execv_arr);

        perror("Child failed to execvp the command!");
        exit(1);
    }

    if (childpids[child_idx] == -1) {
        perror("Child failed to fork!\n");
        exit(1);
    }
}

void wait_for_all_children() {
    //int status;
    pid_t pid;
    printf("OSS: Waiting for all children to exit\n");
    fprintf(fp, "OSS: Waiting for all children to exit\n");
    
    while ((pid = wait(NULL))) {
        if (pid < 0) {
            if (errno == ECHILD) {
                perror("wait");
                break;
            }
        }
    }
}

void terminate_children() {
    printf("OSS: Sending SIGTERM to all children\n");
    fprintf(fp, "OSS: Sending SIGTERM to all children\n");
    int i;
    for (i = 0; i < TOTAL_PROC_LIMIT; i++) {
        if (childpids[i] == 0) {
            continue;
        }
        if (kill(childpids[i], SIGTERM) < 0) {
            if (errno != ESRCH) {
                // Child process exists and kill failed
                perror("kill");
            }
        }
    }
    free(childpids);
}

void add_signal_handlers() {
    struct sigaction act;
    act.sa_handler = handle_sigint; // Signal handler
    sigemptyset(&act.sa_mask);      // No other signals should be blocked
    act.sa_flags = 0;               // 0 so do not modify behavior
    if (sigaction(SIGINT, &act, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    act.sa_handler = handle_sigalrm; // Signal handler
    sigemptyset(&act.sa_mask);       // No other signals should be blocked
    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
}

void handle_sigint(int sig) {
    printf("\nOSS: Caught SIGINT signal %d\n", sig);
    fprintf(fp, "\nOSS: Caught SIGINT signal %d\n", sig);
    if (cleaning_up == 0) {
        cleaning_up = 1;
        cleanup_and_exit();
    }
}

void handle_sigalrm(int sig) {
    printf("\nOSS: Caught SIGALRM signal %d\n", sig);
    fprintf(fp, "\nOSS: Caught SIGALRM signal %d\n", sig);
    if (cleaning_up == 0) {
        cleaning_up = 1;
        cleanup_and_exit();
    }

}

void cleanup_and_exit() {
    terminate_children();
    printf("OSS: Removing message queues and shared memory\n");
    fprintf(fp, "OSS: Removing message queues and shared memory\n");
    remove_message_queue(scheduler_id);
    wait_for_all_children();
    cleanup_shared_memory(simulated_clock_id, sysclock);
    cleanup_shared_memory(proc_ctrl_tbl_id, pct);
    fclose(fp);
    exit(0);
}

void print_and_write(char* str) {
    fputs(str, stdout);
    fputs(str, fp);
}

struct Statistics get_new_stats(){
    struct Statistics stats =  {
        .turnaround_time = 0,   
        .wait_time = {.seconds = 0, .nanoseconds = 0},
        .sleep_time = {.seconds = 0, .nanoseconds = 0},       
        .idle_time = {.seconds = 0, .nanoseconds = 0},    
        .total_cpu_time = {.seconds = 0, .nanoseconds = 0},
        .total_sys_time = {.seconds = 0, .nanoseconds = 0},
    };
    return stats;
}

int get_shared_memory() {
    int shmemid;

    shmemid = shmget(IPC_PRIVATE, getpagesize(), IPC_CREAT | S_IRUSR | S_IWUSR);

    if (shmemid == -1) {
        perror("shmget");
        exit(1);
    }
    
    return shmemid;
}

void* attach_to_shared_memory(int shmemid, unsigned int readonly) {
    void* p;
    int shmflag;

    if (readonly) {
        shmflag = SHM_RDONLY;
    }
    else {
        shmflag = 0;
    }

    p = (void*)shmat(shmemid, 0, shmflag);

    if (!p) {
        perror("shmat");
        exit(1);
    }

    return p;

}

void cleanup_shared_memory(int shmemid, void* p) {
    detach_from_shared_memory(p);
    deallocate_shared_memory(shmemid);
}

void detach_from_shared_memory(void* p) {
    if (shmdt(p) == -1) {
        perror("shmdt");
        exit(1);
    }
}

void deallocate_shared_memory(int shmemid) {
    if (shmctl(shmemid, IPC_RMID, 0) == 1) {
        perror("shmctl");
        exit(1);
    }
}

void init_queue(struct Queue *q) {
    q->first = 0;
    q->last = QUEUESIZE-1;
    q->count = 0;
}

void enqueue(struct Queue *q, int x) {
    if (q->count >= QUEUESIZE)
    printf("Warning: queue overflow enqueue x=%d\n",x);
    else {
            q->last = (q->last+1) % QUEUESIZE;
            q->q[ q->last ] = x;    
            q->count = q->count + 1;
    }
}

int dequeue(struct Queue *q) {
    int x;

    if (q->count <= 0) printf("Warning: empty queue dequeue.\n");
    else {
        x = q->q[ q->first ];
        q->first = (q->first+1) % QUEUESIZE;
        q->count = q->count - 1;
    }

    return(x);
}

bool empty(struct Queue *q) {
    if (q->count <= 0) return (1);
    else return (0);
}

void print_queue(struct Queue *q) {
    int i;

    i=q->first; 
    
    while (i != q->last) {
        printf("%c ",q->q[i]);
        i = (i+1) % QUEUESIZE;
    }

    printf("%2d ",q->q[i]);
    printf("\n");
}

int get_message_queue() {
    int msgqid;

    msgqid = msgget(IPC_PRIVATE, IPC_CREAT | 0666);

    if (msgqid == -1) {
        perror("msgget");
        exit(1);
    }

    return msgqid;
}

void receive_msg(int msgqid, struct msgbuf* mbuf, int mtype) {
    if (msgrcv(msgqid, mbuf, sizeof(mbuf->mtext), mtype, 0) == -1) {
        perror("msgrcv");
        exit(1);
    }
}

void send_msg(int msgqid, struct msgbuf* mbuf, int mtype) {
    mbuf->mtype = mtype;
    if (msgsnd(msgqid, mbuf, sizeof(mbuf->mtext), IPC_NOWAIT) < 0) {
        perror("msgsnd");
        exit(1);
    }
}

void remove_message_queue(int msgqid) {
    if (msgctl(msgqid, IPC_RMID, NULL) == -1) {
        perror("msgctl");
        exit(1);
    }
}

char** split_string(char* str, char* delimeter) {
    char* substr;
    char** strings = malloc(10 * sizeof(char));

    substr = strtok(str, delimeter);

    int i = 0;
    while (substr != NULL)
    {
        strings[i] = malloc(20 * sizeof(char));
        strings[i] = substr;
        substr = strtok(NULL, delimeter);
        i++;
    }

    return strings;

}

char* get_timestamp() {
    char* timestamp = malloc(sizeof(char)*10);
    time_t rawtime;
    struct tm* timeinfo;

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    sprintf(timestamp, "%d:%d:%d", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    
    return timestamp;
}

void parse_cmd_line_args(int argc, char* argv[]) {
    int option;
    while ((option = getopt (argc, argv, "h")) != -1)
    switch (option) {
        case 'h':
            print_usage();
            break;
        default:
            print_usage();
    }
}

void print_usage() {
    fprintf(stderr, "Usage: oss\n");
    exit(0);
}


void set_timer(int duration) {
    struct itimerval value;
    value.it_interval.tv_sec = duration;
    value.it_interval.tv_usec = 0;
    value.it_value = value.it_interval;
    if (setitimer(ITIMER_REAL, &value, NULL) == -1) {
        perror("setitimer");
        exit(1);
    }
}

bool event_occured(unsigned int pct_chance) {
    unsigned int percent = (rand() % 100) + 1;
    if (percent <= pct_chance) {
        return 1;
    }
    else {
        return 0;
    }
}

void increment_clock(struct clock* clock, int increment) {
    clock->nanoseconds += increment;
    if (clock->nanoseconds >= ONE_BILLION) {
        clock->seconds += 1;
        clock->nanoseconds -= ONE_BILLION;
    }
}

struct clock add_clocks(struct clock c1, struct clock c2) {
    struct clock out = {
        .seconds = 0,
        .nanoseconds = 0
    };
    out.seconds = c1.seconds + c2.seconds;
    increment_clock(&out, c1.nanoseconds + c2.nanoseconds);
    return out;
}

int compare_clocks(struct clock c1, struct clock c2) {
    if (c1.seconds > c2.seconds) {
        return 1;
    }
    if ((c1.seconds == c2.seconds) && (c1.nanoseconds > c2.nanoseconds)) {
        return 1;
    }
    if ((c1.seconds == c2.seconds) && (c1.nanoseconds == c2.nanoseconds)) {
        return 0;
    }
    return -1;
}

long double clock_to_seconds(struct clock c) {
    long double seconds = c.seconds;
    long double nanoseconds = (long double)c.nanoseconds / ONE_BILLION; 
    seconds += nanoseconds;
    return seconds;
}

struct clock seconds_to_clock(long double seconds) {
    struct clock clk = { .seconds = (int)seconds };
    seconds -= clk.seconds;
    clk.nanoseconds = seconds * ONE_BILLION;
    return clk;
}

struct clock calculate_avg_time(struct clock clk, int divisor) {
    long double seconds = clock_to_seconds(clk);
    long double avg_seconds = seconds / divisor;
    return seconds_to_clock(avg_seconds);
}

struct clock subtract_clocks(struct clock c1, struct clock c2) {
    long double seconds1 = clock_to_seconds(c1);
    long double seconds2 = clock_to_seconds(c2);
    long double result = seconds1 - seconds2;
    return seconds_to_clock(result);
}

void print_clock(char* name, struct clock clk) {
    printf("%-15s: %ld:%ld\n", name, clk.seconds, clk.nanoseconds);
}

void optset(int argc, char *argv[])
{
	int choice;
	while((choice = getopt(argc, argv, "hn:")) != -1)
	{
		switch(choice)
		{
			case 'h':
				helpme();
				exit(EXIT_SUCCESS);
			case 'n':
				pcap = atoi(optarg);
				if(pcap > 18)
				{
					pcap = 18;
				}
				if(pcap <= 0)
				{
					pcap = 18;
				}
				break;
			case '?':
				fprintf(stderr, "\noss: error: invalid argument\n");
				exit(EXIT_FAILURE);				
		}
	}
}

void helpme()
{
	printf("\n|HELP|MENU|\n\n");
  printf("\t-h : display help menu\n");
	printf("\t-n : specify number of processes\n");
}