
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "sharememory.h"
#include "queue.h"
const int probthatprocterminates = 15;
const int probthatprocenervates  = 85;
shmem* smseg;
int sipcid;
int tousr;
int tooss;
int entire = 1;
int pcaps = 18;

typedef struct 
{
	long msgtype;
	char message[100];
	char quantum[100];
} oss_msg_t;

void sminit();
void msginit();
int uspsdispatch(int, oss_msg_t *, int);
void timeinc(simclock *, int);
int findaseat();
int bitvector(int);
void clockinc(simclock *, int, int);

/*
 * static functions
 */
static int findapid(int pid);

int main(int argc, char *argv[])
{
    oss_msg_t msg;

    sminit();

    msginit();

	simclock blockout; 

    int pid = getpid();

    /* time(NULL) ensures a different value every second. (getpid() << 16))
       increases the odds that a different value computes for each process
       because process IDs typically are not re-used that often          */
    srand(time(NULL) ^ (getpid() << 16));

	while(1)
	{
		msgrcv(tousr, &msg, sizeof(oss_msg_t), pid, 0);

		/* if the process decides that it will go terminate */
		if(((rand() % 100) <= probthatprocterminates))
		{
			msg.msgtype = pid;
			strcpy(msg.message, "EXPIRED");

			if((uspsdispatch(tooss, &msg, sizeof(msg))) == -1)
			{
					perror("\nusr: error: failed to dispatch");
					exit(EXIT_FAILURE);
			}

			char tspercent[20];
			int tsamount = (rand() % 100);
			sprintf(tspercent, "%i", tsamount);
			msg.msgtype = pid;

			strcpy(msg.message, tspercent);
			if((uspsdispatch(tooss, &msg, sizeof(oss_msg_t))) == -1)
			{
					perror("\nusr: error: failed to dispatch");
					exit(EXIT_FAILURE);
			}

			exit(42);
		}

		/* if process decides its not going to terminate */
		else if(((rand() % 100) > probthatprocterminates))
		{
			/* and to be or not to be */	
			int decision = (rand() % 2);

			if(decision == entire)
			{
				msg.msgtype = pid;
				strcpy(msg.message, "EXHAUSTED");
				
				if((uspsdispatch(tooss, &msg, sizeof(oss_msg_t))) == -1)
				{
					perror("\nusr: error: failed to dispatch");
					exit(EXIT_FAILURE);
				}

			} else {
				blockout.secs = smseg->simtime.secs;
				blockout.nans = smseg->simtime.nans;

				int r = (rand() % 3) + 1;
				int s = (rand() % 1001) * 1000000;
				int p = (rand() % 99) + 1;

				clockinc(&(blockout), r, s);
				clockinc(&(smseg->pctable[findapid(pid)].smblktime), r, s);

				char pconv[20];
				sprintf(pconv, "%i", p);

				msg.msgtype = pid;
				strcpy(msg.message, "SLICED");
				
				if((uspsdispatch(tooss, &msg, sizeof(oss_msg_t))) == -1)
				{
					perror("\nusr: error: failed to dispatch");
					exit(EXIT_FAILURE);
				}

				msg.msgtype = pid;
				strcpy(msg.message, pconv);
				
				if((uspsdispatch(tooss, &msg, sizeof(oss_msg_t))) == -1)
				{
					perror("\nusr: error: failed to dispatch");
					exit(EXIT_FAILURE);
				}

				/* spinlock */
				while(1)
				{
					if((smseg->simtime.secs >= blockout.secs) && (smseg->simtime.nans >= blockout.nans))
					{
						break;
					}
				}

				msg.msgtype = pid;
				strcpy(msg.message, "FINALIZED");
				msgsnd(tooss, &msg, sizeof(oss_msg_t), IPC_NOWAIT);
			}

		}
	}
    return 0;
}

static int findapid(int pid)
{
	int searcher;
	for(searcher = 0; searcher < pcaps; searcher++)
	{
		if(smseg->pctable[searcher].pids == pid)
		{
			return searcher; 
		}
	}
	return -1;
}

void clockinc(simclock* khronos, int sec, int nan)
{
	khronos->secs = khronos->secs + sec;
	timeinc(khronos, nan);
}

void timeinc(simclock* khronos, int duration)
{
	int temp = khronos->nans + duration;
	while(temp >= 1000000000)
	{
		temp -= 1000000000;
		(khronos->secs)++;
	}
	khronos->nans = temp;
}

int uspsdispatch(int id, oss_msg_t * buf, int size)
{
	int result;
	if((result = msgsnd(id, buf, size, 0)) == -1)
	{
		return(-1);
	}
	return(result);
}

void msginit()
{
	key_t msgkey = ftok("msg1", 825);
	if(msgkey == -1)
	{
		perror("\noss: error: ftok failed");
		exit(EXIT_FAILURE);
	}

	tousr = msgget(msgkey, 0600 | IPC_CREAT);
	if(tousr == -1)
	{
		perror("\noss: error: failed to create");
		exit(EXIT_FAILURE);
	}

	msgkey = ftok("msg2", 725);
	if(msgkey == -1)
	{
		perror("\noss: error: ftok failed");
		exit(EXIT_FAILURE);
	}

	tooss = msgget(msgkey, 0600 | IPC_CREAT);
	if(tooss == -1)
	{
		perror("\noss: error: failed to create");
		exit(EXIT_FAILURE);
	}
}
void sminit()
{
	key_t smkey = ftok("shmfile", 'a');
	if(smkey == -1)
	{
		perror("\noss: error: ftok failed");
		exit(EXIT_FAILURE);
	}

	sipcid = shmget(smkey, sizeof(shmem), 0600 | IPC_CREAT);
	if(sipcid == -1)
	{
		perror("\noss: error: failed to create shared memory");
		exit(EXIT_FAILURE);
	}

	smseg = (shmem*)shmat(sipcid,(void*)0, 0);

	if(smseg == (void*)-1)
	{
		perror("\noss: error: failed to attach shared memory");
		exit(EXIT_FAILURE);
	}
}
