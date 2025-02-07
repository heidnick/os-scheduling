#include "process.h"

// Process class methods
Process::Process(ProcessDetails details, uint64_t current_time)
{
    int i;
    pid = details.pid;
    start_time = details.start_time;
    num_bursts = details.num_bursts;
    current_burst = 0;
    last_burst = 0;
    current_wait_burst = 0;
    last_wait_burst = 0;
    last_burst_time_added = 0;
    last_wait_burst_time_added = 0;
    ready_start_time = 0;
    burst_times = new uint32_t[num_bursts];
    for (i = 0; i < num_bursts; i++)
    {
        burst_times[i] = details.burst_times[i];
    }
    priority = details.priority;
    state = (start_time == 0) ? State::Ready : State::NotStarted;
    if (state == State::Ready)
    {
        launch_time = current_time;
    }
    is_interrupted = false;
    ready_out = true;
    core = -1;
    turn_time = 0;
    wait_time = 0;
    cpu_time = 0;
    remain_time = 0;
    for (i = 0; i < num_bursts; i+=2)
    {
        remain_time += burst_times[i];
    }
    io_time = 0;
}

Process::~Process()
{
    delete[] burst_times;
}

uint16_t Process::getPid() const
{
    return pid;
}

uint32_t Process::getStartTime() const
{
    return start_time;
}

uint8_t Process::getPriority() const
{
    return priority;
}

uint64_t Process::getBurstStartTime() const
{
    return burst_start_time;
}

Process::State Process::getState() const
{
    return state;
}

bool Process::isInterrupted() const
{
    return is_interrupted;
}

int8_t Process::getCpuCore() const
{
    return core;
}

double Process::getTurnaroundTime() const
{
    return (double)turn_time / 1000.0;
}

double Process::getWaitTime() const
{
    return (double)wait_time / 1000.0;
}

double Process::getCpuTime() const
{
    return (double)cpu_time / 1000.0;
}

double Process::getRemainingTime() const
{
    return (double)remain_time / 1000.0;
}

uint16_t Process::getNum_bursts() const 
{
    return num_bursts;
}


uint16_t Process::getCurrent_burst() const 
{
    return current_burst;
}

uint32_t Process::getCurrent_burst_time() const
{
    return burst_times[current_burst];
}


uint32_t* Process::getBurst_times() const
{
    return burst_times;
}

void Process::incrementCurrentBurst()
{
    current_burst++;
}

void Process::incrementCurrentWaitBurst()
{
    current_wait_burst++;
}


void Process::setBurstStartTime(uint64_t current_time)
{
    burst_start_time = current_time;
}

void Process::setState(State new_state, uint64_t current_time)
{
    if (state == State::NotStarted && new_state == State::Ready)
    {
        launch_time = current_time;
    }
    state = new_state;
}

void Process::setCpuCore(uint8_t core_num)
{
    core = (int8_t)core_num;
}

void Process::interrupt()
{
    is_interrupted = true;
}

void Process::interruptHandled()
{
    is_interrupted = false;
}

void Process::updateProcess(uint64_t current_time)
{
    // use `current_time` to update turnaround time, wait time, burst times, 
    // cpu time, and remaining time
    

    if (getState() != Process::State::Terminated){
        turn_time = current_time - launch_time;
    }
    if (getState() == Process::State::Ready) {
        if (current_wait_burst > last_wait_burst) {
            last_wait_burst = current_wait_burst;
            last_wait_burst_time_added = 0;
        }
        int32_t new_wait_time_added = current_time - ready_start_time;
        wait_time +=  new_wait_time_added - last_wait_burst_time_added;
        last_wait_burst_time_added = current_time - ready_start_time;

    }
    if (getState() == Process::State::Running) {
        if (current_burst > last_burst) {
            last_burst = current_burst;
            last_burst_time_added = 0;
        }
        int32_t new_cpu_time_added = current_time - burst_start_time;
        int32_t old_remain_time = remain_time;
        cpu_time +=  new_cpu_time_added - last_burst_time_added;
        remain_time = old_remain_time - new_cpu_time_added + last_burst_time_added;
        last_burst_time_added = current_time - burst_start_time; 
        
        
    }
}

void Process::updateBurstTime(int burst_idx, uint32_t new_time)
{
    burst_times[burst_idx] = new_time;
}


// Comparator methods: used in std::list sort() method
// No comparator needed for FCFS or RR (ready queue never sorted)

// SJF - comparator for sorting read queue based on shortest remaining CPU time
bool SjfComparator::operator ()(const Process *p1, const Process *p2)
{
    // your code here!
    if (p1->getRemainingTime() <= p2->getRemainingTime()){
        return true;
    }else {
        return false;
    }
}

// PP - comparator for sorting read queue based on priority
bool PpComparator::operator ()(const Process *p1, const Process *p2)
{
    // your code here!
    if (p1->getPriority() <= p2->getPriority()){
        return true;
    }else {
        return false;
    }
}

void Process::setTurn_time(uint64_t current_time)
{
    turn_time = current_time - launch_time;
}

void Process::setReady_start_time(uint64_t new_ready_start_time)
{
    ready_start_time = new_ready_start_time;
}

void Process::readyOut(bool param)
{
    ready_out = param;
}


