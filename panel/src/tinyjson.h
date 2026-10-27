#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace tj {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    Value() = default;
    Value(bool v) : type(Type::Bool), b(v) {}
    Value(double v) : type(Type::Number), num(v) {}
    Value(int v) : type(Type::Number), num(v) {}
    Value(const char* v) : type(Type::String), str(v ? v : "") {}
    Value(const std::string& v) : type(Type::String), str(v) {}

    bool is_null()   const { return type == Type::Null; }
    bool is_object() const { return type == Type::Object; }

    const Value* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string get_str(const std::string& key, const std::string& def = "") const {
        const Value* v = find(key);
        if (!v) return def;
        if (v->type == Type::String) return v->str;
        if (v->type == Type::Number) { char t[32]; snprintf(t, sizeof(t), "%g", v->num); return t; }
        if (v->type == Type::Bool)   return v->b ? "true" : "false";
        return def;
    }
    bool get_bool(const std::string& key, bool def = false) const {
        const Value* v = find(key);
        if (!v) return def;
        if (v->type == Type::Bool) return v->b;
        if (v->type == Type::Number) return v->num != 0;
        if (v->type == Type::String) return v->str == "true" || v->str == "1";
        return def;
    }
    void set(const std::string& key, const Value& v) {
        for (auto& kv : obj) if (kv.first == key) { kv.second = v; return; }
        obj.emplace_back(key, v);
    }
};

struct Parser {
    const char* p;
    const char* end;
    bool ok = true;

    Parser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

    void skip_ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }

    bool parse(Value& out) {
        skip_ws();
        Value v;
        if (!parse_value(v)) { ok = false; return false; }
        out = v;
        return true;
    }

    bool parse_value(Value& v) {
        skip_ws();
        if (p >= end) return false;
        char c = *p;
        if (c == '{') return parse_object(v);
        if (c == '[') return parse_array(v);
        if (c == '"') { v = Value(parse_string()); return true; }
        if (c == 't') { if (end - p >= 4 && !strncmp(p, "true", 4)) { p += 4; v = Value(true);  return true; } return false; }
        if (c == 'f') { if (end - p >= 5 && !strncmp(p, "false", 5)) { p += 5; v = Value(false); return true; } return false; }
        if (c == 'n') { if (end - p >= 4 && !strncmp(p, "null", 4)) { p += 4; v = Value(); return true; } return false; }
        return parse_number(v);
    }

    bool parse_object(Value& v) {
        v = Value(); v.type = Type::Object;
        p++;
        skip_ws();
        if (p < end && *p == '}') { p++; return true; }
        while (p < end) {
            skip_ws();
            if (p >= end || *p != '"') return false;
            std::string key = parse_string();
            skip_ws();
            if (p >= end || *p != ':') return false;
            p++;
            Value child;
            if (!parse_value(child)) return false;
            v.obj.emplace_back(std::move(key), std::move(child));
            skip_ws();
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == '}') { p++; return true; }
            return false;
        }
        return false;
    }

    bool parse_array(Value& v) {
        v = Value(); v.type = Type::Array;
        p++;
        skip_ws();
        if (p < end && *p == ']') { p++; return true; }
        while (p < end) {
            Value child;
            if (!parse_value(child)) return false;
            v.arr.push_back(std::move(child));
            skip_ws();
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == ']') { p++; return true; }
            return false;
        }
        return false;
    }

    std::string parse_string() {
        std::string out;
        p++;
        while (p < end) {
            char c = *p++;
            if (c == '"') break;
            if (c == '\\' && p < end) {
                char e = *p++;
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (end - p >= 4) {
                            unsigned cp = 0;
                            for (int i = 0; i < 4; i++) {
                                char h = *p++;
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= (h - '0');
                                else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            }

                            if (cp < 0x80) out += (char)cp;
                            else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                            else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                        }
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    bool parse_number(Value& v) {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) p++;
        bool digits = false;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '-' || *p == '+')) {
            if (*p >= '0' && *p <= '9') digits = true;
            p++;
        }
        if (!digits) return false;
        v = Value(atof(std::string(start, p).c_str()));
        return true;
    }
};

inline bool parse(const std::string& text, Value& out) {
    Parser parser(text);
    if (!parser.parse(out)) return false;
    return true;
}

inline void escape_to(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) { char t[8]; snprintf(t, sizeof(t), "\\u%04x", c); out += t; }
                else out += c;
        }
    }
    out += '"';
}

inline void dump_to(std::string& out, const Value& v, int indent, int depth) {
    char pad[64]; int npad = indent * depth; if (npad > 60) npad = 60;
    for (int i = 0; i < npad; i++) pad[i] = ' ';
    pad[npad] = '\0';

    switch (v.type) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += v.b ? "true" : "false"; break;
        case Type::Number: {
            double n = v.num;
            if (n == (long long)n && std::fabs(n) < 1e15) { char t[32]; snprintf(t, sizeof(t), "%lld", (long long)n); out += t; }
            else { char t[32]; snprintf(t, sizeof(t), "%g", n); out += t; }
            break;
        }
        case Type::String:  escape_to(out, v.str); break;
        case Type::Array: {
            if (v.arr.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t i = 0; i < v.arr.size(); i++) {
                out += pad; if (indent) out += ' ';
                dump_to(out, v.arr[i], indent, depth + 1);
                if (i + 1 < v.arr.size()) out += ',';
                out += '\n';
            }
            out += pad; out += ']';
            break;
        }
        case Type::Object: {
            if (v.obj.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t i = 0; i < v.obj.size(); i++) {
                out += pad; if (indent) out += ' ';
                escape_to(out, v.obj[i].first);
                out += ": ";
                dump_to(out, v.obj[i].second, indent, depth + 1);
                if (i + 1 < v.obj.size()) out += ',';
                out += '\n';
            }
            out += pad; out += '}';
            break;
        }
    }
}

inline std::string dump(const Value& v, int indent = 2) {
    std::string out;
    dump_to(out, v, indent, 0);
    out += '\n';
    return out;
}

}
