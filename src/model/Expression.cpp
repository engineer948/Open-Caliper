#include "model/Expression.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <numbers>
#include <set>

namespace model {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

constexpr double kDeg = std::numbers::pi / 180.0;

const std::set<std::string>& functions() {
  static const std::set<std::string> f{"sqrt", "abs", "sin", "cos", "tan", "min", "max", "round", "floor", "ceil"};
  return f;
}

// Recursive descent:
//   sum     = product (('+' | '-') product)*
//   product = unary (('*' | '/') unary)*
//   unary   = ('-' | '+') unary | power
//   power   = primary ('^' unary)?
//   primary = number | name | name '(' sum (',' sum)* ')' | '(' sum ')'
class Parser {
public:
  Parser(const std::string& text, const Variables* vars, std::vector<std::string>* names)
      : m_text(text), m_vars(vars), m_names(names) {}

  double parse() {
    const double v = sum();
    skip();
    if (m_pos != m_text.size()) fail("Unexpected '" + m_text.substr(m_pos, 1) + "' in \"" + m_text + "\".");
    return v;
  }

private:
  void skip() {
    while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos]))) ++m_pos;
  }
  bool eat(char c) {
    skip();
    if (m_pos < m_text.size() && m_text[m_pos] == c) {
      ++m_pos;
      return true;
    }
    return false;
  }

  double sum() {
    double v = product();
    for (;;) {
      if (eat('+')) v += product();
      else if (eat('-')) v -= product();
      else return v;
    }
  }
  double product() {
    double v = unary();
    for (;;) {
      if (eat('*')) v *= unary();
      else if (eat('/')) {
        const double d = unary();
        if (d == 0 && !m_names) fail("Division by zero in \"" + m_text + "\".");
        v = d == 0 ? 0 : v / d;
      } else return v;
    }
  }
  double unary() {
    if (eat('-')) return -unary();
    if (eat('+')) return unary();
    return power();
  }
  double power() {
    const double base = primary();
    if (eat('^')) return std::pow(base, unary());
    return base;
  }
  double primary() {
    skip();
    if (m_pos >= m_text.size()) fail("The expression \"" + m_text + "\" is incomplete.");
    const char c = m_text[m_pos];
    if (eat('(')) {
      const double v = sum();
      if (!eat(')')) fail("Missing ')' in \"" + m_text + "\".");
      return v;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
      // from_chars: always '.' as the decimal point, whatever the system language
      double v = 0;
      const char* start = m_text.data() + m_pos;
      const auto [end, err] = std::from_chars(start, m_text.data() + m_text.size(), v, std::chars_format::fixed);
      if (err != std::errc() || end == start) fail("Bad number in \"" + m_text + "\".");
      m_pos += size_t(end - start);
      return v;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      const size_t start = m_pos;
      while (m_pos < m_text.size() && (std::isalnum(static_cast<unsigned char>(m_text[m_pos])) || m_text[m_pos] == '_'))
        ++m_pos;
      const std::string name = m_text.substr(start, m_pos - start);
      if (functions().count(name)) return call(name);
      if (name == "pi") return std::numbers::pi;
      if (m_names) {
        m_names->push_back(name);
        return 1;
      }
      const auto it = m_vars->find(name);
      if (it == m_vars->end()) throw UnknownName(name);
      return it->second;
    }
    fail("Unexpected '" + std::string(1, c) + "' in \"" + m_text + "\".");
  }
  double call(const std::string& f) {
    if (!eat('(')) fail(f + " needs parentheses, e.g. " + f + "(2).");
    std::vector<double> args{sum()};
    while (eat(',')) args.push_back(sum());
    if (!eat(')')) fail("Missing ')' in \"" + m_text + "\".");
    const bool two = f == "min" || f == "max";
    if (args.size() != (two ? 2u : 1u)) fail(f + " takes " + (two ? "two values." : "one value."));
    const double a = args[0];
    if (f == "sqrt") {
      if (a < 0 && !m_names) fail("Square root of a negative number in \"" + m_text + "\".");
      return std::sqrt(std::max(0.0, a));
    }
    if (f == "abs") return std::abs(a);
    if (f == "sin") return std::sin(a * kDeg);
    if (f == "cos") return std::cos(a * kDeg);
    if (f == "tan") return std::tan(a * kDeg);
    if (f == "min") return std::min(a, args[1]);
    if (f == "max") return std::max(a, args[1]);
    if (f == "round") return std::round(a);
    if (f == "floor") return std::floor(a);
    return std::ceil(a);
  }

  const std::string& m_text;
  const Variables* m_vars;
  std::vector<std::string>* m_names; // collect names instead of evaluating
  size_t m_pos = 0;
};

// "2,5" as typed with a decimal comma: commas outside parentheses become
// points; inside a function call they separate the arguments.
std::string decimalPoints(std::string text) {
  int depth = 0;
  for (char& c : text) {
    if (c == '(') ++depth;
    else if (c == ')') --depth;
    else if (c == ',' && depth == 0) c = '.';
  }
  return text;
}

} // namespace

UnknownName::UnknownName(const std::string& n) : std::runtime_error("Unknown parameter '" + n + "'."), name(n) {}

double evaluate(const std::string& input, const Variables& vars) {
  const std::string text = decimalPoints(input);
  if (text.find_first_not_of(" \t") == std::string::npos) fail("Enter a value.");
  const double v = Parser(text, &vars, nullptr).parse();
  if (!std::isfinite(v)) fail("\"" + text + "\" has no finite value.");
  return v;
}

bool isPlainNumber(const std::string& input) {
  const std::string text = decimalPoints(input);
  const size_t first = text.find_first_not_of(" \t"), last = text.find_last_not_of(" \t");
  if (first == std::string::npos) return false;
  const char* start = text.data() + first + (text[first] == '+' ? 1 : 0);
  const char* stop = text.data() + last + 1;
  double v = 0;
  const auto [end, err] = std::from_chars(start, stop, v, std::chars_format::fixed);
  return err == std::errc() && end == stop;
}

bool isValidName(const std::string& name) {
  if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
  for (char c : name)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  return name != "pi" && !functions().count(name);
}

std::vector<std::string> namesIn(const std::string& input) {
  std::vector<std::string> names;
  const std::string text = decimalPoints(input);
  try {
    Parser(text, nullptr, &names).parse();
  } catch (const std::exception&) {
    return {};
  }
  return names;
}

Variables evaluateParameters(const std::vector<Parameter>& params) {
  Variables vars;
  std::set<std::string> seen;
  for (const Parameter& p : params) {
    if (!isValidName(p.name)) fail("\"" + p.name + "\" is not a valid parameter name (letters, digits, _).");
    if (!seen.insert(p.name).second) fail("The parameter '" + p.name + "' is defined twice.");
  }
  // Repeated passes: each one evaluates the parameters whose names are known by now.
  std::vector<bool> done(params.size(), false);
  for (size_t pass = 0; pass <= params.size(); ++pass) {
    bool progress = false;
    for (size_t i = 0; i < params.size(); ++i) {
      if (done[i]) continue;
      try {
        vars[params[i].name] = evaluate(params[i].expression, vars);
        done[i] = progress = true;
      } catch (const UnknownName& e) {
        if (!seen.count(e.name)) fail(params[i].name + ": unknown parameter '" + e.name + "'.");
      } catch (const std::exception& e) {
        fail(params[i].name + ": " + e.what());
      }
    }
    if (!progress) break;
  }
  for (size_t i = 0; i < params.size(); ++i)
    if (!done[i]) fail("The parameter '" + params[i].name + "' refers to itself (directly or through others).");
  return vars;
}

} // namespace model
