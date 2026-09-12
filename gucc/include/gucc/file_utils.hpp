#pragma once

#include <string>       // for string
#include <string_view>  // for string_view

namespace gucc::file_utils {

// Read the entire contents of a file.
[[nodiscard]] auto read_whole_file(std::string_view filepath) noexcept -> std::string;
// Read only the first line of a file (trims trailing newline/carriage return).
[[nodiscard]] auto read_first_line(std::string_view filepath) noexcept -> std::string;
// Write data to a file.
auto write_to_file(std::string_view data, std::string_view filepath) noexcept -> bool;

// If the file doesn't exist, then it create one and write into it.
// If the file exists already, then it will overwrite file content with provided data.
auto create_file_for_overwrite(std::string_view filepath, std::string_view data) noexcept -> bool;

}  // namespace gucc::file_utils
