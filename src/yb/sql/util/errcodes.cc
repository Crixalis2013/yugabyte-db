//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Convert internal error code into readable texts. This text doesn't have to be English, and this
// file can be translated into any languages that YugaByte supports.
//--------------------------------------------------------------------------------------------------

#include "yb/sql/util/errcodes.h"

#include <unordered_map>

#include "yb/util/logging.h"

namespace yb {
namespace sql {

using std::unordered_map;

const unordered_map<int64_t, const char*> kYbSqlErrorMessage {
  // Implementation related errors [-1, -50).
  { static_cast<int64_t>(ErrorCode::FAILURE), "" },
  { static_cast<int64_t>(ErrorCode::SQL_STATEMENT_INVALID), "Invalid SQL Statement" },
  { static_cast<int64_t>(ErrorCode::CQL_STATEMENT_INVALID), "Invalid CQL Statement" },
  { static_cast<int64_t>(ErrorCode::FEATURE_NOT_YET_IMPLEMENTED), "Feature Not Yet Implemented" },
  { static_cast<int64_t>(ErrorCode::FEATURE_NOT_SUPPORTED), "Feature Not Supported" },

  //------------------------------------------------------------------------------------------------
  // Lexical errors [-50, -100).
  { static_cast<int64_t>(ErrorCode::LEXICAL_ERROR), "Lexical Error" },
  { static_cast<int64_t>(ErrorCode::CHARACTER_NOT_IN_REPERTOIRE), "Character Not in Repertoire" },
  { static_cast<int64_t>(ErrorCode::INVALID_ESCAPE_SEQUENCE), "Invalid Escape Sequence" },
  { static_cast<int64_t>(ErrorCode::NAME_TOO_LONG), "Name Too Long" },
  { static_cast<int64_t>(ErrorCode::NONSTANDARD_USE_OF_ESCAPE_CHARACTER),
      "Nonstandard Use of Escape Character" },

  //------------------------------------------------------------------------------------------------
  // Syntax errors [-100, -200).
  { static_cast<int64_t>(ErrorCode::SYNTAX_ERROR), "Syntax Error" },
  { static_cast<int64_t>(ErrorCode::INVALID_PARAMETER_VALUE), "Invalid Parameter Value" },

  //------------------------------------------------------------------------------------------------
  // Semantic errors [-200, -300).
  { static_cast<int64_t>(ErrorCode::SEM_ERROR), "Semantic Error" },
  { static_cast<int64_t>(ErrorCode::DATATYPE_MISMATCH), "Datatype Mismatch" },
  { static_cast<int64_t>(ErrorCode::DUPLICATE_TABLE), "Duplicate Table" },
  { static_cast<int64_t>(ErrorCode::UNDEFINED_COLUMN), "Undefined Column" },
  { static_cast<int64_t>(ErrorCode::DUPLICATE_COLUMN), "Duplicate Column" },
  { static_cast<int64_t>(ErrorCode::MISSING_PRIMARY_KEY), "Missing Primary Key" },
  { static_cast<int64_t>(ErrorCode::INVALID_PRIMARY_COLUMN_TYPE),
      "Invalid Primary Key Column Datatype" },
  { static_cast<int64_t>(ErrorCode::MISSING_ARGUMENT_FOR_PRIMARY_KEY),
      "Missing Argument for Primary Key" },
  { static_cast<int64_t>(ErrorCode::NULL_ARGUMENT_FOR_PRIMARY_KEY),
      "Null Argument for Primary Key" },
  { static_cast<int64_t>(ErrorCode::INCOMPARABLE_DATATYPES), "Incomparable Datatypes" },
  { static_cast<int64_t>(ErrorCode::INVALID_TABLE_PROPERTY), "Invalid Table Property" },
  { static_cast<int64_t>(ErrorCode::DUPLICATE_TABLE_PROPERTY), "Duplicate Table Property" },
  { static_cast<int64_t>(ErrorCode::INVALID_DATATYPE), "Invalid Datatype" },
  { static_cast<int64_t>(ErrorCode::SYSTEM_NAMESPACE_READONLY), "system namespace is read-only" },
  { static_cast<int64_t>(ErrorCode::INVALID_FUNCTION_CALL), "Invalid Function Call" },

  //------------------------------------------------------------------------------------------------
  // Execution errors [-300, x).
  { static_cast<int64_t>(ErrorCode::EXEC_ERROR), "Execution Error" },
  { static_cast<int64_t>(ErrorCode::TABLE_NOT_FOUND), "Table Not Found" },
  { static_cast<int64_t>(ErrorCode::INVALID_TABLE_DEFINITION), "Invalid Table Definition" },
  { static_cast<int64_t>(ErrorCode::WRONG_METADATA_VERSION), "Wrong Metadata Version" },
  { static_cast<int64_t>(ErrorCode::INVALID_ARGUMENTS), "Invalid Arguments" },
  { static_cast<int64_t>(ErrorCode::TOO_FEW_ARGUMENTS), "Too Few Arguments" },
  { static_cast<int64_t>(ErrorCode::TOO_MANY_ARGUMENTS), "Too Many Arguments" },
  { static_cast<int64_t>(ErrorCode::KEYSPACE_ALREADY_EXISTS), "Keyspace Already Exists" },
  { static_cast<int64_t>(ErrorCode::KEYSPACE_NOT_FOUND), "Keyspace Not Found" },
  { static_cast<int64_t>(ErrorCode::TABLET_NOT_FOUND), "Tablet Not Found" },

  //------------------------------------------------------------------------------------------------
  { static_cast<int64_t>(ErrorCode::SUCCESS), "Success" },

  //------------------------------------------------------------------------------------------------
  // Warning. Start with 100.
  { static_cast<int64_t>(ErrorCode::NOTFOUND), "Not Found" },
};

ErrorCode GetErrorCode(const Status& s) {
  return s.IsSqlError() ? static_cast<ErrorCode>(s.sql_error_code()) : ErrorCode::FAILURE;
}

const char *ErrorText(ErrorCode error_code) {
  return kYbSqlErrorMessage.at(static_cast<int64_t>(error_code));
}

}  // namespace sql
}  // namespace yb
