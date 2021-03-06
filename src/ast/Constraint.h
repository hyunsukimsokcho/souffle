/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2014, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Constraint.h
 *
 * Defines an abstract class for constraints
 *
 ***********************************************************************/

#pragma once

#include "ast/Literal.h"

namespace souffle {

/**
 * @class AstConstraint
 * @brief Abstract class for AST constraints
 */
class AstConstraint : public AstLiteral {
public:
    using AstLiteral::AstLiteral;

    AstConstraint* clone() const override = 0;
};

}  // end of namespace souffle
