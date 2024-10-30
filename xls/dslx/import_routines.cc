// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/dslx/import_routines.h"

#include <filesystem>  // NOLINT
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "xls/common/config/xls_config.h"
#include "xls/common/file/get_runfile_path.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/parser.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/frontend/scanner.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/type_system/type_info.h"

namespace xls::dslx {

// Data structure holding a path to a DSLX source file.
struct DslxPath {
  // The path to the source file as passed to the tool or import statement.
  std::filesystem::path source_path;
  // Path to the source file in the filesystem. This may include gobbledy gook
  // (runtimes path) build system path for embedded files in build targets.
  std::filesystem::path filesystem_path;
};

static absl::StatusOr<DslxPath> FindExistingPath(
    const ImportTokens& subject, const std::filesystem::path& stdlib_path,
    absl::Span<const std::filesystem::path> additional_search_paths,
    const Span& import_span, const FileTable& file_table,
    VirtualizableFilesystem& vfs) {
  absl::Span<std::string const> pieces = subject.pieces();
  std::optional<std::string> subject_parent_path;
  const absl::flat_hash_set<std::string> builtins = {
      "std", "apfloat", "float32", "float64", "bfloat16"};

  // Initialize subject and parent subject path.
  std::filesystem::path subject_path;
  if (pieces.size() == 1 && builtins.contains(pieces[0])) {
    subject_path = stdlib_path / absl::StrCat(pieces[0], ".x");
  } else {
    subject_path = absl::StrJoin(pieces, "/") + ".x";
    subject_parent_path = absl::StrJoin(pieces.subspan(1), "/") + ".x";
  }

  std::vector<std::string> attempted;

  // Helper that tries to see if "path" is present relative to "base".
  auto try_path = [&attempted, &vfs](const std::filesystem::path& base,
                                     const std::filesystem::path& path)
      -> std::optional<std::filesystem::path> {
    auto full_path = std::filesystem::path(base) / path;
    VLOG(3) << "Trying path: " << full_path;
    attempted.push_back(std::string{full_path});
    if (vfs.FileExists(full_path).ok()) {
      VLOG(3) << "Found existing file for import path: " << full_path;
      return full_path;
    }
    return std::nullopt;
  };
  // Helper that tries to see if the path/parent_path are present
  auto try_paths =
      [&try_path, subject_path, subject_parent_path](
          const std::filesystem::path& base) -> std::optional<DslxPath> {
    if (std::optional<std::filesystem::path> result =
            try_path(base, subject_path)) {
      return DslxPath{.source_path = *result, .filesystem_path = *result};
    }
    if (subject_parent_path.has_value()) {
      if (std::optional<std::filesystem::path> result =
              try_path(base, *subject_parent_path)) {
        return DslxPath{.source_path = *result, .filesystem_path = *result};
      }
    }
    return std::nullopt;
  };

  VLOG(3) << "Attempting CWD-relative import path.";
  if (std::optional<std::filesystem::path> cwd_relative_path =
          try_path("", subject_path)) {
    return DslxPath{.source_path = *cwd_relative_path,
                    .filesystem_path = *cwd_relative_path};
  }

  VLOG(3) << "Attempting runfile-based import path via " << subject_path;
  if (absl::StatusOr<std::string> runfile_path =
          GetXlsRunfilePath(GetXLSRootDir() / subject_path);
      runfile_path.ok() && vfs.FileExists(*runfile_path).ok()) {
    return DslxPath{.source_path = subject_path,
                    .filesystem_path = *runfile_path};
  }

  if (subject_parent_path.has_value()) {
    // This one is generally required for genrules in-house, where the first
    // part of the path under the depot root is stripped off for some reason.
    VLOG(3) << "Attempting CWD-based parent import path via "
            << *subject_parent_path;
    if (std::optional<std::filesystem::path> cwd_relative_path =
            try_path("", *subject_parent_path)) {
      return DslxPath{.source_path = *subject_parent_path,
                      .filesystem_path = *cwd_relative_path};
    }
    VLOG(3) << "Attempting runfile-based parent import path via "
            << *subject_parent_path;
    if (absl::StatusOr<std::string> runfile_path = GetXlsRunfilePath(
            absl::StrCat(GetXLSRootDir(), subject_parent_path->c_str()));
        runfile_path.ok() && vfs.FileExists(*runfile_path).ok()) {
      return DslxPath{.source_path = subject_path,
                      .filesystem_path = *runfile_path};
    }
  }
  // Look through the externally-supplied additional search paths.
  for (const std::filesystem::path& search_path : additional_search_paths) {
    VLOG(3) << "Attempting search path root: " << search_path;
    if (auto found = try_paths(search_path)) {
      return *found;
    }
  }

  return absl::NotFoundError(absl::StrFormat(
      "ImportError: %s Could not find DSLX file for import; "
      "attempted: [ %s ]; working "
      "directory: \"%s\"; stdlib directory: \"%s\"",
      import_span.ToString(file_table), absl::StrJoin(attempted, " :: "),
      vfs.GetCurrentDirectory().value(), stdlib_path));
}

absl::StatusOr<ModuleInfo*> DoImport(const TypecheckModuleFn& ftypecheck,
                                     const ImportTokens& subject,
                                     ImportData* import_data,
                                     const Span& import_span,
                                     VirtualizableFilesystem& vfs) {
  XLS_RET_CHECK(import_data != nullptr);
  if (import_data->Contains(subject)) {
    VLOG(3) << "DoImport (cached) subject: " << subject.ToString();
    return import_data->Get(subject);
  }

  VLOG(3) << "DoImport (uncached) subject: " << subject.ToString();

  FileTable& file_table = import_data->file_table();
  XLS_ASSIGN_OR_RETURN(DslxPath dslx_path,
                       FindExistingPath(subject, import_data->stdlib_path(),
                                        import_data->additional_search_paths(),
                                        import_span, file_table, vfs));

  XLS_RETURN_IF_ERROR(
      import_data->AddToImporterStack(import_span, dslx_path.source_path));
  absl::Cleanup cleanup = absl::MakeCleanup(
      [&] { CHECK_OK(import_data->PopFromImporterStack(import_span)); });

  // Use the "filesystem_path" for reading the contents but the "source_path"
  // for other uses. This avoids decorated paths like
  // "/build/work/.../runfiles/...a/b/c/foo.x" appearing in the file table and
  // artifacts. Instead the original "a/b/c/foo.x" path is used.
  XLS_ASSIGN_OR_RETURN(std::string contents,
                       vfs.GetFileContents(dslx_path.filesystem_path));

  absl::Span<std::string const> pieces = subject.pieces();
  std::string fully_qualified_name = absl::StrJoin(pieces, ".");
  VLOG(3) << "Parsing and typechecking " << fully_qualified_name << ": start";

  VLOG(4) << "Subject = " << subject.ToString();
  VLOG(4) << "Source path = " << dslx_path.source_path.c_str();
  VLOG(4) << "Filesystem path = " << dslx_path.filesystem_path.c_str();

  Fileno fileno = file_table.GetOrCreate(dslx_path.source_path.c_str());
  Scanner scanner(file_table, fileno, contents);
  Parser parser(/*module_name=*/fully_qualified_name, &scanner);
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Module> module, parser.ParseModule());
  XLS_ASSIGN_OR_RETURN(TypeInfo * type_info, ftypecheck(module.get()));

  VLOG(3) << "Parsing and typechecking " << fully_qualified_name << ": done";

  return import_data->Put(
      subject, std::make_unique<ModuleInfo>(std::move(module), type_info,
                                            std::move(dslx_path.source_path)));
}

}  // namespace xls::dslx
