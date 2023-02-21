#include "node_queue_scheduler.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

// Using boost::atomic_flag as our use case with waiting requires GCC 11+.
// TODO(anybody): switch to std::atomic_flag once we require at least GCC 11.
#include <boost/atomic/atomic_flag.hpp>

#include "abstract_task.hpp"
#include "hyrise.hpp"
#include "job_task.hpp"
#include "task_queue.hpp"
#include "uid_allocator.hpp"
#include "utils/assert.hpp"
#include "worker.hpp"

namespace hyrise {

NodeQueueScheduler::NodeQueueScheduler() {
  _worker_id_allocator = std::make_shared<UidAllocator>();
}

NodeQueueScheduler::~NodeQueueScheduler() {
  if (HYRISE_DEBUG && _active) {
    // We cannot throw an exception because destructors are noexcept by default.
    std::cerr << "NodeQueueScheduler::finish() wasn't called prior to destroying it" << std::endl;
    std::exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
  }
}

void NodeQueueScheduler::begin() {
  _shutdown_flag = false;
  DebugAssert(!_active, "Scheduler is already active");

  _workers.reserve(Hyrise::get().topology.num_cpus());
  _queue_count = Hyrise::get().topology.nodes().size();
  _queues.reserve(_queue_count);

  for (auto node_id = NodeID{0}; node_id < Hyrise::get().topology.nodes().size(); ++node_id) {
    auto queue = std::make_shared<TaskQueue>(node_id);

    _queues.emplace_back(queue);

    const auto& topology_node = Hyrise::get().topology.nodes()[node_id];

    for (const auto& topology_cpu : topology_node.cpus) {
      _workers.emplace_back(std::make_shared<Worker>(queue, WorkerID{_worker_id_allocator->allocate()},
                                                     topology_cpu.cpu_id, _shutdown_flag));
    }
  }

  _workers_per_node = _workers.size() / _queue_count;
  _active = true;

  for (auto& worker : _workers) {
    worker->start();
  }

  // We wait for each worker to start. Without waiting, test might shut down the scheduler before any workers have
  // started.
  for (const auto& worker : _workers) {
    worker->is_ready.wait(false);
  }

  // Sleep to ensure that worker threads have been set up correctly. Otherwise, tests that immediate take the scheduler
  // down might create tasks before the workers are set up.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

void NodeQueueScheduler::wait_for_all_tasks() {
  auto wait_loops = size_t{0};
  while (true) {
    auto num_finished_tasks = uint64_t{0};
    for (const auto& worker : _workers) {
      num_finished_tasks += worker->num_finished_tasks();
    }

    if (num_finished_tasks == _task_counter) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (wait_loops > 10'000) {
      Fail("Time out during wait_for_all_tasks().");
    }
    ++wait_loops;
  }
}

void NodeQueueScheduler::finish() {
  // Lock finish() to ensure that the shutdown tasks are not send twice (we later check for empty queues).
  auto lock = std::lock_guard<std::mutex>{_finish_mutex};

  if (!_active) {
    return;
  }

  // Signal workers that scheduler is shutting down.
  _shutdown_flag = true;

  {
    auto wake_up_loop_count = size_t{0};
    while (true) {
      auto wait_flag = std::atomic_flag{};
      auto waiting_workers_counter = std::atomic_uint32_t{0};

      // Schedule non-op jobs (one for each worker). Can be necessary as workers might sleep and wait for queue events. The
      // tasks cannot be stolen to ensure that we reach each worker of each node.
      auto jobs = std::vector<std::shared_ptr<AbstractTask>>{};
      jobs.reserve(_queue_count * _workers_per_node);
      for (auto node_id = NodeID{0}; node_id < _queue_count; ++node_id) {
        for (auto worker_id = size_t{0}; worker_id < _workers_per_node; ++worker_id) {
          auto shutdown_signal_task = std::make_shared<JobTask>([&] () {
            ++waiting_workers_counter;
            wait_flag.wait(false);
          }, SchedulePriority::Default, false);
          jobs.push_back(std::move(shutdown_signal_task));
          jobs.back()->schedule(node_id);
        }
      }

      const auto worker_count = _workers.size();
      auto wait_loop_count = size_t{0};
      while (waiting_workers_counter < worker_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // We wait up to 3 seconds (tests might run on congested servers) per loop.
        if (wait_loop_count > 30) {
          continue;
        }
        ++wait_loop_count;
      }

      std::cout << "run " << wake_up_loop_count << std::endl;

      wait_flag.test_and_set();
      wait_flag.notify_all();

      if (waiting_workers_counter == worker_count) {
        break;
      }

      ++wake_up_loop_count;
    }
  }  

  wait_for_all_tasks();

  // All queues SHOULD be empty by now
  if (HYRISE_DEBUG) {
    for (auto& queue : _queues) {
      Assert(queue->empty(), "NodeQueueScheduler bug: queue wasn't empty even though all tasks finished");
    }
  }

  _active = false;

  for (auto& worker : _workers) {
    worker->join();
  }

  _workers = {};
  _queues = {};
  _task_counter = 0;
}

bool NodeQueueScheduler::active() const {
  return _active;
}

const std::vector<std::shared_ptr<TaskQueue>>& NodeQueueScheduler::queues() const {
  return _queues;
}

const std::vector<std::shared_ptr<Worker>>& NodeQueueScheduler::workers() const {
  return _workers;
}

void NodeQueueScheduler::schedule(std::shared_ptr<AbstractTask> task, NodeID preferred_node_id,
                                  SchedulePriority priority) {
  /**
   * Add task to the queue of the preferred node if it is ready for execution.
   */
  DebugAssert(_active, "Can't schedule more tasks after the NodeQueueScheduler was shut down");
  DebugAssert(task->is_scheduled(), "Don't call NodeQueueScheduler::schedule(), call schedule() on the task");

  const auto task_counter = _task_counter++;  // Atomically take snapshot of counter
  task->set_id(TaskID{task_counter});

  if (!task->is_ready()) {
    return;
  }

  const auto node_id_for_queue = determine_queue_id_for_task(task, preferred_node_id);
  DebugAssert((static_cast<size_t>(node_id_for_queue) < _queues.size()),
              "Node ID is not within range of available nodes.");
  _queues[node_id_for_queue]->push(task, priority);
}

NodeID NodeQueueScheduler::determine_queue_id_for_task(const std::shared_ptr<AbstractTask>& task,
                                                       const NodeID preferred_node_id) const {
  // Early out: no need to check for preferred node or other queues, if there is only a single node queue.
  if (_queue_count == 1) {
    return NodeID{0};
  }

  if (preferred_node_id != CURRENT_NODE_ID) {
    return preferred_node_id;
  }

  // If the current node is requested, try to obtain node from current worker.
  const auto& worker = Worker::get_this_thread_worker();
  if (worker) {
    return worker->queue()->node_id();
  }

  // Initial min values with Node 0.
  auto min_load_queue_id = NodeID{0};
  auto min_load = _queues[0]->estimate_load();

  // When the current load of node 0 is small, do not check other queues.
  if (min_load < _workers_per_node) {
    return NodeID{0};
  }

  for (auto queue_id = NodeID{1}; queue_id < _queue_count; ++queue_id) {
    const auto queue_load = _queues[queue_id]->estimate_load();
    if (queue_load < min_load) {
      min_load_queue_id = queue_id;
      min_load = queue_load;
    }
  }

  return min_load_queue_id;
}

void NodeQueueScheduler::_group_tasks(const std::vector<std::shared_ptr<AbstractTask>>& tasks) const {
  // Adds predecessor/successor relationships between tasks so that only NUM_GROUPS tasks can be executed in parallel.
  // The optimal value of NUM_GROUPS depends on the number of cores and the number of queries being executed
  // concurrently. The current value has been found with a divining rod.
  //
  // Approach: Skip all tasks that already have predecessors or successors, as adding relationships to these could
  // introduce cyclic dependencies. Again, this is far from perfect, but better than not grouping the tasks.

  auto round_robin_counter = 0;
  auto common_node_id = std::optional<NodeID>{};

  std::vector<std::shared_ptr<AbstractTask>> grouped_tasks(NUM_GROUPS);
  for (const auto& task : tasks) {
    if (!task->predecessors().empty() || !task->successors().empty()) {
      return;
    }

    if (common_node_id) {
      // This is not really a hard assertion. As the chain will likely be executed on the same Worker (see
      // Worker::execute_next), we would ignore all but the first node_id. At the time of writing, we did not do any
      // smart node assignment. This assertion is only here so that this behavior is understood if we ever assign NUMA
      // node ids.
      DebugAssert(task->node_id() == *common_node_id, "Expected all grouped tasks to have the same node_id");
    } else {
      common_node_id = task->node_id();
    }

    const auto group_id = round_robin_counter % NUM_GROUPS;
    const auto& first_task_in_group = grouped_tasks[group_id];
    if (first_task_in_group) {
      task->set_as_predecessor_of(first_task_in_group);
    }
    grouped_tasks[group_id] = task;
    ++round_robin_counter;
  }
}

}  // namespace hyrise
