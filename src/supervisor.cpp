// This file is part of the "x0" project, http://xzero.io/
//   (c) 2009-2014 Christian Parpart <trapni@gmail.com>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include "sysconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <assert.h>
#include <getopt.h>
#include <initializer_list>
#include <vector>
#include <fstream>
#include <iostream>
#include <string>
#include <memory>

// TODO
// - implement watching child pidfile (--child-pidfile=FILE)
//   to determine main PID
// - implement --pidfile=FILE
// - implement --fork

// {{{ PidTracker
class PidTracker {
 public:
  PidTracker();
  ~PidTracker();

  void add(int pid);
  std::vector<int> get();
  void dump(const char* msg);
};

PidTracker::PidTracker() {
  char path[80];
  snprintf(path, sizeof(path), "/sys/fs/cgroup/cpu/%d.supervisor", getpid());
  int rv = mkdir(path, 0777);

  if (rv < 0) {
    perror("PidTracker: mkdir");
  }
}

PidTracker::~PidTracker() {
  char path[80];
  snprintf(path, sizeof(path), "/sys/fs/cgroup/cpu/%d.supervisor", getpid());
  rmdir(path);
}

void PidTracker::add(int pid) {
  char path[80];
  snprintf(path, sizeof(path), "/sys/fs/cgroup/cpu/%d.supervisor/tasks",
           getpid());

  char buf[64];
  ssize_t n = snprintf(buf, sizeof(buf), "%d", pid);

  int fd = open(path, O_WRONLY);
  write(fd, buf, n);
  close(fd);
}

std::vector<int> PidTracker::get() {
  std::vector<int> result;

  char path[80];
  snprintf(path, sizeof(path), "/sys/fs/cgroup/cpu/%d.supervisor/tasks",
           getpid());

  std::ifstream tasksFile(path);
  std::string line;

  while (std::getline(tasksFile, line)) {
    result.push_back(stoi(line));
  }

  return result;
}

void PidTracker::dump(const char* msg) {
  assert(msg && *msg);
  printf("PID tracking dump (%s): ", msg);

  const auto pids = get();
  for (int pid : pids) {
    printf(" %d", pid);
  }
  printf("\n");
}
// }}}
class Program {  // {{{
 public:
  Program(const std::string& exe, const std::vector<std::string>& argv);
  ~Program();

  bool start();
  bool restart();
  bool resume();
  void signal(int signo);

  int pid() const { return pid_; }

 private:
  bool spawn();

 private:
  std::string exe_;
  std::vector<std::string> argv_;
  int pid_;
  PidTracker pidTracker_;
};

Program::Program(const std::string& exe, const std::vector<std::string>& argv)
    : exe_(exe), argv_(argv), pid_(0), pidTracker_() {}

Program::~Program() {}

bool Program::start() {
  // just spawn
  return spawn();
}

bool Program::restart() {
  pidTracker_.dump("restart");

  const auto pids = pidTracker_.get();
  if (!pids.empty()) {
    pid_ = pids[0];
    printf("supervisor: reattaching to child PID %d\n", pid_);
    return true;
  }

  return spawn();
}

bool Program::resume() {
  pidTracker_.dump("resume");
  const auto pids = pidTracker_.get();
  if (pids.empty()) {
    return false;
  }

  pid_ = pids[0];

  return true;
}

void Program::signal(int signo) {
  // just send signal to PID
  kill(pid_, signo);
}

bool Program::spawn() {
  printf("supervisor: spawning program (%s)...\n", exe_.c_str());

  pid_t pid = fork();

  if (pid < 0) {
    fprintf(stderr, "supervisor: fork failed. %s\n", strerror(errno));
    return false;
  } else if (pid > 0) {  // parent
    pid_ = pid;
    pidTracker_.add(pid);
    printf("supervisor: child pid is %d\n", pid);
    pidTracker_.dump("spawn");

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
      fprintf(stderr, "supervisor: prctl(PR_SET_CHILD_SUBREAPER) failed. %s",
              strerror(errno));

      // if this one fails, we can still be functional to *SOME* degree,
      // like, auto-restarting still works, but
      // the supervised child is forking to re-exec, that'll not work then.
    }
    return true;
  } else {  // child
    std::vector<char*> argv;

    for (const std::string& arg : argv_) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    execvp(exe_.c_str(), argv.data());
    fprintf(stderr, "execvp failed. %s\n", strerror(errno));
    abort();
  }
}
// }}}
class Supervisor {  // {{{
 public:
  Supervisor();
  ~Supervisor();

