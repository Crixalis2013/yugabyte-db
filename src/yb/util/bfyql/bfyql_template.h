//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// This module defines a few templates to be used in file "bfyql.h". It defines the actual
// implementation for compilation and execution of a builtin call.
//--------------------------------------------------------------------------------------------------

#ifndef YB_UTIL_BFYQL_BFYQL_TEMPLATE_H_
#define YB_UTIL_BFYQL_BFYQL_TEMPLATE_H_

#include <vector>

#include "yb/client/client.h"
#include "yb/util/logging.h"
#include "yb/util/bfyql/gen_opcodes.h"
#include "yb/util/bfyql/gen_operator.h"
#include "yb/gutil/strings/substitute.h"

namespace yb {
namespace bfyql {

//--------------------------------------------------------------------------------------------------
// Find the builtin opcode, declaration, and return type for a builtin call.
// Inputs: Builtin function name and parameter types.
// Outputs: opcode and bfdecl.
// In/Out parameter: return_type
//   If return_type is given, check if it is compatible with the declaration.
//   If not, return_type is an output parameter whose value is the return type of the builtin.
Status FindOpcodeByType(const string& yql_name,
                        const std::vector<DataType>& actual_types,
                        BFOpcode *opcode,
                        const BFDecl **bfdecl,
                        DataType *return_type);

// The effect is the same as function "FindOpcodeByType()", but it takes arguments instead types.
// NOTE:
//   RTypePtr can be either a raw (*) or shared (const shared_ptr&) pointer.
//   PTypePtrCollection can be any standard collection of PType raw or shared pointer.
//     std::vector<PTypePtr>, std::list<PTypePtr>,  std::set<PTypePtr>, ...
template<typename PTypePtrCollection, typename RTypePtr>
Status FindOpcode(const string& yql_name,
                  const PTypePtrCollection& params,
                  BFOpcode *opcode,
                  const BFDecl **bfdecl,
                  RTypePtr result) {

  // Read argument types.
  std::vector<DataType> actual_types(params.size(), DataType::UNKNOWN_DATA);
  int pindex = 0;
  for (const auto& param : params) {
    actual_types[pindex] = param->yql_type_id();
    pindex++;
  }

  // Get the opcode and declaration.
  if (result == nullptr) {
    return FindOpcodeByType(yql_name, actual_types, opcode, bfdecl, nullptr);
  }

  // Get the opcode, declaration, and return type.
  DataType return_type = result->yql_type_id();
  RETURN_NOT_OK(FindOpcodeByType(yql_name, actual_types, opcode, bfdecl, &return_type));
  result->set_yql_type_id(return_type);

  return Status::OK();
}
} // namespace bfyql
} // namespace yb

#endif  // YB_UTIL_BFYQL_BFYQL_TEMPLATE_H_
