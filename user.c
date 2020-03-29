#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>
#include <sys/time.h>
#include <locale.h>
#include <signal.h>
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


bool will_terminate();
bool use_entire_timeslice();
unsigned int get_random_pct();
struct clock get_event_wait_time();
void add_signal_handlers();
void handle_sigterm(int sig);

const unsigned int CHANCE_TERMINATE = 4;
const unsigned int CHANCE_ENTIRE_TIMESLICE = 70; // == (1 - CHANCE_ENTIRE_TIMESLICE) == CHANCE_BLOCKED

int main (int argc, char *argv[]) {
    add_signal_handlers();
    srand(time(NULL) ^ getpid());
    setlocale(LC_NUMERIC, "");      // For comma separated integers in printf
    unsigned int nanosecs;

    // Get shared memory IDs
    int sysclock_id = atoi(argv[SYSCLOCK_ID_IDX]);
    int proc_ctrl_tbl_id = atoi(argv[PCT_ID_IDX]);
    int pid = atoi(argv[PID_IDX]);
    int scheduler_id = atoi(argv[SCHEDULER_IDX]);
    
    // Attach to shared memory
    struct clock* sysclock = attach_to_shared_memory(sysclock_id, 1);
    struct process_ctrl_table* pct = attach_to_shared_memory(proc_ctrl_tbl_id, 0);

    struct process_ctrl_block* pcb = &pct->pcbs[pid];
    
    struct msgbuf scheduler;
    while(1) {
        // Blocking receive - wait until scheduled
        receive_msg(scheduler_id, &scheduler, pid);
        // Received message from OSS telling me to run
        pcb->status = RUNNING;
        
        if (will_terminate()) {
            // Run for some random pct of time quantum
            nanosecs = pcb->time_quantum / get_random_pct();
            pcb->last_run = nanosecs;
            increment_clock(&pcb->cpu_time_used, nanosecs);

            break;
        }

        if (use_entire_timeslice()) {
            // Run for entire time slice and do not get blocked
            increment_clock(&pcb->cpu_time_used, pcb->time_quantum);
            pcb->last_run = pcb->time_quantum;

            pcb->status = READY;
        }
        else {
            // Blocked on an event
            pcb->status = BLOCKED;

            // Run for some random pct of time quantum
            nanosecs = pcb->time_quantum / get_random_pct(); 
            pcb->last_run = nanosecs;
            increment_clock(&pcb->cpu_time_used, nanosecs);

            pcb->time_blocked = get_event_wait_time();

            // Set the time when this process is unblocked
            pcb->time_unblocked.seconds = pcb->time_blocked.seconds + sysclock->seconds;
            pcb->time_unblocked.nanoseconds = pcb->time_blocked.nanoseconds + sysclock->nanoseconds;
        }
        
        // Add PROC_CTRL_TBL_SZE to message type to let OSS know we are done
        send_msg(scheduler_id, &scheduler, (pid + PROC_CTRL_TBL_SZE)); 
    }

    pcb->status = TERMINATED;

    pcb->time_finished.seconds = sysclock->seconds;
    pcb->time_finished.nanoseconds = sysclock->nanoseconds;

    pcb->sys_time_used = subtract_clocks(pcb->time_finished, pcb->time_scheduled);

    // Add PROC_CTRL_TBL_SZE to message type to let OSS know we are done
    send_msg(scheduler_id, &scheduler, (pid + PROC_CTRL_TBL_SZE)); 

    return 0;  
}

bool will_terminate() {
    return event_occured(CHANCE_TERMINATE);
}

bool use_entire_timeslice() {
    return event_occured(CHANCE_ENTIRE_TIMESLICE);
}

unsigned int get_random_pct() {
    return (rand() % 99) + 1;
}

struct clock get_event_wait_time() {
    struct clock event_wait_time;
    event_wait_time.seconds = rand() % 6;
    event_wait_time.nanoseconds = rand() % 1001;
    return event_wait_time;
}

void add_signal_handlers() {
    struct sigaction act;
    act.sa_handler = handle_sigterm; // Signal handler
    sigemptyset(&act.sa_mask);      // No other signals should be blocked
    act.sa_flags = 0;               // 0 so do not modify behavior
    if (sigaction(SIGTERM, &act, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
}

void handle_sigterm(int sig) {
    //printf("USER %d: Caught SIGTERM %d\n", getpid(), sig);
    _exit(0);
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
    printf("%-15s: %'ld:%'ld\n", name, clk.seconds, clk.nanoseconds);
}