  int run(int argc, char* argv[]);

  static Supervisor* self() { return self_; }

 private:
  bool parseArgs(int argc, char* argv[]);
  void printVersion();
  void printHelp();
  bool restart();
  static void sighandler(int signum);

 private:
  static Supervisor* self_;
  std::unique_ptr<Program> program_;
  std::string pidfile_;
  std::string childPidfile_;
  int restartLimit_;
  int restartDelay_;
  int restartOnError_;
  bool fork_;
};

Supervisor* Supervisor::self_ = nullptr;

Supervisor::Supervisor()
    : program_(nullptr),
      pidfile_(),
      childPidfile_(),
      restartLimit_(0),    // do not auto-restart
      restartDelay_(0),    // do not wait during restarts
      restartOnError_(0),  // boolean
      fork_(false) {
  assert(self_ == nullptr);
  self_ = this;
}

Supervisor::~Supervisor() { self_ = nullptr; }

bool Supervisor::parseArgs(int argc, char* argv[]) {
  if (argc <= 1) {
    printHelp();
    return false;
  }

  struct option opts[] = {
      {"fork", no_argument, nullptr, 'f'},
      {"pidfile", required_argument, nullptr, 'p'},
      {"restart-limit", required_argument, &restartLimit_, 'r'},
      {"restart-delay", required_argument, &restartDelay_, 'd'},
      {"restart-on-error", no_argument, &restartOnError_, 'R'},
      {"signal", required_argument, nullptr, 's'},
      {"child-pidfile", required_argument, nullptr, 'P'},
      //.
      {"version", no_argument, nullptr, 'v'},
      {"copyright", no_argument, nullptr, 'y'},
      {"help", no_argument, nullptr, 'h'},
      //.
      {0, 0, 0, 0}};

  for (;;) {
    int long_index = 0;
    switch (getopt_long(argc, argv, "fp:r:d:Rs:P:vh", opts, &long_index)) {
      case 'f':
        fork_ = true;
        printf("fork\n");
        break;
      case 'p':
        pidfile_ = optarg;
        printf("pid file: %s\n", optarg);
        break;
      case 'r':
        restartLimit_ = atoi(optarg);
        printf("restart limit: %d\n", restartLimit_);
        break;
      case 'd':
        restartDelay_ = atoi(optarg);
        printf("restart delay: %d\n", restartDelay_);
        break;
      case 'R':
        restartOnError_ = true;
        printf("restart on error: true\n");
        break;
      case 's':
        printf("TODO add signal: %s\n", optarg);
        break;
      case 'P':
        childPidfile_ = optarg;
        printf("child pid file: %s\n", optarg);
        break;
      case 'v':
        printVersion();
        return false;
      case 'h':
        printHelp();
        return false;
      case 0:  // long option with (val!=nullptr && flag=0)
        break;
      case -1: {
        // EOF - everything parsed.
        if (optind == argc) {
          fprintf(stderr, "supervisor: no program path given\n");
          return false;
        }

        std::vector<std::string> args;
        while (optind < argc) {
          args.push_back(argv[optind++]);
        }

        program_.reset(new Program(args[0], args));

        return true;
      }
      case '?':  // ambiguous match / unknown arg
      default:
        return false;
    }
  }
  return true;
}

void Supervisor::printVersion() {
  printf("supervisor: %s\n", SUPERVISOR_VERSION);
}

