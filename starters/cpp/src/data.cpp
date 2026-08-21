#include "data.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <shared_mutex>

namespace obsidio {
namespace {

// Same eight symbols and base prices as the Go/Node/Python/Java starters.
std::array<Symbol, 8> g_symbols = {{
    {"AAPL", 187.42, {}, {}},
    {"GOOG", 141.80, {}, {}},
    {"MSFT", 412.30, {}, {}},
    {"AMZN", 178.10, {}, {}},
    {"NVDA", 120.15, {}, {}},
    {"META", 502.60, {}, {}},
    {"TSLA", 244.70, {}, {}},
    {"JPM",  198.35, {}, {}},
}};

// Guards base_price / price_json only. The series is immutable after startup.
// POST /price is not part of the graded load mix, so this lock is uncontended
// in practice; measure before replacing it with anything cleverer.
std::shared_mutex g_price_mutex;

// Shortest round-trip double formatting, matching what Go's encoding/json and
// JavaScript's JSON.stringify produce (187.42 stays "187.42", not
// "187.41999999999999").
void append_double(std::string& out, double v) {
  char buf[32];
  auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
  if (ec == std::errc()) out.append(buf, end - buf);
  else out.append("0");
}

void render_price_locked(const Symbol& sym, std::string& out) {
  out.assign("{\"symbol\":\"");
  out.append(sym.name);
  out.append("\",\"price\":");
  append_double(out, sym.base_price);
  out.push_back('}');
}

}  // namespace

void init_data() {
  for (auto& sym : g_symbols) {
    sym.series.resize(kSeriesLength);
    for (std::size_t i = 0; i < kSeriesLength; ++i) {
      sym.series[i] = sym.base_price * (1.0 + std::sin(static_cast<double>(i)) / 50.0);
    }
    render_price_locked(sym, sym.price_json);
  }
}

Symbol* find_symbol(std::string_view name) {
  // Eight short strings: a linear scan with a length guard beats a hash map,
  // and it is branch-predictable.
  for (auto& sym : g_symbols) {
    if (sym.name.size() == name.size() && sym.name == name) return &sym;
  }
  return nullptr;
}

bool update_price(std::string_view name, double price) {
  Symbol* sym = find_symbol(name);
  if (sym == nullptr) return false;
  std::unique_lock lock(g_price_mutex);
  sym->base_price = price;
  render_price_locked(*sym, sym->price_json);
  return true;
}

void render_price(const Symbol& sym, std::string& out) {
  std::shared_lock lock(g_price_mutex);
  out.assign(sym.price_json);
}

void render_stats(const Symbol& sym, std::string& out) {
  const double* data = sym.series.data();
  const double n = static_cast<double>(kSeriesLength);

  double sum = 0.0;
  double mn = data[0];
  double mx = data[0];
  for (std::size_t i = 0; i < kSeriesLength; ++i) {
    const double v = data[i];
    sum += v;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  const double mean = sum / n;

  double variance = 0.0;
  for (std::size_t i = 0; i < kSeriesLength; ++i) {
    const double d = data[i] - mean;
    variance += d * d;
  }
  const double stddev = std::sqrt(variance / n);

  out.assign("{\"symbol\":\"");
  out.append(sym.name);
  out.append("\",\"mean\":");
  append_double(out, mean);
  out.append(",\"min\":");
  append_double(out, mn);
  out.append(",\"max\":");
  append_double(out, mx);
  out.append(",\"stddev\":");
  append_double(out, stddev);
  out.push_back('}');
}

}  // namespace obsidio
