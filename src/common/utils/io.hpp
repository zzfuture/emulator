#pragma once

#include <span>
#include <vector>
#include <filesystem>

namespace utils::io
{
    bool remove_file(const std::filesystem::path& file);
    bool move_file(const std::filesystem::path& src, const std::filesystem::path& target);
    bool file_exists(const std::filesystem::path& file);
    bool write_file(const std::filesystem::path& file, std::span<const std::byte> data, bool append = false);
    bool read_file(const std::filesystem::path& file, std::vector<std::byte>* data);
    std::vector<std::byte> read_file(const std::filesystem::path& file);
    size_t file_size(const std::filesystem::path& file);
    bool create_directory(const std::filesystem::path& directory);
    bool directory_exists(const std::filesystem::path& directory);
    bool directory_is_empty(const std::filesystem::path& directory);
    void copy_folder(const std::filesystem::path& src, const std::filesystem::path& target);

    std::vector<std::filesystem::path> list_files(const std::filesystem::path& directory, bool recursive = false);
}
