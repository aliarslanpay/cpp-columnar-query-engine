#include "cqe/zone_mapped_table.hpp"

namespace cqe {

ZoneMappedTable::ZoneMappedTable(std::span<const Event> rows, std::size_t block_capacity)
    : store_(rows), zone_map_(store_, block_capacity) {}

}  // namespace cqe