void Supervisor::printHelp() {
  printf(
      "supervisor: a process supervising tool\n"
      "  (c) 2009-2014 Christian Parpart <trapni@gmail.com>\n"
      "\n"
      "usage:\n"
      "  supervisor [-f|--fork] [-p|--pidfile=PATH] -- cmd ...\n"
      "\n"
      "options:\n"
      "  -f,--fork             fork supervisor into background\n"
      "  -p,--pidfile=PATH     location to store the current supervisor PID\n"
      "  -r,--restart-limit=N  automatically restart program, if crashed\n"
      "  -d,--restart-delay=N  number of seconds to wait before we retry\n"
      "                        to restart the application\n"
      "  -R,--restart-on-error Restart the application also on normal\n"
      "                        termination but with an exit code != 0.\n"
      "  -s,--signal=SIGNAL    Adds given signal to the list of signals\n"
      "                        to forward to the supervised program.\n"
      "                        Defaults to (INT, TERM, QUIT, USR1, USR2, HUP)\n"
      "  -P,--child-pidfile=PATH\n"
      "                        Path to the child process' managed PID file.\n"
      "                        The supervisor is watching this file for "
      "updates.\n"
      "  -v,--version          Prints program version number and exits\n"
      "  -h,--help             Prints this help and exits.\n"
      "\n"
      "Examples:\n"
      "    supervisor -- /usr/sbin/x0d --no-fork\n"
      "    supervisor -p /var/run/xzero/supervisor.pid -- /usr/sbin/x0d \\\n"
      "               --no-fork\n"
      "\n");
}

int Supervisor::run(int argc, char* argv[]) {
  try {
    if (!parseArgs(argc, argv)) return 1;

    printf("Installing signal handler...\n");
    for (int sig : {SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGUSR1, SIGUSR2}) {
      signal(sig, &Supervisor::sighandler);
    }

    // if (setsid() < 0) {
    //   fprintf(stderr, "Error creating session. %s\n", strerror(errno));
    //   return EXIT_FAILURE;
    // }

    // if (setpgrp() < 0) {
    //   fprintf(stderr, "Error creating process group. %s\n", strerror(errno));
    //   return EXIT_FAILURE;
    // }

    program_->start();

    for (;;) {
      int status = 0;
      if (waitpid(program_->pid(), &status, 0) < 0) {
        perror("waitpid");
        throw EXIT_FAILURE;
      }

      if (WIFEXITED(status)) {
        printf(
            "supervisor: Program PID %d terminated normmally "
            "with exit code %d\n",
            program_->pid(), WEXITSTATUS(status));

        if (program_->resume()) {
          printf("supervisor: Reattaching to child PID %d.\n", program_->pid());
          continue;
        }

        if (WEXITSTATUS(status) != EXIT_SUCCESS && restartOnError_) {
          printf("supervisor: Restarting due to error code %d.",
                 WEXITSTATUS(status));

          if (restart()) {
            continue;
          }
        }

        return WEXITSTATUS(status);
      }

      if (WIFSIGNALED(status)) {
        printf("Child %d terminated with signal %s (%d)\n", program_->pid(),
               strsignal(WTERMSIG(status)), WTERMSIG(status));
        if (!restart()) {
          return EXIT_FAILURE;
        }
        continue;
      }

      fprintf(stderr,
              "Child %d terminated (neither normally nor abnormally. "
              "Status code %d\n",
              program_->pid(), status);

      if (!restart()) {
        return EXIT_FAILURE;
      }
    }
  }
  catch (int exitCode) {
    return exitCode;
  }
}

bool Supervisor::restart() {
  if (restartLimit_ == 0) {
    return false;
  }

  if (restartLimit_ > 0) {
    restartLimit_--;
    printf("supervisor: Restarting application (death counter: %d)\n",
           restartLimit_);
  }

  if (restartDelay_) {
    sleep(restartDelay_);
  }

  return program_->restart();
}

void Supervisor::sighandler(int signum) {
  if (self()->program_->pid()) {
    printf("Signal %s (%d) received. Forwarding to child PID %d.\n",
           strsignal(signum), signum, self()->program_->pid());

    // forward to child process
    self()->program_->signal(signum);
  }
}
// }}}

int main(int argc, char* argv[]) {
  Supervisor supervisor;
#if !defined(NDEBUG)
  if (argc == 1) {
    static const char* args[] = {
        argv[0],     "--restart-on-error", "--restart-limit=3", "--",
        "/bin/echo", "Hello, World",       nullptr};
    return supervisor.run(sizeof(args) / sizeof(*args) - 1, (char**)args);
  } else {
    return supervisor.run(argc, argv);
  }
#else
  return supervisor.run(argc, argv);
#endif
}

// vim:ts=2:sw=2