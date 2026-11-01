#include "job_runner.h"
#include <chrono>
#include <thread>
#include <iostream>

int main() {
  tinyborg::JobRunner runner;

  // Start a long-running job we'll kill partway through
  runner.StartJob("job1", "/bin/sleep", {"30"});

  for (int i = 0; i < 8; i++) {
    runner.ReapExited();

    tinyborg::RunningJob job;
    if (runner.GetJob("job1", &job)) {
      std::cout << "Tick " << i << ": job1 state = "
                << static_cast<int>(job.state) << "\n";
    }

    if (i == 3) {
      std::cout << "--- killing job1 ---\n";
      runner.KillJob("job1");
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  return 0;
}
