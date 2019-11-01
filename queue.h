	 #pragma once

#include <queue>
#include <atomic>
#include <mutex>
#include <thread>
#include <array>
#include <iostream>

struct QueueConfig {
	int retries = 2;
};

class IQueue {
public:
	IQueue(const QueueConfig& config)
		: config(config)
	{}

	// Записывает элемент в очередь.
	// Гсли очередь фиксированного размер и заполнена,
	// поток повисает внутри функции пока не освободится место
	virtual void push(uint8_t val) = 0;
	// Если очередь пуста, ждем 1 мс записи в очередь.
	// Если очередь не пуста, помещает значение головы в val,
	// удаляет голову и возвращает true.
	// Если очередь по прежнему пуста, возвращаем false
	virtual bool pop(uint8_t& val) = 0;

	void one_producer_started() {
		thread_count.fetch_add(1);
	}

	void one_producer_finished() {
		thread_count.fetch_sub(1);
	}

	bool is_producing_finished() {
		return thread_count.load() == 0;
	}
protected:
	QueueConfig config;
private:
	std::atomic<int> thread_count;
};

class DynamicLockQueue : public IQueue {
public:
	DynamicLockQueue(const QueueConfig& config)
		: IQueue(config)
	{}

	void push(uint8_t val) override {
		std::lock_guard<std::mutex> guard(m);
		q.push(val);
	}

	bool pop(uint8_t& val) override {
		for (int retries = 0; retries < config.retries; retries++) {
			{
				std::lock_guard<std::mutex> guard(m);
				if (!q.empty()) {
					val = q.front();
					q.pop();
					return true; 
				}
			}
			if (retries != config.retries - 1) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		return false;
	}

private:
	std::queue<uint8_t> q;
	std::mutex m;
};


template<size_t capacity>
class FixedSizeLockQueue : public IQueue {
public:
	FixedSizeLockQueue(const QueueConfig& config)
		: IQueue(config)
		, ql(0)
		, qr(0)
		, is_full(false)
	{

	}

	void push(uint8_t val) override {
		std::unique_lock<std::mutex> lock(m);

		while (is_full) {
			full_cond_var.wait(lock);  //когда поток пришел в while он отпускает мьютекс и потом, когда дождался нотифай, то лочит обратно
		}

		data[qr++] = val;
		qr %= capacity;
		is_full = full();
	}

	bool pop(uint8_t& val) override {
		for (int retries = 0; retries < config.retries; retries++) {
			{
				std::unique_lock<std::mutex> lock(m);

				if (!empty()) {
					val = data[ql++];
					ql %= capacity;

					if (is_full) {
						is_full = false;
						full_cond_var.notify_all();
					}
					return true;
				}
			}
			if (retries != config.retries - 1) { 
				std::this_thread::sleep_for(std::chrono::nanoseconds(500));
			}
		}
		return false;
	}

	size_t size() const {
		if (ql <= qr)
			return qr - ql;
		size_t res = capacity - (ql - qr); 
		return res;
	}

	bool empty() const {
		return size() == 0;
	}

	bool full() const {
		return size() == capacity - 1;
	}
private:
	std::array<uint8_t, capacity> data; // на самом деле размер на один меньше, чем передаем в темплейт
	size_t ql, qr;

	std::mutex m;
	
	std::condition_variable full_cond_var;
	bool is_full;
};

template<size_t capacity>
class FixedSizeNoLockQueue : public IQueue {
public:
	FixedSizeNoLockQueue(const QueueConfig& config)
		: IQueue(config)
		, ql(0)
		, qr(0)
	{

	}

	void push(uint8_t val) override {
		while (true) {
			size_t pos = qr.load();
			DataWithRef back = data[pos % capacity].load(std::memory_order_acquire); //чтобы никакие записи не были раньше чем наше чтение

			if (pos != qr.load(std::memory_order_acquire)) continue; // вдруг другой уже запушил
			if (pos == ql.load(std::memory_order_acquire) + capacity) { //очередь полная
				{
					// scope to recieve notification
					std::unique_lock<std::mutex> lock(m); 
					full_cond_var.wait(lock); // wait_until timeout_time
				}
				continue;
			}
			if (back.data == 0) {													
				if (data[pos % capacity].compare_exchange_weak(back, DataWithRef{val, back.ref + 1}, std::memory_order_release)) {
					qr.compare_exchange_strong(pos, pos + 1, std::memory_order_release);
					break;
				}
			} else {
				qr.compare_exchange_strong(pos, pos + 1, std::memory_order_release); // помогаем другому сделать ++
			}
		}
	}

