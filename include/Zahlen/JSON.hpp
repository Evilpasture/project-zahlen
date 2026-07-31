// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cmath>
#include <detail/Reflection.hpp>
#include <expected>
#include <simdjson.h>
#include <string_view>
#include <type_traits>

namespace ZHLN {

enum class JSONError : uint8_t { Success = 0, InvalidJSON, TypeMismatch, MissingField, UnknownError };

namespace ReflectJSON {

// Forward declaration of recursive object parsing
template <typename T>
std::expected<T, ZHLN::Error> ParseObject(simdjson::dom::element elem);

template <typename FieldType>
std::expected<FieldType, ZHLN::Error> GetJSONValue(simdjson::dom::element elem) {
    using Decayed = std::decay_t<FieldType>;

    if constexpr (std::is_same_v<Decayed, int> || std::is_same_v<Decayed, int32_t>) {
        int64_t val   = 0;
        auto    error = elem.get<int64_t>().get(val);
        if (error) {
            return std::unexpected(JSONError::TypeMismatch);
        }
        return static_cast<FieldType>(val);
    } else if constexpr (std::is_same_v<Decayed, uint32_t>) {
        uint64_t val   = 0;
        auto     error = elem.get<uint64_t>().get(val);
        if (error) {
            return std::unexpected(JSONError::TypeMismatch);
        }
        return static_cast<FieldType>(val);
    } else if constexpr (std::is_same_v<Decayed, float>) {
        double val   = NAN;
        auto   error = elem.get<double>().get(val);
        if (error) {
            return std::unexpected(JSONError::TypeMismatch);
        }
        return static_cast<float>(val);
    } else if constexpr (std::is_same_v<Decayed, double>) {
        double val   = NAN;
        auto   error = elem.get<double>().get(val);
        if (error) {
            return std::unexpected(JSONError::TypeMismatch);
        }
        return val;
    } else if constexpr (std::is_same_v<Decayed, bool>) {
        bool val   = false;
        auto error = elem.get<bool>().get(val);
        if (error) {
            return std::unexpected(JSONError::TypeMismatch);
        }
        return val;
    } else if constexpr (std::is_same_v<Decayed, std::string_view>) {
        std::string_view val;
        auto             error = elem.get<std::string_view>().get(val);
        if (error) {
            return std::unexpected(JSONError::TypeMismatch);
        }
        return val;
    } else if constexpr (std::is_same_v<Decayed, std::string>) {
        std::string_view val;
        auto             error = elem.get<std::string_view>().get(val);
        if (error) {
            return std::unexpected(JSONError::TypeMismatch);
        }
        return std::string(val);
    } else if constexpr (std::is_enum_v<Decayed>) {
        // Resolve enums safely using Reflection's encapsulated StringToEnum
        std::string_view val;
        auto             error = elem.get<std::string_view>().get(val);
        if (error) {
            return std::unexpected(JSONError::TypeMismatch);
        }
        auto enum_opt = ZHLN::Reflect::StringToEnum<Decayed>(val);
        if (!enum_opt) {
            return std::unexpected(JSONError::TypeMismatch);
        }
        return *enum_opt;
    } else if constexpr (std::ranges::range<Decayed>) {
        // Parse arrays/lists of items recursively
        simdjson::dom::array json_arr;
        auto                 error = elem.get<simdjson::dom::array>().get(json_arr);
        if (error) {
            return std::unexpected(JSONError::TypeMismatch);
        }

        Decayed container;
        using ElementType = typename Decayed::value_type;
        for (auto item: json_arr) {
            auto parsed_item = GetJSONValue<ElementType>(item);
            if (!parsed_item) {
                return std::unexpected(parsed_item.error());
            }
            container.push_back(std::move(*parsed_item));
        }
        return container;
    } else if constexpr (ZHLN::Reflect::FieldCount<Decayed>() > 0) {
        return ParseObject<Decayed>(elem);
    } else {
        return std::unexpected(JSONError::UnknownError);
    }
}

template <typename T>
std::expected<T, ZHLN::Error> ParseObject(simdjson::dom::element elem) {
    T                     obj {};
    simdjson::dom::object json_object;
    auto                  error = elem.get<simdjson::dom::object>().get(json_object);
    if (error) {
        return std::unexpected(JSONError::TypeMismatch);
    }

    ZHLN::Error err = JSONError::Success;

    // Use the abstract, encapsulated visitor to populate our mutable lvalue reference
    ZHLN::Reflect::ForEachFieldWithName(obj, [&](std::string_view fieldName, auto& fieldVal) {
        if (err) {
            return; // Skip if a previous step failed
        }

        using FieldType = std::decay_t<decltype(fieldVal)>;

        simdjson::dom::element field_elem;
        auto                   lookup_err = json_object.at_key(fieldName).get(field_elem);
        if (lookup_err) {
            err = JSONError::MissingField;
            return;
        }

        auto value_res = GetJSONValue<FieldType>(field_elem);
        if (!value_res) {
            err = value_res.error();
            return;
        }

        fieldVal = std::move(*value_res);
    });

    if (err) {
        return std::unexpected(err);
    }
    return obj;
}

/**
 * @brief Parses JSON string safely and returns an expected object.
 */
template <typename T>
std::expected<T, ZHLN::Error> TryParse(std::string_view jsonString) {
    simdjson::dom::parser parser;

    // Copying to padded_string ensures SIMD boundary safety required by simdjson
    simdjson::padded_string padded(jsonString);

    simdjson::dom::element doc;
    auto                   error = parser.parse(padded).get(doc);
    if (error) {
        return std::unexpected(JSONError::InvalidJSON);
    }

    return ParseObject<T>(doc);
}

/**
 * @brief Parses JSON string and panics on failure. Ideal for fast setup.
 */
template <typename T>
T Parse(std::string_view jsonString) {
    auto res = TryParse<T>(jsonString);
    if (!res) [[unlikely]] {
        ZHLN::Panic("Failed to parse JSON for type '{}': {}", ZHLN::Reflect::TypeName<T>(), res.error().Message());
    }
    return std::move(*res);
}

} // namespace ReflectJSON
} // namespace ZHLN
