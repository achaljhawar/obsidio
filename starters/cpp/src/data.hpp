// Fixed symbol set and price series. Must match k6/grading.js exactly.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace obsidio {

constexpr std::size_t kSeriesLength{500};

struct Symbol {
  std::string_view name;
  double base_price;
  std::vector<double> series;   // kSeriesLength points, built at startup
  std::string price_json;       // pre-rendered {"symbol":...,"price":...}
};

// Initialise the table. Call once before serving.
void init_data();

// Look up a symbol by name; nullptr if unknown. Thread-safe for reads.
Symbol* find_symbol(std::string_view name);

// Update a price and re-render its JSON. Returns false if unknown.
bool update_price(std::string_view name, double price);

// Render the current /price response for `sym` into `out`.
void render_price(const Symbol& sym, std::string& out);

// Compute mean/min/max/stddev over the series and render /stats. The pass runs
// on every call -- the spec forbids caching the answer.
void render_stats(const Symbol& sym, std::string& out);

}  // namespace obsidio
