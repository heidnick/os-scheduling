#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include "configreader.h"
#include "process.h"

// Shared data for all cores
typedef struct SchedulerData {
    std::mutex mutex;
    std::condition_variable condition;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process*> ready_queue;
    bool all_terminated;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex);
void clearOutput(int num_lines);
uint64_t currentTime();
std::string processStateToString(Process::State state);

int main(int argc, char **argv)
{
    // Ensure user entered a command line parameter for configuration file name
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Declare variables used throughout main
    int i;
    SchedulerData *shared_data;
    std::vector<Process*> processes;

    // Read configuration file for scheduling simulation
    SchedulerConfig *config = readConfigFile(argv[1]);

    // Store configuration parameters in shared data object
    uint8_t num_cores = config->cores;
    shared_data = new SchedulerData();
    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;

    // Create processes
    uint64_t start = currentTime();
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            shared_data->ready_queue.push_back(p);
        }
    }

    // Free configuration data from memory
    deleteConfig(config);

    // Launch 1 scheduling thread per cpu core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }

    // Main thread work goes here
    int num_lines = 0;
    uint64_t last_time_slice = start;
    int time_slice_flag = 0;
    while (!(shared_data->all_terminated))
    {        
        // Clear output from previous iteration
        clearOutput(num_lines);

        // Do the following:
        //   - Get current time
        uint64_t curTime = currentTime();

        //   - Time slice check
        if (curTime >= last_time_slice + shared_data->time_slice) {
            last_time_slice = last_time_slice + shared_data->time_slice;
            time_slice_flag = 1;
            //std::cout << last_time_slice << std::endl;  
        }

        //   1 - *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
        //   2 - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
        //   3 - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
        //   4 - *Sort the ready queue (if needed - based on scheduling algorithm)
        //   5 - Determine if all processes are in the terminated state
        //   - * = accesses shared data (ready queue), so be sure to use proper synchronization
        {
            //Creating mutex lock
            std::lock_guard<std::mutex> lock(shared_data->mutex);

            int count_terminated = 0;
            //This loops through all processes
            for (int i=0; i<processes.size(); i++){

                // 1 - Sets not started process to ready and loads into ready queue based on elapsed time
                if (processes[i]->getStartTime() <= (curTime - start) && processes[i]->getState() == Process::State::NotStarted) {
                    processes[i]->setState(Process::State::Ready, curTime);
                    shared_data->ready_queue.push_back(processes[i]);
                }

                // 2 - Check if process has finished IO burst, put in ready queue if they have completed time
                if (processes[i]->getState() == Process::State::IO) {
                    if (processes[i]->getBurstStartTime() + processes[i]->getBurst_times()[processes[i]->getCurrent_burst()] >= curTime){
                        processes[i]->setState(Process::State::Ready, curTime);
                        shared_data->ready_queue.push_back(processes[i]);
                        processes[i]->setCurrentBurst();
                    }
                }

                //PP sorting
                if (shared_data->algorithm == ScheduleAlgorithm::PP) {
                    shared_data->ready_queue.sort(PpComparator());
                }
                
                // 3 - Running process interrupt check
                if (processes[i]->getState() == Process::State::Running) {
                    
                    // If the algorithm is RR, check time slice for interrupt
                    if (shared_data->algorithm == ScheduleAlgorithm::RR && time_slice_flag == 1) {
                        processes[i]->interrupt();
                        processes[i]->setCpuCore(-1);
                        uint32_t current_burst_time = processes[i]->getCurrent_burst_time();
                        uint32_t elapsed_burst_time = current_burst_time - (curTime - processes[i]->getBurstStartTime());
                        processes[i]->updateBurstTime(processes[i]->getCurrent_burst(), elapsed_burst_time);
                        processes[i]->setState(Process::State::Ready, curTime);
                        shared_data->ready_queue.push_back(processes[i]);
                    }
                    
                    // If the algorithm is PP, check if newly created process has greater priority
                    if (shared_data->algorithm == ScheduleAlgorithm::PP) {
                        if (shared_data->ready_queue.size() > 0) {
                            if (shared_data->ready_queue.front()->getPriority() > processes[i]->getPriority()){
                                processes[i]->interrupt();
                                processes[i]->setCpuCore(-1);
                                uint32_t current_burst_time = processes[i]->getCurrent_burst_time();
                                uint32_t elapsed_burst_time = current_burst_time - (curTime - processes[i]->getBurstStartTime());
                                processes[i]->updateBurstTime(processes[i]->getCurrent_burst(), elapsed_burst_time);
                                processes[i]->setState(Process::State::Ready, curTime);
                                shared_data->ready_queue.push_back(processes[i]);
                            }
                        }
                    }
                }

                //terminated check
                if (processes[i]->getState() == Process::State::Terminated) {
                    count_terminated ++;
                }
            }//for loop

            if (count_terminated == processes.size()) {
                shared_data->all_terminated == true;
            }

            //SJF sorting
            if (shared_data->algorithm == ScheduleAlgorithm::SJF) {
                shared_data->ready_queue.sort(SjfComparator());
            }

            //Reset time slice flag
            time_slice_flag = 0;

            // 5 - Ready queue is now sorted, now notifies waiting coreRunProcesses threads
            if (!shared_data->all_terminated){
                shared_data->condition.notify_all();
            }
        }//mutex scope


        // output process status table
        num_lines = printProcessOutput(processes, shared_data->mutex);

        // sleep 50 ms
        usleep(50000);
    }//while loop


    // wait for threads to finish
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }

    // print final statistics
    //  - CPU utilization
    //  - Throughput
    //     - Average for first 50% of processes finished
    //     - Average for second 50% of processes finished
    //     - Overall average
    //  - Average turnaround time
    //  - Average waiting time
    


    // Clean up before quitting program
    processes.clear();

    return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
    // Work to be done by each core idependent of the other cores
    // Repeat until all processes in terminated state:
    //  1 - *Get process at front of ready queue
    //  2 - Simulate the processes running until one of the following:
    //    2a - CPU burst time has elapsed
    //    2b - Interrupted (RR time slice has elapsed or process preempted by higher priority process)
    //  3 - Place the process back in the appropriate queue
    //    3a - I/O queue if CPU burst finished (and process not finished) -- no actual queue, simply set state to IO
    //    3b - Terminated if CPU burst finished and no more bursts remain -- no actual queue, simply set state to Terminated
    //    3c - *Ready queue if interrupted (be sure to modify the CPU burst time to now reflect the remaining time)
    //  4 - Wait context switching time
    //  - * = accesses shared data (ready queue), so be sure to use proper synchronization
    while(!(shared_data->all_terminated)){
        //Creating the lock, condition calls wait for main to release lock
        std::unique_lock<std::mutex> lock(shared_data->mutex);
        shared_data->condition.wait(lock);

        // 1 - Setting pointer p to the first element in the ready queue, (removing process from the queue?->uncomment pop_front)
        if (shared_data->ready_queue.size() > 0) {
            Process *p = shared_data->ready_queue.front();
            //shared_data->ready_queue.pop_front();
        }


        lock.unlock();
    }
    
}

