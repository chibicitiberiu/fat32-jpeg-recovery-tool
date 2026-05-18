/*
 * json_writer.cpp - Minimal JSON writer.
 */
#include "corrsim.h"

void JsonWriter::write_indent()
{
    for (int i = 0; i < indent_; i++)
        fprintf(f_, "  ");
}

void JsonWriter::maybe_comma()
{
    if (need_comma_) {
        fprintf(f_, ",");
    }
    fprintf(f_, "\n");
    need_comma_ = true;
}

void JsonWriter::begin_object()
{
    maybe_comma();
    write_indent();
    fprintf(f_, "{");
    comma_stack_.push_back(need_comma_);
    need_comma_ = false;
    indent_++;
}

void JsonWriter::end_object()
{
    indent_--;
    fprintf(f_, "\n");
    write_indent();
    fprintf(f_, "}");
    need_comma_ = comma_stack_.back();
    comma_stack_.pop_back();
    need_comma_ = true;
}

void JsonWriter::begin_array()
{
    maybe_comma();
    write_indent();
    fprintf(f_, "[");
    comma_stack_.push_back(need_comma_);
    need_comma_ = false;
    indent_++;
}

void JsonWriter::end_array()
{
    indent_--;
    fprintf(f_, "\n");
    write_indent();
    fprintf(f_, "]");
    need_comma_ = comma_stack_.back();
    comma_stack_.pop_back();
    need_comma_ = true;
}

void JsonWriter::key(const char *k)
{
    maybe_comma();
    write_indent();
    fprintf(f_, "\"%s\": ", k);
    need_comma_ = false;
}

static void write_escaped(FILE *f, const std::string &s)
{
    fputc('"', f);
    for (char c : s) {
        switch (c) {
            case '"':  fprintf(f, "\\\""); break;
            case '\\': fprintf(f, "\\\\"); break;
            case '\n': fprintf(f, "\\n"); break;
            case '\r': fprintf(f, "\\r"); break;
            case '\t': fprintf(f, "\\t"); break;
            default:
                if ((unsigned char)c < 0x20)
                    fprintf(f, "\\u%04x", (unsigned char)c);
                else
                    fputc(c, f);
        }
    }
    fputc('"', f);
}

void JsonWriter::value_string(const std::string &v)
{
    // If preceded by key(), need_comma_ is false and no newline needed
    // If inside array, need comma + newline
    if (need_comma_) {
        fprintf(f_, ",\n");
        write_indent();
    }
    write_escaped(f_, v);
    need_comma_ = true;
}

void JsonWriter::value_int(int64_t v)
{
    if (need_comma_) {
        fprintf(f_, ",\n");
        write_indent();
    }
    fprintf(f_, "%ld", v);
    need_comma_ = true;
}

void JsonWriter::value_uint(uint64_t v)
{
    if (need_comma_) {
        fprintf(f_, ",\n");
        write_indent();
    }
    fprintf(f_, "%lu", v);
    need_comma_ = true;
}

void JsonWriter::value_double(double v, int precision)
{
    if (need_comma_) {
        fprintf(f_, ",\n");
        write_indent();
    }
    fprintf(f_, "%.*g", precision, v);
    need_comma_ = true;
}

void JsonWriter::value_bool(bool v)
{
    if (need_comma_) {
        fprintf(f_, ",\n");
        write_indent();
    }
    fprintf(f_, "%s", v ? "true" : "false");
    need_comma_ = true;
}

void JsonWriter::value_null()
{
    if (need_comma_) {
        fprintf(f_, ",\n");
        write_indent();
    }
    fprintf(f_, "null");
    need_comma_ = true;
}
