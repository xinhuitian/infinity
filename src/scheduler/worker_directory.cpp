#include "worker_directory.h"
#include "common/utility/threadutil.h"
#include "common/utility/infinity_assert.h"

#include <vector>

namespace infinity{

WorkerDirectory::~WorkerDirectory(){
	for(auto *worker : workers_){
		worker->~Worker();
		delete worker;
	}	
}

void WorkerDirectory::CreateWorkers(std::uint16_t max_cores){
	cpu_topology_.build(max_cores);
	max_cores_ = cpu_topology_.size();
	for(auto worker_id = 0U; worker_id < max_cores_; ++worker_id) {
		const auto cpu_id = cpu_topology_[worker_id];
		workers_[worker_id] = new Worker(worker_id, cpu_id);
	}	
}

void WorkerDirectory::Start(){
	std::vector<std::thread> worker_threads(max_cores_);
	for(auto worker_id = 0U; worker_id < max_cores_; ++worker_id){
        worker_threads[worker_id] = std::thread([this, worker_id] 
            { this->workers_[worker_id]->Execute(); });
		ThreadUtil::pin(worker_threads[worker_id], workers_[worker_id]->CPUID());
	}
    for (auto &worker_thread : worker_threads){
        worker_thread.join();
    }	
}

Worker* WorkerDirectory::GetLeastLoadedWorker() const {
	return nullptr;
}

Worker* WorkerDirectory::GetMostLoadedWorker() const {
	return nullptr;
}

Worker* WorkerDirectory::GetWorker(std::uint16_t worker_id) const {
	ExecutorAssert(worker_id < max_cores_, "worker_id overflow")
	return workers_[worker_id];
}

}