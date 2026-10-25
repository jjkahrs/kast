# kast_plain_asset_refs(<var>): turns each asset ref in the project file text <var> back into its legacy plain form
# (docs/engine/asset-refs.html): a JSON {"id": ..., "path": ...} (inline or 2-space indented) into its path string, and
# drops the data-kast-id attributes of GUI markup. Tests that edit or match paths as text use it: the editor resolves a
# ref by its id first, so an edited path hint would be ignored.
function(kast_plain_asset_refs var)
  string(REGEX REPLACE "{[ \r\n]*\"id\": \"[0-9a-f-]+\",[ \r\n]*\"path\": (\"[^\"]*\")[ \r\n]*}" "\\1" text "${${var}}")
  string(REGEX REPLACE " data-kast-id=\"[^\"]*\"" "" text "${text}")
  set(${var} "${text}" PARENT_SCOPE)
endfunction()
