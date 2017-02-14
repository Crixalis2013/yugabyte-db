//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//--------------------------------------------------------------------------------------------------

#include "yb/util/bfyql/bfyql.h"

#include <functional>
#include <unordered_map>

#include "yb/util/bfyql/directory.h"

using std::function;
using std::vector;
using std::unordered_map;

namespace yb {
namespace bfyql {

inline bool IsCompatible(DataType left, DataType right) {
  return YQLType::IsImplicitlyConvertible(left, right);
}

//--------------------------------------------------------------------------------------------------
// HasExactSignature() is a predicate to check if the datatypes of actual parameters (arguments)
// and formal parameters (signature) are identical.
// NOTES:
//   PTypePtr can be a (shared_ptr<MyClass>) or a raw pointer (MyClass*)

static bool HasExactTypeSignature(const std::vector<DataType>& signature,
                                  const std::vector<DataType>& actual_types) {
  // Check parameter count.
  const int formal_count = signature.size();
  const int actual_count = actual_types.size();

  // Check for exact match.
  int index;
  for (index = 0; index < formal_count; index++) {
    // Check if the signature accept varargs which can be matched with the rest of arguments.
    if (signature[index] == DataType::TYPEARGS) {
      return true;
    }

    // Return false if one of the following is true.
    // - The number of arguments is less than the formal count.
    // - The datatype of an argument is not an exact match of the signature type.
    if (index >= actual_count || signature[index] != actual_types[index]) {
      return false;
    }
  }

  // Assert that the number of arguments is the same as the formal count.
  if (index < actual_count) {
    return false;
  }

  return true;
}

//--------------------------------------------------------------------------------------------------
// HasSimilarSignature() is a predicate to check if the datatypes of actual parameters (arguments)
// and formal parameters (signature) are similar.
//
// Similar is mainly used for integers vs floating point values.
//   INT8 is "similar" to INT64.
//   INT8 is NOT "similar" to DOUBLE.
//   FLOAT is "similar" to DOUBLE.
// This rule is to help resolve the overloading functions between integers and float point data.
//
// NOTES:
//   PTypePtr and RTypePtr can be either a (shared_ptr<MyClass>) or a raw pointer (MyClass*)

static bool HasSimilarTypeSignature(const std::vector<DataType>& signature,
                                    const std::vector<DataType>& actual_types) {
  const int formal_count = signature.size();
  const int actual_count = actual_types.size();

  // Check for exact match.
  int index;
  for (index = 0; index < formal_count; index++) {
    // Check if the signature accept varargs which can be matched with the rest of arguments.
    if (signature[index] == DataType::TYPEARGS) {
      return true;
    }

    // Return false if one of the following is true.
    // - The number of arguments is less than the formal count.
    // - The datatype of an argument is not an similar match of the signature type.
    if (index >= actual_count || !YQLType::IsSimilar(signature[index], actual_types[index])) {
      return false;
    }
  }

  // Assert that the number of arguments is the same as the formal count.
  if (index < actual_count) {
    return false;
  }

  return true;
}

//--------------------------------------------------------------------------------------------------
// HasCompatibleSignature() is a predicate to check if the arguments is convertible to the
// signature.
// Examples:
// - INT16 is convertible to DOUBLE, so passing an int16 value to func(DOUBLE) is valid.
// - In CQL, DOUBLE is not convertible to INT16, so passing a double value to func(INT26) is
// invalid. This case would become valid if YugaByte eases this conversion restriction.
//
// NOTES:
//   PTypePtr and RTypePtr can be either a (shared_ptr<MyClass>) or a raw pointer (MyClass*).

static bool HasCompatibleTypeSignature(const std::vector<DataType>& signature,
                                       const std::vector<DataType>& actual_types) {

  const int formal_count = signature.size();
  const int actual_count = actual_types.size();

  // Check for compatible match.
  int index;
  for (index = 0; index < formal_count; index++) {
    // Check if the signature accept varargs which can be matched with the rest of arguments.
    if (signature[index] == DataType::TYPEARGS) {
      return true;
    }

    // Return false if one of the following is true.
    // - The number of arguments is less than the formal count.
    // - The datatype of an argument is not a compatible match of the signature type.
    if (index >= actual_count || !IsCompatible(signature[index], actual_types[index])) {
      return false;
    }
  }

  // Assert that the number of arguments is the same as the formal count.
  if (index < actual_count) {
    return false;
  }

  return true;
}

//--------------------------------------------------------------------------------------------------
// Searches all overloading versions of a function and finds exactly one function specification
// whose signature matches with the datatypes of the arguments.
// NOTES:
//   "compare_signature" is a predicate to compare datatypes of formal and actual parameters.
//   PTypePtr and RTypePtr can be either a (shared_ptr<MyClass>) or a raw pointer (MyClass*)
static Status FindMatch(
    function<bool(const std::vector<DataType>&, const std::vector<DataType>&)> compare_signature,
    BFOpcode max_opcode,
    const std::vector<DataType>& actual_types,
    BFOpcode *found_opcode,
    const BFDecl **found_decl,
    DataType *return_type) {

  // Find a compatible operator, and raise error if there's more than one match.
  const BFOperator *compatible_operator = nullptr;
  while (true) {
    const BFOperator *bf_operator = kBFOperators[static_cast<int>(max_opcode)].get();
    DCHECK(max_opcode == bf_operator->opcode());

    // Check if each parameter has compatible type match.
    if (compare_signature(bf_operator->param_types(), actual_types)) {
      // Found a compatible match. Make sure that it is the only match.
      if (compatible_operator != nullptr) {
        return STATUS(InvalidArgument, "Found too many matched builtin functions");
      }
      compatible_operator = bf_operator;
    }

    // Break the loop if we have processed all operators in the overloading chain.
    if (max_opcode == bf_operator->overloaded_opcode()) {
      break;
    }

    // Jump to the next overloading opcode.
    max_opcode = bf_operator->overloaded_opcode();
  }

  // Returns error if no match is found.
  if (compatible_operator == nullptr) {
    return STATUS(NotFound, "No match is found for builtin with the given arguments.");
  }

  // Returns error if the return type is not compatible.
  if (return_type != nullptr) {
    if (YQLType::IsUnknown(*return_type)) {
      *return_type = compatible_operator->return_type();
    } else if (!IsCompatible(*return_type, compatible_operator->return_type())) {
      return STATUS(InvalidArgument, "Return type is not matched");
    }
  }

  *found_opcode = compatible_operator->opcode();
  *found_decl = compatible_operator->op_decl();
  return Status::OK();
}

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
                        DataType *return_type) {

  auto entry = kBFYqlName2Opcode.find(yql_name);
  if (entry == kBFYqlName2Opcode.end()) {
    VLOG(3) << strings::Substitute("Builtin function $0 is not found", yql_name);
    return STATUS(NotFound, strings::Substitute("Builtin function $0 is not found", yql_name));
  }

  // Seek the correct overload functions in the following order:
  // - Find the exact signature match.
  //   Example:
  //   . Overload #1: FuncX(int8_t i) would be used for the call FuncX(int8_t(7)).
  //   . Overload #2: FuncX(int16_t i) would be used for the call FuncX(int16_t(7)).
  //
  // - For "cast" operator, if exact match is not found, return error. For all other operators,
  //   continue to the next steps.
  //
  // - Find the similar signature match.
  //   Example:
  //   . Overload #2: FuncY(int64_t i) would be used for FuncY(int8_t(7)).
  //       int64_t and int8_t are both integer values.
  //   . Overload #1: FuncY(double d) would be used for FuncY(float(7)).
  //       double & float are both floating values.
  //
  // - Find the compatible match. Signatures are of convertible datatypes.
  Status s = FindMatch(HasExactTypeSignature, entry->second, actual_types,
                       opcode, bfdecl, return_type);
  VLOG(3) << "Find exact match for function call " << yql_name << "(): " << s.ToString();

  if (yql_name != kCastFuncName && s.IsNotFound()) {
    s = FindMatch(HasSimilarTypeSignature, entry->second, actual_types,
                  opcode, bfdecl, return_type);
    VLOG(3) << "Find similar match for function call " << yql_name << "(): " << s.ToString();

    if (s.IsNotFound()) {
      s = FindMatch(HasCompatibleTypeSignature, entry->second, actual_types,
                    opcode, bfdecl, return_type);
      VLOG(3) << "Find compatible match for function call " << yql_name << "(): " << s.ToString();
    }
  }

  return s;
}

} // namespace bfyql
} // namespace yb
