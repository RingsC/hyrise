#include "node_queue_scheduler.hpp"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "abstract_task.hpp"
#include "hyrise.hpp"
#include "job_task.hpp"
#include "task_queue.hpp"
#include "worker.hpp"

#include "uid_allocator.hpp"
#include "utils/assert.hpp"

namespace hyrise {

NodeQueueScheduler::NodeQueueScheduler() {
  _worker_id_allocator = std::make_shared<UidAllocator>();
  _num_workers = Hyrise::get().topology.num_cpus();

  // Create more task groups than workers to distribute load equally in case of skewed task runtimes.
  _num_task_groups = _num_workers * 4;

  std::printf("Scheduler initialized with %lu cores and _num_task_groups of %lu\n", Hyrise::get().topology.num_cpus(), _num_task_groups);
}

NodeQueueScheduler::~NodeQueueScheduler() {
  if (HYRISE_DEBUG && _active) {
    // We cannot throw an exception because destructors are noexcept by default.
    std::cerr << "NodeQueueScheduler::finish() wasn't called prior to destroying it" << std::endl;
    std::exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
  }
}

void NodeQueueScheduler::begin() {
  DebugAssert(!_active, "Scheduler is already active");

  _workers.reserve(_num_workers);
  _queues.reserve(Hyrise::get().topology.nodes().size());

  for (auto node_id = NodeID{0}; node_id < Hyrise::get().topology.nodes().size(); node_id++) {
    auto queue = std::make_shared<TaskQueue>(node_id);

    _queues.emplace_back(queue);

    const auto& topology_node = Hyrise::get().topology.nodes()[node_id];

    for (const auto& topology_cpu : topology_node.cpus) {
      _workers.emplace_back(
          std::make_shared<Worker>(queue, WorkerID{_worker_id_allocator->allocate()}, topology_cpu.cpu_id));
    }
  }

  _active = true;

  for (auto& worker : _workers) {
    worker->start();
  }
}

void NodeQueueScheduler::wait_for_all_tasks() {
  while (true) {
    uint64_t num_finished_tasks = 0;
    for (auto& worker : _workers) {
      num_finished_tasks += worker->num_finished_tasks();
    }

    if (num_finished_tasks == _task_counter) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void NodeQueueScheduler::finish() {
  wait_for_all_tasks();

  // All queues SHOULD be empty by now
  if (HYRISE_DEBUG) {
    for (auto& queue : _queues) {
      Assert(queue->empty(), "NodeQueueScheduler bug: Queue wasn't empty even though all tasks finished");
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

  // Lookup node id for current worker.
  if (preferred_node_id == CURRENT_NODE_ID) {
    auto worker = Worker::get_this_thread_worker();
    if (worker) {
      preferred_node_id = worker->queue()->node_id();
    } else {
      // TODO(all): Actually, this should be ANY_NODE_ID, LIGHT_LOAD_NODE or something
      preferred_node_id = NodeID{0};
    }
  }

  DebugAssert(!(static_cast<size_t>(preferred_node_id) >= _queues.size()),
              "preferred_node_id is not within range of available nodes");

  auto queue = _queues[preferred_node_id];
  queue->push(task, static_cast<uint32_t>(priority));
}

std::optional<std::vector<std::shared_ptr<AbstractTask>>> NodeQueueScheduler::_group_tasks(const std::vector<std::shared_ptr<AbstractTask>>& tasks) const {
  constexpr auto MIN_JOBS_PER_GROUPED_TASK = 5;
  if (tasks.size() < (_num_task_groups * MIN_JOBS_PER_GROUPED_TASK)) {
    // If we do not create more than _num_task_groups tasks per worker, we consider grouping unneccessary.
    // If we don't have at least MIN_JOBS_PER_GROUPED_TASK jobs per grouped task, there is no reason to group.
    return std::nullopt;
  }

  const auto task_count_per_group = static_cast<size_t>(std::ceil(tasks.size() / _num_task_groups));
  // std::printf("task_count_per_group is %lu (tasks: %lu, _num_task_groups: %lu)\n", task_count_per_group, tasks.size(), _num_task_groups);
  auto new_grouped_tasks = std::vector<std::shared_ptr<AbstractTask>>{};
  new_grouped_tasks.reserve(tasks.size());  // reserve to largest possible size, when all jobs are dependent (see below).

  auto add_task_group_and_schedule = [&](std::shared_ptr<std::vector<std::shared_ptr<AbstractTask>>>& grouped_tasks) {
    if (grouped_tasks->empty()) {
      return;
    }

    // std::printf("Merging %lu tasks.\n", grouped_tasks->size());
    new_grouped_tasks.emplace_back(std::make_shared<JobTask>([grouped_tasks]() {
      // std::printf("now executing task with %lu subtasks.\n", grouped_tasks->size());
      // int i = 0;
      for (const auto& task : *grouped_tasks) {
        DebugAssert(std::dynamic_pointer_cast<JobTask>(task), "Requiring an executable JobTask.");
        const auto job_task = std::static_pointer_cast<JobTask>(task);
        job_task->now();
      }
    }));
    new_grouped_tasks.back()->schedule();
  };

  auto task_group = std::make_shared<std::vector<std::shared_ptr<AbstractTask>>>();
  task_group->reserve(task_count_per_group);

  auto common_node_id = std::optional<NodeID>{};
  auto task_counter = size_t{0};
  for (const auto& task : tasks) {
    ++task_counter;

    if (HYRISE_DEBUG) {
      if (common_node_id) {
        // This is not really a hard assertion. As the chain will likely be executed on the same Worker (see
        // Worker::execute_next), we would ignore all but the first node_id. At the time of writing, we did not do any
        // smart node assignment. This assertion is only here so that this behavior is understood if we ever assign NUMA
        // node ids.
        DebugAssert(task->node_id() == *common_node_id, "Expected all grouped tasks to have the same node_id");
      } else {
        common_node_id = task->node_id();
      }
    }

    // For immediately start the processing, we start the first jobs directly.
    if (task_counter <= _num_workers) {
      new_grouped_tasks.emplace_back(task);
      new_grouped_tasks.back()->schedule();
      continue;
    }

    if (!task->predecessors().empty() || !task->successors().empty()) {
      // Add grouped task for previously gathered tasks.
      add_task_group_and_schedule(task_group);

      // Add single task to main task queue and schedule it immediately.
      new_grouped_tasks.emplace_back(task);
      new_grouped_tasks.back()->schedule();

      // Start new task group.
      task_group = std::make_shared<std::vector<std::shared_ptr<AbstractTask>>>();
      task_group->reserve(task_count_per_group);
      
      continue;
    }

    if (task_group->size() >= task_count_per_group) {
      add_task_group_and_schedule(task_group);
      task_group = std::make_shared<std::vector<std::shared_ptr<AbstractTask>>>();
      task_group->reserve(task_count_per_group);
    }

    task_group->emplace_back(task);
  }

  // Add last task group and schedule.
  add_task_group_and_schedule(task_group);
  // std::printf("In: %lu \t Grouped tasks: %lu\n", tasks.size(), new_grouped_tasks.size());
  return new_grouped_tasks;
}

}  // namespace hyrise
