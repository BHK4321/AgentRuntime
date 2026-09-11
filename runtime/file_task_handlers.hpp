#pragma once

#include "task_handler_registry.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace file_tasks {

inline std::string json_string_field(const std::string& payload, std::string_view field) {
    const auto key = payload.find("\"" + std::string(field) + "\"");
    if (key == std::string::npos) {
        throw std::invalid_argument("payload requires " + std::string(field));
    }
    const auto colon = payload.find(':', key + field.size() + 2);
    const auto quote = payload.find('"', colon == std::string::npos ? key : colon + 1);
    if (colon == std::string::npos || quote == std::string::npos) {
        throw std::invalid_argument("payload has invalid " + std::string(field));
    }

    std::string value;
    bool escaped = false;
    for (auto index = quote + 1; index < payload.size(); ++index) {
        const auto character = payload[index];
        if (escaped) {
            switch (character) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: throw std::invalid_argument(
                "payload uses unsupported escape in " + std::string(field));
            }
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            if (value.empty()) {
                throw std::invalid_argument("payload field cannot be empty: " +
                                            std::string(field));
            }
            return value;
        } else {
            value.push_back(character);
        }
    }
    throw std::invalid_argument("payload has unterminated " + std::string(field));
}

inline std::filesystem::path work_root() {
    if (const auto* configured = std::getenv("AGENTOS_WORK_DIR");
        configured != nullptr && *configured != '\0') {
        return std::filesystem::absolute(configured).lexically_normal();
    }
    return (std::filesystem::current_path() / "AgentOS" / "work").lexically_normal();
}

inline std::filesystem::path confined_path(const std::filesystem::path& root,
                                           const std::string& relative_value) {
    const std::filesystem::path relative(relative_value);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name()) {
        throw std::invalid_argument("file task paths must be relative to AGENTOS_WORK_DIR");
    }
    for (const auto& component : relative) {
        if (component == "..") {
            throw std::invalid_argument("file task path cannot contain ..");
        }
    }
    std::filesystem::create_directories(root);
    const auto canonical_root = std::filesystem::weakly_canonical(root);
    const auto candidate = std::filesystem::weakly_canonical(canonical_root / relative);
    auto root_component = canonical_root.begin();
    auto candidate_component = candidate.begin();
    while (root_component != canonical_root.end() && candidate_component != candidate.end() &&
           *root_component == *candidate_component) {
        ++root_component;
        ++candidate_component;
    }
    if (root_component != canonical_root.end()) {
        throw std::invalid_argument("file task path escapes AGENTOS_WORK_DIR");
    }
    return candidate;
}

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open input file: " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

inline void write_file(const std::filesystem::path& path, const std::string& content,
                       const std::string& task_id) {
    if (!path.has_parent_path()) {
        throw std::invalid_argument("output path requires a parent directory");
    }
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.string() + "." +
                           std::to_string(std::hash<std::string>{}(task_id)) + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot open output file: " + temporary);
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!output) {
            throw std::runtime_error("failed writing output file: " + temporary);
        }
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::rename(temporary, path);
}

inline void register_handlers(TaskHandlerRegistry& handlers,
                              std::filesystem::path root = work_root()) {
    root = std::filesystem::absolute(std::move(root)).lexically_normal();
    handlers.register_handler("text_transform", [root](const std::string& payload,
                                                        const TaskContext& context) {
        const auto input_path = confined_path(root, json_string_field(payload, "input_path"));
        const auto output_path = confined_path(root, json_string_field(payload, "output_path"));
        const auto operation = json_string_field(payload, "operation");
        auto content = read_file(input_path);
        if (operation == "uppercase") {
            for (auto& character : content) {
                character = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(character)));
            }
        } else if (operation == "lowercase") {
            for (auto& character : content) {
                character = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(character)));
            }
        } else {
            throw std::invalid_argument("text_transform operation must be uppercase or lowercase");
        }
        if (context.cancellation && context.cancellation->load()) {
            return;
        }
        write_file(output_path, content, context.task_id);
    });

    handlers.register_handler("word_count", [root](const std::string& payload,
                                                    const TaskContext& context) {
        const auto input_path = confined_path(root, json_string_field(payload, "input_path"));
        const auto output_path = confined_path(root, json_string_field(payload, "output_path"));
        const auto content = read_file(input_path);

        std::size_t words = 0;
        std::size_t lines = 0;
        bool in_word = false;
        for (const auto character : content) {
            if (context.cancellation && context.cancellation->load()) {
                return;
            }
            if (character == '\n') {
                ++lines;
            }
            if (std::isspace(static_cast<unsigned char>(character))) {
                in_word = false;
            } else if (!in_word) {
                ++words;
                in_word = true;
            }
        }
        if (!content.empty() && content.back() != '\n') {
            ++lines;
        }

        std::ostringstream result;
        result << "{\n  \"bytes\": " << content.size()
               << ",\n  \"lines\": " << lines
               << ",\n  \"words\": " << words << "\n}\n";
        write_file(output_path, result.str(), context.task_id);
    });
}

} // namespace file_tasks
