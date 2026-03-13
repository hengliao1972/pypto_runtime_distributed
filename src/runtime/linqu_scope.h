#ifndef LINQU_RUNTIME_SCOPE_H
#define LINQU_RUNTIME_SCOPE_H

#include <cstdint>
#include <vector>

namespace linqu {

struct LinquTaskDescriptor;

struct LinquScopeStack {
    void init(int32_t max_depth, int32_t max_tasks);
    void reset();

    void scope_begin();
    void scope_end(LinquTaskDescriptor* get_desc_fn(int32_t task_id, void* ctx),
                   void* ctx);

    void add_task(int32_t task_id);

    int32_t depth() const { return stack_top_; }
    int32_t tasks_in_current_scope() const;

private:
    std::vector<int32_t> scope_tasks_;
    int32_t scope_tasks_size_ = 0;
    std::vector<int32_t> scope_begins_;
    int32_t stack_top_ = -1;
    int32_t max_depth_ = 0;
};

}

#endif
