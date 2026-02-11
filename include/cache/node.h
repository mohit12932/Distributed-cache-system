#pragma once

#include <string>
#include <chrono>

namespace dcs {
namespace cache {

/**
 * A node in the doubly linked list used by the LRU cache.
 * Each node stores a key-value pair and pointers to its neighbors.
 * Hand-rolled (no std::list) for O(1) move/detach operations.
 */
struct Node {
    std::string key;
    std::string value;
    Node* prev;
    Node* next;
    bool dirty;  // Marks node as modified (for write-back sync)
    std::chrono::steady_clock::time_point last_access;

    Node()
        : prev(nullptr)
        , next(nullptr)
        , dirty(false)
        , last_access(std::chrono::steady_clock::now()) {}

    Node(const std::string& k, const std::string& v)
        : key(k)
        , value(v)
        , prev(nullptr)
        , next(nullptr)
        , dirty(false)
        , last_access(std::chrono::steady_clock::now()) {}
};

/**
 * Intrusive doubly linked list.
 * Head sentinel = MRU side, Tail sentinel = LRU side.
 * All operations are O(1).
 */
class DoublyLinkedList {
public:
    DoublyLinkedList() {
        head_ = new Node();  // sentinel head
        tail_ = new Node();  // sentinel tail
        head_->next = tail_;
        tail_->prev = head_;
        size_ = 0;
    }

    ~DoublyLinkedList() {
        Node* curr = head_->next;
        while (curr != tail_) {
            Node* next = curr->next;
            delete curr;
            curr = next;
        }
        delete head_;
        delete tail_;
    }

    // Non-copyable
    DoublyLinkedList(const DoublyLinkedList&) = delete;
    DoublyLinkedList& operator=(const DoublyLinkedList&) = delete;

    // Move semantics
    DoublyLinkedList(DoublyLinkedList&& other) noexcept
        : head_(other.head_), tail_(other.tail_), size_(other.size_) {
        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.size_ = 0;
    }

    /** Push a node right after the head sentinel (MRU position). */
    void push_front(Node* node) {
        node->next = head_->next;
        node->prev = head_;
        head_->next->prev = node;
        head_->next = node;
        ++size_;
    }

    /** Remove and return the tail node (LRU position). Returns nullptr if empty. */
    Node* pop_back() {
        if (size_ == 0) return nullptr;
        Node* lru = tail_->prev;
        detach(lru);
        return lru;
    }

    /** Detach a node from its current position (does NOT delete it). */
    void detach(Node* node) {
        node->prev->next = node->next;
        node->next->prev = node->prev;
        node->prev = nullptr;
        node->next = nullptr;
        --size_;
    }

    /** Move an existing node to the MRU position (front). */
    void move_to_front(Node* node) {
        detach(node);
        push_front(node);
    }

    /** Peek at the LRU node without removing it. */
    Node* back() const {
        if (size_ == 0) return nullptr;
        return tail_->prev;
    }

    /** Peek at the MRU node. */
    Node* front() const {
        if (size_ == 0) return nullptr;
        return head_->next;
    }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    // Iterator support for traversal (LRU -> MRU)
    Node* head_sentinel() const { return head_; }
    Node* tail_sentinel() const { return tail_; }

private:
    Node* head_;  // sentinel
    Node* tail_;  // sentinel
    size_t size_;
};

}  // namespace cache
}  // namespace dcs