	bool pop(uint8_t& val) override {
		for (int retries = 0; retries < config.retries; retries++) {
			
			while (true) {
				size_t pos = ql.load();
				DataWithRef front = data[pos % capacity].load(std::memory_order_acquire);
				if (pos != ql.load(std::memory_order_acquire)) continue;

				if (pos == qr.load(std::memory_order_acquire)) { //очередь пуста
					full_cond_var.notify_one(); 
					break; 
				}

				if (front.data != 0) {									
					if (data[pos % capacity].compare_exchange_weak(front, DataWithRef{0, front.ref + 1}, std::memory_order_release)) {
						ql.compare_exchange_strong(pos, pos + 1, std::memory_order_release);

						val = front.data;
				
						// scope to notify about free cells
						{
							full_cond_var.notify_one(); 
						}
						return true;
					}
				} else {
					ql.compare_exchange_strong(pos, pos + 1, std::memory_order_release);
				}
			}
		
			if (retries != config.retries - 1) { 
				std::this_thread::sleep_for(std::chrono::nanoseconds(500));
			}
		}
		return false;
	}
private:
	struct DataWithRef {
		uint8_t data;
		int ref;
	};

	std::array<std::atomic<DataWithRef>, capacity> data;
	std::atomic<size_t> ql, qr;

	std::mutex m;

	std::condition_variable full_cond_var;
};


class DynamicNoLockQueue : public IQueue {
public:
	DynamicNoLockQueue(const QueueConfig& config)
		: IQueue(config)
	{
		auto dummy = new node();
		head.store(tagged_ptr<node>(dummy, 0));
		tail.store(tagged_ptr<node>(dummy, 0));
		std::cout << head.is_lock_free();
	}

	void push(uint8_t val) override {
		node* newNode = new node();
		newNode->data = val;
		newNode->next.store(tagged_ptr<node>());

		while (true) {
			tagged_ptr<node> last = tail.load();
			tagged_ptr<node> next = last->next;

			if (last != tail.load()) continue;
			if (next.ptr == nullptr) {
				// мы в конце очереди
				if (tail.load()->next.compare_exchange_weak(next, tagged_ptr<node>(newNode, next.ref + 1))) { 
					tail.compare_exchange_strong(last, tagged_ptr<node>(newNode, last.ref + 1));
					break;
				}
			} else {
				//мы не в конце, поэтому продвигаем тейл дальше 
				tail.compare_exchange_strong(last, tagged_ptr<node>(next.ptr, last.ref + 1)); 
			}
		}
	}

	bool pop(uint8_t& val) override {
		for (int retries = 0; retries < config.retries; retries++) {
			
			while (true) {
				tagged_ptr<node> first = head.load();
				tagged_ptr<node> last = tail.load();
				tagged_ptr<node> next = first->next.load();
				
				if (first == head.load()) {
					// пустая очередь
					if (first.ptr == last.ptr) { 
						if (next.ptr == nullptr) {
							// пустая очередь
							break;
						}
						// tail.next != null, продвигаем 
						tail.compare_exchange_strong(last, tagged_ptr<node>(next.ptr, last.ref + 1)); 
					} else { //
						val = next->data;
						// первый двигаем на next
						if (head.compare_exchange_weak(first, tagged_ptr<node>(next.ptr, first.ref + 1))) {
							// controlled_mem_leak(head); //проблема чтения из мусорной памяти
							return true;
						}
					}
				}
			}
		
			if (retries != config.retries - 1) {
				std::this_thread::sleep_for(std::chrono::nanoseconds(500));
			}
		}
		return false;
	}
private:
	template<typename T> 
	struct tagged_ptr {
		T* ptr;
		int ref;

		tagged_ptr() 
			: ptr(nullptr)
			, ref(0) 
		{}

	    tagged_ptr(T* p)
	    	: ptr(p)
	    	, ref(0) 
	    {}

	    tagged_ptr(T * p, unsigned int n)
	    	: ptr(p)
	    	, ref(n) 
	    {}

	    T* operator->() const { 
	    	return ptr; 
	    }

	    bool operator == (const tagged_ptr& t) const {
	    	return ptr == t.ptr && ref == t.ref;
	    }

	    bool operator != (const tagged_ptr& t) const {
	    	return ptr != t.ptr || ref != t.ref;
	    }
	};

	struct node {
		uint8_t data;
		std::atomic<tagged_ptr<node>> next;
		
		node()
			: next(0)
			, data(0)
		{}
	};

	std::atomic<tagged_ptr<node>> head;
	std::atomic<tagged_ptr<node>> tail;
};
