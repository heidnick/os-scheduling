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
    bool finished_main;
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
        //std::cout << p << std::endl;
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            p->setReady_start_time(start);
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
    uint64_t last_time_slice;
    int time_slice_flag = 0;
    uint64_t finish;
    //Creating mutex lock
    std::unique_lock<std::mutex> lock(shared_data->mutex, std::defer_lock);
    while (!(shared_data->all_terminated))
    {        
        // Clear output from previous iteration - 89 182
        clearOutput(num_lines);

        // Do the following:
        //   - Get current time
        uint64_t curTime = currentTime();

        //   1 - *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
        //   2 - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
        //   3 - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
        //   4 - *Sort the ready queue (if needed - based on scheduling algorithm)
        //   5 - Determine if all processes are in the terminated state
        //   - * = accesses shared data (ready queue), so be sure to use proper synchronization
        
        int count_terminated = 0;
        //This loops through all processes
        for (int i=0; i<processes.size(); i++){

            //terminated check
            if (processes[i]->getState() == Process::State::Terminated) {
                //std::cout << "terminated check" << std::endl;
                count_terminated ++;
            }
            if (processes[i]->getState() == Process::State::Ready) {
                processes[i]->updateProcess(currentTime());
            }


            // 1 - Sets not started process to ready and loads into ready queue based on elapsed time
            if (processes[i]->getStartTime() <= (curTime - start) && processes[i]->getState() == Process::State::NotStarted) {
                processes[i]->setState(Process::State::Ready, curTime);
                processes[i]->setReady_start_time(curTime);
                processes[i]->incrementCurrentWaitBurst();
                lock.lock();
                shared_data->ready_queue.push_back(processes[i]);
                lock.unlock();
            }

            // 2 - Check if process has finished IO burst, put in ready queue if they have completed time
            if (processes[i]->getState() == Process::State::IO) {
                if ((processes[i]->getBurstStartTime() + processes[i]->getCurrent_burst_time()) <= curTime){
                    processes[i]->setState(Process::State::Ready, curTime);
                    processes[i]->setReady_start_time(curTime);
                    processes[i]->incrementCurrentWaitBurst();
                    processes[i]->incrementCurrentBurst();
                    lock.lock();
                    shared_data->ready_queue.push_back(processes[i]);
                    lock.unlock();
                }
            }
            
            // 3 - Running process interrupt check
            if (processes[i]->getState() == Process::State::Running) {
                last_time_slice = processes[i]->getBurstStartTime();
                if (curTime >= last_time_slice + shared_data->time_slice) {
                    time_slice_flag = 1;
                }
                
                // If the algorithm is RR, check time slice for interrupt
                if (shared_data->algorithm == ScheduleAlgorithm::RR && time_slice_flag == 1) {
                    processes[i]->interrupt();
                    time_slice_flag = 0;
                }
                
                // If the algorithm is PP, check if newly created process has greater priority
                if (shared_data->algorithm == ScheduleAlgorithm::PP) {
                    if (shared_data->ready_queue.size() > 0) {
                        if (shared_data->ready_queue.back()->getPriority() < processes[i]->getPriority()){
                            processes[i]->interrupt();
                        }
                    }
                }
            }
            
            //PP sorting
            lock.lock();
            if (shared_data->algorithm == ScheduleAlgorithm::PP && shared_data->ready_queue.size() > 1) {
                shared_data->ready_queue.sort(PpComparator());
            }
            lock.unlock();

            //SJF sorting
            lock.lock();
            if (shared_data->algorithm == ScheduleAlgorithm::SJF && shared_data->ready_queue.size() > 1) {
                shared_data->ready_queue.sort(SjfComparator());
            }
            lock.unlock();

            //processes[i]->updateProcess(curTime); 
        }//for loop

        if (count_terminated == processes.size()) {
            //std::cout << "all terminated " << std::endl;
            shared_data->all_terminated = true;
            finish = curTime;
        }


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
    int turn, wait, cpu, tot_run;
    int32_t mid = (finish - start) / 2;

    int low, high = 0;
    for (int i=0; i<processes.size(); i++) {
        turn += processes[i]->getTurnaroundTime();
        wait += processes[i]->getWaitTime();
        cpu += processes[i]->getCpuTime();
        if (processes[i]->getTurnaroundTime() < mid) {
            low++;
        }else{
            high++;
        }
    }
    cpu = cpu / turn;
    turn = turn / processes.size();
    wait = wait / processes.size();

    printf("CPU utilization: %d\n", cpu);
    printf("Troughput 0-50: %d\n", low);
    printf("Troughput 50-100: %d\n", high);
    printf("Troughput 0-100: %d\n", low+high);
    printf("Average turnaround time: %d\n", turn);
    printf("Average waiting time: %d\n", wait);


    

    


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
    std::unique_lock<std::mutex> lock(shared_data->mutex, std::defer_lock);
    while(!(shared_data->all_terminated)){
        
        uint64_t current_time = currentTime();

        Process *p;
        lock.lock();
        if (shared_data->ready_queue.size() > 0){
            p = shared_data->ready_queue.front();
            shared_data->ready_queue.pop_front();
            p->setState(Process::State::Running, current_time);
            p->readyOut(false);
        }
        lock.unlock();
            
        if (p->getState() == Process::State::Running) {
            
            p->setCpuCore(core_id);
            p->setBurstStartTime(current_time);
            uint32_t current_burst_time = p->getCurrent_burst_time();
            uint64_t elapsed_burst_time = current_time - p->getBurstStartTime();

            // 2 - Simulate process until 2a - interrupt or 2b - end of burst
            int done = 0;
            while (!done){
                current_time = currentTime();

                //find time elapsed
                elapsed_burst_time = current_time - p->getBurstStartTime();
                if (elapsed_burst_time >= current_burst_time) {
                    done = 1;
                }

                if (p->isInterrupted()) {
                    done = 1;
                }
                
                //update process
                p->updateProcess(current_time);

            }
            
            // 3a - 3b - 3c
            if (p->isInterrupted()){
                p->setCpuCore(-1);
                uint32_t updated_burst_time = current_burst_time - elapsed_burst_time;
                p->updateBurstTime(p->getCurrent_burst(), updated_burst_time);
                p->setState(Process::State::Ready, current_time);
                p->setReady_start_time(current_time);
                p->incrementCurrentWaitBurst();
                p->interruptHandled();
                lock.lock();
                shared_data->ready_queue.push_back(p);
                lock.unlock();
            }else if (p->getNum_bursts() == (p->getCurrent_burst() + 1)) {
                p->setCpuCore(-1);
                p->setState(Process::State::Terminated, current_time);
            }else {
                p->setState(Process::State::IO, current_time);
                p->setCpuCore(-1);
                p->setBurstStartTime(current_time);
                p->incrementCurrentBurst();
            }
            

            //perform context switch
            usleep((int)(shared_data->context_switch) * 1000);
        }else {
            current_time = currentTime();
            //p->updateProcess(current_time);
        }
        
    }//while loop
    
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
