#pragma once

// JSON for the programs of the plugin system that carry no Qt: a value, a
// parser (UTF-8 in, \uXXXX and surrogate pairs decoded, nesting limited) and
// a writer. Enough for the runner's control messages and the test plugin's
// JSON-RPC; not a general library.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ws::plugin::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>; // keys sorted: good enough for messages

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b)
        : m_type(Type::Bool)
        , m_bool(b)
    {
    }
    Value(double d)
        : m_type(Type::Number)
        , m_number(d)
    {
    }
    Value(int i)
        : Value(static_cast<double>(i))
    {
    }
    Value(long long i)
        : Value(static_cast<double>(i))
    {
    }
    Value(unsigned long u)
        : Value(static_cast<double>(u))
    {
    }
    Value(unsigned long long u)
        : Value(static_cast<double>(u))
    {
    }
    Value(std::string s)
        : m_type(Type::String)
        , m_string(std::move(s))
    {
    }
    Value(const char* s)
        : Value(std::string(s))
    {
    }
    Value(Array a)
        : m_type(Type::Array)
        , m_array(std::make_shared<Array>(std::move(a)))
    {
    }
    Value(Object o)
        : m_type(Type::Object)
        , m_object(std::make_shared<Object>(std::move(o)))
    {
    }

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool toBool(bool fallback = false) const { return isBool() ? m_bool : fallback; }
    double toNumber(double fallback = 0) const { return isNumber() ? m_number : fallback; }
    long long toInt(long long fallback = 0) const { return isNumber() ? static_cast<long long>(m_number) : fallback; }
    const std::string& toString() const
    {
        static const std::string empty;
        return isString() ? m_string : empty;
    }
    const Array& toArray() const
    {
        static const Array empty;
        return isArray() ? *m_array : empty;
    }
    const Object& toObject() const
    {
        static const Object empty;
        return isObject() ? *m_object : empty;
    }
    // Object member, or a null value.
    const Value& operator[](const char* key) const
    {
        static const Value null;
        if (!isObject())
            return null;
        const auto it = m_object->find(key);
        return it == m_object->end() ? null : it->second;
    }
    bool has(const char* key) const { return isObject() && m_object->count(key) > 0; }

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    std::string m_string;
    std::shared_ptr<Array> m_array;
    std::shared_ptr<Object> m_object;
};

// One JSON text; nullopt when it is not one (with `problem` set).
std::optional<Value> parse(const std::string& text, std::string* problem = nullptr);

// Compact JSON of a value (strings escaped, non-ASCII kept as UTF-8).
std::string write(const Value& value);
std::string quote(const std::string& s); // a JSON string with its quotes

} // namespace ws::plugin::json
