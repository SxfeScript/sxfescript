# Feeds tests/fixtures/lsp-semantic-tokens.jsonrpc to `sxn lsp --stdio` and
# checks the tokens that come back. A ctest of its own because the server reads
# a framed session on stdin, which add_test cannot redirect on its own.
#
# The fixture opens this document:
#
#   safe let mut a = 0;
#   bump(&mut a);
#   // mut here is a comment
#   const safe = 1;
#
# and the expected answer pins down all four judgements the server exists to
# make: `safe` is a keyword where it qualifies a declaration and a plain name
# where it does not, `mut` is a keyword after `let` and after `&`, the `&` is
# the borrow sigil, and a keyword inside a comment is just a word.

execute_process(
  COMMAND "${SXN}" lsp --stdio
  INPUT_FILE "${FIXTURE}"
  OUTPUT_VARIABLE output
  RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "sxn lsp exited with ${status}")
endif()

set(expected "\"data\":[0,0,4,0,0,0,9,3,0,0,1,5,1,1,0,0,1,3,0,0]")
string(FIND "${output}" "${expected}" found)
if(found EQUAL -1)
  message(FATAL_ERROR "expected ${expected}\nin: ${output}")
endif()
