#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <sys/types.h>

namespace tinyborg {

enum class JobState {
    RUNNING,
    EXITED,
    KILLED,
    FAILED_TO_START
};

struct RunningJob {
    std::string job_id;
    pid_t pid;
    JobState state;
    int exit_code = -1;
};

class JobRunner {
    public:
        bool StartJob(const std::string& job_id, const std::string& binary_path, const std::vector<std::string>& args);

        void ReapExited();

        bool KillJob(const std::string& job_id);

        bool GetJob(const std::string& job_id, RunningJob* out) const;

    private:
        std::unordered_map<std::string, RunningJob> jobs_;

};

}
