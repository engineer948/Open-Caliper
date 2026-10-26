#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// Arithmetic for user parameters: "width / 2 + 5", "sqrt(a^2 + b^2)".
// Numbers, + - * / ^, parentheses, pi, sqrt abs sin cos tan (degrees) min max
// round floor ceil, and parameter names. No Qt, no OCCT.
namespace model {

using Variables = std::map<std::string, double>;

// Thrown for a name that is not in the variables (so callers can tell
// "not defined yet" from a syntax error).
struct UnknownName : std::runtime_error {
  explicit UnknownName(const std::string& name);
  std::string name;
};

// Throws std::runtime_error (or UnknownName) with a user-facing message.
double evaluate(const std::string& text, const Variables& vars = {});
// True for plain numbers such as "12.5" or "-3", which need no expression.
bool isPlainNumber(const std::string& text);
// Letters, digits and _; starts with a letter or _; not a function name or pi.
bool isValidName(const std::string& name);
// Parameter names used in `text` (empty when it does not parse).
std::vector<std::string> namesIn(const std::string& text);

// A named value the user can refer to in any size: "width = 50".
struct Parameter {
  std::string name;
  std::string expression;
};

// Values of all parameters; they may refer to each other in any order.
// Throws on errors (unknown names, cycles, bad syntax), naming the parameter.
Variables evaluateParameters(const std::vector<Parameter>& params);

} // namespace model
