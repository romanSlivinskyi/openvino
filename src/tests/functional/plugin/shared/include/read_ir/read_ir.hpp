// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "shared_test_classes/read_ir/read_ir.hpp"

namespace LayerTestsDefinitions {

TEST_P(ReadIRTest, ReadIR) {
    Run();
}

TEST_P(ReadIRTest, QueryNetwork) {
    QueryNetwork();
}

} // namespace LayerTestsDefinitions