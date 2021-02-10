/* Kyle McKenzie
 * Last Edited: 08/04/2020
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <sys/times.h>

struct PCB
{
    int PR;
    int numCPUBurst, numIOBurst;
    int *cpuBurst, *ioBurst;
    int cpuIndex, ioIndex;
    struct timespec timeEnterReadyQ;
    double waitTime;
    struct PCB *prev;
    struct PCB *next;
};

//Globals
struct PCB *readyQHead;
struct PCB *readyQTail;
struct PCB *ioQHead;
struct PCB *ioQTail;
char *algorithm = NULL;
int quantumTime = 0;
int totalJobs = 0;
int error = 0;
double totalExecTime, elapsed;
double totalWaitTime = 0, totalTurnTime = 0;
struct timespec ts, tsBegin, tsEnd;
char *fileName = NULL;
FILE *fp;
char delimeters[] = " \t\n";
sem_t semCPU, semIO;
pthread_mutex_t mutex;
int fileReadDone=0, cpuSchDone = 0, ioSysDone = 0,cpuBusy=0, ioBusy=0;

//Thread functions
void *FileReadTh();
void *IOSysTh();
void *CPUSchedTh();

//Helper functions
char *ReadLineFile(FILE *infile);
void execute(struct PCB *temp, int execTime);
void insertReadyQ(struct PCB *pcb);
void insertIOQ(struct PCB *pcb);
struct PCB *getNextPCBFIFO();
struct PCB *getNextPCBSJF();
struct PCB *getNextPCBRR();
struct PCB *getNextPCBFromIOQ();
struct PCB *getNextPCBPriority();

int main(int argc, char *argv[])
{   
    //Check command line args
    if(strcmp(argv[1], "-alg") == 0)
    {
        algorithm = argv[2];
    }
    
    if(strcmp(argv[3], "-quantum") == 0)
    {
        quantumTime = atoi(argv[4]);
    }
    
    if(quantumTime == 0)
    {
        if(strcmp(argv[3], "-input") == 0)
        {
            fileName = argv[4];
        }
    }
    else
    {
        if(strcmp(argv[5], "-input") == 0)
        {
            fileName = argv[6];
        }
    }
    
    //prgram variables
    long tid1, tid2, tid3;
    readyQHead = NULL;
    readyQTail = NULL;
    ioQHead = NULL;
    ioQTail = NULL;
    
    //initialize semaphores for thread communication
    sem_init(&semIO, 0, 0);
    sem_init(&semCPU, 0, 0);
    
    //get current time
    clock_gettime(CLOCK_MONOTONIC, &tsBegin);
    
    if(error = pthread_create(&tid1, NULL, FileReadTh, NULL))
    {
        fprintf(stderr, "Failed to create  file read thread: %s\n", strerror(error));
        exit(1);
    }

    if(error = pthread_create(&tid2, NULL, CPUSchedTh, NULL))
    {
        fprintf(stderr, "Failed to create  CPUSched thread: %s\n", strerror(error));
        exit(1);
    }
    if(error = pthread_create(&tid3, NULL, IOSysTh, NULL))
    {
        fprintf(stderr, "Failed to create  IOSys thread: %s\n", strerror(error));
        exit(1);
    }
    
    //wait for threads to finish
    pthread_join(tid1, NULL);
    pthread_join(tid2, NULL);
    pthread_join(tid3, NULL);
    
    //Result calculations
    clock_gettime(CLOCK_MONOTONIC, &tsEnd);
    double elapsed = tsEnd.tv_sec - tsBegin.tv_sec;
    elapsed += (tsEnd.tv_nsec - tsBegin.tv_nsec)/1000000000.0;
    printf("Input File Name: %s\n", fileName);
    
    if(strcmp(algorithm, "RR") == 0)
    {
        printf("CPU Scheduling Alg: %s quantum: %d\n", algorithm, quantumTime);
    }
    else
    {
        printf("CPU Scheduling Alg: %s\n", algorithm);
    }
    printf("CPU utilization: %.3lf\n", ((double)totalExecTime/(elapsed * 1000)) * 100);
    printf("Throughput: %.3lf jobs per ms\n", ((double)totalJobs/(elapsed * 1000)));
    printf("Avg. Turnaround time: %.1lf ms\n", (((double)totalTurnTime/(double)totalJobs) * 1000));
    printf("Avg. Waiting time in R queue: %.1lf ms\n", (((double)totalWaitTime/(double)totalJobs) * 1000));
    
    return 0;
}

void *FileReadTh()
{
    //open file
    if((fp = fopen(fileName, "r")) == NULL)
    {
        fprintf(stderr, "Error opening %s: %s\n", fileName, strerror(errno));
        exit(1);
    }
    
    struct PCB *newPCB;
    char *buffer;
    
    while((buffer = ReadLineFile(fp))) //read each line until the end
    {   
        char *token;
        token = strtok(buffer, delimeters);
        int tokenCnt = 1;
        
        //check the command i.e. proc, sleep, or stop
        if(strcmp(token, "proc") == 0)
        {   
            //allocate new PCB
            char *tempToken;
            if((newPCB = (struct PCB *)malloc(sizeof(struct PCB))) == NULL)
            {
                fprintf(stderr, "Error allocating new process: %s\n", strerror(errno));
                exit(1);
            }
            
            //Set PCB variables
            newPCB->PR = atoi(strtok(NULL, delimeters));
            tokenCnt++;
            newPCB->numCPUBurst = (atoi(strtok(NULL, delimeters)) / 2) + 1;
            tokenCnt++;
            newPCB->numIOBurst = newPCB->numCPUBurst - 1;
            
            int *cpuBurstArray;
            int *ioBurstArray;
            
            //Allocate space for cpu and io burst arrays
            if((cpuBurstArray = (int *)malloc(sizeof(int) * newPCB->numCPUBurst)) == NULL)
            {
                fprintf(stderr, "Error allocating cpuBurst: %s\n", strerror(errno));
                exit(1);
            }
            
            if((ioBurstArray = (int *)malloc(sizeof(int) * newPCB->numIOBurst)) == NULL)
            {
                fprintf(stderr, "Error allocating ioBurst: %s\n", strerror(errno));
                exit(1);
            }
            
            memset(cpuBurstArray, 0, (sizeof(int) * newPCB->numCPUBurst));
            memset(ioBurstArray, 0, (sizeof(int) * newPCB->numIOBurst));
            int index = 0;
            
            //Assign values to arrays. Even indexs are IO burst values. Odd are CPU burst values
            while((tempToken = strtok(NULL, delimeters)))
            {
                if((tokenCnt % 2) != 0)
                {
                    cpuBurstArray[index] = atoi(tempToken);
                    tokenCnt++;
                }
                else
                {
                    ioBurstArray[index] = atoi(tempToken);
                    index++;
                    tokenCnt++;
                }   
            }
            
            //Initialize PCB variables
            newPCB->cpuBurst = cpuBurstArray;
            newPCB->ioBurst = ioBurstArray;
            newPCB->cpuIndex = 0;
            newPCB->ioIndex = 0;
            newPCB->next = NULL;
            newPCB->prev = NULL;
            newPCB->waitTime = 0.0;
            clock_gettime(CLOCK_MONOTONIC, &newPCB->timeEnterReadyQ);
            
            //critical section. lock then insert to the ready queue.
            pthread_mutex_lock(&mutex);
            insertReadyQ(newPCB);
            pthread_mutex_unlock(&mutex);
            
            //notify cpu thread
            sem_post(&semCPU);
            
        }
        
        //sleep if sleep command found
        if(strcmp(token, "sleep") == 0)
        {
            long sleepTime = atoi(strtok(NULL, delimeters));
            usleep(sleepTime * 1000);
        }
        
        //terminate if stop command found
        if(strcmp(token, "stop") == 0)
        {
            break;
        }
    }
    
    //critical section lock then adjust value
    pthread_mutex_lock(&mutex);
    fileReadDone = 1;
    pthread_mutex_unlock(&mutex);
}

void *IOSysTh()
{
    struct PCB *temp;
    while(1)
    {
        //break out of loop if there are no more PCB's and the file thread is done.
        if(readyQHead == NULL && !cpuBusy && ioQHead == NULL && !ioBusy && fileReadDone)
        {
            break;
        }
        
        //get the current time
        if(clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
        {
            perror("ERROR: could not get time in IOSys Thread\n");
            exit(1);
        }
        
        //Wait for a second to avoid a deadlock here. Save current time
        ts.tv_sec = 1;
        if(sem_timedwait(&semIO, &ts) == -1 && errno == ETIMEDOUT)
        {
            continue;
        }
        
        //Critical section. Lock then change ioBusy value and access
        //the IO queue.
        pthread_mutex_lock(&mutex);
        ioBusy = 1;
        temp = getNextPCBFromIOQ();
        pthread_mutex_unlock(&mutex);
        
        //If there are no more PCB's IO thead is done.
        if(temp == NULL)
        {
            sem_post(&semCPU);
            break;
        }
        
        //Allow PCB to "use" IO device. Simulated by sleeping
        usleep(temp->ioBurst[temp->ioIndex] * 1000);
        temp->ioIndex++;
        //Critical Section. lock then access the ready queue and set ioBusy
        pthread_mutex_lock(&mutex);
        clock_gettime(CLOCK_MONOTONIC, &temp->timeEnterReadyQ);
        insertReadyQ(temp);
        ioBusy = 0;
        pthread_mutex_unlock(&mutex);
        //notify cpu thread
        sem_post(&semCPU);
    }
    //critical section. lock then change ioSysDone value.
    pthread_mutex_lock(&mutex);
    ioSysDone = 1;
    pthread_mutex_unlock(&mutex);
}   

void *CPUSchedTh()
{
    while(1)
    {
        //Break when no more PCB's and file reading thread is done
        if(readyQHead == NULL && !cpuBusy && ioQHead == NULL && !ioBusy && fileReadDone)
        {
            break;
        }
        
        //First in first out algorithm
        if(strcmp(algorithm, "FIFO") == 0)
        {
            if(clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
            {
                perror("ERROR: could not get time in CPUSched Thread\n");
                exit(1);
            }
            
            //avoid deadlock
            ts.tv_sec = 1;
            if(sem_timedwait(&semCPU, &ts)== -1 && errno == ETIMEDOUT)
            {
                continue;
            }
            
            //critical section
            pthread_mutex_lock(&mutex);
            cpuBusy = 1;
            struct PCB *temp;
            temp = getNextPCBFIFO();
            pthread_mutex_unlock(&mutex);
            
            //if temp is null, there are no more PCB's
            if(temp == NULL)
            {
                sem_post(&semIO);
                break;
            }
            //Simulate PCB using the CPU
            execute(temp, temp->cpuBurst[temp->cpuIndex]);   
        }
        
        //Priority algorithm
        else if(strcmp(algorithm, "PR") == 0)
        {
            if(clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
            {
                perror("ERROR: could not get time in CPUSched Thread\n");
                exit(1);
            }
            
            //avoid deadlock
            ts.tv_sec = 1;
            if(sem_timedwait(&semCPU, &ts)== -1 && errno == ETIMEDOUT)
            {
                continue;
            }
            
            //critical section
            pthread_mutex_lock(&mutex);
            cpuBusy = 1;
            struct PCB *temp;
            temp = getNextPCBPriority();
            pthread_mutex_unlock(&mutex);
            
            //if tem is null, there are no more PCB's. 
            if(temp == NULL)
            {
                sem_post(&semIO);
                break;
            }
            //simulate execution
            execute(temp, temp->cpuBurst[temp->cpuIndex]);   
        }
        //SJF algorithm
        else if(strcmp(algorithm, "SJF") == 0)
        {
            if(clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
            {
                perror("ERROR: could not get time in CPUSched Thread\n");
                exit(1);
            }
            
            //avoid deadlock
            ts.tv_sec = 1;
            if(sem_timedwait(&semCPU, &ts)== -1 && errno == ETIMEDOUT)
            {
                continue;
            }
            
            //critical section
            pthread_mutex_lock(&mutex);
            cpuBusy = 1;
            struct PCB *temp;
            temp = getNextPCBSJF();
            pthread_mutex_unlock(&mutex);
            
            //if temp null, no more PCB's
            if(temp == NULL)
            {
                sem_post(&semIO);
                break;
            }
            //simulate execution
            execute(temp, temp->cpuBurst[temp->cpuIndex]);   
        }
        //Round Robin algorithm
        else if(strcmp(algorithm, "RR") == 0)
        {
            if(clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
            {
                perror("ERROR: could not get time in CPUSched Thread\n");
                exit(1);
            }
            
            //avoid deadlock
            ts.tv_sec = 1;
            if(sem_timedwait(&semCPU, &ts)== -1 && errno == ETIMEDOUT)
            {
                continue;
            }
            
            //critical section
            pthread_mutex_lock(&mutex);
            cpuBusy = 1;
            struct PCB *temp;
            temp = getNextPCBFIFO();
            pthread_mutex_unlock(&mutex);
            
            //if temp null, no more PCB's
            if(temp == NULL)
            {
                sem_post(&semIO);
                break;
            }
            //simulate execution
            execute(temp, quantumTime);   
        }   
    }
    
    //critical section
    pthread_mutex_lock(&mutex);
    cpuSchDone = 1;
    clock_gettime(CLOCK_MONOTONIC, &tsEnd);
    pthread_mutex_unlock(&mutex);
}

struct PCB *getNextPCBSJF()
{
    struct PCB *temp;
    temp = NULL;
    
    if(readyQHead == NULL && readyQTail == NULL)
    {
        return NULL;
    }
    
    if(readyQHead == readyQTail)
    {
        temp = readyQHead;
        temp->prev = NULL;
        temp->next = NULL;
        readyQHead = NULL;
        readyQTail = NULL;
    }
    else
    {
        struct PCB *curr;
        temp = readyQHead;
        
        for(curr = readyQHead; curr != NULL; curr = curr->next)
        {
            if(curr->cpuBurst[curr->cpuIndex] < temp->cpuBurst[temp->cpuIndex])
            {
                temp = curr;
            }
        }
        
        if(temp == readyQHead)
        {
            readyQHead = readyQHead->next;
            readyQHead->prev = NULL;
            temp->prev = NULL;
            temp->next = NULL;
        }
        else if(temp == readyQTail)
        {
            readyQTail = readyQTail->prev;
            readyQTail->next = NULL;
            temp->prev = NULL;
            temp->next = NULL;
        }
        else
        {
            temp->prev->next = temp->next;
            temp->next->prev = temp->prev;
            temp->prev = NULL;
            temp->next = NULL;
        }
    }
    
    return temp;
}

struct PCB *getNextPCBPriority()
{
    struct PCB *temp;
    temp = NULL;
    
    if(readyQHead == NULL && readyQTail == NULL)
    {
        return NULL;
    }
    
    if(readyQHead == readyQTail)
    {
        temp = readyQHead;
        temp->prev = NULL;
        temp->next = NULL;
        readyQHead = NULL;
        readyQTail = NULL;
    }
    else
    {
        struct PCB *curr;
        temp = readyQHead;
        
        for(curr = readyQHead; curr != NULL; curr = curr->next)
        {
            if(curr->PR > temp->PR)
            {
                temp = curr;
            }
        }
        
        if(temp == readyQHead)
        {
            readyQHead = readyQHead->next;
            readyQHead->prev = NULL;
            temp->prev = NULL;
            temp->next = NULL;
        }
        else if(temp == readyQTail)
        {
            readyQTail = readyQTail->prev;
            readyQTail->next = NULL;
            temp->prev = NULL;
            temp->next = NULL;
        }
        else
        {
            temp->prev->next = temp->next;
            temp->next->prev = temp->prev;
            temp->prev = NULL;
            temp->next = NULL;
        }
    }
    
    return temp;
}

struct PCB *getNextPCBFIFO()
{
    struct PCB *temp;
    temp = NULL;
    
    if(readyQHead == NULL && readyQTail == NULL)
    {
        return NULL;
    }
    
    if(readyQHead == readyQTail)
    {
        temp = readyQHead;
        temp->prev = NULL;
        temp->next = NULL;
        readyQHead = NULL;
        readyQTail = NULL;
    }
    else
    {
        temp = readyQHead;
        readyQHead = readyQHead->next;
        readyQHead->prev = NULL;
        temp->next = NULL;
        temp->prev = NULL;
    }
    
    return temp;
}

struct PCB *getNextPCBFromIOQ()
{
    struct PCB *temp;
    temp = NULL;
    
    if(ioQHead == NULL && ioQTail == NULL)
    {
        return NULL;
    }
    
    if(ioQHead == ioQTail)
    {
        temp = ioQHead;
        temp->prev = NULL;
        temp->next = NULL;
        ioQHead = NULL;
        ioQTail = NULL;
    }
    else
    {
        temp = ioQHead;
        ioQHead = ioQHead->next;
        ioQHead->prev = NULL;
        temp->next = NULL;
        temp->prev = NULL;
    }
    
    return temp;
}

void execute(struct PCB *temp, int execTime)
{
    clock_gettime(CLOCK_MONOTONIC, &tsEnd);
    double elapsed = tsEnd.tv_sec - temp->timeEnterReadyQ.tv_sec;
    elapsed += (tsEnd.tv_nsec - temp->timeEnterReadyQ.tv_nsec)/1000000000.0;
    temp->waitTime += elapsed;
    
    if(temp->cpuBurst[temp->cpuIndex] > execTime)
    {
        usleep(execTime * 1000);
        totalExecTime += execTime;
        temp->cpuBurst[temp->cpuIndex] -= execTime;
        pthread_mutex_lock(&mutex);
        insertReadyQ(temp);
        cpuBusy = 0;
        pthread_mutex_unlock(&mutex);
        sem_post(&semCPU);
    }
    else
    {
        usleep(temp->cpuBurst[temp->cpuIndex] * 1000);
        totalExecTime += temp->cpuBurst[temp->cpuIndex];
        temp->cpuBurst[temp->cpuIndex] -= temp->cpuBurst[temp->cpuIndex];
        temp->cpuIndex++;
        
        if(temp->cpuIndex >= temp->numCPUBurst)
        {
            totalWaitTime += temp->waitTime;
            totalJobs += 1;
            clock_gettime(CLOCK_MONOTONIC, &tsEnd);
            double elapsed = tsEnd.tv_sec - tsBegin.tv_sec;
            elapsed += (tsEnd.tv_nsec - tsBegin.tv_nsec)/1000000000.0;
            totalTurnTime += elapsed;
            cpuBusy = 0;
            free(temp);
        }
        else
        {
            pthread_mutex_lock(&mutex);
            insertIOQ(temp);
            cpuBusy = 0;
            pthread_mutex_unlock(&mutex);
            sem_post(&semIO);   
        } 
    }  
}

void insertReadyQ(struct PCB *pcb)
{
    if(readyQHead == NULL && readyQTail == NULL)
    {
        readyQHead = pcb;
        readyQTail = pcb;
    }
    else
    {
        readyQTail->next = pcb;
        pcb->prev = readyQTail;
        readyQTail = pcb;
    }
}

void insertIOQ(struct PCB *pcb)
{
    if(ioQHead == NULL && ioQTail == NULL)
    {
        ioQHead = pcb;
        ioQTail = pcb;
    }
    else
    {
        ioQTail->next = pcb;
        pcb->prev = ioQTail;
        ioQTail = pcb;
    }
}

char *ReadLineFile(FILE *infile)
{
    // allocate memory for input buffer.
    char *inputBuffer;
    inputBuffer = (char *)malloc(sizeof(char) * (100));
    if(inputBuffer == NULL) // confirm allocation. exit if failed.
    {
        printf("Could not allocate buffer!\n");
        exit(1);
    }
    
    int tempChar;
    int index = 0;
    int currentBufferSize = 100;
    
    while(1)
    {
        tempChar = fgetc(infile);
        if(tempChar == EOF && index == 0) // if input stream empty free memory and return NULL
        {
            free(inputBuffer);
            return NULL;
        }
        
        // newline or EOF reached, allocate exact size and copy data. 
        if(tempChar == '\n' || tempChar == EOF)
        {
            char *newBuffer;
            newBuffer = (char *)malloc(sizeof(char) * (index + 1));
            if(newBuffer == NULL)
            {
                printf("Could not allocate new buffer!\n");
                exit(1);
            }
            memcpy(newBuffer, inputBuffer, index);
            newBuffer[index] = '\0';
            free(inputBuffer); // free old buffer
            return newBuffer;
        }
        
        if(index > currentBufferSize) // if size is not big enough reallocate
        {
            inputBuffer = (char *)realloc(inputBuffer, currentBufferSize * 2);
            if(inputBuffer == NULL)// confirm reallocation
            {
                printf("Could not re-allocate buffer!\n");
                free(inputBuffer);
                exit(1);
            }
            currentBufferSize = currentBufferSize * 2;// adjust current buffer size
        }

        inputBuffer[index] = tempChar; //add character to input buffer
        index++; // advance index
    }
    
  return(NULL);   // if there is any error!
}