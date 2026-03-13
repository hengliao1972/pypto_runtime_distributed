#include "runtime/linqu_scope.h"
#include "ring/linqu_task_ring.h"
#include <cassert>

namespace linqu {

void LinquScopeStack::init(int32_t max_depth, int32_t max_tasks) {
    max_depth_ = max_depth;
    scope_tasks_.resize(static_cast<size_t>(max_tasks), 0);
    scope_begins_.resize(static_cast<size_t>(max_depth), 0);
    scope_tasks_size_ = 0;
    stack_top_ = -1;
}

void LinquScopeStack::reset() {
    scope_tasks_size_ = 0;
    stack_top_ = -1;
}

void LinquScopeStack::scope_begin() {
    stack_top_++;
    assert(stack_top_ < max_depth_);
    scope_begins_[stack_top_] = scope_tasks_size_;
}

void LinquScopeStack::scope_end(
    LinquTaskDescriptor* get_desc(int32_t task_id, void* ctx), void* ctx) {
    assert(stack_top_ >= 0);
    int32_t begin = scope_begins_[stack_top_];
    for (int32_t i = begin; i < scope_tasks_size_; i++) {
        int32_t tid = scope_tasks_[i];
        LinquTaskDescriptor* desc = get_desc(tid, ctx);
        if (!desc->task_freed) {
            desc->ref_count++;
        }
    }
    scope_tasks_size_ = begin;
    stack_top_--;
}

void LinquScopeStack::add_task(int32_t task_id) {
    assert(scope_tasks_size_ < static_cast<int32_t>(scope_tasks_.size()));
    scope_tasks_[scope_tasks_size_++] = task_id;
}

int32_t LinquScopeStack::tasks_in_current_scope() const {
    if (stack_top_ < 0) return 0;
    return scope_tasks_size_ - scope_begins_[stack_top_];
}

}
