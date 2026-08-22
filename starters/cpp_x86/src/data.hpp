// The fixed symbol set and price series. Must match the other starters and the
// symbol list in k6/grading.js exactly.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace obsidio {

constexpr std::size_t kSeriesLength = 500;

struct Symbol {
  std::string_view name;
  double base_price;
  std::vector<double> series;   // kSeriesLength points, built at startup
  std::string price_json;       // pre-rendered {"symbol":...,"price":...}
};

// Initialise the table. Call once before serving.
void init_data();

// Look up a symbol by name. Returns nullptr if unknown.
// Thread-safe for reads; see update_price() for the write path.
Symbol* find_symbol(std::string_view name);

// Optional POST /price support. Updates the price and re-renders price_json.
// Returns false if the symbol is unknown.
bool update_price(std::string_view name, double price);

// Render the current /price response for `sym` into `out`.
void render_price(const Symbol& sym, std::string& out);

// Compute mean/min/max/stddev over the series and render the /stats response.
// The series pass is done on EVERY call by design -- the spec forbids caching
// the answer.
void render_stats(const Symbol& sym, std::string& out);

}  // namespace obsidio
