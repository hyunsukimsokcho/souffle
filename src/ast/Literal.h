/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2014, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Literal.h
 *
 * Abstract class definitions for AST nodes
 *
 ***********************************************************************/

#pragma once

#include "ast/Node.h"

namespace souffle {

/**
 * Literal
 * e.g. atoms, binary relations, and negated atoms
 * in the body and head of a clause.
 */
class AstLiteral : public AstNode {
public:
    using AstNode::AstNode;

    AstLiteral* clone() const override = 0;
};

}  // end of namespace souffle