//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// This module defines the interface BFOperator, which is a wrapper around C++ function pointers.
//
// For each builtin function, one operator is generated and used to compile and execute its call.
// - During compilation, the signature BFDecl is used to type checking.
// - During execution, the Exec function is used.
//
// See the header of file "/util/bfyql/directory.h" for more general overall information.
//--------------------------------------------------------------------------------------------------

#ifndef YB_UTIL_BFYQL_BASE_OPERATOR_H_
#define YB_UTIL_BFYQL_BASE_OPERATOR_H_

#include <memory>

#include "yb/util/bfyql/directory.h"
#include "yb/util/bfyql/gen_opcodes.h"

namespace yb {
namespace bfyql {

class BFOperator {
 public:
  typedef std::shared_ptr<BFOperator> SharedPtr;
  typedef std::shared_ptr<const BFOperator> SharedPtrConst;

  BFOpcode opcode() const {
    return opcode_;
  }

  BFOpcode overloaded_opcode() const {
    return overloaded_opcode_;
  }

  const BFDecl *op_decl() const {
    return op_decl_;
  }

  const std::vector<DataType>& param_types() const {
    return op_decl_->param_types();
  }

  const DataType& return_type() const {
    return op_decl_->return_type();
  }

 protected:
  // BFOperators are constructed only for the operator table, and this construction can be called
  // only by its subclasses.
  BFOperator(BFOpcode opcode,
             BFOpcode overloaded_opcode,
             const BFDecl *op_decl)
      : opcode_(opcode),
        overloaded_opcode_(overloaded_opcode),
        op_decl_(op_decl) {
  }

  virtual ~BFOperator() {
  }

  // The opcode of this operator, and the original opcode that this operator is overloading.
  // Suppose Xyz() function is overloaded into 4 different versions, we'd have 4 different opcodes.
  // The overloaded_opcode value will help creating a chain to link all overloading functions.
  //   opcode_ = OP_XYZ_1 , overloaded_opcode_ =  OP_XYZ_1
  //   opcode_ = OP_XYZ_2 , overloaded_opcode_ =  OP_XYZ_1
  //   opcode_ = OP_XYZ_3 , overloaded_opcode_ =  OP_XYZ_2
  //   opcode_ = OP_XYZ_4 , overloaded_opcode_ =  OP_XYZ_3
  BFOpcode opcode_;
  BFOpcode overloaded_opcode_;

  // Operator declaration, an entry in buitin function directory.
  const BFDecl *op_decl_;
};

} // namespace bfyql
} // namespace yb

#endif  // YB_UTIL_BFYQL_BASE_OPERATOR_H_
