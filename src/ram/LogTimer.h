/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2017, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file LogTimer.h
 *
 ***********************************************************************/

#pragma once

#include "ram/AbstractLog.h"
#include "ram/Node.h"
#include "ram/NodeMapper.h"
#include "ram/Statement.h"
#include "utility/MiscUtil.h"
#include "utility/StreamUtil.h"
#include "utility/StringUtil.h"
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace souffle {

/**
 * @class RamLogTimer
 * @brief Execution time logger for a statement
 *
 * Logs the execution time of a statement. Before and after
 * the execution of the logging statement the wall-clock time
 * is taken to compute the time duration for the statement.
 * Duration and logging message is printed after the execution
 * of the statement.
 *
 * For example:
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * START_TIMER "@runtime\;"
 *   BEGIN_STRATUM 0
 *     ...
 *   ...
 * END_TIMER
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
class RamLogTimer : public RamStatement, public RamAbstractLog {
public:
    RamLogTimer(std::unique_ptr<RamStatement> stmt, std::string msg)
            : RamAbstractLog(std::move(stmt), std::move(msg)) {}

    std::vector<const RamNode*> getChildNodes() const override {
        return RamAbstractLog::getChildNodes();
    }

    RamLogTimer* clone() const override {
        return new RamLogTimer(souffle::clone(statement), message);
    }

    void apply(const RamNodeMapper& map) override {
        RamAbstractLog::apply(map);
    }

protected:
    void print(std::ostream& os, int tabpos) const override {
        os << times(" ", tabpos) << "START_TIMER \"" << stringify(message) << "\"" << std::endl;
        RamStatement::print(statement.get(), os, tabpos + 1);
        os << times(" ", tabpos) << "END_TIMER" << std::endl;
    }
};

}  // end of namespace souffle
