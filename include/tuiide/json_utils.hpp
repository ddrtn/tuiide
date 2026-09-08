#pragma once

#include <nlohmann/json.hpp>

#include <utility>

namespace tuiide {

/**
 * Читает поле внешнего JSON с проверкой типа. Отсутствующее поле, `null` и
 * несовместимый тип возвращают fallback вместо исключения nlohmann-json.
 */
template <typename T>
auto jsonValueOr(const nlohmann::json& object, const char* key, T fallback)
    -> T {
  if (!object.is_object()) return fallback;
  const auto value = object.find(key);
  if (value == object.end() || value->is_null()) return fallback;
  try {
    return value->template get<T>();
  } catch (const nlohmann::json::exception&) {
    return fallback;
  }
}

/** Возвращает поле без преобразования либо настоящий JSON `null`. */
inline auto jsonFieldOrNull(const nlohmann::json& object, const char* key)
    -> const nlohmann::json& {
  static const nlohmann::json null_value = nullptr;
  if (!object.is_object()) return null_value;
  const auto value = object.find(key);
  return value == object.end() ? null_value : *value;
}

}  // namespace tuiide
