/*  Copyright (C) 2020  Henry Harvey --- See LICENSE file
 *
 *  tracer
 *
 *      TODO
 */
#ifndef FORKTRACE_TRACER_HPP
#define FORKTRACE_TRACER_HPP

#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <queue>
#include <functional>

#include "system.hpp"

class Process; // defined in process.hpp
struct Tracee;
class Tracer;

/* The tracer will raise this exception when an event appears to occur out-of-
 * order or at a strange time. If this exception is raised, the tracer will
 * stop tracking the pid and will leave it as is - (it should be either killed 
 * or detached). This error could happen pretty much due to three reasons:
 *
 *  (a) Someone steps in while the trace is happening and changes something
 *      or interferes with the tracer/tracee in a way that cause a ptrace call
 *      to fail or events to come in an unexpected sequence.
 *
 *  (b) Kernel bugs.
 *
 *  (c) Bugs in this program, likely due to not carefully enough implementing
 *      the ptrace semantics for some type of scenario.
 */
class BadTraceError : public std::exception 
{
private:
    pid_t _pid;
    std::string _message;

public:
    /* Call this when a weird event occurs. */
    BadTraceError(pid_t pid, std::string_view message);
    
    const char* what() const noexcept { return _message.c_str(); }
    pid_t pid() const noexcept { return _pid; } // TODO noexcept needed here?
};

/* We use this class to keep track of blocking system calls. When a tracee 
 * reaches a syscall-entry-stop for a blocking syscall that we care about,
 * we'll use this class to maintain the state of the system call so that we
 * can finish it at a later time. This class may still be used to represent
 * system calls do not always block (e.g., wait/waitpid with WNOHANG). */
class BlockingCall 
{
public:
    virtual ~BlockingCall() { }

    /* Returns false if the tracee died while trying to prepare or finalise
     * the call. Throws an exception if some other error occurred. 
     *
     * Cleanup:
     *  If false is returned, then reaping the tracee is left to the caller
     */
    virtual bool prepare(Tracer& tracer, Tracee& tracee) = 0;
    virtual bool finalise(Tracer& tracer, Tracee& tracee) = 0;
};

/* Used for book-keeping by the Tracer class. */
struct Tracee
{
    enum State
    {
        RUNNING,
        STOPPED,
        DEAD,
    };

    pid_t pid;
    State state;
    int syscall;    // Current syscall, SYSCALL_NONE if not in one
    int signal;     // Pending signal to be delivered when next resumed
    std::shared_ptr<Process> process;
    std::unique_ptr<BlockingCall> blockingCall;

    /* Create a tracee started in the stopped state */
    Tracee(pid_t pid, std::shared_ptr<Process> process)
        : pid(pid), state(State::STOPPED), syscall(SYSCALL_NONE),
        signal(0), process(std::move(process)) { }

    /* Need a default constructor for use with unordered_map<>::operator[] */
    Tracee() : pid(-1), state(State::RUNNING), 
        syscall(SYSCALL_NONE), signal(0) { }
};

/* All the public member functions are "thread-safe". */
class Tracer 
{
private:
    /* This class is defined in tracer.cpp and needs access to us to help
     * handle a successful wait call. I could make public member functions
     * for that but I don't want to expose those functions to everyone. */
    template<class, bool, int> friend class WaitCall;

    /* We use a single lock for everything to keep it all simple. Currently,
     * only the public functions lock it - private functions are all unlocked
     * and rely on the public functions to do the locking for them. This also
     * means that you'll need to think twice before calling a public function
     * from a private function (since you could get a deadlock). */
    mutable std::mutex _lock;

    /* Keep track of the processes that are currently active. By 'active', I
     * mean the process is either currently running or is a zombie (i.e., the
     * pid is not available for recycling yet). */
    std::unordered_map<pid_t, Tracee> _tracees;

    /* A queue of all the orphans that we've been notified about. We don't
     * handle them straight away since notify_orphan may be called from a
     * separate thread and we want to be able to print error messages and
     * throw exceptions in the main thread that calls step(). */
    std::queue<pid_t> _orphans;
    
    struct Leader
    {
        bool execed; // has the initial exec succeeded yet?
        Leader() : execed(false) { }
    };

    /* Keep track of the PIDs of our direct children. */
    std::unordered_map<pid_t, Leader> _leaders;
    
    /* Stores PIDs that have been recycled by the system. This can occur when
     * the reaper process reaps a tracee, but then the system recycles its PID
     * before we get notified about it. Each time we encounter a recycled PID,
     * we pop it onto the end of this vector. When collecting PIDs of orphans,
     * we then check this vector first, to make sure we don't get confused into
     * thinking that a currently running process has been orphaned. */
    std::vector<pid_t> _recycledPIDs;

    /* Private functions, see source file */
    void collect_orphans();
    bool are_tracees_running() const;
    bool all_tracees_dead() const;
    bool resume(Tracee&);
    bool wait_for_stop(Tracee&, int&);
    void handle_wait_notification(pid_t, int);
    void handle_wait_notification(Tracee&, int);
    void handle_syscall_entry(Tracee&, int, size_t[]);
    void handle_syscall_exit(Tracee&);
    void handle_fork(Tracee&);
    void handle_failed_fork(Tracee&);
    void handle_exec(Tracee&, const char*, const char**);
    void handle_new_location(Tracee&, unsigned, const char*, const char*);
    void handle_signal_stop(Tracee&, int);
    void handle_stopped(Tracee&, int);
    Tracee& add_tracee(pid_t, std::shared_ptr<Process>);
    void expect_ended(Tracee&);
    void initiate_wait(Tracee&, std::unique_ptr<BlockingCall>);

public:
    Tracer() { }

    Tracer(const Tracer&) = delete;
    Tracer(Tracer&&) = delete;
    ~Tracer() { }

    /* Start a tracee from command line arguments. The path will be searched
     * for the program. This tracee will become our child and the new leader 
     * process. The args list includes argv[0]. Throws either a system_error
     * or runtime_error on failure. */
    std::shared_ptr<Process> start(std::string_view path, 
                                   std::vector<std::string> argv);

    /* Continue all tracees until they all stop. Returns true if there are any
     * tracees remaining and false if all are dead. */
    bool step();

    /* Notify the tracer that an orphan has been reaped by the reaper process.
     * This function is safe to call from a separate thread. */
    void notify_orphan(pid_t pid);

    /* Will forcibly kill everything. Safe to call from separate thread. */
    void nuke();

    /* Prints a list of all the active processes to std::cerr. */
    void print_list() const;
};

#endif /* FORKTRACE_TRACER_HPP */
