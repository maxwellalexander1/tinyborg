#include "job_runner.h"

#include <system_error>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstring>
#include <iostream>

namespace tinyborg {
    bool JobRunner::StartJob(const std::string& job_id, const std::string& binary_path, const std::vector<std::string>& args) {
        pid_t pid = fork();

        if (pid < 0) {
            std::cerr << "fork() failed for job " << job_id << ": "
                << strerror(errno) << "\n";
            return false;
        }

        if (pid == 0) {
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(binary_path.c_str()));
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);

            execvp(binary_path.c_str(), argv.data());

            std::cerr << "execvp failed for " << binary_path << ": "
                << strerror(errno) << "\n";
            _exit(127);
        }

        RunningJob job;
        job.job_id = job_id;
        job.pid = pid;
        job.state = JobState::RUNNING;
        jobs_[job_id] = job;

        return true;
    }

    void JobRunner::ReapExited() {
        for (auto& [job_id, job] : jobs_) {
            if (job.state != JobState::RUNNING) {
                continue;
            }

            int status;

            pid_t result = waitpid(job.pid, &status, WNOHANG);

            if (result == 0) {
                continue;
            }

            if (result < 0) {
                std::cerr << "waitpid failed for job " << job_id << ": "
                    << strerror(errno) << "\n";
                continue;
            }

            if (WIFEXITED(status)){
                job.exit_code = WEXITSTATUS(status);
                job.state = JobState::EXITED;
            } else if (WIFSIGNALED(status)) {
                job.exit_code = 128 + WTERMSIG(status);
                job.state = JobState::KILLED;
            }
        }
    }

    bool JobRunner::KillJob(const std::string& job_id) {
      auto it = jobs_.find(job_id);
      if (it == jobs_.end()) {
        return false;  // no such job
      }

      RunningJob& job = it->second;
      if (job.state != JobState::RUNNING) {
        return false;  // already exited, nothing to kill
      }

      if (kill(job.pid, SIGTERM) != 0) {
        std::cerr << "kill(SIGTERM) failed for job " << job_id << ": "
                  << strerror(errno) << "\n";
        return false;
      }

      // Don't mark as KILLED here yet, we sent the signal but the process
      // hasn't necessarily died. Let ReapExited() catch the real state
      // transition once waitpid confirms it via WIFSIGNALED.
      return true;
    }

    bool JobRunner::GetJob(const std::string& job_id, RunningJob* out) const {
      auto it = jobs_.find(job_id);
      if (it == jobs_.end()) {
        return false;
      }
      *out = it->second;
      return true;
    }
}