int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex)
{
    int i;
    int num_lines = 2;
    std::lock_guard<std::mutex> lock(mutex);
    printf("|   PID | Priority |      State | Core | Turn Time | Wait Time | CPU Time | Remain Time |\n");
    printf("+-------+----------+------------+------+-----------+-----------+----------+-------------+\n");
    for (i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::State::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
            double turn_time = processes[i]->getTurnaroundTime();
            double wait_time = processes[i]->getWaitTime();
            double cpu_time = processes[i]->getCpuTime();
            double remain_time = processes[i]->getRemainingTime();
            printf("| %5u | %8u | %10s | %4s | %9.1lf | %9.1lf | %8.1lf | %11.1lf |\n", 
                   pid, priority, process_state.c_str(), cpu_core.c_str(), turn_time, 
                   wait_time, cpu_time, remain_time);
            num_lines++;
        }
    }
    return num_lines;
}

void clearOutput(int num_lines)
{
    int i;
    for (i = 0; i < num_lines; i++)
    {
        fputs("\033[A\033[2K", stdout);
    }
    rewind(stdout);
    fflush(stdout);
}

uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return ms;
}

std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
        case Process::State::NotStarted:
            str = "not started";
            break;
        case Process::State::Ready:
            str = "ready";
            break;
        case Process::State::Running:
            str = "running";
            break;
        case Process::State::IO:
            str = "i/o";
            break;
        case Process::State::Terminated:
            str = "terminated";
            break;
        default:
            str = "unknown";
            break;
    }
    return str;
}
