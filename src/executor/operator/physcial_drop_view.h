//
// Created by JinHai on 2022/7/28.
//

#pragma once

#include "executor/physical_operator.h"

namespace infinity {
class PhysicalDropView : public PhysicalOperator {
public:
    explicit PhysicalDropView(uint64_t id)
        : PhysicalOperator(PhysicalOperatorType::kDropView, nullptr, nullptr, id) {}
    ~PhysicalDropView() = default;
};

}
