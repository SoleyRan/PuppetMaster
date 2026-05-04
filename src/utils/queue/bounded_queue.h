#ifndef BOUNDED_QUEUE_H
#define BOUNDED_QUEUE_H

#include <atomic>

#include "base_queue.h"
#include <time_stamp.hpp>
#include <common/namespace_macros.h>

PUPPET_MASTER_UTILS_NS_BEGIN

template <typename T>
class BoundedQueue : public BaseQueue
{
public:
    explicit BoundedQueue(size_t max_size) : max_size_(max_size) 
    {
        Reset();
    }
    
    BoundedQueue& operator=(const BoundedQueue& other) = delete;
    BoundedQueue(const BoundedQueue& other) = delete;

    ~BoundedQueue() 
    { 
        Destroy(); 
    }

    bool Enqueue(const void* element) override
    {
        return EnqueueInternal(static_cast<const T*>(element));
    }

    bool Dequeue(void* element, int64_t& time_diff) override
    {
        return DequeueInternal(static_cast<T*>(element), time_diff);
    }

public:
    void Clear() 
    {
        Destroy();
        Reset();
    }

    size_t Size() const
    { 
        return size_.load(); 
    }

    bool Empty() const
    { 
        return size_.load() == 0; 
    }
    
    bool Full() const
    {
        return max_size_ > 0 && size_.load() >= max_size_;
    }
    
    size_t GetMaxSize() const 
    { 
        return max_size_; 
    }

private:
    bool EnqueueInternal(const T* element)
    {
        auto enqueue_time = TimeStamp::MillisecondsSinceEpoch();
        if (max_size_ > 0) 
        {
            size_t current_size = size_.load();
            if (current_size >= max_size_) 
                return false;
        }
        
        auto node = new Node();
        node->data = *element;
        node->enqueue_time = enqueue_time;  // 记录入队时间
        Node* old_tail = tail_.load();

        while (true) 
        {
            if (tail_.compare_exchange_strong(old_tail, node)) 
            {
                old_tail->next = node;
                old_tail->release();
                size_.fetch_add(1);
                break;
            }
            
            // 在CAS失败后重新检查队列大小
            if (max_size_ > 0) 
            {
                size_t current_size = size_.load();
                if (current_size >= max_size_) 
                {
                    // 在竞争过程中队列已满，需要清理已创建的节点
                    delete node;
                    return false;
                }
            }
        }
        
        return true;
    }

    bool DequeueInternal(T* element, int64_t& time_diff) 
    {
        Node* old_head = head_.load();
        Node* head_next = nullptr;
        do 
        {
            head_next = old_head->next;
            if (head_next == nullptr)
                return false;
        } while (!head_.compare_exchange_strong(old_head, head_next));
        
        *element = head_next->data;
        
        auto dequeue_time = TimeStamp::MillisecondsSinceEpoch();
        time_diff = static_cast<int64_t>(dequeue_time) - static_cast<int64_t>(head_next->enqueue_time);

        size_.fetch_sub(1);
        old_head->release();
        return true;
    }

private:
    struct Node 
    {
        T data;
        uint64_t enqueue_time;  // 入队时间戳
        std::atomic<uint32_t> ref_count;
        Node* next = nullptr;
        
        Node() 
        { 
            ref_count.store(2);
            enqueue_time = TimeStamp::MillisecondsSinceEpoch();  // 默认初始化时间
        }
        
        void release() 
        {
            ref_count.fetch_sub(1);
            if (ref_count.load() == 0)
                delete this;
        }
    };

    void Reset() 
    {
        auto node = new Node();
        head_.store(node);
        tail_.store(node);
        size_.store(0);
    }

    void Destroy() 
    {
        auto ite = head_.load();
        Node* tmp = nullptr;
        while (ite != nullptr) 
        {
            tmp = ite->next;
            // 强制将 ref_count 设为 1，确保调用 release() 后能安全释放
            ite->ref_count.store(1);
            ite->release();  // ref_count 减到 0，触发 delete
            ite = tmp;
        }
    }

private:
    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
    std::atomic<size_t> size_;
    const size_t max_size_;  // 0 表示无限制
};

PUPPET_MASTER_UTILS_NS_END

#endif 