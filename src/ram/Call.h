/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2014, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Call.h
 *
 ***********************************************************************/

#pragma once

#include "ram/Node.h"
#include "ram/Statement.h"
#include "utility/StreamUtil.h"
#include <ostream>
#include <string>
#include <utility>

namespace souffle {

/**
 * @class RamCall
 * @brief Call a subroutine
 *
 * Calls a subroutine
 *
 * The following example shows how subroutine A is invoked
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * CALL A
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

class RamCall : public RamStatement {
public:
    RamCall(std::string name) : name(std::move(name)) {}

    /** @brief Get call name */
    const std::string& getName() const {
        return name;
    }

    RamCall* clone() const override {
        return new RamCall(name);
    }

protected:
    void print(std::ostream& os, int tabpos) const override {
        os << times(" ", tabpos) << "CALL " << name << std::endl;
    }

    bool equal(const RamNode& node) const override {
        const auto& other = static_cast<const RamCall&>(node);
        return name == other.name;
    }

    /** name of subroutine */
    std::string name;
};

}  // end of namespace souffle
