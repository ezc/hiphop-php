/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <compiler/statement/if_branch_statement.h>
#include <compiler/expression/constant_expression.h>

using namespace HPHP;

///////////////////////////////////////////////////////////////////////////////
// constructors/destructors

IfBranchStatement::IfBranchStatement
(STATEMENT_CONSTRUCTOR_PARAMETERS,
 ExpressionPtr condition, StatementPtr stmt)
  : Statement(STATEMENT_CONSTRUCTOR_PARAMETER_VALUES(IfBranchStatement)),
    m_condition(condition), m_stmt(stmt) {
}

StatementPtr IfBranchStatement::clone() {
  IfBranchStatementPtr stmt(new IfBranchStatement(*this));
  stmt->m_condition = Clone(m_condition);
  stmt->m_stmt = Clone(m_stmt);
  return stmt;
}

///////////////////////////////////////////////////////////////////////////////
// parser functions

///////////////////////////////////////////////////////////////////////////////
// static analysis functions

void IfBranchStatement::analyzeProgram(AnalysisResultPtr ar) {
  if (m_condition) m_condition->analyzeProgram(ar);
  if (m_stmt) m_stmt->analyzeProgram(ar);
}

ConstructPtr IfBranchStatement::getNthKid(int n) const {
  switch (n) {
    case 0:
      return m_condition;
    case 1:
      return m_stmt;
    default:
      assert(false);
      break;
  }
  return ConstructPtr();
}

int IfBranchStatement::getKidCount() const {
  return 2;
}

void IfBranchStatement::setNthKid(int n, ConstructPtr cp) {
  switch (n) {
    case 0:
      m_condition = boost::dynamic_pointer_cast<Expression>(cp);
      break;
    case 1:
      m_stmt = boost::dynamic_pointer_cast<Statement>(cp);
      break;
    default:
      assert(false);
      break;
  }
}

void IfBranchStatement::inferTypes(AnalysisResultPtr ar) {
  if (m_condition) m_condition->inferAndCheck(ar, Type::Boolean, false);
  if (m_stmt) m_stmt->inferTypes(ar);
}

///////////////////////////////////////////////////////////////////////////////
// code generation functions

void IfBranchStatement::outputPHP(CodeGenerator &cg, AnalysisResultPtr ar) {
  if (m_condition) {
    cg_printf("if (");
    m_condition->outputPHP(cg, ar);
    cg_printf(") ");
  } else {
    cg_printf(" ");
  }
  if (m_stmt) {
    m_stmt->outputPHP(cg, ar);
  } else {
    cg_printf("{}\n");
  }
}

void IfBranchStatement::outputCPPImpl(CodeGenerator &cg, AnalysisResultPtr ar) {
  not_reached();
}

int IfBranchStatement::outputCPPIfBranch(CodeGenerator &cg,
                                         AnalysisResultPtr ar) {
  int varId = -1;
  if (m_condition) {
    if (m_condition->preOutputCPP(cg, ar, 0)) {
      cg.wrapExpressionBegin();
      varId = cg.createNewLocalId(shared_from_this());
      m_condition->getType()->outputCPPDecl(cg, ar, getScope());
      cg_printf(" %s%d;\n", Option::TempPrefix, varId);

      cg_indentBegin("{\n");
      m_condition->preOutputCPP(cg, ar, 0);
      cg_printf("%s%d = (", Option::TempPrefix, varId);
      m_condition->outputCPP(cg, ar);
      cg_printf(");\n");
      m_condition->outputCPPEnd(cg, ar);
    }

    cg_printf("if (");
    if (varId >= 0) {
      cg_printf("%s%d", Option::TempPrefix, varId);
    } else {
      m_condition->outputCPP(cg, ar);
    }
    cg_printf(") ");
  }
  if (m_stmt) {
    cg_indentBegin("{\n");
    m_stmt->outputCPP(cg, ar);
    cg_indentEnd("}\n");
  } else {
    cg_printf("{}\n");
  }
  return varId >= 0 ? 1 : 0;
}
