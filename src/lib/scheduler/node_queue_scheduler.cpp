#include "node_queue_scheduler.hpp"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "abstract_task.hpp"
#include "hyrise.hpp"
#include "task_queue.hpp"
#include "worker.hpp"

#include "uid_allocator.hpp"
#include "utils/assert.hpp"


namespace {

//constexpr auto MIN_LOAD_TO_DISTRIBUTE_TASKS = size_t{128};

}

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
  DebugAssert(!_active, "Scheduler is already active");

  _workers.reserve(Hyrise::get().topology.num_cpus());
  _queue_count = Hyrise::get().topology.nodes().size();
  _queues.reserve(_queue_count);

  for (auto node_id = NodeID{0}; node_id < Hyrise::get().topology.nodes().size(); node_id++) {
    auto queue = std::make_shared<TaskQueue>(node_id);

    _queues.emplace_back(queue);

    const auto& topology_node = Hyrise::get().topology.nodes()[node_id];

    for (const auto& topology_cpu : topology_node.cpus) {
      _workers.emplace_back(
          std::make_shared<Worker>(queue, WorkerID{_worker_id_allocator->allocate()}, topology_cpu.cpu_id));
    }
  }
  _worker_count = _workers.size();
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

  // Lookup node id for current worker.
  if (preferred_node_id == CURRENT_NODE_ID) {
    auto worker = Worker::get_this_thread_worker();
    if (worker) {
      //std::cout << "default path\n";
      preferred_node_id = worker->queue()->node_id();
/*    } else if (_queue_count > 1) {
      //std::stringstream ss;
      //ss << "new path. previously default to 0. now we check the lengths: ";
      //for (const auto& queue : _queues) {
      //  ss << queue->estimate_load() << " and ";
      //}
      
      auto min_load_queue_id = size_t{0};
      const auto load_queue_0 = _queues[0]->estimate_load();
      auto min_load = load_queue_0;

      if (min_load >= MIN_LOAD_TO_DISTRIBUTE_TASKS) {
        //autostd::vector<std::pair<NodeID, size_t>>(_queue_count);
        for (auto queue_id = size_t{1}; queue_id < _queue_count; ++queue_id) {
          const auto queue_load = _queues[queue_id]->estimate_load();
	  if (queue_load < min_load) {
            min_load_queue_id = queue_id;
	    min_load = queue_load;
	  }
        }

        //const auto min_queue = std::min_element(_queues.cbegin(), _queues.cend(), [](const auto& lhs, const auto& rhs) {
          //std::printf("loads: %lu & %lu\n", lhs->estimate_load(), lhs->estimate_load());
          //return lhs->estimate_load() < lhs->estimate_load();
        //});
        //std::printf("%lu",std::distance(_queues.cbegin(), min_queue));

        if (load_queue_0 >= MIN_LOAD_TO_DISTRIBUTE_TASKS) {
          preferred_node_id = NodeID{static_cast<uint32_t>(min_load_queue_id)};
          //std::printf("(%d)\n", static_cast<uint32_t>(preferred_node_id));
        } else {
          preferred_node_id = NodeID{0};
        }

        //ss << " and chose queue id: " << preferred_node_id << "\n";
        //std::cout << ss.str() << std::flush;
      } else {
        preferred_node_id = NodeID{0};
      }
*/
    } else {
      //std::printf("Else\n");
      preferred_node_id = NodeID{0};
    }
  }

  DebugAssert(!(static_cast<size_t>(preferred_node_id) >= _queues.size()),
              "preferred_node_id is not within range of available nodes");

  auto queue = _queues[preferred_node_id];
  queue->push(task, priority);
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
