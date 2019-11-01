#include "queue.h"
#include "timer.h"
#include <iostream>
#include <vector>
#include <algorithm>
#include <string>
#include <thread>
#include <memory>

template<typename TQueue>
void process(const QueueConfig& config, const int task_num = 4 << 20) {
	const auto thread_sizes = std::vector<size_t>{1U, 2U, 4U};

	for (size_t cnt = 0; cnt < thread_sizes.size(); cnt++) {
		for (size_t cnt_2 = 0; cnt_2 < thread_sizes.size(); cnt_2++) {
			std::shared_ptr<TQueue> q = std::make_shared<TQueue>(config);

			std::vector<std::thread> consumers, producers;

			const size_t consumer_size = thread_sizes[cnt], producer_size = thread_sizes[cnt_2];
			std::cout << "consumer_size: " << consumer_size << std::endl;
			std::cout << "producer_size: " << producer_size << std::endl;
			std::vector<int> results(consumer_size);
			int producers_time = 0, consumers_time = 0;

			for (size_t i = 0; i < producer_size; i++) {
				
				producers.emplace_back([task_num](const auto task_queue_ptr, int& producers_time) {
					Timer t1;

					task_queue_ptr->one_producer_started();
					for (int i = 0; i < task_num; i++) {
						task_queue_ptr->push(1);
					}
					task_queue_ptr->one_producer_finished();

					producers_time += t1.Passed();
				}, q, std::ref(producers_time));
				
			}
			
			for (size_t i = 0; i < consumer_size; i++) {
				consumers.emplace_back([](const auto task_queue_ptr, int& result, int& consumers_time) {
					Timer t2;
					int sum = 0;
					
					while (true) {
						if (uint8_t val = 0; task_queue_ptr->pop(val)) {
							sum += val;
						} else if (task_queue_ptr->is_producing_finished()) {
							break;
						}
					}

					result = sum;
					consumers_time += t2.Passed();
				}, q, std::ref(results[i]), std::ref(consumers_time));

				
			}

			for (size_t i = 0; i < consumer_size; i++) {
				consumers[i].join(); 
			}

			for (size_t i = 0; i < producer_size; i++) {
				producers[i].join();
			}

			for (auto e : results) {	
				std::cout << e << ' ';
			}
			std::cout << "\n";

			std::cout << "producers_time: " << producers_time << "ms" << std::endl;
			std::cout << "consumers_time: " << consumers_time << "ms" << std::endl;
		}
	}

	
}

int main() {
	// std::cout << "Dynamic lock queue" << std::endl;
	// process<DynamicLockQueue>({}, 4 << 8);

	// std::cout << "Fixed lock queue" << std::endl;
	// process<FixedSizeLockQueue<2>>({}, 4 << 14);
	// process<FixedSizeLockQueue<4>>({}, 4 << 16);
	// process<FixedSizeLockQueue<16>>({}, 4 << 18);

	// std::cout << "Fixed no-lock queue" << std::endl;
	// process<FixedSizeNoLockQueue<1>>({}, 4 << 16);
	// process<FixedSizeNoLockQueue<2>>({}, 4 << 18);
	// process<FixedSizeNoLockQueue<4>>({}, 4 << 20);
	// process<FixedSizeNoLockQueue<16>>({}, 4 << 20);

	std::cout << "Dynamic no-lock queue" << std::endl;
	process<DynamicNoLockQueue>({});

	 return 0;
}